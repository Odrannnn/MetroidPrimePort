// The Echoes suits conversion. See port_echoes_suits.h.

#include "port_echoes_suits.h"

#include "port_dread_samus_mod.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>

namespace PortEchoesSuits {
namespace {

constexpr uint32_t kCMDL = 0x434D444C;
constexpr uint32_t kCSKR = 0x43534B52;
constexpr uint32_t kCINF = 0x43494E46;
constexpr uint32_t kTXTR = 0x54585452;
constexpr uint32_t kANCS = 0x414E4353;

std::string Hex(uint32_t id) {
  char text[16];
  std::snprintf(text, sizeof(text), "%08X", id);
  return text;
}

bool Fail(std::string* error, const std::string& why) {
  if (error != nullptr) {
    *error = why;
  }
  return false;
}

// Bounds-checked big-endian reads over one buffer; a read past the end sets `bad` and yields 0.
struct Reader {
  const uint8_t* data;
  size_t size;
  bool bad = false;

  bool Has(size_t at, size_t n) {
    if (at > size || n > size - at) {
      bad = true;
      return false;
    }
    return true;
  }
  uint32_t U32(size_t at) {
    if (!Has(at, 4)) return 0;
    return (uint32_t(data[at]) << 24) | (uint32_t(data[at + 1]) << 16) | (uint32_t(data[at + 2]) << 8) |
           uint32_t(data[at + 3]);
  }
  uint16_t U16(size_t at) {
    if (!Has(at, 2)) return 0;
    return uint16_t((data[at] << 8) | data[at + 1]);
  }
  float F32(size_t at) {
    const uint32_t bits = U32(at);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
  }
};

void Put32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(uint8_t(value >> 24));
  out.push_back(uint8_t(value >> 16));
  out.push_back(uint8_t(value >> 8));
  out.push_back(uint8_t(value));
}

void Set32(std::vector<uint8_t>& out, size_t at, uint32_t value) {
  out[at] = uint8_t(value >> 24);
  out[at + 1] = uint8_t(value >> 16);
  out[at + 2] = uint8_t(value >> 8);
  out[at + 3] = uint8_t(value);
}

void Append(std::vector<uint8_t>& out, const uint8_t* data, size_t size) { out.insert(out.end(), data, data + size); }

void Pad32(std::vector<uint8_t>& out) { out.resize((out.size() + 31) & ~size_t(31), 0); }

// A run length past its instruction's bit field: each zero byte adds 255, the first non-zero ends it.
bool ReadExtendedLength(const uint8_t*& ip, const uint8_t* ipEnd, size_t& len) {
  while (ip < ipEnd && *ip == 0) {
    len += 255;
    ++ip;
  }
  if (ip >= ipEnd) {
    return false;
  }
  len += *ip++;
  return true;
}

// The UV animation modes' parameter counts (CMaterial UV animation types 0-8).
constexpr uint32_t kUvParams[] = {0, 0, 4, 2, 4, 4, 0, 2, 9};

// One Echoes material at `at` of the material set `r`, appended to `out` in Prime's layout.
bool ConvertMaterial(Reader& r, size_t& at, std::vector<uint8_t>& out, uint32_t& vertexFlags, std::string* error) {
  const uint32_t flags = r.U32(at);
  const uint32_t ntex = r.U32(at + 4);
  if (r.bad || !r.Has(at + 8, size_t(ntex) * 4)) {
    return Fail(error, "material cut short");
  }
  // Bit 14 is the port's PBR flag in Prime; Echoes sets it for its own reasons.
  Put32(out, flags & ~0x4000u);
  Append(out, r.data + at + 4, 4 + size_t(ntex) * 4);
  at += 8 + size_t(ntex) * 4;
  // Bits 24-31: matrix indices before each vertex, which ConvertSurface strips.
  vertexFlags = r.U32(at);
  Put32(out, vertexFlags & 0x00FFFFFF);
  // at + 4, at + 8: two words Prime does not have.
  Put32(out, r.U32(at + 12));  // group
  at += 16;
  const size_t start = at;
  if (flags & 0x8) {
    at += 4 + size_t(r.U32(at)) * 4;  // konst colours
  }
  at += 4;  // blend factors
  if (flags & 0x400) {
    at += 4;
  }
  at += 4 + size_t(r.U32(at)) * 4;  // colour channels
  const size_t stages = r.U32(at);
  at += 4 + stages * 20 + stages * 4;  // TEV stages, their inputs
  at += 4 + size_t(r.U32(at)) * 4;     // texgens
  const size_t animSize = r.U32(at);
  const size_t animStart = at + 4;
  const uint32_t anims = r.U32(animStart);
  size_t p = animStart + 4;
  for (uint32_t i = 0; i < anims && !r.bad; ++i) {
    const uint32_t mode = r.U32(p);
    if (mode >= std::size(kUvParams)) {
      return Fail(error, "unknown UV animation mode " + std::to_string(mode));
    }
    p += 4 + kUvParams[mode] * 4;
  }
  if (r.bad || p != animStart + animSize || !r.Has(start, p - start)) {
    return Fail(error, "material layout not understood");
  }
  Append(out, r.data + start, p - start);
  at = p;
  return true;
}

bool ConvertMaterialSet(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::vector<uint32_t>& flags,
                        std::vector<uint32_t>& textures, std::string* error) {
  Reader r{data, size};
  const uint32_t ntex = r.U32(0);
  if (r.bad || !r.Has(4, size_t(ntex) * 4)) {
    return Fail(error, "material set cut short");
  }
  for (uint32_t i = 0; i < ntex; ++i) {
    textures.push_back(r.U32(4 + i * 4));
  }
  size_t at = 4 + size_t(ntex) * 4;
  const uint32_t nmat = r.U32(at);
  if (r.bad || nmat > size / 4) {
    return Fail(error, "material set cut short");
  }
  at += 4 + size_t(nmat) * 4;
  std::vector<std::vector<uint8_t>> materials(nmat);
  flags.assign(nmat, 0);
  for (uint32_t i = 0; i < nmat; ++i) {
    if (!ConvertMaterial(r, at, materials[i], flags[i], error)) {
      return false;
    }
  }
  out.clear();
  Append(out, data, 4 + size_t(ntex) * 4);
  Put32(out, nmat);
  uint32_t end = 0;
  for (const auto& material : materials) {
    end += uint32_t(material.size());
    Put32(out, end);
  }
  for (const auto& material : materials) {
    Append(out, material.data(), material.size());
  }
  Pad32(out);
  return true;
}

bool ConvertSurface(const uint8_t* data, size_t size, const std::vector<uint32_t>& vertexFlags,
                    std::vector<uint8_t>& out, std::string* error) {
  Reader r{data, size};
  const uint32_t material = r.U32(0x0C);
  const uint32_t extra = r.U32(0x1C);
  if (r.bad || !r.Has(0x30, extra)) {
    return Fail(error, "surface header cut short");
  }
  if (material >= vertexFlags.size()) {
    return Fail(error, "surface material " + std::to_string(material) + " out of range");
  }
  const uint32_t vaf = vertexFlags[material];
  const size_t matrices = size_t(std::popcount(vaf >> 24));
  size_t vertexSize = matrices;
  for (int i = 0; i < 11; ++i) {
    switch ((vaf >> (2 * i)) & 3) {
    case 0:
      break;
    case 1:
      return Fail(error, "direct vertex attribute");
    case 2:
      vertexSize += 1;
      break;
    case 3:
      vertexSize += 2;
      break;
    }
  }
  std::vector<uint8_t> list;
  size_t at = (0x30 + size_t(extra) + 31) & ~size_t(31);
  while (at < size && data[at] != 0) {
    const uint16_t count = r.U16(at + 1);
    if (r.bad || !r.Has(at + 3, size_t(count) * vertexSize)) {
      return Fail(error, "display list cut short");
    }
    Append(list, data + at, 3);
    at += 3;
    for (uint16_t v = 0; v < count; ++v) {
      Append(list, data + at + matrices, vertexSize - matrices);
      at += vertexSize;
    }
  }
  list.push_back(0);
  Pad32(list);
  out.clear();
  Append(out, data, 0x2C);  // Echoes' word at 0x2C is not Prime's
  Append(out, data + 0x30, extra);
  Set32(out, 0x10, (r.U32(0x10) & 0xFFFF0000) | uint32_t(list.size()));
  Pad32(out);
  Append(out, list.data(), list.size());
  return true;
}

// A Prime material set taken apart; a material's vertex flags sit after its texture indices.
struct MaterialSet {
  std::vector<uint32_t> textures;
  std::vector<std::vector<uint8_t>> materials;
};

bool ParseSet(const std::vector<uint8_t>& in, MaterialSet& out, std::string* error) {
  Reader r{in.data(), in.size()};
  const uint32_t ntex = r.U32(0);
  if (r.bad || ntex > in.size() / 4) {
    return Fail(error, "material set cut short");
  }
  out = MaterialSet{};
  for (uint32_t i = 0; i < ntex; ++i) {
    out.textures.push_back(r.U32(4 + i * 4));
  }
  const size_t count = 4 + size_t(ntex) * 4;
  const uint32_t nmat = r.U32(count);
  if (r.bad || nmat > in.size() / 4) {
    return Fail(error, "material set cut short");
  }
  size_t at = count + 4 + size_t(nmat) * 4;
  uint32_t begin = 0;
  for (uint32_t i = 0; i < nmat; ++i) {
    const uint32_t end = r.U32(count + 4 + i * 4);
    if (r.bad || end < begin || !r.Has(at + begin, end - begin)) {
      return Fail(error, "material " + std::to_string(i) + " cut short");
    }
    out.materials.emplace_back(in.begin() + std::ptrdiff_t(at + begin), in.begin() + std::ptrdiff_t(at + end));
    Reader m{out.materials.back().data(), out.materials.back().size()};
    m.U32(8 + size_t(m.U32(4)) * 4);
    if (m.bad) {
      return Fail(error, "material " + std::to_string(i) + " has no vertex flags");
    }
    begin = end;
  }
  return true;
}

std::vector<uint8_t> WriteSet(const MaterialSet& set) {
  std::vector<uint8_t> out;
  Put32(out, uint32_t(set.textures.size()));
  for (const uint32_t id : set.textures) {
    Put32(out, id);
  }
  Put32(out, uint32_t(set.materials.size()));
  uint32_t end = 0;
  for (const auto& material : set.materials) {
    end += uint32_t(material.size());
    Put32(out, end);
  }
  for (const auto& material : set.materials) {
    Append(out, material.data(), material.size());
  }
  Pad32(out);
  return out;
}

size_t VertexFlagsAt(const std::vector<uint8_t>& material) {
  Reader m{material.data(), material.size()};
  return 8 + size_t(m.U32(4)) * 4;
}

uint32_t VertexFlags(const std::vector<uint8_t>& material) {
  Reader m{material.data(), material.size()};
  return m.U32(VertexFlagsAt(material));
}

// The vertex flags of `surface`'s material in `set`.
bool SurfaceFlags(const std::vector<uint8_t>& surface, const MaterialSet& set, uint32_t& vaf, std::string* error) {
  Reader r{surface.data(), surface.size()};
  const uint32_t material = r.U32(0x0C);
  if (r.bad || material >= set.materials.size()) {
    return Fail(error, "surface material out of range");
  }
  vaf = VertexFlags(set.materials[material]);
  return true;
}

// `surface`'s display list (vertex flags `vaf`) rebuilt: each primitive's format through `format`,
// each index through `index` (attribute 0-10, its value), written 16-bit when `wide`, else at its
// own width.
bool RewriteList(std::vector<uint8_t>& surface, uint32_t vaf, bool wide,
                 const std::function<bool(uint8_t&)>& format,
                 const std::function<bool(int, uint32_t&)>& index, std::string* error) {
  Reader r{surface.data(), surface.size()};
  const uint32_t sizeWord = r.U32(0x10);
  const uint32_t extra = r.U32(0x1C);
  const size_t start = (0x2C + size_t(extra) + 31) & ~size_t(31);
  if (r.bad || start > surface.size()) {
    return Fail(error, "surface header cut short");
  }
  size_t vertexSize = 0;
  for (int i = 0; i < 11; ++i) {
    const uint32_t mode = (vaf >> (2 * i)) & 3;
    if (mode == 1) {
      return Fail(error, "direct vertex attribute");
    }
    vertexSize += mode == 0 ? 0 : mode - 1;
  }
  std::vector<uint8_t> list;
  size_t at = start;
  while (at < surface.size() && surface[at] != 0) {
    uint8_t command = surface[at];
    const uint16_t count = r.U16(at + 1);
    if (r.bad || !r.Has(at + 3, size_t(count) * vertexSize)) {
      return Fail(error, "display list cut short");
    }
    if (!format(command)) {
      return Fail(error, "vertex format " + std::to_string(command & 7) + " not supported here");
    }
    list.push_back(command);
    Append(list, surface.data() + at + 1, 2);
    at += 3;
    for (uint16_t v = 0; v < count; ++v) {
      for (int i = 0; i < 11; ++i) {
        const uint32_t mode = (vaf >> (2 * i)) & 3;
        if (mode == 0) continue;
        uint32_t value = mode == 2 ? surface[at] : r.U16(at);
        at += mode - 1;
        if (!index(i, value)) {
          return Fail(error, "vertex index out of range");
        }
        if (wide || mode == 3) {
          if (value > 0xFFFF) return Fail(error, "vertex index past 16 bits");
          list.push_back(uint8_t(value >> 8));
        } else if (value > 0xFF) {
          return Fail(error, "vertex index past 8 bits");
        }
        list.push_back(uint8_t(value));
      }
    }
  }
  list.push_back(0);
  Pad32(list);
  surface.resize(start);
  Append(surface, list.data(), list.size());
  Set32(surface, 0x10, (sizeWord & 0xFFFF0000) | uint32_t(list.size()));
  return true;
}

// Each list of `model` checked to read texture 0 as floats (formats 0 and 1), then its short UVs
// dropped: CCubeModel binds them only for packed lightmaps, which those formats cannot read.
bool DropShortUvs(PrimeCmdl& model, const MaterialSet& set, std::string* error) {
  for (auto& surface : model.surfaces) {
    uint32_t vaf;
    if (!SurfaceFlags(surface, set, vaf, error) ||
        !RewriteList(
            surface, vaf, false, [](uint8_t& c) { return (c & 7) < 2; }, [](int, uint32_t&) { return true; },
            error)) {
      return false;
    }
  }
  Reader r{model.header.data(), model.header.size()};
  const uint32_t flags = r.U32(8);
  if (flags & 0x4) {
    if (model.arrays.size() != 5) return Fail(error, "short UVs flagged without their array");
    model.arrays.pop_back();
    Set32(model.header, 8, flags & ~0x4u);
  }
  return true;
}

}  // namespace

bool LzoInflate(const uint8_t* source, size_t sourceLen, uint8_t* dest, size_t& destLen) {
  // LZO1X, written for the Metroid Prime 2 port from the stream format (MIT, CLZOSupportPort.cpp
  // there); bounds-checked, unlike retail's lzo1x_decompress.
  const uint8_t* ip = source;
  const uint8_t* const ipEnd = source + sourceLen;
  uint8_t* op = dest;
  uint8_t* const opEnd = dest + destLen;
  // Literals the last instruction copied: 0-3 from a match's low bits, 4 after a literal run.
  // It decides what an instruction below 16 means.
  size_t state = 0;
  bool ok = false;

#define LZO_NEED_IN(n) \
  if (size_t(ipEnd - ip) < size_t(n)) goto done
#define LZO_NEED_OUT(n) \
  if (size_t(opEnd - op) < size_t(n)) goto done

  if (ip < ipEnd && *ip > 17) {
    const size_t len = *ip++ - 17;
    LZO_NEED_IN(len);
    LZO_NEED_OUT(len);
    std::memcpy(op, ip, len);
    ip += len;
    op += len;
    state = len < 4 ? len : 4;
  }

  while (ip < ipEnd) {
    const size_t inst = *ip++;
    size_t len;
    size_t dist;
    size_t next;
    if (inst >= 64) {
      // M2: 3-8 bytes, up to 2 KiB back.
      LZO_NEED_IN(1);
      len = (inst >> 5) + 1;
      dist = ((inst >> 2) & 7) + (size_t(*ip++) << 3) + 1;
      next = inst & 3;
    } else if (inst >= 32) {
      // M3: up to 16 KiB back.
      len = inst & 31;
      if (len == 0) {
        len = 31;
        if (!ReadExtendedLength(ip, ipEnd, len)) goto done;
      }
      len += 2;
      LZO_NEED_IN(2);
      const size_t d = ip[0] | (size_t(ip[1]) << 8);
      ip += 2;
      dist = (d >> 2) + 1;
      next = d & 3;
    } else if (inst >= 16) {
      // M4: 16-48 KiB back; distance 0 ends the stream.
      len = inst & 7;
      if (len == 0) {
        len = 7;
        if (!ReadExtendedLength(ip, ipEnd, len)) goto done;
      }
      len += 2;
      LZO_NEED_IN(2);
      const size_t d = ip[0] | (size_t(ip[1]) << 8);
      ip += 2;
      dist = ((inst & 8) << 11) + (d >> 2);
      if (dist == 0) {
        break;
      }
      dist += 0x4000;
      next = d & 3;
    } else if (state == 0) {
      // Literal run of 4 or more bytes.
      len = inst;
      if (len == 0) {
        len = 15;
        if (!ReadExtendedLength(ip, ipEnd, len)) goto done;
      }
      len += 3;
      LZO_NEED_IN(len);
      LZO_NEED_OUT(len);
      std::memcpy(op, ip, len);
      ip += len;
      op += len;
      state = 4;
      continue;
    } else {
      // M1: a 2-byte match right after short literals, or 3 bytes 2-3 KiB back after a long run.
      LZO_NEED_IN(1);
      len = state == 4 ? 3 : 2;
      dist = (inst >> 2) + (size_t(*ip++) << 2) + (state == 4 ? 2049 : 1);
      next = inst & 3;
    }

    if (dist > size_t(op - dest)) goto done;
    LZO_NEED_OUT(len);
    {
      // Byte by byte: a match may overlap the bytes it is producing.
      const uint8_t* mp = op - dist;
      for (size_t i = 0; i < len; ++i) {
        op[i] = mp[i];
      }
      op += len;
    }

    LZO_NEED_IN(next);
    LZO_NEED_OUT(next);
    for (size_t i = 0; i < next; ++i) {
      *op++ = *ip++;
    }
    state = next;
  }
  // The end marker, or the input used up exactly; a bad instruction jumps past this.
  ok = true;

#undef LZO_NEED_IN
#undef LZO_NEED_OUT

done:
  destLen = size_t(op - dest);
  return ok;
}

bool Decompress(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::string* error) {
  Reader r{data, size};
  const uint32_t total = r.U32(0);
  if (r.bad || total > (256u << 20)) {
    return Fail(error, "bad compressed size");
  }
  out.assign(total, 0);
  size_t at = 4;
  size_t written = 0;
  while (written < total) {
    const int16_t length = int16_t(r.U16(at));
    at += 2;
    if (r.bad) {
      return Fail(error, "compressed data cut short");
    }
    if (length == 0) {
      return Fail(error, "empty segment");
    }
    if (length < 0) {
      const size_t n = size_t(-int32_t(length));
      if (!r.Has(at, n) || n > total - written) {
        return Fail(error, "stored segment out of bounds");
      }
      std::memcpy(out.data() + written, data + at, n);
      at += n;
      written += n;
      continue;
    }
    if (!r.Has(at, size_t(length))) {
      return Fail(error, "compressed segment cut short");
    }
    const size_t expected = std::min<size_t>(0x4000, total - written);
    size_t got = expected;
    if (!LzoInflate(data + at, size_t(length), out.data() + written, got) || got != expected) {
      return Fail(error, "corrupt LZO segment");
    }
    at += size_t(length);
    written += got;
  }
  return true;
}

std::vector<uint8_t> WriteCmdl(const PrimeCmdl& model) {
  std::vector<std::vector<uint8_t>> sections = model.materialSets;
  sections.insert(sections.end(), model.arrays.begin(), model.arrays.end());
  std::vector<uint8_t> offsets;
  Put32(offsets, uint32_t(model.surfaces.size()));
  uint32_t end = 0;
  for (const auto& surface : model.surfaces) {
    end += uint32_t(surface.size());
    Put32(offsets, end);
  }
  Pad32(offsets);
  sections.push_back(std::move(offsets));
  sections.insert(sections.end(), model.surfaces.begin(), model.surfaces.end());

  std::vector<uint8_t> out = model.header;
  Set32(out, 0x24, uint32_t(sections.size()));
  Set32(out, 0x28, uint32_t(model.materialSets.size()));
  for (const auto& section : sections) {
    Put32(out, uint32_t(section.size()));
  }
  Pad32(out);
  for (const auto& section : sections) {
    Append(out, section.data(), section.size());
  }
  return out;
}

bool ConvertCmdl(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, std::vector<uint32_t>& textures,
                 std::string* error) {
  PrimeCmdl model;
  if (!ConvertCmdl(in, model, textures, error)) {
    return false;
  }
  out = WriteCmdl(model);
  return true;
}

bool ConvertCmdl(const std::vector<uint8_t>& in, PrimeCmdl& model, std::vector<uint32_t>& textures,
                 std::string* error) {
  Reader r{in.data(), in.size()};
  if (r.U32(0) != 0xDEADBABE || r.U32(4) != 4) {
    return Fail(error, "not an Echoes CMDL");
  }
  const uint32_t flags = r.U32(8);
  const uint32_t nsec = r.U32(0x24);
  const uint32_t nms = r.U32(0x28);
  if (r.bad || nsec > in.size() / 4) {
    return Fail(error, "CMDL header cut short");
  }
  std::vector<std::pair<size_t, size_t>> sections(nsec);
  size_t at = (0x2C + size_t(nsec) * 4 + 31) & ~size_t(31);
  for (uint32_t i = 0; i < nsec; ++i) {
    const size_t length = r.U32(0x2C + i * 4);
    if (!r.Has(at, length)) {
      return Fail(error, "CMDL section " + std::to_string(i) + " cut short");
    }
    sections[i] = {at, length};
    at += length;
  }
  const uint32_t arrays = (flags & 0x4) ? 5 : 4;
  const size_t surfaceHeader = size_t(nms) + arrays;
  if (surfaceHeader >= nsec) {
    return Fail(error, "CMDL has no surfaces");
  }
  const uint32_t nsurf = r.U32(sections[surfaceHeader].first);
  if (surfaceHeader + 1 + size_t(nsurf) != nsec) {
    return Fail(error, "CMDL section count does not match its surfaces");
  }

  model = PrimeCmdl{};
  std::vector<uint32_t> vertexFlags;
  for (uint32_t i = 0; i < nms; ++i) {
    std::vector<uint8_t> set;
    std::vector<uint32_t> setFlags;
    if (!ConvertMaterialSet(in.data() + sections[i].first, sections[i].second, set, setFlags, textures, error)) {
      return false;
    }
    if (i == 0) {
      vertexFlags = std::move(setFlags);
    }
    model.materialSets.push_back(std::move(set));
  }
  for (uint32_t i = 0; i < arrays; ++i) {
    const auto& [start, length] = sections[nms + i];
    model.arrays.emplace_back(in.begin() + std::ptrdiff_t(start), in.begin() + std::ptrdiff_t(start + length));
  }
  model.surfaces.resize(nsurf);
  for (uint32_t i = 0; i < nsurf; ++i) {
    const auto& [start, length] = sections[surfaceHeader + 1 + i];
    if (!ConvertSurface(in.data() + start, length, vertexFlags, model.surfaces[i], error)) {
      return false;
    }
  }
  model.header.assign(in.begin(), in.begin() + 0x2C);
  Set32(model.header, 4, 2);
  std::sort(textures.begin(), textures.end());
  textures.erase(std::unique(textures.begin(), textures.end()), textures.end());
  return true;
}

bool KeepMaterialSet(PrimeCmdl& model, size_t set, std::vector<uint32_t>& textures, std::string* error) {
  if (set >= model.materialSets.size()) {
    return Fail(error, "no material set " + std::to_string(set));
  }
  MaterialSet parsed;
  if (!ParseSet(model.materialSets[set], parsed, error)) {
    return false;
  }
  model.materialSets = {std::move(model.materialSets[set])};
  textures = std::move(parsed.textures);
  std::sort(textures.begin(), textures.end());
  textures.erase(std::unique(textures.begin(), textures.end()), textures.end());
  return true;
}

bool PatchGunAncs(std::vector<uint8_t>& ancs, std::string* error) {
  for (const Gun& gun : kGuns) {
    const uint32_t ids[3] = {gun.cmdl, gun.cskr, kGunCinf};
    uint8_t needle[12];
    for (int i = 0; i < 12; ++i) needle[i] = uint8_t(ids[i / 4] >> (24 - 8 * (i % 4)));
    size_t found = ancs.size();
    for (size_t at = 0; at + 12 <= ancs.size(); ++at) {
      if (std::memcmp(ancs.data() + at, needle, 12) != 0) continue;
      if (found != ancs.size()) {
        return Fail(error, std::string(gun.what) + " " + Hex(gun.cmdl) + " is in it twice");
      }
      found = at;
    }
    if (found == ancs.size()) {
      return Fail(error, std::string(gun.what) + " " + Hex(gun.cmdl) + " is not in it");
    }
    Set32(ancs, found, Cannon(gun.cmdl));
    Set32(ancs, found + 4, Cannon(gun.cskr));
  }
  return true;
}

bool FloatNormals(PrimeCmdl& model, std::string* error) {
  Reader h{model.header.data(), model.header.size()};
  const uint32_t flags = h.U32(8);
  if (h.bad || model.materialSets.empty() || model.arrays.size() < 4) {
    return Fail(error, "model has no arrays");
  }
  if (!(flags & 0x2)) {
    return true;
  }
  for (const auto& setBytes : model.materialSets) {
    MaterialSet set;
    if (!ParseSet(setBytes, set, error)) {
      return false;
    }
    // Every set draws the same lists; checking against each covers the vertex sizes all see.
    for (auto surface : model.surfaces) {
      uint32_t vaf;
      if (!SurfaceFlags(surface, set, vaf, error) ||
          !RewriteList(
              surface, vaf, false, [](uint8_t& c) { return (c & 7) == 1; }, [](int, uint32_t&) { return true; },
              error)) {
        return false;
      }
    }
  }
  MaterialSet set;
  ParseSet(model.materialSets[0], set, error);
  for (auto& surface : model.surfaces) {
    uint32_t vaf;
    SurfaceFlags(surface, set, vaf, error);
    RewriteList(
        surface, vaf, false,
        [](uint8_t& c) {
          c &= ~7;
          return true;
        },
        [](int, uint32_t&) { return true; }, error);
  }
  const std::vector<uint8_t>& shorts = model.arrays[1];
  Reader n{shorts.data(), shorts.size()};
  std::vector<uint8_t> floats;
  for (size_t at = 0; at + 2 <= shorts.size(); at += 2) {
    const float value = float(int16_t(n.U16(at))) / 16384.f;
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    Put32(floats, bits);
  }
  // Whole normals only: a short array padded to 32 bytes can end mid-normal.
  floats.resize(floats.size() / 12 * 12);
  Pad32(floats);
  model.arrays[1] = std::move(floats);
  Set32(model.header, 8, flags & ~0x2u);
  return true;
}

bool MergeCmdl(PrimeCmdl& a, const PrimeCmdl& b, std::string* error) {
  if (a.materialSets.size() != 1 || b.materialSets.size() != 1) {
    return Fail(error, "merge needs one material set each");
  }
  MaterialSet setA, setB;
  if (!ParseSet(a.materialSets[0], setA, error) || !ParseSet(b.materialSets[0], setB, error)) {
    return false;
  }
  PrimeCmdl other = b;
  if (!DropShortUvs(a, setA, error) || !DropShortUvs(other, setB, error)) {
    return false;
  }
  Reader ha{a.header.data(), a.header.size()};
  Reader hb{other.header.data(), other.header.size()};
  const uint32_t flags = ha.U32(8);
  if (ha.bad || hb.bad || flags != hb.U32(8) || a.arrays.size() != 4 || other.arrays.size() != 4) {
    return Fail(error, "merged models differ in their vertex layout");
  }
  // Positions, normals, colours (both channels), float UVs (textures 0-6).
  const size_t stride[4] = {12, size_t(flags & 0x2 ? 6 : 12), 4, 8};
  constexpr int kArrayOf[11] = {0, 1, 2, 2, 3, 3, 3, 3, 3, 3, 3};
  uint32_t base[4];
  for (int k = 0; k < 4; ++k) {
    base[k] = uint32_t(a.arrays[k].size() / stride[k]);
    a.arrays[k].resize(base[k] * stride[k]);
    Append(a.arrays[k], other.arrays[k].data(), other.arrays[k].size());
    Pad32(a.arrays[k]);
  }
  const uint32_t materials = uint32_t(setA.materials.size());
  for (auto& surface : other.surfaces) {
    uint32_t vaf;
    if (!SurfaceFlags(surface, setB, vaf, error) ||
        !RewriteList(
            surface, vaf, true, [](uint8_t&) { return true; },
            [&base, &kArrayOf](int attr, uint32_t& value) {
              value += base[kArrayOf[attr]];
              return true;
            },
            error)) {
      return false;
    }
    Reader r{surface.data(), surface.size()};
    Set32(surface, 0x0C, r.U32(0x0C) + materials);
    a.surfaces.push_back(std::move(surface));
  }
  const uint32_t textures = uint32_t(setA.textures.size());
  for (auto& material : setB.materials) {
    Reader m{material.data(), material.size()};
    const uint32_t ntex = m.U32(4);
    for (uint32_t i = 0; i < ntex; ++i) {
      Set32(material, 8 + i * 4, m.U32(8 + i * 4) + textures);
    }
    // Every index 16-bit now, as the rewritten lists are.
    const size_t at = VertexFlagsAt(material);
    uint32_t vaf = m.U32(at);
    for (int i = 0; i < 11; ++i) {
      if (((vaf >> (2 * i)) & 3) == 2) vaf |= 1u << (2 * i);
    }
    Set32(material, at, vaf);
    setA.materials.push_back(std::move(material));
  }
  setA.textures.insert(setA.textures.end(), setB.textures.begin(), setB.textures.end());
  a.materialSets[0] = WriteSet(setA);
  for (int i = 0; i < 3; ++i) {
    const float lo = std::min(ha.F32(0x0C + i * 4), hb.F32(0x0C + i * 4));
    const float hi = std::max(ha.F32(0x18 + i * 4), hb.F32(0x18 + i * 4));
    uint32_t bits;
    std::memcpy(&bits, &lo, 4);
    Set32(a.header, 0x0C + i * 4, bits);
    std::memcpy(&bits, &hi, 4);
    Set32(a.header, 0x18 + i * 4, bits);
  }
  return true;
}

bool RigidSkin(PrimeCmdl& model, uint32_t joint, std::vector<uint8_t>& cskr, std::string* error) {
  Reader h{model.header.data(), model.header.size()};
  const uint32_t flags = h.U32(8);
  if (h.bad || model.arrays.size() < 2 || (flags & 0x3)) {
    return Fail(error, "rigid skin needs an unskinned model with float normals");
  }
  // Eight 12-byte entries fill 96 bytes, so the count stays whole after padding to 32.
  const size_t count = (std::max(model.arrays[0].size(), model.arrays[1].size()) / 12 + 7) & ~size_t(7);
  model.arrays[0].resize(count * 12, 0);
  model.arrays[1].resize(count * 12, 0);
  Set32(model.header, 8, flags | 0x1);
  cskr.clear();
  Put32(cskr, 1);  // groups
  Put32(cskr, 1);  // weights in the group
  Put32(cskr, joint);
  Put32(cskr, 0x3F800000);  // 1.0
  Put32(cskr, uint32_t(count));
  Put32(cskr, 0xFFFFFFFF);
  Put32(cskr, uint32_t(count));
  Put32(cskr, 0xFFFFFFFF);
  Put32(cskr, uint32_t(count));
  return true;
}

bool MainJoint(const std::vector<uint8_t>& cskr, uint32_t& joint, std::string* error) {
  Reader r{cskr.data(), cskr.size()};
  const uint32_t groups = r.U32(0);
  if (r.bad || groups > cskr.size() / 8) {
    return Fail(error, "CSKR cut short");
  }
  std::vector<std::pair<uint32_t, double>> votes;
  size_t at = 4;
  for (uint32_t g = 0; g < groups; ++g) {
    const uint32_t n = r.U32(at);
    if (r.bad || !r.Has(at + 4, size_t(n) * 8 + 4)) {
      return Fail(error, "CSKR group cut short");
    }
    const uint32_t vertices = r.U32(at + 4 + size_t(n) * 8);
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t bone = r.U32(at + 4 + i * 8);
      const double weight = double(r.F32(at + 8 + i * 8)) * vertices;
      auto same = std::find_if(votes.begin(), votes.end(), [bone](const auto& v) { return v.first == bone; });
      if (same == votes.end()) {
        votes.emplace_back(bone, weight);
      } else {
        same->second += weight;
      }
    }
    at += 8 + size_t(n) * 8;
  }
  if (votes.empty()) {
    return Fail(error, "CSKR has no weights");
  }
  joint = std::max_element(votes.begin(), votes.end(),
                           [](const auto& x, const auto& y) { return x.second < y.second; })
              ->first;
  return true;
}

bool ParseCinf(const uint8_t* data, size_t size, std::vector<EchoesJoint>& out, std::string* error) {
  Reader r{data, size};
  out.clear();
  const uint32_t count = r.U32(0);
  if (r.bad || count > size / 4) {
    return Fail(error, "CINF cut short");
  }
  size_t at = 4;
  std::map<uint32_t, size_t> byId;
  for (uint32_t i = 0; i < count; ++i) {
    EchoesJoint joint;
    joint.id = r.U32(at);
    joint.parent = r.U32(at + 4);
    at += 8 + 12 + 16 + 16;  // position, rotation, local rotation
    const uint32_t links = r.U32(at);
    at += 4 + size_t(links) * 4;
    if (r.bad || links > size) {
      return Fail(error, "CINF joints cut short");
    }
    byId[joint.id] = out.size();
    out.push_back(std::move(joint));
  }
  const uint32_t order = r.U32(at);
  at += 4 + size_t(order) * 4;
  const uint32_t names = r.U32(at);
  at += 4;
  if (r.bad || order > size || names > size) {
    return Fail(error, "CINF cut short");
  }
  for (uint32_t i = 0; i < names; ++i) {
    const uint8_t* text = static_cast<const uint8_t*>(at < size ? std::memchr(data + at, 0, size - at) : nullptr);
    if (text == nullptr) {
      return Fail(error, "CINF name cut short");
    }
    std::string name(reinterpret_cast<const char*>(data + at), size_t(text - (data + at)));
    at += name.size() + 1;
    const uint32_t id = r.U32(at);
    at += 4;
    if (r.bad) {
      return Fail(error, "CINF names cut short");
    }
    const auto found = byId.find(id);
    if (found != byId.end()) {
      out[found->second].name = std::move(name);
    }
  }
  return true;
}

bool ConvertCskr(const std::vector<uint8_t>& in, const std::vector<EchoesJoint>& from,
                 const PortDread::PrimeSkeleton& to, std::vector<uint8_t>& out, std::vector<std::string>* folded,
                 std::string* error) {
  // Each named Echoes joint to its Prime joint: of its name, else of its nearest named ancestor's.
  // Echoes renamed the Varia shoulder pads' joints; their Prime ones are L/R_varias2_SDK.
  const auto primeJoint = [&to](const std::string& name) -> int {
    int found = to.Find(name);
    if (found < 0 && name == "L_varia_SDK") found = to.Find("L_varias2_SDK");
    if (found < 0 && name == "R_varia_SDK") found = to.Find("R_varias2_SDK");
    return found;
  };
  std::map<uint32_t, const EchoesJoint*> byId;
  for (const EchoesJoint& joint : from) {
    byId[joint.id] = &joint;
  }
  std::map<uint32_t, uint32_t> remap;
  std::set<std::string> foldedNames;
  for (const EchoesJoint& joint : from) {
    if (joint.name.empty()) {
      continue;
    }
    const EchoesJoint* walk = &joint;
    int found = primeJoint(walk->name);
    for (size_t depth = 0; found < 0 && depth < from.size(); ++depth) {
      const auto parent = byId.find(walk->parent);
      if (parent == byId.end() || parent->second->name.empty()) {
        walk = nullptr;
        break;
      }
      walk = parent->second;
      found = primeJoint(walk->name);
    }
    if (found < 0) {
      if (walk != nullptr) {
        return Fail(error, "joint " + joint.name + " loops in its parents");
      }
      foldedNames.insert(joint.name + "->?");
      continue;
    }
    remap[joint.id] = to.joints[size_t(found)].id;
    if (walk != &joint) {
      foldedNames.insert(joint.name + "->" + to.joints[size_t(found)].name);
    }
  }
  if (folded != nullptr) {
    folded->assign(foldedNames.begin(), foldedNames.end());
  }

  Reader r{in.data(), in.size()};
  const uint32_t groups = r.U32(0);
  if (r.bad || groups > in.size() / 8) {
    return Fail(error, "CSKR cut short");
  }
  out.clear();
  Put32(out, groups);
  size_t at = 4;
  uint32_t total = 0;
  for (uint32_t g = 0; g < groups; ++g) {
    const uint32_t n = r.U32(at);
    at += 4;
    if (r.bad || !r.Has(at, size_t(n) * 8 + 4)) {
      return Fail(error, "CSKR group cut short");
    }
    // Prime's joints in first-use order, their weights summed (in double, rounded once).
    std::vector<std::pair<uint32_t, double>> weights;
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t bone = r.U32(at);
      const float weight = r.F32(at + 4);
      at += 8;
      const auto mapped = remap.find(bone);
      if (mapped == remap.end()) {
        const auto joint = byId.find(bone);
        return Fail(error, "weight on joint " + (joint != byId.end() ? joint->second->name : Hex(bone)) +
                               ", which has no Prime joint");
      }
      auto same = std::find_if(weights.begin(), weights.end(),
                               [&mapped](const auto& w) { return w.first == mapped->second; });
      if (same == weights.end()) {
        weights.emplace_back(mapped->second, double(weight));
      } else {
        same->second += double(weight);
      }
    }
    const uint32_t vertices = r.U32(at);
    at += 4;
    Put32(out, uint32_t(weights.size()));
    for (const auto& [bone, weight] : weights) {
      Put32(out, bone);
      const float rounded = float(weight);
      uint32_t bits;
      std::memcpy(&bits, &rounded, 4);
      Put32(out, bits);
    }
    Put32(out, vertices);
    total += vertices;
  }
  // Echoes' footer (its own vertex tables) is dropped; Prime's names no extra vertices.
  Put32(out, 0xFFFFFFFF);
  Put32(out, total);
  Put32(out, 0xFFFFFFFF);
  Put32(out, total);
  return true;
}

bool Import(const Io& io, Result& result, std::string& error) {
  const auto log = [&io](const std::string& line) {
    if (io.log) io.log(line);
  };
  const auto cancelled = [&io] { return io.cancelled && io.cancelled(); };
  const auto write = [&io, &result, &error](const std::string& name, const std::vector<uint8_t>& data) {
    if (!io.write(name, data)) {
      error = "Could not write " + name + ".";
      return false;
    }
    result.files.push_back(name);
    return true;
  };

  // Prime's ball ANCS, the offset of its character 0's model and skin (after the name from byte
  // 14), for pointing them at new ones, and that skin.
  const auto primeBall = [&io, &error](const PortDreadSamus::BallTarget& target, std::vector<uint8_t>& ancs,
                                       size_t& at, uint32_t& skin, std::vector<uint8_t>& retailSkin) {
    if (!io.retail(kANCS, target.ancs, ancs)) {
      error = "Prime's ball " + Hex(target.ancs) + " is missing.";
      return false;
    }
    at = 14;
    while (at < ancs.size() && ancs[at] != 0) ++at;
    ++at;
    Reader a{ancs.data(), ancs.size()};
    skin = a.U32(at + 4);
    if (a.bad || a.U32(at) != target.cmdl || !io.retail(kCSKR, skin, retailSkin)) {
      error = "Prime's ball " + Hex(target.ancs) + " is not the one this importer knows.";
      return false;
    }
    return true;
  };

  std::map<uint32_t, std::vector<EchoesJoint>> echoesCinfs;
  std::map<uint32_t, PortDread::PrimeSkeleton> primeCinfs;
  std::set<uint32_t> textures;
  std::map<uint32_t, std::vector<uint8_t>> written;  // CMDL/CSKR by Prime id, for kCopies
  // + Dark Samus's ball, the gun
  const size_t total = std::size(kTargets) + std::size(kBalls) + 1 + std::size(kArms) + 1;
  for (size_t i = 0; i < std::size(kTargets); ++i) {
    const Target& t = kTargets[i];
    if (cancelled()) {
      error = "Cancelled.";
      return false;
    }
    if (io.progress) io.progress(i, total);
    const std::string what = std::string(t.suit) + " " + t.skeleton;
    std::string why;
    if (!echoesCinfs.count(t.echoesCinf)) {
      std::vector<uint8_t> data;
      if (!io.echoes(kCINF, t.echoesCinf, data)) {
        error = "The Echoes disc has no skeleton " + Hex(t.echoesCinf) + ": is it Metroid Prime 2: Echoes (USA)?";
        return false;
      }
      if (!ParseCinf(data.data(), data.size(), echoesCinfs[t.echoesCinf], &why)) {
        error = "Echoes skeleton " + Hex(t.echoesCinf) + ": " + why;
        return false;
      }
    }
    if (!primeCinfs.count(t.cinf)) {
      std::vector<uint8_t> data;
      if (!io.retail(kCINF, t.cinf, data) ||
          !PortDread::ParseCinf(data.data(), data.size(), primeCinfs[t.cinf], &why)) {
        error = "Prime's skeleton " + Hex(t.cinf) + " is missing or unreadable" + (why.empty() ? "" : ": " + why);
        return false;
      }
    }
    std::vector<uint8_t> cmdl, cskr;
    if (!io.echoes(kCMDL, t.echoesCmdl, cmdl) || !io.echoes(kCSKR, t.echoesCskr, cskr)) {
      error = "The Echoes disc has no " + what + " body (" + Hex(t.echoesCmdl) + "): is it Metroid Prime 2: "
              "Echoes (USA)?";
      return false;
    }
    std::vector<uint8_t> outCmdl, outCskr;
    std::vector<uint32_t> cmdlTextures;
    std::vector<std::string> folded;
    if (!ConvertCmdl(cmdl, outCmdl, cmdlTextures, &why)) {
      error = what + " model " + Hex(t.echoesCmdl) + ": " + why;
      return false;
    }
    if (!ConvertCskr(cskr, echoesCinfs[t.echoesCinf], primeCinfs[t.cinf], outCskr, &folded, &why)) {
      error = what + " skin " + Hex(t.echoesCskr) + ": " + why;
      return false;
    }
    textures.insert(cmdlTextures.begin(), cmdlTextures.end());
    std::string line = "echoes: " + what + " -> " + Hex(t.cmdl) + ", " + std::to_string(cmdlTextures.size()) +
                       " textures";
    if (!folded.empty()) {
      line += ", folded:";
      for (const std::string& f : folded) line += " " + f;
    }
    log(line);
    if (!write(Hex(t.cmdl) + ".CMDL", outCmdl) || !write(Hex(t.cskr) + ".CSKR", outCskr)) {
      return false;
    }
    written[t.cmdl] = std::move(outCmdl);
    written[t.cskr] = std::move(outCskr);
    ++result.converted;
  }
  for (const Copy& copy : kCopies) {
    const auto found = written.find(copy.from);
    if (found == written.end()) {
      error = "Internal error: copy of " + Hex(copy.from) + " has no source.";
      return false;
    }
    if (!write(Hex(copy.to) + (copy.type == kCSKR ? ".CSKR" : ".CMDL"), found->second)) {
      return false;
    }
  }
  for (size_t i = 0; i < std::size(kBalls); ++i) {
    const Ball& ball = kBalls[i];
    if (cancelled()) {
      error = "Cancelled.";
      return false;
    }
    if (io.progress) io.progress(std::size(kTargets) + i, total);
    const std::string what = std::string(ball.suit) + (ball.spider ? " Spider Ball" : " Morph Ball");
    const PortDreadSamus::BallTarget* target = nullptr;
    for (const auto& t : PortDreadSamus::kBallTargets) {
      if (t.suit == ball.primeSuit && t.spider == ball.spider) target = &t;
    }
    if (target == nullptr) {
      error = "Internal error: " + what + " has no Prime ball.";
      return false;
    }
    std::vector<uint8_t> ancs, retailSkin;
    size_t at;
    uint32_t skin;
    if (!primeBall(*target, ancs, at, skin, retailSkin)) {
      return false;
    }
    const uint32_t modelId = PortModelVariant::Id(target->cmdl, ball.primeSuit);
    const uint32_t skinId = PortModelVariant::Id(skin, ball.primeSuit);
    const uint32_t ancsId = PortModelVariant::Id(target->ancs, ball.primeSuit);
    const uint32_t lowId = PortModelVariant::Id(target->low, ball.primeSuit);
    for (const uint32_t id : {modelId, skinId, ancsId, lowId}) {
      if (id == 0 || io.retailId(id)) {
        error = what + ": output id " + Hex(id) + " is a Prime resource.";
        return false;
      }
    }
    std::string why;
    PrimeCmdl model;
    std::vector<uint32_t> ballTextures;
    std::vector<uint8_t> cmdl, cskr;
    if (!io.echoes(kCMDL, ball.echoesCmdl, cmdl)) {
      error = "The Echoes disc has no " + what + " (" + Hex(ball.echoesCmdl) + "): is it Metroid Prime 2: "
              "Echoes (USA)?";
      return false;
    }
    if (!ConvertCmdl(cmdl, model, ballTextures, &why)) {
      error = what + " model " + Hex(ball.echoesCmdl) + ": " + why;
      return false;
    }
    if (ball.caps != 0) {
      PrimeCmdl caps;
      if (!io.echoes(kCMDL, ball.caps, cmdl)) {
        error = "The Echoes disc has no " + what + " caps (" + Hex(ball.caps) + ").";
        return false;
      }
      if (!ConvertCmdl(cmdl, caps, ballTextures, &why) || !MergeCmdl(model, caps, &why)) {
        error = what + " caps " + Hex(ball.caps) + ": " + why;
        return false;
      }
    }
    const std::vector<uint8_t> low = WriteCmdl(model);
    uint32_t joint;
    if (!MainJoint(retailSkin, joint, &why) || !FloatNormals(model, &why) || !RigidSkin(model, joint, cskr, &why)) {
      error = what + ": " + why;
      return false;
    }
    Set32(ancs, at, modelId);
    Set32(ancs, at + 4, skinId);
    if (!write(Hex(modelId) + ".CMDL", WriteCmdl(model)) || !write(Hex(skinId) + ".CSKR", cskr) ||
        !write(Hex(lowId) + ".CMDL", low) || !write(Hex(ancsId) + ".ANCS", ancs)) {
      return false;
    }
    textures.insert(ballTextures.begin(), ballTextures.end());
    log("echoes: " + what + " -> " + Hex(modelId) + " (rigid joint " +
        std::to_string(joint) + "), " + std::to_string(ballTextures.size()) + " textures");
    ++result.balls;
  }
  // Dark Samus's ball: skinned in Echoes, but on one joint at full weight, so unskinned it is the
  // static model the other balls are, fit the same way to each Prime ball it stands in for.
  {
    if (cancelled()) {
      error = "Cancelled.";
      return false;
    }
    if (io.progress) io.progress(std::size(kTargets) + std::size(kBalls), total);
    std::string why;
    PrimeCmdl dark;
    std::vector<uint32_t> ballTextures;
    std::vector<uint8_t> cmdl;
    if (!io.echoes(kCMDL, kDarkBall, cmdl)) {
      error = "The Echoes disc has no Dark Samus Morph Ball (" + Hex(kDarkBall) + "): is it Metroid Prime 2: "
              "Echoes (USA)?";
      return false;
    }
    if (!ConvertCmdl(cmdl, dark, ballTextures, &why)) {
      error = "Dark Samus Morph Ball model " + Hex(kDarkBall) + ": " + why;
      return false;
    }
    Set32(dark.header, 8, Reader{dark.header.data(), dark.header.size()}.U32(8) & ~0x1u);
    if (!FloatNormals(dark, &why)) {
      error = "Dark Samus Morph Ball: " + why;
      return false;
    }
    const std::vector<uint8_t> low = WriteCmdl(dark);
    std::set<uint32_t> ancsDone, lowDone;  // balls share them across suits
    for (const auto& t : PortDreadSamus::kBallTargets) {
      if (!ancsDone.insert(t.ancs).second) continue;
      std::vector<uint8_t> ancs, retailSkin, cskr;
      size_t at;
      uint32_t skin, joint;
      if (!primeBall(t, ancs, at, skin, retailSkin)) {
        return false;
      }
      const uint32_t modelId = DarkBallId(t.cmdl);
      const uint32_t skinId = DarkBallSkinId(t.cmdl);
      const uint32_t ancsId = DarkBallId(t.ancs);
      const uint32_t lowId = DarkBallId(t.low);
      for (const uint32_t id : {modelId, skinId, ancsId, lowId}) {
        if (id == 0 || io.retailId(id)) {
          error = "Dark Samus Morph Ball: output id " + Hex(id) + " is a Prime resource.";
          return false;
        }
      }
      PrimeCmdl model = dark;
      if (!MainJoint(retailSkin, joint, &why) || !RigidSkin(model, joint, cskr, &why)) {
        error = "Dark Samus Morph Ball on " + Hex(t.ancs) + ": " + why;
        return false;
      }
      Set32(ancs, at, modelId);
      Set32(ancs, at + 4, skinId);
      if (!write(Hex(modelId) + ".CMDL", WriteCmdl(model)) || !write(Hex(skinId) + ".CSKR", cskr) ||
          !write(Hex(ancsId) + ".ANCS", ancs) ||
          (lowDone.insert(t.low).second && !write(Hex(lowId) + ".CMDL", low))) {
        return false;
      }
      log("echoes: Dark Samus Morph Ball on " + Hex(t.ancs) + " -> " + Hex(modelId) + " (rigid joint " +
          std::to_string(joint) + ")");
      ++result.balls;
    }
    textures.insert(ballTextures.begin(), ballTextures.end());
    log("echoes: Dark Samus Morph Ball: " + std::to_string(ballTextures.size()) + " textures");
  }
  std::vector<uint8_t> arm;
  for (size_t i = 0; i < std::size(kArms); ++i) {
    const Arm& a = kArms[i];
    if (cancelled()) {
      error = "Cancelled.";
      return false;
    }
    if (io.progress) io.progress(std::size(kTargets) + std::size(kBalls) + 1 + i, total);
    const std::string what = std::string(a.suit) + " arm";
    if (arm.empty() && !io.echoes(kCMDL, kEchoesArm, arm)) {
      error = "The Echoes disc has no arm (" + Hex(kEchoesArm) + "): is it Metroid Prime 2: Echoes (USA)?";
      return false;
    }
    std::string why;
    PrimeCmdl model;
    std::vector<uint32_t> armTextures;
    if (!ConvertCmdl(arm, model, armTextures, &why) || !KeepMaterialSet(model, a.set, armTextures, &why)) {
      error = what + " " + Hex(kEchoesArm) + ": " + why;
      return false;
    }
    if (!write(Hex(a.cmdl) + ".CMDL", WriteCmdl(model))) {
      return false;
    }
    textures.insert(armTextures.begin(), armTextures.end());
    log("echoes: " + what + " -> " + Hex(a.cmdl) + ", " + std::to_string(armTextures.size()) + " textures");
    ++result.arms;
  }
  {
    if (io.progress) io.progress(total - 1, total);
    std::vector<uint8_t> data;
    std::string why;
    std::vector<EchoesJoint> echoesCinf;
    PortDread::PrimeSkeleton primeCinf;
    if (!io.echoes(kCINF, kGunCinf, data) || !ParseCinf(data.data(), data.size(), echoesCinf, &why)) {
      error = "The Echoes gun skeleton " + Hex(kGunCinf) + " is missing or unreadable" +
              (why.empty() ? "" : ": " + why);
      return false;
    }
    if (!io.retail(kCINF, kGunCinf, data) || !PortDread::ParseCinf(data.data(), data.size(), primeCinf, &why)) {
      error = "Prime's gun skeleton " + Hex(kGunCinf) + " is missing or unreadable" + (why.empty() ? "" : ": " + why);
      return false;
    }
    for (const Gun& gun : kGuns) {
      for (const uint32_t id : {Cannon(gun.cmdl), Cannon(gun.cskr)}) {
        if (io.retailId(id)) {
          error = std::string(gun.what) + ": output id " + Hex(id) + " is a Prime resource.";
          return false;
        }
      }
      std::vector<uint8_t> cmdl, cskr, outCmdl, outCskr;
      std::vector<uint32_t> gunTextures;
      std::vector<std::string> folded;
      PrimeCmdl model;
      if (!io.echoes(kCMDL, gun.cmdl, cmdl) || !io.echoes(kCSKR, gun.cskr, cskr)) {
        error = "The Echoes disc has no " + std::string(gun.what) + " (" + Hex(gun.cmdl) +
                "): is it Metroid Prime 2: Echoes (USA)?";
        return false;
      }
      if (!ConvertCmdl(cmdl, model, gunTextures, &why)) {
        error = std::string(gun.what) + " model " + Hex(gun.cmdl) + ": " + why;
        return false;
      }
      // The gun has a set per beam; the hologram one for all.
      if (model.materialSets.size() != 1 && model.materialSets.size() != std::size(kGunSets)) {
        error = std::string(gun.what) + " model " + Hex(gun.cmdl) + " has " +
                std::to_string(model.materialSets.size()) + " material sets, not 1 or " +
                std::to_string(std::size(kGunSets)) + ".";
        return false;
      }
      if (model.materialSets.size() == std::size(kGunSets)) {
        std::vector<std::vector<uint8_t>> sets;
        for (const uint32_t set : kGunSets) sets.push_back(model.materialSets[set]);
        model.materialSets = std::move(sets);
      }
      outCmdl = WriteCmdl(model);
      if (!ConvertCskr(cskr, echoesCinf, primeCinf, outCskr, &folded, &why)) {
        error = std::string(gun.what) + " skin " + Hex(gun.cskr) + ": " + why;
        return false;
      }
      // Prime's gun animations move these joints by name: one without a namesake would freeze.
      if (!folded.empty()) {
        error = std::string(gun.what) + " skin " + Hex(gun.cskr) + ": joint " + folded.front() +
                " has no namesake in Prime's gun skeleton.";
        return false;
      }
      if (!write(Hex(Cannon(gun.cmdl)) + ".CMDL", outCmdl) || !write(Hex(Cannon(gun.cskr)) + ".CSKR", outCskr)) {
        return false;
      }
      textures.insert(gunTextures.begin(), gunTextures.end());
      log("echoes: " + std::string(gun.what) + " -> " + Hex(Cannon(gun.cmdl)) + ", " +
          std::to_string(gunTextures.size()) + " textures");
    }
    for (const uint32_t id : kBeamAncs) {
      const uint32_t out = Cannon(id);
      std::vector<uint8_t> ancs;
      if (io.retailId(out)) {
        error = "Gun " + Hex(id) + ": output id " + Hex(out) + " is a Prime resource.";
        return false;
      }
      if (!io.retail(kANCS, id, ancs)) {
        error = "Prime's gun " + Hex(id) + " is missing.";
        return false;
      }
      if (!PatchGunAncs(ancs, &why)) {
        error = "Prime's gun " + Hex(id) + ": " + why;
        return false;
      }
      if (!write(Hex(out) + ".ANCS", ancs)) {
        return false;
      }
      ++result.guns;
    }
  }
  for (const uint32_t id : textures) {
    if (cancelled()) {
      error = "Cancelled.";
      return false;
    }
    std::vector<uint8_t> data;
    if (!io.echoes(kTXTR, id, data)) {
      error = "The Echoes disc has no texture " + Hex(id) + ".";
      return false;
    }
    // A texture id Prime also uses would replace Prime's texture everywhere: only when they are
    // the same picture.
    if (io.retailId(id)) {
      std::vector<uint8_t> retail;
      if (!io.retail(kTXTR, id, retail) || retail != data) {
        error = "Echoes texture " + Hex(id) + " has the id of a different Prime resource.";
        return false;
      }
      continue;
    }
    if (!write(Hex(id) + ".TXTR", data)) {
      return false;
    }
  }
  if (io.progress) io.progress(total, total);
  log("echoes: " + std::to_string(result.converted) + " bodies and left arms, " + std::to_string(result.balls) +
      " balls, " + std::to_string(result.arms) + " right arms, " + std::to_string(result.guns) + " beam guns, " +
      std::to_string(textures.size()) + " textures");
  return true;
}

}  // namespace PortEchoesSuits
