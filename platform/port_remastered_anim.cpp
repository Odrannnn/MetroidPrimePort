// See platform/include/port_remastered_anim.h for what this is and why it exists.
// A port of build/mpr/anim/chpr_anim.py and chpr_skel.py. The arithmetic is done in
// doubles with the same float roundings the Python applies, so the output matches it.

#include "port_remastered_anim.h"
#include "port_strings.h"
#include "port_bytes.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>

namespace PortRemasteredAnim {
namespace {

constexpr float kFramesPerRate = 60.0f;  // Anim::fps

// A problem with the file or a layout the decoder does not know. Thrown inside this
// file only and turned into the error string at the API boundary.
struct Failure {
  std::string message;
};

[[noreturn]] void Fail(const std::string& message) { throw Failure{message}; }

using port::ReadLE16;
using port::ReadLE32;
using port::ReadLEFloat;

// The Python rounds to single precision at a few points; this is that rounding.
double F32(double x) { return double(float(x)); }

// The file as a bounds-checked view.
struct Bytes {
  const uint8_t* data;
  size_t size;

  bool Has(size_t offset, size_t count) const { return offset <= size && count <= size - offset; }
  void Need(size_t offset, size_t count, const char* what) const {
    if (!Has(offset, count)) {
      Fail(std::string("truncated ") + what);
    }
  }
};

// CAnimBitStream: bits are read least significant first. Reading past the end gives
// zeros, as the Python does; callers that loop on the stream check Overrun().
class BitStream {
public:
  BitStream(const Bytes& bytes, uint64_t bitPos) : m_bytes(bytes), m_pos(bitPos) {}

  uint32_t Read(int count) {
    uint32_t value = 0;
    for (int i = 0; i < count; ++i) {
      const uint64_t byteIndex = m_pos >> 3;
      const uint32_t byte = byteIndex < m_bytes.size ? m_bytes.data[byteIndex] : 0;
      value |= ((byte >> (m_pos & 7)) & 1u) << i;
      ++m_pos;
    }
    return value;
  }

  bool Overrun() const { return m_pos > uint64_t(m_bytes.size) * 8 + 64; }

private:
  Bytes m_bytes;
  uint64_t m_pos;
};

int64_t SignExtend(int64_t v, int bits) {
  if (bits <= 0) {
    return 0;
  }
  return (v & (int64_t(1) << (bits - 1))) ? v - (int64_t(1) << bits) : v;
}

using Quat = std::array<double, 4>;  // w x y z while decoding

Quat Normalize(const Quat& q) {
  double n = 0.0;
  for (double c : q) {
    n += c * c;
  }
  const double r = n > 0.0 ? 1.0 / std::sqrt(n) : 1.0;
  return {q[0] * r, q[1] * r, q[2] * r, q[3] * r};
}

Quat Lerp(const Quat& a, const Quat& b, double t) {
  return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t,
          a[3] + (b[3] - a[3]) * t};
}

// anim_rot_lerp2 from the exe: a two-segment lerp through the normalized midpoint,
// taking the short way round.
Quat RotLerp2(const Quat& a, Quat b, double t) {
  double dot = 0.0;
  for (int i = 0; i < 4; ++i) {
    dot += a[size_t(i)] * b[size_t(i)];
  }
  if (dot < -0.001953125) {
    for (double& c : b) {
      c = -c;
    }
  }
  const Quat mid =
      Normalize({(a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5, (a[2] + b[2]) * 0.5, (a[3] + b[3]) * 0.5});
  Quat r;
  if (t < 0.5) {
    r = Lerp(a, mid, 2 * t);
  } else if (t > 0.5) {
    r = Lerp(mid, b, 2 * (t - 0.5));
  } else {
    r = mid;
  }
  return Normalize(r);
}

// One value of a track (a rotation, or for the vec3 track types a translation or scale):
// how to dequantize it and the two keyframes it is currently interpolating between.
struct Value {
  bool custom = false;  // types 3 to 5: only a bit count in the data stream, no bone
  int realExp = 0;  // type 4: the value is cur / 2^realExp (tween_real32 0xba35d0)
  int customBits = 0;  // bits the data stream holds per keyframe of this value
  bool vec = false;  // a vec3 value: no flag7 or sign bits, one to three components
  int W = 0;  // bits per component
  int q = 0;  // fixed-point shift
  int isSigned = 0;
  int mask = 0;  // bit i set: component i (x, y, z) is stored
  int flag7 = 0;  // interpolate with RotLerp2 instead of lerp + normalize
  int sgnPrev = 0, sgnCur = 0;  // w is negative
  int64_t prev[3] = {0, 0, 0};
  int64_t cur[3] = {0, 0, 0};
  int64_t curFrame = 0, nextFrame = 0;
  double inv = 0.0;
};

struct Track {
  int type = 0;
  int flag = 0;
  uint32_t boolWords = 0;  // type 3: how many words the bool data holds (init-dequant op 5)
  std::vector<Value> values;
};

struct StreamInfo {
  Bytes blob;
  uint32_t offInit = 0;
  uint32_t w1 = 0;  // byte offset of the data bit stream
  uint32_t w2 = 0;  // byte offset of the op stream
  float headerFloat = 0.0f;
  uint32_t frames = 0;
  uint32_t v = 0;  // quantization steps per frame
  uint32_t u6 = 0;  // bits flagging a keyframe change per frame
  uint32_t ctx = 0;  // index of the constant pool this stream reads (InitConstData)
  std::vector<Track> tracks;
};

// Blob layout: 24 byte header, header bits, init-dequant bit stream, then per stream info a
// data bit stream and an op stream. CAnimCompStream::Load reads one info per stream; every
// stream has its own tracks, step length and bit streams.
std::vector<StreamInfo> ParseBlob(const Bytes& d, size_t base) {
  d.Need(base, 24, "animation header");
  const uint32_t size = ReadLE32(d.data + base);
  if (size < 24 || !d.Has(base, size)) {
    Fail("animation blob runs past the end of the file");
  }
  const Bytes blob{d.data + base, size};
  const uint16_t offBits = ReadLE16(d.data + base + 4);
  const uint32_t offInit = ReadLE16(d.data + base + 6);
  const float headerFloat = ReadLEFloat(d.data + base + 12);
  const uint32_t frames = ReadLE16(d.data + base + 16);

  BitStream bs(blob, uint64_t(offBits) * 8);
  const uint32_t streamInfos = bs.Read(4);
  bs.Read(10);  // total tracks
  bs.Read(4);
  std::vector<StreamInfo> out;
  for (uint32_t s = 0; s < streamInfos; ++s) {
    StreamInfo info;
    info.blob = blob;
    info.offInit = offInit;
    info.headerFloat = headerFloat;
    info.frames = frames;
    info.w1 = bs.Read(20);
    info.w2 = bs.Read(20);
    const uint32_t trackCount = bs.Read(4);
    const uint32_t hasConsts = bs.Read(1);
    bs.Read(1);
    info.v = bs.Read(4);
    info.u6 = bs.Read(4);
    info.ctx = bs.Read(4);  // constant pool index
    for (uint32_t i = 0; i < trackCount; ++i) {
      const uint32_t count = bs.Read(10);
      const uint32_t type = bs.Read(3);
      const uint32_t flag = bs.Read(1);
      uint32_t extra = 0;
      if (type == 3) {
        extra = bs.Read(6);  // the bool word count tween_bool_words_op reads
      }
      if (type > 5) {
        Fail("track type " + std::to_string(type) + " is not supported");
      }
      Track t;
      t.type = int(type);
      t.boolWords = extra;
      t.flag = int(flag);
      t.values.resize(count);
      for (Value& val : t.values) {
        val.custom = type > 2;
        val.customBits = type == 3 ? 1 : 0;  // 4 and 5 get their width in InitDequant
      }
      info.tracks.push_back(std::move(t));
    }
    if (hasConsts) {
      // Inline constants (InitConstData): u16 id, f32, optional quaternion, optional vec3.
      const uint32_t entries = bs.Read(4);
      for (uint32_t k = 0; k < entries; ++k) {
        bs.Read(16);
        bs.Read(32);
        if (bs.Read(1)) {
          for (int c = 0; c < 4; ++c) {
            bs.Read(32);
          }
        }
        if (bs.Read(1)) {
          for (int c = 0; c < 3; ++c) {
            bs.Read(32);
          }
        }
      }
    }
    if (bs.Overrun()) {
      Fail("animation header bits run past the blob");
    }
    out.push_back(std::move(info));
  }
  return out;
}

// The init-dequant bit stream is shared by all streams: op 0 selects the stream the next
// ops apply to (CAnimCompStreamObj::InitDequant, init_stream_inst).
void InitDequant(std::vector<StreamInfo>& infos) {
  if (infos.empty()) {
    return;
  }
  BitStream bs(infos[0].blob, uint64_t(infos[0].offInit) * 8);
  size_t current = 0;
  for (;;) {
    if (bs.Overrun()) {
      Fail("init-dequant stream has no end marker");
    }
    const uint32_t op = bs.Read(4);
    if (op == 0) {
      current = bs.Read(4);
      if (current >= infos.size()) {
        Fail("init-dequant selects a stream that does not exist");
      }
      continue;
    }
    if (op == 1) {
      break;
    }
    if (op < 2 || op > 6) {
      Fail("init-dequant op " + std::to_string(op) + " is not supported");
    }
    StreamInfo& info = infos[current];
    const uint32_t index = bs.Read(4);
    if (index >= info.tracks.size()) {
      Fail("init-dequant names a track that does not exist");
    }
    Track& track = info.tracks[index];
    if (op == 2 || op == 3) {  // 2 = rotation values, 3 = vec3 values; same layout
      for (Value& val : track.values) {
        val.W = int(bs.Read(5));
        val.q = int(bs.Read(5));
        val.isSigned = int(bs.Read(1));
        val.mask = int(bs.Read(3));
        val.vec = track.type != 0;
        // The game's do-while loads one component even for mask 0 (into x).
        if (val.vec && val.mask == 0) {
          val.mask = 1;
        }
      }
    } else if (op == 4) {  // real32 values: bit width, fraction exponent, signed flag
      // NAnimComp::init_dequant_real32 0xbb7bf0 stores them; dquant_int32_and_real32_gen
      // 0xba2cc0 reads `width` bits per keyframe. The exponent and flag only shape the value.
      if (track.type != 4) {
        Fail("init-dequant real32 names a track of another type");
      }
      for (Value& val : track.values) {
        val.customBits = int(bs.Read(5));
        val.realExp = int(bs.Read(5));
        val.isSigned = int(bs.Read(1));
      }
    } else if (op == 5) {  // bool: 12 bit word count, then a flag plus 16 bits each
      const uint32_t entries = bs.Read(12);
      for (uint32_t k = 0; k < entries; ++k) {
        bs.Read(1);
        bs.Read(16);
      }
    } else {  // 6: int32 values, bit width and signed flag (init_dequant_int32 0xbb7e90)
      if (track.type != 5) {
        Fail("init-dequant int32 names a track of another type");
      }
      for (Value& val : track.values) {
        val.customBits = int(bs.Read(5));
        val.isSigned = int(bs.Read(1));
      }
    }
  }
}

// CAnimCompStreamInst: walks the data bit stream one frame at a time.
class Stream {
public:
  explicit Stream(StreamInfo& info)
      : m_info(info), m_bs(info.blob, uint64_t(info.w1) * 8) {}

  int64_t Cur() const { return m_cur; }

  void InitFrames() {
    m_cur = 0;
    for (Track& tr : m_info.tracks) {
      for (Value& val : tr.values) {
        for (int i = 0; i < 3; ++i) {
          val.cur[i] = 0;
          val.prev[i] = 0;
        }
        val.curFrame = 0;
        val.nextFrame = 0;
        val.inv = 0.0;
        val.sgnCur = val.sgnPrev = 0;
      }
    }
    Load(SetupNext());
    Load(SetupStepped(0));
  }

  void DequantFrame() {
    ++m_cur;
    if (m_bs.Read(int(m_info.u6))) {
      if (m_cur == (int64_t(m_info.frames) - 1) * int64_t(m_info.v)) {
        Load(SetupStepped(1));
      } else {
        Load(SetupNext());
      }
    }
  }

private:
  std::vector<Value*> SetupNext() {
    const int width = int(m_bs.Read(4));
    std::vector<Value*> list;
    for (Track& tr : m_info.tracks) {
      for (Value& val : tr.values) {
        if (val.nextFrame == m_cur) {
          const int64_t delta = m_bs.Read(width);
          val.curFrame = val.nextFrame;
          val.nextFrame += delta;
          val.inv = delta ? F32(1.0 / double(delta)) : 0.0;
          list.push_back(&val);
        }
      }
    }
    return list;
  }

  std::vector<Value*> SetupStepped(int flag) {
    std::vector<Value*> list;
    for (Track& tr : m_info.tracks) {
      if (tr.flag == flag) {
        for (Value& val : tr.values) {
          list.push_back(&val);
        }
      }
    }
    return list;
  }

  void Load(const std::vector<Value*>& list) {
    for (Value* val : list) {
      if (val->custom) {
        // NAnimOp::dquant_bits 0xba2de0 (type 3: one bit per value) and
        // dquant_int32_and_real32_gen (types 4, 5: `width` bits) only fill custom variables.
        // The int32 read sign-extends when the init flag is set (dquant_int32_and_real32_gen).
        int64_t x = int64_t(m_bs.Read(val->customBits));
        if (val->isSigned) {
          x = SignExtend(x, val->customBits);
        }
        val->prev[0] = val->cur[0];
        val->cur[0] = x;
        continue;
      }
      for (int i = 0; i < 3; ++i) {
        val->prev[i] = val->cur[i];
        val->cur[i] = 0;
      }
      val->sgnPrev = val->sgnCur;
      if (!val->vec) {
        val->flag7 = int(m_bs.Read(1));
        val->sgnCur = int(m_bs.Read(1));
      }
      for (int c = 0; c < 3; ++c) {
        if (val->mask & (1 << c)) {
          int64_t x = m_bs.Read(val->W);
          if (val->isSigned) {
            x = SignExtend(x, val->W);
          }
          val->cur[c] = x;
        }
      }
    }
  }

  StreamInfo& m_info;
  BitStream m_bs;
  int64_t m_cur = 0;
};

// The rotation of one value at frame time t, as (w, x, y, z).
Quat TweenRot(const Value& val, int64_t t) {
  auto make = [&](const int64_t* ints, int negative) {
    const double scale = std::ldexp(1.0, -val.q);
    double c[3];
    double sum = 0.0;
    for (int i = 0; i < 3; ++i) {
      c[i] = F32(double(ints[i]) * scale);
      sum += c[i] * c[i];
    }
    const double w = std::sqrt(std::max(0.0, 1.0 - sum));
    return Quat{negative ? -w : w, c[0], c[1], c[2]};
  };
  const Quat prev = make(val.prev, val.sgnPrev);
  if (t == val.curFrame) {
    return prev;
  }
  const Quat cur = make(val.cur, val.sgnCur);
  const double frac = double(t - val.curFrame) * val.inv;
  if (val.flag7) {
    return RotLerp2(prev, cur, frac);
  }
  return Normalize(Lerp(prev, cur, frac));
}

// A vec3 value at frame time t: linear between the two keyframes.
std::array<double, 3> TweenVec3(const Value& val, int64_t t) {
  const double scale = std::ldexp(1.0, -val.q);
  std::array<double, 3> prev, cur;
  for (size_t i = 0; i < 3; ++i) {
    prev[i] = F32(double(val.prev[i]) * scale);
    cur[i] = F32(double(val.cur[i]) * scale);
  }
  if (t == val.curFrame) {
    return prev;
  }
  const double frac = double(t - val.curFrame) * val.inv;
  return {prev[0] + (cur[0] - prev[0]) * frac, prev[1] + (cur[1] - prev[1]) * frac,
          prev[2] + (cur[2] - prev[2]) * frac};
}

// A type 4 (real32) value at frame time t: linear between the two keyframes.
double TweenReal(const Value& val, int64_t t) {
  const double scale = std::ldexp(1.0, -val.realExp);
  const double prev = F32(double(val.prev[0]) * scale);
  if (t == val.curFrame) {
    return prev;
  }
  const double cur = F32(double(val.cur[0]) * scale);
  return prev + (cur - prev) * (double(t - val.curFrame) * val.inv);
}

// One entry of the op stream: which bone, and where its rotation, scale and
// translation come from (a track value, or an index into the constant pool).
struct Op {
  uint32_t bone;
  uint16_t rot, scale, trans;
  bool noScale = false;  // sub-op 3: the scale is left at its default
};

// The op stream is `01 <u16 skip>` and then sub-ops, each `<id byte><payload>`, until id 0
// (NAnimOp::tween_solve over sTweenFuncSet): 2 = bone coordinates with scale, 3 = bone
// coordinates without scale, 4 = bool words, 5 = move update, 6/7 = custom real32/int32.
// Only 2 and 3 produce bone keys; the others are stepped over by their payload size.
std::vector<Op> ParseOps(const StreamInfo& info) {
  const Bytes& b = info.blob;
  size_t p = info.w2;
  std::vector<Op> out;
  auto byteAt = [&](size_t at) -> uint8_t {
    b.Need(at, 1, "op stream");
    return b.data[at];
  };
  // The list may end at the end of the blob without a terminator.
  while (p < b.size) {
    const uint8_t op = b.data[p++];
    if (op != 1) {
      break;  // 0 ends the list; the static animation ends in padding instead
    }
    p += 2;
    while (p < b.size) {
      const uint8_t sub = b.data[p++];
      if (sub == 0) {
        return out;  // done_func ends the solve; the next group belongs to another stream
      }
      if (sub == 5) {
        continue;  // tween_update_move: no payload
      }
      if (sub == 4) {  // tween_bool_words_op: a track index, then that track's word count of u16
        const uint8_t track = byteAt(p + 1);
        if (track >= info.tracks.size()) {
          Fail("bool words op names a track that does not exist");
        }
        p += 2 + 2 * size_t(info.tracks[track].boolWords);
        continue;
      }
      if (sub == 6 || sub == 7) {  // u8 count, u16 header, count u16 references
        p += 3 + 2 * size_t(byteAt(p));
        continue;
      }
      if (sub != 2 && sub != 3) {
        Fail("op stream sub-op " + std::to_string(sub) + " is not supported");
      }
      const size_t entry = sub == 2 ? 6 : 4;
      const uint32_t count = byteAt(p);
      b.Need(p + 1, 2, "op stream");
      const uint32_t hdr = ReadLE16(b.data + p + 1);
      p += 3;
      const uint32_t bone = (hdr >> 5) & 0x7ff;
      for (uint32_t k = 0; k < count; ++k) {
        b.Need(p, entry, "op stream");
        Op o{bone + k, ReadLE16(b.data + p), 0, 0};
        if (sub == 2) {
          o.scale = ReadLE16(b.data + p + 2);
          o.trans = ReadLE16(b.data + p + 4);
        } else {
          o.noScale = true;
          o.trans = ReadLE16(b.data + p + 2);
        }
        out.push_back(o);
        p += entry;
      }
    }
  }
  return out;
}

// The pooled names: i32 size at 0x25, then NUL separated strings.
std::vector<std::string> ReadNames(const Bytes& d) {
  d.Need(0x25, 4, "name pool");
  const int32_t n = int32_t(ReadLE32(d.data + 0x25));
  if (n < 0 || !d.Has(0x29, size_t(n))) {
    Fail("name pool runs past the end of the file");
  }
  std::vector<std::string> names;
  std::string cur;
  for (int32_t i = 0; i < n; ++i) {
    const char c = char(d.data[0x29 + size_t(i)]);
    if (c == 0) {
      if (!cur.empty()) {
        names.push_back(cur);
      }
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) {
    names.push_back(cur);
  }
  return names;
}

struct Record {
  size_t offset;
  uint32_t id;
  size_t end;  // the size field's position + size (CAnimCompStream::Load 0xbb45f4)
};

// Compressed animation records: 01 <id u32> <len u32> payload (the CPooledName of the SBaseInfo,
// then the blob whose first u32 is its size from that field on); the next record's SBaseInfo
// header is 7 bytes past the end, so it starts 12 + len on.
std::vector<Record> ReadRecords(const Bytes& d, size_t start) {
  std::vector<Record> recs;
  size_t o = start;
  while (d.Has(o, 9) && d.data[o] == 1) {
    const uint32_t id = ReadLE32(d.data + o + 1);
    const uint32_t len = ReadLE32(d.data + o + 5);
    if (id > 0xff || len < 0x18 || !d.Has(o, size_t(12) + len)) {  // a blob header is 24 bytes
      break;
    }
    recs.push_back({o, id, o + 5 + len});
    o += 12 + len;
  }
  return recs;
}

// The longest chain of at least two records found by trying every start.
std::vector<Record> FindRecords(const Bytes& d) {
  std::vector<Record> best;
  for (size_t o = 0x100; d.Has(o, 9); ++o) {
    if (d.data[o] == 1 && d.data[o + 2] == 0 && d.data[o + 3] == 0 && d.data[o + 4] == 0) {
      std::vector<Record> r = ReadRecords(d, o);
      if (r.size() >= 2 && r.size() > best.size()) {
        best = std::move(r);
      }
    }
  }
  if (!best.empty()) {
    return best;
  }
  // A file with one animation has no chain: look for `01 id 00 00 00 <size> 18 00 <off> 00`,
  // the record marker followed by the blob header. The init-dequant offset <off> is 0x28 with
  // one context and larger (0x30 seen) with more, so it only has to lie inside the blob.
  for (size_t o = 0; d.Has(o, 13); ++o) {
    if (d.data[o] == 1 && d.data[o + 2] == 0 && d.data[o + 3] == 0 && d.data[o + 4] == 0 &&
        ReadLE16(d.data + o + 9) == 0x18) {
      const uint32_t len = ReadLE32(d.data + o + 5);
      const uint32_t dequant = ReadLE16(d.data + o + 11);
      if (len != 0 && dequant > 0x18 && dequant < len && d.Has(o, size_t(12) + len)) {
        best.push_back({o, d.data[o + 1], o + 5 + len});
        o += 12 + len - 1;  // the regex scan does not overlap matches
      }
    }
  }
  return best;
}

// The CharInfo's constant pools (SStreamConstData::Load): bool 1, i32 count, then per pool
// u16 a, u16 n and n floats starting 1 0 0 0. They end a few bytes before the first record;
// the list that ends closest to it wins. A stream reads pool `ctx` (InitConstData). When small
// placeholder records sit between the animations the longest chain starts after them, so
// without `near` the closest list anywhere before it is taken.
std::vector<std::vector<double>> FindPoolsIn(const Bytes& d, size_t first, bool near) {
  const size_t from = near && first >= 0x20000 ? first - 0x20000 : 0;
  std::vector<std::vector<double>> best;
  size_t bestEnd = 0;
  for (size_t s = from; s + 5 < first; ++s) {
    if (d.data[s] != 1) {
      continue;
    }
    const uint32_t count = ReadLE32(d.data + s + 1);
    if (count < 1 || count > 16) {
      continue;
    }
    size_t p = s + 5;
    bool ok = true;
    for (uint32_t i = 0; i < count && ok; ++i) {
      if (p + 20 > first) {
        ok = false;
        break;
      }
      const uint32_t a = ReadLE16(d.data + p);
      const uint32_t n = ReadLE16(d.data + p + 2);
      const size_t end = p + 4 + 4 * size_t(n);
      ok = a < 16 && n >= 4 && end <= first && ReadLEFloat(d.data + p + 4) == 1.0f &&
           ReadLEFloat(d.data + p + 8) == 0.0f && ReadLEFloat(d.data + p + 12) == 0.0f &&
           ReadLEFloat(d.data + p + 16) == 0.0f;
      p = end;
    }
    if (!ok || (near && first - p >= 128) || (!best.empty() && p <= bestEnd)) {
      continue;
    }
    std::vector<std::vector<double>> pools;
    size_t q = s + 5;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t n = ReadLE16(d.data + q + 2);
      std::vector<double> pool;
      for (uint32_t k = 0; k < n; ++k) {
        pool.push_back(double(ReadLEFloat(d.data + q + 4 + 4 * size_t(k))));
      }
      pools.push_back(std::move(pool));
      q += 4 + 4 * size_t(n);
    }
    best = std::move(pools);
    bestEnd = p;
  }
  return best;
}

std::vector<std::vector<double>> FindPools(const Bytes& d, size_t first) {
  for (const bool near : {true, false}) {
    std::vector<std::vector<double>> pools = FindPoolsIn(d, first, near);
    if (!pools.empty()) {
      return pools;
    }
  }
  return {{1.0, 0.0, 0.0, 0.0}};
}

void Evaluate(const StreamInfo& info, const std::vector<Op>& ops, const std::vector<double>& pool,
              int64_t t, std::vector<Key>& frameKeys, std::vector<bool>& has) {
  auto poolAt = [&](size_t index, size_t count) -> const double* {
    if (index > pool.size() || count > pool.size() - index) {
      Fail("constant pool index out of range");
    }
    return pool.data() + index;
  };
  for (const Op& op : ops) {
    Quat q;  // w x y z
    if (op.rot & 0x8000) {
      const size_t track = (op.rot >> 10) & 0xf;
      const size_t index = op.rot & 0x3ff;
      if (track >= info.tracks.size() || index >= info.tracks[track].values.size()) {
        Fail("rotation names a track value that does not exist");
      }
      q = TweenRot(info.tracks[track].values[index], t);
    } else {
      const double* p = poolAt(op.rot & 0x3fff, 4);
      q = {p[0], p[1], p[2], p[3]};
    }
    auto vecTrack = [&](uint16_t ref) -> std::array<double, 3> {
      const size_t track = (ref >> 10) & 0xf;
      const size_t index = ref & 0x3ff;
      if (track >= info.tracks.size() || index >= info.tracks[track].values.size()) {
        Fail("a vec3 reference names a track value that does not exist");
      }
      return TweenVec3(info.tracks[track].values[index], t);
    };
    double sc[3] = {0.0, 0.0, 0.0};
    if (op.noScale) {
      // left at the fragment's default, a delta of zero
    } else if (op.scale & 0x8000) {
      if (op.scale & 0x4000) {
        // tween_scale_for_coords 0xba3be0: one real32 value for all three axes.
        const size_t track = (op.scale >> 10) & 0xf;
        const size_t index = op.scale & 0x3ff;
        if (track >= info.tracks.size() || index >= info.tracks[track].values.size() ||
            info.tracks[track].type != 4) {
          Fail("a single-real scale names a value that is not a real32");
        }
        sc[0] = sc[1] = sc[2] = TweenReal(info.tracks[track].values[index], t);
      } else {
        const std::array<double, 3> v = vecTrack(op.scale);
        sc[0] = v[0];
        sc[1] = v[1];
        sc[2] = v[2];
      }
    } else if (op.scale & 0x4000) {
      sc[0] = sc[1] = sc[2] = *poolAt(op.scale & 0x3fff, 1);
    } else {
      const double* p = poolAt(op.scale & 0x3fff, 3);
      sc[0] = p[0];
      sc[1] = p[1];
      sc[2] = p[2];
    }
    std::array<double, 3> tr;
    if (op.trans & 0x8000) {
      tr = vecTrack(op.trans);
    } else {
      const double* p = poolAt(op.trans & 0x3fff, 3);
      tr = {p[0], p[1], p[2]};
    }
    Key key;
    key.rotation[0] = float(q[1]);
    key.rotation[1] = float(q[2]);
    key.rotation[2] = float(q[3]);
    key.rotation[3] = float(q[0]);
    for (int i = 0; i < 3; ++i) {
      key.scale[i] = float(F32(sc[i] + 1.0));  // stored as a delta from one
      key.translation[i] = float(tr[size_t(i)]);
    }
    frameKeys[op.bone] = key;
    has[op.bone] = true;
  }
}

Anim DecodeAnim(const Bytes& d, const Record& rec,
                const std::vector<std::vector<double>>& pools) {
  std::vector<StreamInfo> infos = ParseBlob(d, rec.offset + 5);
  InitDequant(infos);
  struct Active {
    StreamInfo* info;
    std::vector<Op> ops;
  };
  std::vector<Active> active;
  for (StreamInfo& info : infos) {
    std::vector<Op> ops = ParseOps(info);
    if (ops.empty()) {
      continue;  // custom variables and bool words only: nothing for the skeleton
    }
    active.push_back({&info, std::move(ops)});
  }
  if (infos.empty()) {
    Fail("animation has no stream info");
  }
  const StreamInfo& head = infos[0];

  Anim anim;
  anim.id = rec.id;
  anim.fps = kFramesPerRate * head.headerFloat;
  anim.frames = head.frames;
  uint32_t boneCount = 0;
  for (const Active& a : active) {
    for (const Op& op : a.ops) {
      boneCount = std::max(boneCount, op.bone + 1);
    }
  }
  // A damaged header can claim 65535 frames of 2000 bones; real files have well under this.
  if (uint64_t(boneCount) * head.frames > (uint64_t(1) << 23)) {
    Fail("animation is implausibly large");
  }
  anim.bones.assign(boneCount, std::vector<Key>());
  for (std::vector<Key>& keys : anim.bones) {
    keys.reserve(head.frames);
  }
  anim.tracked.assign(boneCount, false);
  for (const Active& a : active) {
    for (const Op& op : a.ops) {
      anim.tracked[op.bone] = true;
    }
  }
  std::vector<Stream> streams;
  streams.reserve(active.size());
  for (const Active& a : active) {
    streams.emplace_back(*a.info);
    streams.back().InitFrames();
  }
  std::vector<Key> frameKeys(boneCount);
  std::vector<bool> has(boneCount);
  for (uint32_t f = 0; f < head.frames; ++f) {
    std::fill(has.begin(), has.end(), false);
    for (size_t i = 0; i < active.size(); ++i) {
      const int64_t t = int64_t(f) * int64_t(active[i].info->v);
      while (streams[i].Cur() < t) {
        streams[i].DequantFrame();
      }
      const uint32_t ctx = active[i].info->ctx;
      if (ctx >= pools.size()) {
        Fail("stream names a constant pool that does not exist");
      }
      Evaluate(*active[i].info, active[i].ops, pools[ctx], t, frameKeys, has);
    }
    for (uint32_t b = 0; b < boneCount; ++b) {
      anim.bones[b].push_back(has[b] ? frameKeys[b] : Key());
    }
  }
  return anim;
}

using port::StartsWith;

bool ContainsNoCase(const std::string& s, const char* needle) {
  std::string lower = s;
  for (char& c : lower) {
    c = char(std::tolower(static_cast<unsigned char>(c)));
  }
  return lower.find(needle) != std::string::npos;
}

// 3x4 affine matrices, row-major with the translation in column 3 (Remastered's layout).
using Mat = std::array<double, 12>;

Mat MatMul(const Mat& a, const Mat& b) {
  Mat r;
  for (size_t i = 0; i < 3; ++i) {
    for (size_t j = 0; j < 4; ++j) {
      double v = 0.0;
      for (size_t k = 0; k < 3; ++k) {
        v += a[i * 4 + k] * b[k * 4 + j];
      }
      if (j == 3) {
        v += a[i * 4 + 3];
      }
      r[i * 4 + j] = v;
    }
  }
  return r;
}

// Inverse of an affine matrix; false when the linear part is singular.
bool MatInverse(const Mat& m, Mat& out) {
  const double a[3][3] = {{m[0], m[1], m[2]}, {m[4], m[5], m[6]}, {m[8], m[9], m[10]}};
  const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
                     a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                     a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
  if (std::fabs(det) < 1e-12) {
    return false;
  }
  double inv[3][3];
  for (size_t i = 0; i < 3; ++i) {
    for (size_t j = 0; j < 3; ++j) {
      const size_t r0 = i == 0 ? 1 : 0, r1 = i == 2 ? 1 : 2;
      const size_t q0 = j == 0 ? 1 : 0, q1 = j == 2 ? 1 : 2;
      const double cof = (a[r0][q0] * a[r1][q1] - a[r0][q1] * a[r1][q0]) * (((i + j) & 1) ? -1.0 : 1.0);
      inv[j][i] = cof / det;
    }
  }
  const double t[3] = {m[3], m[7], m[11]};
  for (size_t i = 0; i < 3; ++i) {
    out[i * 4 + 0] = inv[i][0];
    out[i * 4 + 1] = inv[i][1];
    out[i * 4 + 2] = inv[i][2];
    out[i * 4 + 3] = -(inv[i][0] * t[0] + inv[i][1] * t[1] + inv[i][2] * t[2]);
  }
  return true;
}

// A Key as a matrix: rotation times scale (columns scaled), then translation.
Mat KeyMatrix(const Key& k) {
  const double x = k.rotation[0], y = k.rotation[1], z = k.rotation[2], w = k.rotation[3];
  const double r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                          {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                          {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
  Mat m;
  for (size_t i = 0; i < 3; ++i) {
    for (size_t j = 0; j < 3; ++j) {
      m[i * 4 + j] = r[i][j] * double(k.scale[j]);
    }
    m[i * 4 + 3] = double(k.translation[i]);
  }
  return m;
}

// The rotation (x y z w), scale (column lengths) and translation of an affine matrix.
Key MatrixKey(const Mat& m) {
  Key key;
  double col[3];
  for (size_t j = 0; j < 3; ++j) {
    col[j] = std::sqrt(m[j] * m[j] + m[4 + j] * m[4 + j] + m[8 + j] * m[8 + j]);
    key.scale[j] = float(col[j]);
    if (col[j] == 0.0) {
      col[j] = 1.0;
    }
  }
  double r[3][3];
  for (size_t i = 0; i < 3; ++i) {
    for (size_t j = 0; j < 3; ++j) {
      r[i][j] = m[i * 4 + j] / col[j];
    }
  }
  double q[4];  // x y z w
  const double tr = r[0][0] + r[1][1] + r[2][2];
  if (tr > 0) {
    const double s = std::sqrt(tr + 1) * 2;
    q[0] = (r[2][1] - r[1][2]) / s;
    q[1] = (r[0][2] - r[2][0]) / s;
    q[2] = (r[1][0] - r[0][1]) / s;
    q[3] = s / 4;
  } else {
    size_t i = 0;
    for (size_t k = 1; k < 3; ++k) {
      if (r[k][k] > r[i][i]) {
        i = k;
      }
    }
    const size_t j = (i + 1) % 3, k = (i + 2) % 3;
    const double s = std::sqrt(std::max(0.0, 1 + r[i][i] - r[j][j] - r[k][k])) * 2;
    q[i] = s / 4;
    q[j] = (r[j][i] + r[i][j]) / s;
    q[k] = (r[k][i] + r[i][k]) / s;
    q[3] = (r[k][j] - r[j][k]) / s;
  }
  for (size_t i = 0; i < 4; ++i) {
    key.rotation[i] = float(q[i]);
  }
  key.translation[0] = float(m[3]);
  key.translation[1] = float(m[7]);
  key.translation[2] = float(m[11]);
  return key;
}

// The skeleton and skin palette (NOTES-skel.md): the CCharInfo stream read sequentially.
// All offsets are checked, so a short or damaged file fails with a message.
// The CCharInfo header counts after SChannel::Load (CCharInfo ctor 0xb819b0): u8 nModels,
// u8, u8 nAnchors, u16 nAnims, u16 nParams.
struct Header {
  bool valid = false;
  size_t models = 0;
  size_t anchors = 0;
  size_t anims = 0;
};

void ReadSkeleton(const Bytes& d, Character& out, Header& header) {
  size_t p = 0x29;
  auto u8 = [&](size_t at) -> uint32_t {
    d.Need(at, 1, "skeleton");
    return d.data[at];
  };
  auto u16 = [&](size_t at) -> uint32_t {
    d.Need(at, 2, "skeleton");
    return ReadLE16(d.data + at);
  };
  auto u32 = [&](size_t at) -> uint32_t {
    d.Need(at, 4, "skeleton");
    return ReadLE32(d.data + at);
  };

  // The string pool, keeping empty strings: refs index it.
  d.Need(0x25, 4, "name pool");
  const int32_t poolSize = int32_t(ReadLE32(d.data + 0x25));
  if (poolSize < 0 || !d.Has(0x29, size_t(poolSize))) {
    Fail("name pool runs past the end of the file");
  }
  std::vector<std::string> strs(1);
  for (int32_t i = 0; i < poolSize; ++i) {
    const char c = char(d.data[0x29 + size_t(i)]);
    if (c == 0) {
      strs.emplace_back();
    } else {
      strs.back().push_back(c);
    }
  }
  if (strs.back().empty()) {
    strs.pop_back();
  }
  p = 0x29 + size_t(poolSize);

  const size_t tables = u16(p), sets = u16(p + 2);
  p += 4 + 4 * tables + 4 * sets;
  p += 4 * tables;
  std::vector<std::vector<uint32_t>> nameRefs;  // per name set, the refs
  for (size_t i = 0; i < sets; ++i) {
    const size_t count = u32(p);
    if (count > d.size) {
      Fail("name set runs past the end of the file");
    }
    d.Need(p + 4, 8 * count, "name set");
    std::vector<uint32_t> refs(count);
    for (size_t k = 0; k < count; ++k) {
      refs[k] = ReadLE32(d.data + p + 4 + 4 * count + 4 * k);
    }
    nameRefs.push_back(std::move(refs));
    p += 4 + 8 * count;
  }
  p += 2 + u8(p) * 2;

  // SAnimContext
  {
    const size_t a = u8(p), b = u8(p + 1), c = u16(p + 2), dd = u16(p + 4), e = u16(p + 6);
    p += 8 + 6 * a + c + 2 * b + (dd > 8 * b ? dd - 8 * b : 0) + 2 * a + (e > 8 * a ? e - 8 * a : 0);
  }
  const size_t animCount = u16(p), animSet = u16(p + 2);
  p += 4 + 4 * animCount;

  // SAbsContext
  const size_t absA = u8(p), absB = u16(p + 1), absC = u16(p + 3), absD = u16(p + 5), absE = u16(p + 7);
  p += 9 + 6 * absA;
  d.Need(p, absC, "skeleton parents");
  const size_t cblob = p;
  p += absC + absD + absE;
  const size_t boneCount = u16(p), absSet = u16(p + 2);
  p += 4 + 4 * boneCount + 4 * absB + 1;

  // SRenderContext
  const size_t ra = u16(p), rb = u16(p + 2), rc = u16(p + 4), rd = u16(p + 6);
  p += 12;
  std::vector<uint32_t> arr0(ra);
  d.Need(p, 2 * (ra + rb + rc + rd), "render context");
  for (size_t k = 0; k < ra; ++k) {
    arr0[k] = ReadLE16(d.data + p + 2 * k);
  }
  p += 2 * (ra + rb + rc + rd);
  d.Need(p, 48 * (ra + 1 + rd), "inverse binds");
  {
    // SRenderContext::Load 0xb7e5ec reads 2(ra+rb+rc+rd) + 48(ra+rd+1) + e bytes in one go
    // (e = the op blob), then SChannel::Load 0xb7e9e0: u8 n, per channel a CPooledName
    // (bool, u32 if set), i16, bool, bool and a guid.
    size_t q = p + 48 * (ra + 1 + rd) + u32(p - 2 * (ra + rb + rc + rd) - 4);
    const size_t channels = u8(q++);
    for (size_t i = 0; i < channels; ++i) {
      q += u8(q) != 0 ? 5 : 1;
      q += 4 + 16;
    }
    header.models = u8(q);
    header.anchors = u8(q + 2);
    header.anims = u16(q + 3);
    header.valid = true;
  }
  auto matAt = [&](size_t index) {
    Mat m;
    for (size_t i = 0; i < 12; ++i) {
      m[i] = double(ReadLEFloat(d.data + p + 48 * index + 4 * i));
    }
    return m;
  };

  if (absSet >= nameRefs.size() || animSet >= nameRefs.size() || nameRefs[absSet].size() < boneCount) {
    Fail("skeleton names are missing");
  }
  auto name = [&](uint32_t ref) { return (ref & 0x0fffffff) < strs.size() ? strs[ref & 0x0fffffff] : std::string(); };

  std::vector<int> parent(boneCount, -1);
  std::vector<uint8_t> absOp(boneCount, 0);
  for (size_t o = 0; o + 6 < absC;) {
    const uint8_t* r = d.data + cblob + o;
    const size_t len = r[0] == 6 || r[0] == 7 ? 9 : 7;
    if (r[0] >= 4 && r[0] <= 9 && o + len <= absC) {
      const uint32_t self = ReadLE16(r + 1), par = ReadLE16(r + 3);
      if (self % 6 == 0 && par % 6 == 0 && self / 6 < boneCount && par / 6 < boneCount) {
        parent[self / 6] = int(par / 6);
        absOp[self / 6] = r[0];
        o += len;
        continue;
      }
    }
    ++o;
  }

  std::vector<bool> hasWorld(boneCount, false);
  std::vector<Mat> world(boneCount);
  for (size_t i = 0; i < boneCount && i < rd; ++i) {
    hasWorld[i] = MatInverse(matAt(ra + 1 + i), world[i]);
  }
  out.bones.clear();
  std::vector<std::string> boneNames;
  for (size_t i = 0; i < boneCount; ++i) {
    Bone bone;
    bone.name = name(nameRefs[absSet][i]);
    bone.parent = parent[i];
    bone.absOp = absOp[i];
    bone.invScale = absOp[i] == 6;
    if (hasWorld[i]) {
      Mat loc = world[i];
      Mat parentInv;
      if (parent[i] >= 0 && hasWorld[size_t(parent[i])] && MatInverse(world[size_t(parent[i])], parentInv)) {
        loc = MatMul(parentInv, world[i]);
      }
      bone.bind = MatrixKey(loc);
    }
    boneNames.push_back(bone.name);
    out.bones.push_back(std::move(bone));
  }
  // Duplicate names: the last bone with the name wins, as in the Python.
  out.animBone.clear();
  for (uint32_t ref : nameRefs[animSet]) {
    int found = -1;
    const std::string n = name(ref);
    for (size_t i = 0; i < boneCount; ++i) {
      if (boneNames[i] == n) {
        found = int(i);
      }
    }
    out.animBone.push_back(found);
  }
  out.jointBone.clear();
  out.inverseBind.clear();
  for (size_t k = 0; k < ra; ++k) {
    const uint32_t bone = arr0[k] / 192;
    out.jointBone.push_back(bone < boneCount ? int(bone) : -1);
    std::array<float, 12> inv;
    const Mat m = matAt(k);
    for (size_t i = 0; i < 12; ++i) {
      inv[i] = float(m[i]);
    }
    out.inverseBind.push_back(inv);
  }
}

}  // namespace

bool ReadCharacter(const std::vector<uint8_t>& chpr, Character& out, std::string& error, bool anims) {
  out = Character();
  try {
    const Bytes d{chpr.data(), chpr.size()};
    if (!d.Has(0, 0x29) || std::memcmp(d.data, "RFRM", 4) != 0 || std::memcmp(d.data + 0x14, "CHPR", 4) != 0) {
      Fail("not a CHPR file");
    }
    // Animation names, in record order: the pool names after the last "Primary" minus the
    // "meta" ones (a meta animation has no record), else the names with the usual
    // prefixes. Unless there is one per record, which is which is not known: the
    // animations go unnamed, so Find matches none of them.
    const std::vector<std::string> allNames = ReadNames(d);
    std::vector<std::string> animNames;
    for (const std::string& n : allNames) {
      if (StartsWith(n, "spin_") || StartsWith(n, "static") || StartsWith(n, "idle") || StartsWith(n, "anim")) {
        animNames.push_back(n);
      }
    }
    std::vector<std::string> afterPrimary;
    bool primary = false;
    for (const std::string& n : allNames) {
      if (n == "Primary") {
        primary = true;
        afterPrimary.clear();
      } else if (primary && !ContainsNoCase(n, "meta")) {
        afterPrimary.push_back(n);
      }
    }
    const std::vector<Record> recs = FindRecords(d);
    if (recs.empty()) {
      Fail("no animation records found");
    }
    if (primary && afterPrimary.size() == recs.size()) {
      animNames = afterPrimary;
    }
    const std::vector<std::vector<double>> pools = anims ? FindPools(d, recs[0].offset) : std::vector<std::vector<double>>();
    for (size_t i = 0; anims && i < recs.size(); ++i) {
      Anim anim = DecodeAnim(d, recs[i], pools);
      anim.name = animNames.size() == recs.size() ? animNames[i] : std::string();
      out.anims.push_back(std::move(anim));
    }
    // The skeleton is optional: the animations are usable without it.
    Header header;
    try {
      ReadSkeleton(d, out, header);
    } catch (const Failure& failure) {
      out.bones.clear();
      out.animBone.clear();
      out.jointBone.clear();
      out.inverseBind.clear();
      out.skeletonError = failure.message;
    }
    // CCharInfo ctor 0xb819b0: the last CAnim ends at its size field + size (the 7 bytes that
    // follow every other record are the next CAnim's SBaseInfo), then nAnchors 12-byte
    // SAnchorInfo (u16, u16, u32, u32), then nModels SModelNode (SModelNode ctor 0xb7fa38):
    // CPooledName (bool, u32 if set), i32, i32, the node guid and the SMDL guid. nAnchors and
    // nModels come from the header after SChannel (nAnims counts every CAnim, also those with
    // no compressed record, so the records are at most that many). A file that does not fit
    // has no model.
    if (header.valid && header.models > 0 && recs.size() <= header.anims) {
      size_t node = recs.back().end + 12 * header.anchors;
      if (d.Has(node, 1)) {
        node += d.data[node] != 0 ? 5 : 1;
        node += 8 + 16;
        if (d.Has(node, 16)) {
          std::copy(d.data + node, d.data + node + 16, out.skinnedModel.begin());
        }
      }
    }
  } catch (const Failure& failure) {
    error = failure.message;
    out = Character();
    return false;
  }
  return true;
}

const Anim* Find(const Character& c, std::string_view name) {
  for (const Anim& a : c.anims) {
    if (!a.name.empty() && a.name == name) {
      return &a;
    }
  }
  return nullptr;
}

bool SkinPose(const Character& c, const Anim& anim, uint32_t frame, std::vector<std::array<float, 12>>& out) {
  out.clear();
  const size_t boneCount = c.bones.size();
  if (boneCount == 0 || frame >= anim.frames || c.jointBone.size() != c.inverseBind.size()) {
    return false;
  }
  // Abs ops 7-9 (base pose / extract-inverse-scale) are decoded but not applied: refuse them.
  for (const Bone& b : c.bones) {
    if (b.absOp >= 7) {
      return false;
    }
  }
  // The anim track (op bone id) that drives each skeleton bone.
  std::vector<int> track(boneCount, -1);
  for (size_t j = c.animBone.size(); j-- > 0;) {
    const int b = c.animBone[j];
    if (b >= 0 && size_t(b) < boneCount && j < anim.bones.size() && j < anim.tracked.size() && anim.tracked[j] &&
        frame < anim.bones[j].size()) {
      track[size_t(b)] = int(j);
    }
  }
  std::vector<Mat> world(boneCount);
  std::vector<uint8_t> state(boneCount, 0);  // 0 not done, 1 in progress, 2 done
  // Parents can have a larger index than their children, so resolve by walking up.
  std::vector<size_t> chain;
  for (size_t start = 0; start < boneCount; ++start) {
    chain.clear();
    for (int b = int(start); b >= 0 && state[size_t(b)] == 0; b = c.bones[size_t(b)].parent) {
      state[size_t(b)] = 1;
      chain.push_back(size_t(b));
    }
    for (size_t i = chain.size(); i-- > 0;) {
      const size_t b = chain[i];
      // The keys are relative to the bind pose: a track's frame-0 key is the identity
      // where the animation starts from the bind pose (checked on the debris and the
      // Omega Pirate cinematic, whose joints then land on their bind places exactly). The
      // scale is not: a key's is the bone's own (the debris pieces bound at 0.5 and 0.75
      // keep that in their keys), so the bind's is left out under a key.
      Key unscaled = c.bones[b].bind;
      if (track[b] >= 0) {
        std::fill(std::begin(unscaled.scale), std::end(unscaled.scale), 1.0f);
      }
      const Mat bind = KeyMatrix(unscaled);
      Mat local = track[b] >= 0 ? MatMul(bind, KeyMatrix(anim.bones[size_t(track[b])][frame])) : bind;
      const int par = c.bones[b].parent;
      if (c.bones[b].invScale && par >= 0) {
        // abs_bone_rel_inv_scale (0xba1bf0): local = diag(1/Sp) * R * diag(S), Sp the
        // parent's scale; the translation is left as it is.
        const int pt = track[size_t(par)];
        const float* sp = pt >= 0 ? anim.bones[size_t(pt)][frame].scale : c.bones[size_t(par)].bind.scale;
        for (size_t r = 0; r < 3; ++r) {
          for (size_t col = 0; col < 3; ++col) {
            local[r * 4 + col] /= double(sp[r]);
          }
        }
      }
      if (par >= 0 && state[size_t(par)] == 2) {
        world[b] = MatMul(world[size_t(par)], local);
      } else if (par >= 0) {
        return false;  // a cycle
      } else {
        world[b] = local;
      }
      state[b] = 2;
    }
  }
  for (size_t k = 0; k < c.jointBone.size(); ++k) {
    const int b = c.jointBone[k];
    if (b < 0 || size_t(b) >= boneCount) {
      out.clear();
      return false;
    }
    Mat inv;
    for (size_t i = 0; i < 12; ++i) {
      inv[i] = double(c.inverseBind[k][i]);
    }
    const Mat m = MatMul(world[size_t(b)], inv);
    std::array<float, 12> row;
    for (size_t i = 0; i < 12; ++i) {
      row[i] = float(m[i]);
    }
    out.push_back(row);
  }
  return true;
}

}  // namespace PortRemasteredAnim
