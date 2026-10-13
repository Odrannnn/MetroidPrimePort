#include "port_savestate.h"
#include "port_bytes.h"

#include <cstring>

// The slot file layout. Kept apart from port_savestate.cpp so the unit test
// can build it without the game.
namespace PortSaveState {
namespace {

constexpr char kMagic[4] = {'M', 'P', 'S', 'S'};
constexpr uint32_t kVersion = 1;
// Game-state blobs are a few hundred bytes; anything past this is not ours.
constexpr uint32_t kMaxBlob = 1u << 20;
constexpr uint32_t kMaxLabel = 256;
// The optional room-state trailer after the blob: magic, version, byte length,
// then the body. A version this reader does not know is skipped, not an error.
constexpr char kRoomMagic[4] = {'R', 'M', 'S', 'T'};
constexpr uint32_t kRoomVersion = 1;
constexpr uint32_t kMaxRoomBytes = 8u << 20;
constexpr uint32_t kMaxRoomCount = 1u << 16;
constexpr uint32_t kMaxBlocks = 64;
constexpr uint32_t kMaxBlockBytes = 1u << 16;

using port::AppendLE32;
using port::AppendLE64;
using port::AppendLEFloat;
void PutF64(std::string& out, double d) {
  uint64_t v;
  std::memcpy(&v, &d, 8);
  AppendLE64(out, v);
}
void PutString(std::string& out, const std::string& s) {
  const std::string cut = s.substr(0, kMaxLabel);
  AppendLE32(out, static_cast< uint32_t >(cut.size()));
  out += cut;
}

struct Reader {
  const std::string& data;
  size_t pos = 0;
  bool ok = true;

  bool Need(size_t n) {
    if (!ok || data.size() - pos < n)
      ok = false;
    return ok;
  }
  uint32_t U32() {
    if (!Need(4))
      return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
      v |= static_cast< uint32_t >(static_cast< unsigned char >(data[pos + i])) << (i * 8);
    pos += 4;
    return v;
  }
  uint64_t U64() {
    const uint64_t lo = U32();
    const uint64_t hi = U32();
    return lo | (hi << 32);
  }
  float F32() {
    const uint32_t v = U32();
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  }
  double F64() {
    const uint64_t v = U64();
    double d;
    std::memcpy(&d, &v, 8);
    return d;
  }
  std::string String() {
    const uint32_t n = U32();
    if (n > kMaxLabel || !Need(n))
      return {};
    std::string s = data.substr(pos, n);
    pos += n;
    return s;
  }
};

// Editor ids come in runs, so each is written as a zigzag varint delta of the last.
void AppendDelta(std::string& out, uint32_t& last, uint32_t id) {
  const int32_t d = static_cast< int32_t >(id - last);
  uint32_t v = (static_cast< uint32_t >(d) << 1) ^ static_cast< uint32_t >(d >> 31);
  last = id;
  while (v >= 0x80) {
    out += static_cast< char >(0x80 | (v & 0x7f));
    v >>= 7;
  }
  out += static_cast< char >(v);
}

bool ReadDelta(Reader& in, uint32_t& last, uint32_t& id) {
  uint32_t v = 0;
  for (int shift = 0; shift < 35; shift += 7) {
    if (!in.Need(1))
      return false;
    const uint8_t b = static_cast< uint8_t >(in.data[in.pos++]);
    v |= static_cast< uint32_t >(b & 0x7f) << shift;
    if ((b & 0x80) == 0) {
      const int32_t d = static_cast< int32_t >(v >> 1) ^ -static_cast< int32_t >(v & 1);
      last += static_cast< uint32_t >(d);
      id = last;
      return true;
    }
  }
  return false;
}

std::string EncodeRoom(const RoomState& room) {
  std::string body;
  AppendLE32(body, static_cast< uint32_t >(room.areas.size()));
  for (int32_t a : room.areas)
    AppendLE32(body, static_cast< uint32_t >(a));
  AppendLE32(body, static_cast< uint32_t >(room.objects.size()));
  uint32_t last = 0;
  for (const RoomObject& obj : room.objects) {
    // The present set is the objects' ids; a block count byte carries the active flag.
    AppendDelta(body, last, obj.editorId);
    size_t count = 0;
    for (const RoomBlock& block : obj.blocks)
      count += (block.tag < 256 && block.data.size() < 256) ? 1 : 0;
    body += static_cast< char >((obj.active ? 0x80 : 0) | (count & 0x7f));
    for (const RoomBlock& block : obj.blocks) {
      if (block.tag >= 256 || block.data.size() >= 256)
        continue;
      body += static_cast< char >(block.tag);
      body += static_cast< char >(block.data.size());
      body += block.data;
    }
  }
  std::string out(kRoomMagic, 4);
  AppendLE32(out, kRoomVersion);
  AppendLE32(out, static_cast< uint32_t >(body.size()));
  return out + body;
}

// Reads what follows the blob. False only for a malformed trailer; an unknown
// version leaves `room` empty and succeeds.
bool DecodeRoom(Reader& in, RoomState& room) {
  room = {};
  if (in.pos == in.data.size())
    return true;
  if (!in.Need(12) || std::memcmp(in.data.data() + in.pos, kRoomMagic, 4) != 0)
    return false;
  in.pos += 4;
  const uint32_t version = in.U32();
  const uint32_t size = in.U32();
  if (size > kMaxRoomBytes || !in.Need(size) || in.pos + size != in.data.size())
    return false;
  if (version != kRoomVersion)
    return true;
  RoomState r;
  const uint32_t areaCount = in.U32();
  if (areaCount > kMaxRoomCount)
    return false;
  for (uint32_t i = 0; i < areaCount && in.ok; ++i)
    r.areas.push_back(static_cast< int32_t >(in.U32()));
  const uint32_t objectCount = in.U32();
  if (objectCount > kMaxRoomCount)
    return false;
  uint32_t last = 0;
  for (uint32_t i = 0; i < objectCount && in.ok; ++i) {
    RoomObject obj;
    if (!ReadDelta(in, last, obj.editorId))
      return false;
    const uint32_t flags = in.Need(1) ? static_cast< uint8_t >(in.data[in.pos++]) : 0;
    obj.active = (flags & 0x80) != 0;
    const uint32_t blockCount = flags & 0x7f;
    if (blockCount > kMaxBlocks)
      return false;
    for (uint32_t b = 0; b < blockCount && in.ok; ++b) {
      RoomBlock block;
      if (!in.Need(2))
        return false;
      block.tag = static_cast< uint8_t >(in.data[in.pos++]);
      const uint32_t len = static_cast< uint8_t >(in.data[in.pos++]);
      if (!in.Need(len))
        return false;
      block.data = in.data.substr(in.pos, len);
      in.pos += len;
      obj.blocks.push_back(std::move(block));
    }
    r.present.push_back(obj.editorId);
    r.objects.push_back(std::move(obj));
  }
  if (!in.ok || in.pos != in.data.size())
    return false;
  room = std::move(r);
  return true;
}

} // namespace

void BlockWriter::U32(uint32_t v) { AppendLE32(bytes, v); }
void BlockWriter::F32(float f) { AppendLEFloat(bytes, f); }
void BlockWriter::Str(const std::string& s) { PutString(bytes, s); }

uint8_t BlockReader::U8() {
  if (!ok || bytes.size() - pos < 1) {
    ok = false;
    return 0;
  }
  return static_cast< uint8_t >(bytes[pos++]);
}
uint32_t BlockReader::U32() {
  Reader r{bytes, pos};
  const uint32_t v = r.U32();
  ok = ok && r.ok;
  pos = r.pos;
  return v;
}
float BlockReader::F32() {
  Reader r{bytes, pos};
  const float v = r.F32();
  ok = ok && r.ok;
  pos = r.pos;
  return v;
}
std::string BlockReader::Str() {
  Reader r{bytes, pos};
  std::string v = r.String();
  ok = ok && r.ok;
  pos = r.pos;
  return v;
}

const RoomBlock* FindBlock(const RoomObject& object, uint32_t tag) {
  for (const RoomBlock& block : object.blocks) {
    if (block.tag == tag)
      return &block;
  }
  return nullptr;
}

std::string Encode(const Header& header, const std::vector< uint8_t >& blob) {
  std::string out(kMagic, 4);
  AppendLE32(out, kVersion);
  AppendLE32(out, header.worldId);
  AppendLE32(out, static_cast< uint32_t >(header.areaId));
  for (float f : header.position)
    AppendLEFloat(out, f);
  for (float f : header.forward)
    AppendLEFloat(out, f);
  AppendLE32(out, header.morphed ? 1 : 0);
  PutF64(out, header.playTime);
  AppendLE64(out, static_cast< uint64_t >(header.savedAt));
  PutString(out, header.world);
  PutString(out, header.room);
  AppendLE32(out, static_cast< uint32_t >(blob.size()));
  out.append(reinterpret_cast< const char* >(blob.data()), blob.size());
  if (!header.roomState.empty())
    out += EncodeRoom(header.roomState);
  return out;
}

bool Decode(const std::string& data, Header& header, std::vector< uint8_t >& blob) {
  if (data.size() < 8 || std::memcmp(data.data(), kMagic, 4) != 0)
    return false;
  Reader in{data, 4};
  if (in.U32() != kVersion)
    return false;
  Header h;
  h.worldId = in.U32();
  h.areaId = static_cast< int32_t >(in.U32());
  for (float& f : h.position)
    f = in.F32();
  for (float& f : h.forward)
    f = in.F32();
  h.morphed = in.U32() != 0;
  h.playTime = in.F64();
  h.savedAt = static_cast< int64_t >(in.U64());
  h.world = in.String();
  h.room = in.String();
  const uint32_t size = in.U32();
  if (!in.ok || size == 0 || size > kMaxBlob || !in.Need(size))
    return false;
  for (float f : h.position)
    if (!(f == f) || f > 1e7f || f < -1e7f)
      return false;
  const size_t blobPos = in.pos;
  in.pos += size;
  // Anything after the blob is the room state. A broken one is dropped (the
  // state still loads as a memory card load would).
  if (in.pos != data.size() &&
      (data.size() - in.pos < 12 || std::memcmp(data.data() + in.pos, kRoomMagic, 4) != 0)) {
    return false;
  }
  Reader room{data, in.pos};
  if (!DecodeRoom(room, h.roomState))
    h.roomState = {};
  blob.assign(data.begin() + blobPos, data.begin() + blobPos + size);
  header = h;
  return true;
}

constexpr char kResumeMagic[4] = {'M', 'P', 'R', 'S'};
constexpr uint32_t kResumeVersion = 1;

std::string EncodeResume(const ResumeMeta& meta, const std::string& stateFile) {
  std::string out(kResumeMagic, 4);
  AppendLE32(out, kResumeVersion);
  AppendLE32(out, meta.fileIdx);
  AppendLE64(out, meta.cardSerial);
  PutString(out, meta.identity);
  AppendLE32(out, static_cast< uint32_t >(stateFile.size()));
  out += stateFile;
  return out;
}

bool DecodeResume(const std::string& data, ResumeMeta& meta, std::string& stateFile) {
  if (data.size() < 8 || std::memcmp(data.data(), kResumeMagic, 4) != 0)
    return false;
  Reader in{data, 4};
  if (in.U32() != kResumeVersion)
    return false;
  ResumeMeta m;
  m.fileIdx = in.U32();
  m.cardSerial = in.U64();
  m.identity = in.String();
  const uint32_t size = in.U32();
  if (!in.ok || m.fileIdx > 2 || size == 0 || size > kMaxBlob + 4096 + kMaxRoomBytes + 16 || !in.Need(size) ||
      in.pos + size != data.size())
    return false;
  meta = m;
  stateFile = data.substr(in.pos);
  return true;
}

bool ResumeMatches(const ResumeMeta& stored, const ResumeMeta& current) {
  return stored.cardSerial == current.cardSerial && stored.identity == current.identity &&
         stored.fileIdx <= 2;
}

const char* ResumeSkipReason(const ResumeContext& ctx) {
  if (!ctx.inGame)
    return "not in gameplay";
  if (!ctx.initialized)
    return "the world is still loading";
  if (ctx.quitting)
    return "the game is quitting";
  if (ctx.dead)
    return "Samus is dead";
  if (ctx.cinematic)
    return "a cutscene is playing";
  if (ctx.transition)
    return "a world transition is in progress";
  if (ctx.saveUi)
    return "the save screen is open";
  return nullptr;
}

ResumeWriteChoice ChooseResumeWrite(bool safeNow, bool haveSnapshot) {
  if (safeNow)
    return ResumeWriteChoice::kRefreshThenWrite;
  return haveSnapshot ? ResumeWriteChoice::kWriteSnapshot : ResumeWriteChoice::kNothing;
}

bool ResumeRefreshDue(bool safeNow, bool haveSnapshot, bool sameRoomAsSnapshot, double secondsInRoom,
                      double secondsSinceSnapshot) {
  if (!safeNow)
    return false;
  if (!haveSnapshot)
    return secondsInRoom >= kResumeStableSeconds;
  if (!sameRoomAsSnapshot)
    return secondsInRoom >= kResumeStableSeconds;
  return secondsSinceSnapshot >= kResumeRefreshSeconds;
}

} // namespace PortSaveState
