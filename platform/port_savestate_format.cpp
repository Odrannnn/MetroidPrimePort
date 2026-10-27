#include "port_savestate.h"
#include "port_hash.h"
#include "port_bytes.h"

#include <algorithm>
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
// 1: areas, objects. 2: also one scripting fingerprint per captured area
// (explicitly marked present), so a restore can tell whether the rebuilt area
// loaded the same room script data the state was taken with.
constexpr uint32_t kRoomVersionLegacy = 1;
constexpr uint32_t kRoomVersion = 2;
constexpr uint32_t kRoomVersionLayers = 3;  // version 2 plus the spawn layer section
// Version 3 with 16-bit block lengths (an enemy's kTagFields block runs to kilobytes).
constexpr uint32_t kRoomVersionWide = 4;
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
    if (n > kMaxLabel)
      ok = false;
    if (!Need(n))
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

// A fingerprint section is only meaningful one per captured area, in the same
// order: a reader must never pair an area with another area's fingerprint.
bool FingerprintsLineUp(const RoomState& room) {
  if (room.fingerprints.size() != room.areas.size())
    return false;
  for (size_t i = 0; i < room.areas.size(); ++i) {
    if (room.fingerprints[i].area != room.areas[i])
      return false;
  }
  return true;
}

bool SpawnLayersLineUp(const RoomState& room) {
  if (room.spawnLayers.size() != room.areas.size())
    return false;
  for (size_t i = 0; i < room.areas.size(); ++i) {
    if (room.spawnLayers[i].area != room.areas[i])
      return false;
  }
  return true;
}

std::string EncodeRoom(const RoomState& room) {
  // Fingerprints belong to what this writer produces, so a state whose section
  // does not line up is not written at all rather than as a body whose sections
  // mean something else: an omitted trailer loads like a memory card save, while
  // a legacy body would apply objects to an area that may no longer have them.
  // A state that genuinely carries no fingerprints (an older writer) still
  // writes the legacy body.
  const bool recorded = room.fingerprintsRecorded;
  if (recorded && !FingerprintsLineUp(room))
    return {};
  // The spawn layer section rides on version 3, which also carries fingerprints.
  const bool layers = recorded && room.spawnLayersRecorded;
  if (layers && !SpawnLayersLineUp(room))
    return {};
  std::string body;
  AppendLE32(body, static_cast< uint32_t >(room.areas.size()));
  for (int32_t a : room.areas)
    AppendLE32(body, static_cast< uint32_t >(a));
  if (recorded) {
    // Explicit presence, then one entry per area (in `areas` order): the known
    // bit says whether that area's data was hashed when the state was written;
    // an area whose data could not be read carries no hash.
    body += static_cast< char >(1);
    AppendLE32(body, static_cast< uint32_t >(room.fingerprints.size()));
    for (const AreaFingerprint& fp : room.fingerprints) {
      AppendLE32(body, static_cast< uint32_t >(fp.area));
      body += fp.known ? static_cast< char >(1) : static_cast< char >(0);
      AppendLE64(body, fp.known ? fp.hash : 0);
    }
  }
  if (layers) {
    // One entry per area in `areas` order: the known bit, then the layer mask.
    AppendLE32(body, static_cast< uint32_t >(room.spawnLayers.size()));
    for (const AreaSpawnLayers& sl : room.spawnLayers) {
      AppendLE32(body, static_cast< uint32_t >(sl.area));
      body += sl.known ? static_cast< char >(1) : static_cast< char >(0);
      AppendLE64(body, sl.known ? sl.mask : 0);
    }
  }
  AppendLE32(body, static_cast< uint32_t >(room.objects.size()));
  uint32_t last = 0;
  for (const RoomObject& obj : room.objects) {
    // The present set is the objects' ids; a block count byte carries the active flag.
    AppendDelta(body, last, obj.editorId);
    // At most kMaxBlocks, the most DecodeRoom accepts (the count byte has 7 bits).
    auto fits = [layers](const RoomBlock& block) {
      return block.tag < 256 && block.data.size() < (layers ? kMaxBlockBytes : 256u);
    };
    size_t count = 0;
    for (const RoomBlock& block : obj.blocks)
      count += fits(block) ? 1 : 0;
    count = std::min< size_t >(count, kMaxBlocks);
    body += static_cast< char >((obj.active ? 0x80 : 0) | count);
    size_t written = 0;
    for (const RoomBlock& block : obj.blocks) {
      if (!fits(block))
        continue;
      if (written++ == count)
        break;
      body += static_cast< char >(block.tag);
      body += static_cast< char >(block.data.size());
      if (layers)
        body += static_cast< char >(block.data.size() >> 8);
      body += block.data;
    }
  }
  std::string out(kRoomMagic, 4);
  AppendLE32(out, layers ? kRoomVersionWide : (recorded ? kRoomVersion : kRoomVersionLegacy));
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
  if (version != kRoomVersionLegacy && version != kRoomVersion && version != kRoomVersionLayers &&
      version != kRoomVersionWide)
    return true;
  const bool wide = version >= kRoomVersionWide;
  RoomState r;
  const uint32_t areaCount = in.U32();
  if (areaCount > kMaxRoomCount)
    return false;
  for (uint32_t i = 0; i < areaCount && in.ok; ++i)
    r.areas.push_back(static_cast< int32_t >(in.U32()));
  if (!in.ok)
    return false;
  if (version >= kRoomVersion) {
    // Explicit presence byte: version 2 always says whether it carries
    // fingerprints, so a reader never has to guess from a hash value.
    const uint32_t present = in.Need(1) ? static_cast< uint8_t >(in.data[in.pos++]) : 0;
    if (present > 1)
      return false;
    r.fingerprintsRecorded = present != 0;
    if (r.fingerprintsRecorded) {
      // One entry per captured area, in the same order (the writer records one
      // even when that area's data could not be read). A count or association
      // that does not line up is malformed: the whole room state is then dropped
      // rather than applied to areas it does not describe.
      const uint32_t count = in.U32();
      if (count > kMaxRoomCount || count != areaCount)
        return false;
      for (uint32_t i = 0; i < count && in.ok; ++i) {
        AreaFingerprint fp;
        fp.area = static_cast< int32_t >(in.U32());
        const uint32_t flags = in.Need(1) ? static_cast< uint8_t >(in.data[in.pos++]) : 0;
        if (flags > 1)
          return false;  // only the known bit is defined
        fp.known = (flags & 1) != 0;
        fp.hash = in.U64();
        if (!in.ok)
          return false;
        // Positional association with the area it belongs to, areas increasing
        // as the capture wrote them. The known bit is explicit, so a hash of 0
        // is a valid value for an entry that was hashed; an entry that was not
        // carries no hash.
        if (fp.area != r.areas[i] || (i > 0 && r.areas[i] <= r.areas[i - 1]) || fp.area < 0 ||
            (!fp.known && fp.hash != 0))
          return false;
        r.fingerprints.push_back(fp);
      }
      if (!in.ok)
        return false;
    }
  }
  if (version >= kRoomVersionLayers) {
    // Same shape rules as the fingerprints: one entry per area, same order.
    const uint32_t count = in.U32();
    if (count > kMaxRoomCount || count != areaCount)
      return false;
    r.spawnLayersRecorded = true;
    for (uint32_t i = 0; i < count && in.ok; ++i) {
      AreaSpawnLayers sl;
      sl.area = static_cast< int32_t >(in.U32());
      const uint32_t flags = in.Need(1) ? static_cast< uint8_t >(in.data[in.pos++]) : 0;
      if (flags > 1)
        return false;
      sl.known = flags != 0;
      sl.mask = in.U64();
      if (!in.ok || sl.area != r.areas[i] || (!sl.known && sl.mask != 0))
        return false;
      r.spawnLayers.push_back(sl);
    }
    if (!in.ok)
      return false;
  }
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
      if (!in.Need(wide ? 3 : 2))
        return false;
      block.tag = static_cast< uint8_t >(in.data[in.pos++]);
      uint32_t len = static_cast< uint8_t >(in.data[in.pos++]);
      if (wide)
        len |= static_cast< uint32_t >(static_cast< uint8_t >(in.data[in.pos++])) << 8;
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

std::string EncodeDoorSaveState(const DoorSaveState& state) {
  uint8_t flags = 0;
  if (state.pendingClose)
    flags |= 0x1;
  if (state.refusedClosePending)
    flags |= 0x2;
  if (state.doClose)
    flags |= 0x4;
  flags |= static_cast< uint8_t >(state.partner << kDoorSavePartnerShift);
  std::string out;
  out.push_back(static_cast< char >(state.open ? 1 : 0));
  out.push_back(static_cast< char >(flags));
  return out;
}

bool DecodeDoorSaveState(const std::string& data, DoorSaveState& out) {
  // One byte is the old payload (the open flag alone); two is this build's.
  if (data.empty() || data.size() > 2)
    return false;
  const uint8_t open = static_cast< uint8_t >(data[0]);
  if (open > 1)
    return false;
  // Parsed locally and published only once every byte has validated, so a
  // refused payload leaves the caller's state exactly as it was.
  DoorSaveState parsed;
  parsed.open = open != 0;
  if (data.size() == 2) {
    const uint8_t flags = static_cast< uint8_t >(data[1]);
    if ((flags & ~kDoorSaveFlagsKnown) != 0)
      return false;  // a bit this build does not define
    parsed.pendingClose = (flags & 0x1) != 0;
    parsed.refusedClosePending = (flags & 0x2) != 0;
    parsed.doClose = (flags & 0x4) != 0;
    parsed.partner = static_cast< DoorSaveState::Partner >((flags >> kDoorSavePartnerShift) & 0x3);
    // A door that is open has no close in flight, the two close paths are
    // different branches of Think that cannot both be owed, and only an open door
    // has a side of a door pair.
    if ((parsed.open && (parsed.pendingClose || parsed.refusedClosePending)) ||
        (parsed.pendingClose && parsed.refusedClosePending) ||
        (!parsed.open && parsed.partner != DoorSaveState::kPartnerUnknown))
      return false;
  }
  out = parsed;
  return true;
}

bool JournalsScriptMsg(int msg) {
  // EScriptObjectMessage values (CEntityInfo.hpp); this file stays free of game headers.
  switch (msg) {
  case 3:   // Close
  case 5:   // Decrement
  case 6:   // Follow
  case 7:   // Increment
  case 9:   // Open
  case 10:  // Reset
  case 11:  // ResetAndStart
  case 12:  // SetToMax
  case 13:  // SetToZero
  case 14:  // Start
  case 15:  // Stop
  case 16:  // StopAndReset
  case 18:  // UNKM18
  case 19:  // Action
  case 20:  // Play
  case 21:  // Alert
    return true;
  default:
    // Activate, Deactivate, ToggleActive: the saved active flag. Arrived, Next:
    // waypoint traffic. 22 and up: engine notifications, not script commands.
    return false;
  }
}

bool AppendScriptMsg(std::vector< ScriptMsgEntry >& journal, const ScriptMsgEntry& entry) {
  if (!journal.empty() && journal.back().msg == entry.msg && journal.back().sender == entry.sender)
    return false;
  if (journal.size() >= kMaxScriptMsgs)
    return false;
  journal.push_back(entry);
  return true;
}

std::string EncodeScriptMsgs(const std::vector< ScriptMsgEntry >& journal) {
  const size_t count = std::min(journal.size(), kMaxScriptMsgs);
  std::string out;
  out.push_back(static_cast< char >(count));
  for (size_t i = 0; i < count; ++i) {
    out.push_back(static_cast< char >(journal[i].msg));
    AppendLE32(out, journal[i].sender);
  }
  return out;
}

bool DecodeScriptMsgs(const std::string& data, std::vector< ScriptMsgEntry >& out) {
  if (data.empty())
    return false;
  const size_t count = static_cast< uint8_t >(data[0]);
  if (count > kMaxScriptMsgs || data.size() != 1 + 5 * count)
    return false;
  std::vector< ScriptMsgEntry > parsed;
  for (size_t i = 0; i < count; ++i) {
    const size_t at = 1 + 5 * i;
    ScriptMsgEntry e;
    e.msg = static_cast< uint8_t >(data[at]);
    if (!JournalsScriptMsg(e.msg))
      return false;
    e.sender = 0;
    for (int b = 0; b < 4; ++b)
      e.sender |= static_cast< uint32_t >(static_cast< uint8_t >(data[at + 1 + b])) << (8 * b);
    parsed.push_back(e);
  }
  out = std::move(parsed);
  return true;
}

uint64_t FingerprintScriptData(const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0)
    return 0;
  return port::Fnv1a64(data, size);
}

AreaFingerprint MakeAreaFingerprint(int32_t area, const uint8_t* data, size_t size) {
  AreaFingerprint fp;
  fp.area = area;
  fp.known = data != nullptr && size != 0;
  fp.hash = fp.known ? FingerprintScriptData(data, size) : 0;
  return fp;
}

RoomAreaRestore DecideAreaRestore(const RoomState& room, int32_t area, const uint8_t* data, size_t size) {
  // A state from before fingerprints existed (or one written without them)
  // restores exactly as it always did.
  if (!room.fingerprintsRecorded)
    return RoomAreaRestore::kRestore;
  const AreaFingerprint* found = nullptr;
  for (const AreaFingerprint& fp : room.fingerprints) {
    if (fp.area == area) {
      found = &fp;
      break;
    }
  }
  // No entry for this area, nothing recorded for it at save time, or nothing
  // readable now: nothing proves the loaded room is the one saved against.
  if (found == nullptr || !found->known || data == nullptr || size == 0)
    return RoomAreaRestore::kSkipUnreadable;
  return found->hash == FingerprintScriptData(data, size) ? RoomAreaRestore::kRestore
                                                         : RoomAreaRestore::kSkipChanged;
}

bool PlatformRestoreOrder(const std::vector< std::vector< size_t > >& parents,
                          std::vector< size_t >& order) {
  order.clear();
  // 0 unvisited, 1 on the current path, 2 placed.
  std::vector< uint8_t > mark(parents.size(), 0);
  const auto visit = [&](const auto& self, size_t i) -> bool {
    if (mark[i] == 2)
      return true;
    if (mark[i] == 1)
      return false;
    mark[i] = 1;
    for (size_t parent : parents[i]) {
      if (parent >= parents.size() || !self(self, parent))
        return false;
    }
    mark[i] = 2;
    order.push_back(i);
    return true;
  };
  for (size_t i = 0; i < parents.size(); ++i) {
    if (!visit(visit, i)) {
      order.clear();
      return false;
    }
  }
  return true;
}

const char* RoomAreaRestoreReason(RoomAreaRestore decision) {
  switch (decision) {
  case RoomAreaRestore::kSkipChanged:
    return "the room script changed";
  case RoomAreaRestore::kSkipUnreadable:
    return "no usable room script fingerprint";
  case RoomAreaRestore::kRestore:
    break;
  }
  return "restored";
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
  // Serial 0 is no card: a resume written without one is never offered.
  return stored.cardSerial != 0 && stored.cardSerial == current.cardSerial && stored.identity == current.identity &&
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
