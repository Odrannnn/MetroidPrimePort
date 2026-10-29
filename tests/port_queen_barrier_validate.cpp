// The barrier PART validation (platform/port_queen_barrier_validate.cpp): a converted
// refraction (VMAT feature bit 29, VFX_BarrierShield) or glow (bit 30, VFX_BarrierGlow)
// PART passes, and everything retail's or the port's own reader would refuse fails.
//
// The blobs are written in the exact format the readers take, so a check here means the
// same bytes a conversion writes:
// - the PART stream: platform/port_remastered_effect_convert.cpp SplitRetailEffect,
// - VMAT: src/Kyoto/Particles/CPortVfxMat.cpp PortReadVfxMat (v2, or v3 with the two
//   opacity gain/bias floats appended),
// - VMSH: src/Kyoto/Particles/CParticleDataFactory.cpp PortReadVfxMesh (v2: header, the
//   3x3 orient, the offset, 16 floats a vertex, then 2- or 4-byte indices),
// - VPRM: PortReadVfxParams; PVRT and VSMT: the port-only counted lists.
//
// Built on its own against the converter's reader (no production target, no game build),
// because a PART cannot be checked without the code that writes and reads it.

#include "port_queen_barrier_validate.h"
#include "port_remastered_effect_convert.h"
#include "port_test.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace PortQueenBarrier;

namespace {

using Bytes = std::vector<uint8_t>;
using Guid = std::array<uint8_t, 16>;
using Check = std::function<bool(uint32_t, uint32_t)>;

// The two real variables of the barrier's refraction material (MATI 6429c234): CCH0.x
// (the breakup) and CCH0.y (the alpha gain), in the order the GENP's PVAR table has them.
// The glow's material (ba0acbd6) has the breakup alone.
constexpr Guid kBreakup = {0x0d, 0xfb, 0x4a, 0xb8, 0x8c, 0xb1, 0xdb, 0x4a,
                           0x90, 0x32, 0xee, 0xa3, 0x6a, 0x9f, 0x58, 0xb6};
constexpr Guid kGain = {0xc6, 0x75, 0x16, 0xe5, 0x3e, 0x0f, 0xcd, 0x45,
                        0xa3, 0xed, 0x32, 0x2f, 0xb3, 0x21, 0x73, 0x8d};
constexpr Guid kOther = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                         0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x0f, 0xf1};
constexpr Guid kZero{};

constexpr uint32_t kRefract = 1u << 29;  // aurora::gfx::vfx::BarrierShield
constexpr uint32_t kGlow = 1u << 30;     // aurora::gfx::vfx::BarrierGlow

constexpr uint32_t Tag(const char* text) {
  return uint32_t(text[0]) << 24 | uint32_t(text[1]) << 16 | uint32_t(text[2]) << 8 | uint32_t(text[3]);
}

const float kNaN = std::numeric_limits<float>::quiet_NaN();

void PutWord(Bytes& out, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(uint8_t(value >> shift));
  }
}
void PutFloat(Bytes& out, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  PutWord(out, bits);
}
void PutFourCC(Bytes& out, const char* text) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(uint8_t(text[i]));
  }
}
// Patch a word (or a float's bits) of an already-built property value.
void SetWord(Bytes& value, size_t at, uint32_t word) {
  for (int i = 0; i < 4; ++i) {
    value[at + size_t(i)] = uint8_t(word >> (24 - 8 * i));
  }
}
void SetFloat(Bytes& value, size_t at, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  SetWord(value, at, bits);
}

// Offsets inside a property's value, its 'CNST' and length words included, so they are the
// reader's own: VMAT and VMSH keep their payload after 'CNST' (4) and the length (4).
constexpr size_t kPayload = 8;
constexpr size_t kVmatVersion = kPayload;        // payload 0
constexpr size_t kVmatFeatures = kPayload + 4;   // 1
constexpr size_t kVmatBlend = kPayload + 8;      // 2
constexpr size_t kVmatTextures = kPayload + 12;  // 3
constexpr size_t kVmatTex0 = kPayload + 16;      // the 44-byte texture records
constexpr size_t kVmatTexBytes = 44;
// The seven slots, in the reader's order: colour, opacity, ramp, ramp2, threshold, indirect, palette.
size_t VmatSlot(size_t textures, size_t slot) {
  return kVmatTex0 + kVmatTexBytes * textures + 4 * slot;
}
// One field of a texture record, in the reader's order: asset id, uv set, wrap S, wrap T,
// linear (filter), cols, rows, layers, warped, warp U, warp V.
size_t VmatTexField(size_t texture, size_t field) {
  return kVmatTex0 + kVmatTexBytes * texture + 4 * field;
}
// The material's own floats after the source triples: an Src's value (its row and component
// are ints), then modulate and depthSoften, then a v3's opacity gain and bias.
size_t VmatSrcValue(size_t textures, size_t source) {
  return VmatSlot(textures, 0) + 40 + 12 * source + 8;
}
size_t VmatModulate(size_t textures) { return VmatSlot(textures, 0) + 172; }
size_t VmatDepthSoften(size_t textures) { return VmatSlot(textures, 0) + 176; }
size_t VmatSpriteCenter(size_t textures) { return VmatSlot(textures, 0) + 180; }
size_t VmatOpacityGain(size_t textures) { return VmatSlot(textures, 0) + 184; }
size_t VmatOpacityBias(size_t textures) { return VmatSlot(textures, 0) + 188; }
constexpr size_t kVmshVersion = kPayload;
constexpr size_t kVmshVerts = kPayload + 4;
constexpr size_t kVmshTris = kPayload + 8;
constexpr size_t kVmshFlags = kPayload + 12;
constexpr size_t kVmshFloats = kPayload + 16;  // the 3x3 orient then the offset
constexpr size_t kVmshVertex0 = kPayload + 16 + 12 * 4;
constexpr size_t kVprmVersion = kPayload;
constexpr size_t kVprmRows = kPayload + 8;
constexpr size_t kVprmRow0 = kPayload + 12;
constexpr size_t kPvrtCount = 4;
constexpr size_t kPvrtRecord = 36;
constexpr size_t kVsmtCount = 4;    // the count word
constexpr size_t kVsmtRecords = 8;  // the track, 16 bytes an entry
constexpr size_t kVsmtRecord = 16;
// A PVRT record's four default floats, after its GUID (16) and type (4).
size_t PvrtDefault(size_t record, size_t index) {
  return 8 + kPvrtRecord * record + 20 + 4 * index;
}

// 'CNST' and the payload's length (the port's counted blobs), then the payload.
Bytes Counted(const Bytes& payload, int32_t declaredDelta = 0) {
  Bytes out;
  PutFourCC(out, "CNST");
  PutWord(out, uint32_t(int32_t(payload.size()) + declaredDelta));
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

// A conversion's VMAT: version, features, blend, the texture count, 11 words a texture,
// the seven slots, two ramp rows, an add row, the eleven original sources, then
// modulate/depthSoften/spriteCenter. 24 + 44 nt + 184 bytes.
Bytes VmatPayload(uint32_t version, uint32_t features, uint32_t blend, uint32_t textures) {
  Bytes p;
  PutWord(p, version);
  PutWord(p, features);
  PutWord(p, blend);
  PutWord(p, textures);
  for (uint32_t t = 0; t < textures; ++t) {
    PutWord(p, 0x2fda7108u + t);  // TXTR id (TCH0, then TCH1)
    PutWord(p, 0);                // uv set
    PutWord(p, 0);                // wrap S
    PutWord(p, 0);                // wrap T
    PutWord(p, 1);                // linear
    PutWord(p, 1);                // cols
    PutWord(p, 1);                // rows
    PutWord(p, 1);                // layers
    PutWord(p, 0);                // warped
    PutFloat(p, 0.f);             // warp U
    PutFloat(p, 0.f);             // warp V
  }
  const int32_t slots[7] = {0, -1, -1, -1, -1, textures > 1 ? 1 : -1, -1};
  for (int32_t slot : slots) {
    PutWord(p, uint32_t(slot));
  }
  PutWord(p, 0);  // rampRow[0]
  PutWord(p, 1);  // rampRow[1]
  PutWord(p, 2);  // addRow
  for (int i = 0; i < 11; ++i) {
    PutWord(p, 0xffffffffu);
    PutWord(p, 0);
    PutFloat(p, i == 8 ? 1.f : 0.f);  // kSrcIndexScale
  }
  PutFloat(p, 1.f);  // modulate
  PutFloat(p, 0.f);  // depthSoften
  PutWord(p, 0);     // spriteCenter
  if (version == 3) {
    PutFloat(p, 1.f);  // opacity gain
    PutFloat(p, 0.f);  // opacity bias
  }
  return p;
}

// VMSH v1/v2: version, nv, nt, [flags, orient, offset], then 16 floats a vertex (position,
// normal, uv, tangent, colour) and 3 indices a triangle, two or four bytes each.
Bytes VmshPayload(uint32_t version, uint32_t nv, uint32_t nt, uint32_t flags) {
  Bytes p;
  PutWord(p, version);
  PutWord(p, nv);
  PutWord(p, nt);
  if (version == 2) {
    PutWord(p, flags);
    const float orient[9] = {1.f, 0.f, 0.f, 0.f, 0.f, -1.f, 0.f, 1.f, 0.f};  // a R2G turn
    for (float f : orient) {
      PutFloat(p, f);
    }
    for (int i = 0; i < 3; ++i) {
      PutFloat(p, 0.f);  // offset
    }
  }
  for (uint32_t v = 0; v < nv; ++v) {
    PutFloat(p, float(v));  // position
    PutFloat(p, 0.f);
    PutFloat(p, 0.f);
    PutFloat(p, 0.f);  // normal
    PutFloat(p, 0.f);
    PutFloat(p, 1.f);
    PutFloat(p, 0.f);  // uv
    PutFloat(p, 0.f);
    PutFloat(p, 1.f);  // tangent
    PutFloat(p, 0.f);
    PutFloat(p, 0.f);
    PutFloat(p, 1.f);
    for (int c = 0; c < 4; ++c) {
      PutFloat(p, 1.f);  // the mesh's own colour
    }
  }
  const bool wide = nv > 65535;
  for (uint32_t i = 0; i < nt * 3; ++i) {
    const uint32_t index = i < nv ? i : nv - 1;
    if (wide) {
      PutWord(p, index);
    } else {
      p.push_back(uint8_t(index >> 8));
      p.push_back(uint8_t(index));
    }
  }
  return p;
}

// VPRM v1: version, the NMAP slot, n, n rows of four floats (the MATI's CCH values).
Bytes VprmPayload(uint32_t version, int32_t normalSlot, uint32_t rows) {
  Bytes p;
  PutWord(p, version);
  PutWord(p, uint32_t(normalSlot));
  PutWord(p, rows);
  for (uint32_t r = 0; r < rows; ++r) {
    PutFloat(p, 0.1f);  // CCH0.x: the breakup
    PutFloat(p, 1.f);   // CCH0.y: the alpha gain
    PutFloat(p, 0.f);
    PutFloat(p, 0.f);
  }
  return p;
}

// PVRT: 'CNST', the count, then 36 bytes a variable (GUID, type, four default floats).
Bytes PvrtValue(const std::vector<std::pair<Guid, uint32_t>>& vars) {
  Bytes v;
  PutFourCC(v, "CNST");
  PutWord(v, uint32_t(vars.size()));
  for (const auto& [guid, type] : vars) {
    v.insert(v.end(), guid.begin(), guid.end());
    PutWord(v, type);
    for (int i = 0; i < 4; ++i) {
      PutFloat(v, 0.f);
    }
  }
  return v;
}

// VSMT: 'CNST', the count, then {target, kind=Real, VARF, the PVRT index} a track.
Bytes VsmtValue(const std::vector<std::pair<uint32_t, uint32_t>>& track) {
  Bytes v;
  PutFourCC(v, "CNST");
  PutWord(v, uint32_t(track.size()));
  for (const auto& [target, index] : track) {
    PutWord(v, target);
    PutWord(v, 0);  // CPortVfxElem::kReal
    PutFourCC(v, "VARF");
    PutWord(v, index);
  }
  return v;
}

// Assemble the properties into a PART. `patch` sees each property's value before it is
// written, so a test can break one field of an otherwise good file.
using Patch = std::function<void(const char* fourcc, Bytes& value)>;
Bytes Part(const std::vector<std::pair<const char*, Bytes>>& properties, const Patch& patch = {},
           bool end = true, size_t truncateAt = SIZE_MAX) {
  Bytes out;
  PutFourCC(out, "GPSM");
  for (const auto& [fourcc, value] : properties) {
    Bytes patched = value;
    if (patch) {
      patch(fourcc, patched);
    }
    PutFourCC(out, fourcc);
    out.insert(out.end(), patched.begin(), patched.end());
  }
  if (end) {
    PutFourCC(out, "_END");
  }
  if (out.size() > truncateAt) {
    out.resize(truncateAt);
  }
  return out;
}

// The dependency: every TXTR the barrier needs is present unless it is in `missing`.
struct Assets {
  std::vector<uint32_t> missing;
  mutable int calls = 0;
  bool operator()(uint32_t type, uint32_t id) const {
    ++calls;
    return type == Tag("TXTR") && id != 0 && id != 0xffffffffu &&
           std::find(missing.begin(), missing.end(), id) == missing.end();
  }
};

// A refraction PART as the conversion writes one: VMAT v2 with bit 29, two textures
// (colour = TCH0, indirect = TCH1), a coloured VMSH v2, VPRM v1 with one CCH0 row,
// XFMD 3, PVRT with the material's two real variables and VSMT at targets 32 and 33.
std::vector<std::pair<const char*, Bytes>> RefractionPart() {
  return {
      {"VMAT", Counted(VmatPayload(2, kRefract, 0, 2))},
      {"VMSH", Counted(VmshPayload(2, 3, 1, 2))},
      {"VPRM", Counted(VprmPayload(1, -1, 1))},
      {"VSMT", VsmtValue({{32, 0}, {33, 1}})},
      {"XFMD", [] {
         Bytes v;
         PutFourCC(v, "CNST");
         PutWord(v, 3);
         return v;
       }()},
      {"PVRT", PvrtValue({{kBreakup, 0}, {kGain, 0}})},
  };
}

// The glow's: bit 30, one texture (the pattern through the colour slot), a coloured VMSH,
// VPRM v1's one row, XFMD 3, one real variable and one track at 32.
std::vector<std::pair<const char*, Bytes>> GlowPart() {
  return {
      {"VMAT", Counted(VmatPayload(2, kGlow, 0, 1))},
      {"VMSH", Counted(VmshPayload(2, 3, 1, 2))},
      {"VPRM", Counted(VprmPayload(1, -1, 1))},
      {"VSMT", VsmtValue({{32, 0}})},
      {"XFMD", [] {
         Bytes v;
         PutFourCC(v, "CNST");
         PutWord(v, 3);
         return v;
       }()},
      {"PVRT", PvrtValue({{kBreakup, 0}})},
  };
}

const std::vector<Guid> kRefractVars = {kBreakup, kGain};
const std::vector<Guid> kGlowVars = {kBreakup};

bool Run(const Bytes& part, uint32_t feature, const std::vector<Guid>& vars, std::string& error,
         const Check& dependency) {
  return ValidatePart(part, feature, vars, dependency, error);
}

// A check that the PART is refused with exactly `why`.
void Reject(const Bytes& part, uint32_t feature, const std::vector<Guid>& vars, const char* why,
            const std::string& what, Assets assets = {}) {
  std::string error;
  const bool ok = Run(part, feature, vars, error, assets);
  if (ok) {
    std::fprintf(stderr, "  accepted, expected \"%s\"\n", why);
  }
  CHECK_MSG(!ok, what + ": refused");
  CHECK_EQ(error, std::string(why));
}

// A check that the PART is refused for a reason of its own (the reader's, when a blob's
// length word disagrees with its payload and the stream resynchronises inside it).
void Refuse(const Bytes& part, uint32_t feature, const std::vector<Guid>& vars, const std::string& what) {
  std::string error;
  const bool ok = Run(part, feature, vars, error, Assets{});
  if (ok) {
    std::fprintf(stderr, "  accepted\n");
  }
  CHECK_MSG(!ok, what + ": refused");
  CHECK_MSG(!error.empty(), what + ": refused with a reason");
}

void Accept(const Bytes& part, uint32_t feature, const std::vector<Guid>& vars, const std::string& what) {
  std::string error;
  const bool ok = Run(part, feature, vars, error, Assets{});
  if (!ok) {
    std::fprintf(stderr, "  refused: \"%s\"\n", error.c_str());
  }
  CHECK_MSG(ok, what + ": accepted");
  CHECK_EQ(error, std::string());
}

// Every field of both blobs at the values the conversion writes, and the one the runtime
// readiness checks need (port_vfx_particles.h PortVfxBarrierReady/PortVfxBarrierGlowReady).
void TestPositives() {
  Accept(Part(RefractionPart()), kRefract, kRefractVars, "refraction PART");
  Accept(Part(GlowPart()), kGlow, kGlowVars, "glow PART");

  // VMAT v3 (the opacity gain/bias appended) is a layout the reader takes, and only a
  // material the converter writes it for carries it; validation must not refuse it.
  {
    auto part = RefractionPart();
    part[0].second = Counted(VmatPayload(3, kRefract, 0, 2));
    Accept(Part(part), kRefract, kRefractVars, "VMAT v3 refraction PART");
  }
  // Bit 0 (every vertex has a tangent) beside bit 1: what a model with tangents converts to,
  // and the only other flag the mesh layout defines.
  {
    auto part = RefractionPart();
    part[1].second = Counted(VmshPayload(2, 3, 1, 3));
    Accept(Part(part), kRefract, kRefractVars, "VMSH flags 3 (tangent and colour)");
  }
  // The wider index form of a mesh over 65535 vertices (four bytes an index).
  {
    auto part = RefractionPart();
    part[1].second = Counted(VmshPayload(2, 65536, 1, 2));
    Accept(Part(part), kRefract, kRefractVars, "65536-vertex VMSH");
  }
  // The dependency is asked for both textures, each as a TXTR id.
  {
    const Bytes part = Part(RefractionPart());
    std::string error;
    int calls = 0;
    std::vector<uint32_t> asked;
    const Check dependency = [&](uint32_t type, uint32_t id) {
      ++calls;
      asked.push_back(type);
      CHECK_EQ(type, Tag("TXTR"));
      return id != 0 && id != 0xffffffffu;
    };
    CHECK(ValidatePart(part, kRefract, kRefractVars, dependency, error));
    CHECK_EQ(calls, 2);
    CHECK_EQ(asked.size(), size_t(2));
  }
}

// Truncation: retail's reader stops at the first property it cannot finish, so nothing can
// be validated on a file whose own length disagrees with its blobs.
void TestTruncation() {
  Assets none;
  {
    // The '_END' of a complete file, so the reader is still inside a property.
    Bytes part = Part(RefractionPart(), {}, false);
    std::string error;
    CHECK(!Run(part, kRefract, kRefractVars, error, none));
    CHECK_EQ(error, std::string("no _END"));
  }
  {
    // Cut inside a VMSH: its CNST length no longer fits.
    Bytes part = Part(RefractionPart());
    const size_t meshAt = std::string(reinterpret_cast<const char*>(part.data()), part.size()).find("VMSH");
    REQUIRE(meshAt != std::string::npos);
    part.resize(meshAt + 4 + 12);
    std::string error;
    CHECK(!Run(part, kRefract, kRefractVars, error, none));
    CHECK_MSG(error.find("VMSH does not read") != std::string::npos, "truncated VMSH: " + error);
  }
  {
    // Cut inside a VMAT.
    Bytes part = Part(RefractionPart());
    part.resize(4 + 4 + 12);
    std::string error;
    CHECK(!Run(part, kRefract, kRefractVars, error, none));
    CHECK_MSG(error.find("VMAT does not read") != std::string::npos, "truncated VMAT: " + error);
  }
  {
    Bytes part = Part(RefractionPart());
    part.resize(6);  // the header and two bytes of a fourcc
    std::string error;
    CHECK(!Run(part, kRefract, kRefractVars, error, none));
    CHECK_EQ(error, std::string("no _END"));
  }
  {
    // A property the conversion never writes.
    auto bad = RefractionPart();
    bad.push_back({"ZZZZ", Bytes{0, 0, 0, 0}});
    std::string error;
    CHECK(!Run(Part(bad), kRefract, kRefractVars, error, none));
    CHECK_MSG(error.find("ZZZZ retail does not read") != std::string::npos, "unknown property: " + error);
  }
  {
    Bytes empty;
    std::string error;
    CHECK(!Run(empty, kRefract, kRefractVars, error, none));
    CHECK_EQ(error, std::string("not a GPSM"));
  }
}

// A counted blob whose length word disagrees with its payload. retail's reader takes the
// length word as the blob's extent, so a wrong one moves the stream into the payload: the
// reader reports the garbage fourcc it finds there, and the validator never sees the blob.
void TestFraming() {
  {
    auto part = RefractionPart();
    part[0].second = Counted(VmatPayload(2, kRefract, 0, 2), 4);
    Refuse(Part(part), kRefract, kRefractVars, "VMAT length too long");
  }
  {
    auto part = RefractionPart();
    part[0].second = Counted(VmatPayload(2, kRefract, 0, 2), -8);
    Refuse(Part(part), kRefract, kRefractVars, "VMAT length too short");
  }
  {
    auto part = RefractionPart();
    part[1].second = Counted(VmshPayload(2, 3, 1, 2), 8);
    Refuse(Part(part), kRefract, kRefractVars, "VMSH length too long");
  }
  {
    // The last property, so a short length leaves the reader inside its own payload.
    auto part = RefractionPart();
    part[2].second = Counted(VprmPayload(1, -1, 1), -4);
    Refuse(Part(part), kRefract, kRefractVars, "VPRM length too short");
  }
}

// A PART missing one of the properties the runtime readiness needs.
void TestMissingProperties() {
  const std::pair<const char*, const char*> cases[] = {
      {"VMAT", "missing VMAT"},
      {"VMSH", "missing coloured VMSH v2"},
      {"VPRM", "missing CCH0 row"},
      {"XFMD", "barrier does not follow emitter"},
      {"PVRT", "invalid barrier variable table"},
      {"VSMT", "invalid barrier material track"},
  };
  for (const auto& [fourcc, why] : cases) {
    auto part = RefractionPart();
    part.erase(std::remove_if(part.begin(), part.end(),
                              [&](const auto& p) { return std::strcmp(p.first, fourcc) == 0; }),
               part.end());
    Reject(Part(part), kRefract, kRefractVars, why, std::string("no ") + fourcc);
  }
}

// Retail's reader takes a property once; the validator reads the stream into a map, so a
// second one of the same fourcc has no single value to check.
void TestDuplicateProperties() {
  {
    auto part = RefractionPart();
    part.push_back({"VMAT", Counted(VmatPayload(2, kRefract, 0, 2))});
    Reject(Part(part), kRefract, kRefractVars, "duplicate PART property", "two VMATs");
  }
  {
    auto part = RefractionPart();
    Bytes xfmd;
    PutFourCC(xfmd, "CNST");
    PutWord(xfmd, 3);
    part.push_back({"XFMD", xfmd});
    Reject(Part(part), kRefract, kRefractVars, "duplicate PART property", "two XFMDs");
  }
}

// VMAT: the feature word, the layout, the textures the import must have, and the slots the
// shader reads (colour TCH0, indirect TCH1 for the refraction only).
void TestVmat() {
  const Patch vmat = [](const char* fourcc, Bytes& value) {
    if (std::strcmp(fourcc, "VMAT") == 0) {
      SetWord(value, kVmatFeatures, kGlow);  // the glow's bit in a refraction part
    }
  };
  Reject(Part(RefractionPart(), vmat), kRefract, kRefractVars, "invalid VMAT layout or feature",
         "refraction VMAT carrying bit 30");
  // The converter's ModelBackCull bit (17) is draw state only: both recipes carry it.
  for (const uint32_t feature : {kRefract, kGlow}) {
    const Patch backCull = [feature](const char* fourcc, Bytes& value) {
      if (std::strcmp(fourcc, "VMAT") == 0) SetWord(value, kVmatFeatures, feature | (1u << 17));
    };
    if (feature == kRefract) {
      Accept(Part(RefractionPart(), backCull), kRefract, kRefractVars, "refraction VMAT with back-cull");
    } else {
      Accept(Part(GlowPart(), backCull), kGlow, kGlowVars, "glow VMAT with back-cull");
    }
  }
  {
    auto part = RefractionPart();
    part[0].second = Counted(VmatPayload(1, kRefract, 0, 2));
    Reject(Part(part), kRefract, kRefractVars, "invalid VMAT layout or feature", "VMAT v1");
  }
  {
    auto part = RefractionPart();
    part[0].second = Counted(VmatPayload(2, kRefract, 0, 5));
    Reject(Part(part), kRefract, kRefractVars, "invalid VMAT layout or feature", "five textures");
  }
  {
    auto part = RefractionPart();
    part[0].second = Counted(VmatPayload(2, kRefract, 0, 0));
    Reject(Part(part), kRefract, kRefractVars, "invalid VMAT layout or feature", "no texture");
  }
  {
    // A texture count no layout can hold (the framing arithmetic must not wrap into a pass).
    auto part = RefractionPart();
    part[0].second = Counted(VmatPayload(2, kRefract, 0, 2));
    SetWord(part[0].second, kVmatTextures, 0xffffffffu);
    Reject(Part(part), kRefract, kRefractVars, "invalid VMAT layout or feature", "texture count 2^32-1");
  }
  {
    // The TCH1 pattern map is not in the import.
    Assets assets;
    assets.missing = {0x2fda7109u};
    std::string error;
    CHECK(!Run(Part(RefractionPart()), kRefract, kRefractVars, error, assets));
    CHECK_EQ(error, std::string("missing barrier texture"));
  }
  {
    // A TXTR the PART names but the dependency reports as another type.
    std::string error;
    const Check wrongType = [](uint32_t type, uint32_t) { return type == Tag("CMDL"); };
    CHECK(!Run(Part(RefractionPart()), kRefract, kRefractVars, error, wrongType));
    CHECK_EQ(error, std::string("missing barrier texture"));
  }
  {
    // Warp scale is read as a float and drives the pattern's scroll.
    const Patch warp = [](const char* fourcc, Bytes& value) {
      if (std::strcmp(fourcc, "VMAT") == 0) {
        SetFloat(value, kVmatTex0 + kVmatTexBytes + 36, kNaN);  // texture 1's warp U
      }
    };
    Reject(Part(RefractionPart(), warp), kRefract, kRefractVars, "nonfinite texture transform",
           "NaN warp scale");
  }
  {
    const Patch colour = [](const char* fourcc, Bytes& value) {
      if (std::strcmp(fourcc, "VMAT") == 0) {
        SetWord(value, VmatSlot(2, 0), 2);  // only slots 0 and 1 exist
      }
    };
    Reject(Part(RefractionPart(), colour), kRefract, kRefractVars, "missing barrier texture slot",
           "colour slot out of range");
  }
  {
    const Patch indirect = [](const char* fourcc, Bytes& value) {
      if (std::strcmp(fourcc, "VMAT") == 0) {
        SetWord(value, VmatSlot(2, 5), 2);
      }
    };
    Reject(Part(RefractionPart(), indirect), kRefract, kRefractVars, "missing barrier texture slot",
           "refraction indirect slot out of range");
  }
}

// VMSH: v2 with the mesh's own colour (flags bit 1), the exact framing, finite floats and
// in-range indices.
void TestVmsh() {
  {
    const Patch flags = [](const char* fourcc, Bytes& value) {
      if (std::strcmp(fourcc, "VMSH") == 0) {
        SetWord(value, kVmshFlags, 1);  // a tangent but no colour
      }
    };
    Reject(Part(RefractionPart(), flags), kRefract, kRefractVars, "missing coloured VMSH v2",
           "VMSH without the colour flag");
  }
  {
    auto part = RefractionPart();
    part[1].second = Counted(VmshPayload(1, 3, 1, 0));
    Reject(Part(part), kRefract, kRefractVars, "missing coloured VMSH v2", "VMSH v1");
  }
  {
    // nv says one more vertex than the payload holds.
    const Patch verts = [](const char* fourcc, Bytes& value) {
      if (std::strcmp(fourcc, "VMSH") == 0) {
        SetWord(value, kVmshVerts, 4);
      }
    };
    Reject(Part(RefractionPart(), verts), kRefract, kRefractVars, "invalid VMSH framing",
           "VMSH vertex count past the payload");
  }
  {
    // One more triangle than the index block holds.
    const Patch tris = [](const char* fourcc, Bytes& value) {
      if (std::strcmp(fourcc, "VMSH") == 0) {
        SetWord(value, kVmshTris, 2);
      }
    };
    Reject(Part(RefractionPart(), tris), kRefract, kRefractVars, "invalid VMSH framing",
           "VMSH triangle count past the payload");
  }
  {
    // A vertex count no payload can hold: the framing check must not overflow into a pass
    // (or into a loop over the mesh).
    const Patch huge = [](const char* fourcc, Bytes& value) {
      if (std::strcmp(fourcc, "VMSH") == 0) {
        SetWord(value, kVmshVerts, 0xffffffffu);
      }
    };
    Reject(Part(RefractionPart(), huge), kRefract, kRefractVars, "invalid VMSH framing",
           "vertex count 2^32-1");
  }
  {
    // A vertex float, here the second vertex's position.
    const Patch nan = [](const char* fourcc, Bytes& value) {
      if (std::strcmp(fourcc, "VMSH") == 0) {
        SetFloat(value, kVmshVertex0 + 64, kNaN);
      }
    };
    Reject(Part(RefractionPart(), nan), kRefract, kRefractVars, "nonfinite VMSH", "NaN vertex");
  }
  {
    // The orient's first float is read into the draw's model matrix.
    const Patch nanOrient = [](const char* fourcc, Bytes& value) {
      if (std::strcmp(fourcc, "VMSH") == 0) {
        SetFloat(value, kVmshFloats, kNaN);
      }
    };
    Reject(Part(RefractionPart(), nanOrient), kRefract, kRefractVars, "nonfinite VMSH", "NaN orient");
  }
  {
    // The last index (the third) addressed one past the last vertex.
    const Patch index = [](const char* fourcc, Bytes& value) {
      if (std::strcmp(fourcc, "VMSH") == 0) {
        const size_t at = value.size() - 2;
        value[at] = 0;
        value[at + 1] = 3;
      }
    };
    Reject(Part(RefractionPart(), index), kRefract, kRefractVars, "VMSH index out of bounds",
           "index past the vertex count");
  }
}

// VPRM: v1 with exactly one CCH0 row, and its floats finite (the breakup and gain the
// shader reads).
void TestVprm() {
  {
    auto part = RefractionPart();
    part[2].second = Counted(VprmPayload(1, -1, 0));
    Reject(Part(part), kRefract, kRefractVars, "missing CCH0 row", "VPRM with no row");
  }
  {
    auto part = RefractionPart();
    part[2].second = Counted(VprmPayload(2, -1, 1));  // v2 has the env slot after NMAP
    Reject(Part(part), kRefract, kRefractVars, "missing CCH0 row", "VPRM v2");
  }
  {
    auto part = RefractionPart();
    part[2].second = Counted(VprmPayload(1, -1, 2));
    Reject(Part(part), kRefract, kRefractVars, "missing CCH0 row", "VPRM with two rows");
  }
  {
    const Patch nan = [](const char* fourcc, Bytes& value) {
      if (std::strcmp(fourcc, "VPRM") == 0) {
        SetFloat(value, kVprmRow0, kNaN);
      }
    };
    Reject(Part(RefractionPart(), nan), kRefract, kRefractVars, "nonfinite CCH0", "NaN CCH0.x");
  }
}

// XFMD: the barrier follows its emitter (mode 3), not the emitter's absolute placement.
void TestXfmd() {
  {
    const Patch mode = [](const char* fourcc, Bytes& value) {
      if (std::strcmp(fourcc, "XFMD") == 0) {
        SetWord(value, 4, 4);  // a mover follows its SMVR; the barriers have none
      }
    };
    Reject(Part(RefractionPart(), mode), kRefract, kRefractVars, "barrier does not follow emitter",
           "XFMD 4");
  }
  {
    // Not the port's CNST form at all: the reader refuses the property itself.
    auto part = RefractionPart();
    part[4].second = Bytes{0, 0, 0, 3};
    Reject(Part(part), kRefract, kRefractVars, "XFMD does not read", "XFMD without CNST");
  }
  {
    // A word past the mode the property holds.
    auto part = RefractionPart();
    Bytes longer;
    PutFourCC(longer, "CNST");
    PutWord(longer, 3);
    PutWord(longer, 0);
    part[4].second = longer;
    Refuse(Part(part), kRefract, kRefractVars, "XFMD with a tail");
  }
}

// PVRT and VSMT: the table has the material's real variables, and the track points each of
// the shader's slots (32, then 33) at its own one of them. A swapped track (the variables
// read in the wrong order) misdrives the breakup and the gain.
void TestVariables() {
  {
    auto part = RefractionPart();
    part[5].second = PvrtValue({{kBreakup, 0}});
    Reject(Part(part), kRefract, kRefractVars, "invalid barrier variable table", "one PVRT record");
  }
  {
    auto part = RefractionPart();
    part[5].second = PvrtValue({{kBreakup, 1}, {kGain, 0}});  // an Int variable
    Reject(Part(part), kRefract, kRefractVars, "ambiguous or nonreal variable", "non-real variable");
  }
  {
    auto part = RefractionPart();
    part[5].second = PvrtValue({{kBreakup, 0}, {kBreakup, 0}});  // the same GUID twice
    Reject(Part(part), kRefract, kRefractVars, "ambiguous or nonreal variable", "repeated PVRT record");
  }
  {
    auto part = RefractionPart();
    part[5].second = PvrtValue({{kBreakup, 0}, {kOther, 0}});
    Reject(Part(part), kRefract, kRefractVars, "missing or repeated variable GUID",
           "a variable the material does not have");
  }
  {
    // The real variables are the caller's; a zero one names nothing.
    std::vector<Guid> vars = {kBreakup, kZero};
    Reject(Part(RefractionPart()), kRefract, vars, "zero variable GUID", "zero variable GUID");
  }
  {
    // Both tracks point at the first variable: the gain is never driven.
    auto part = RefractionPart();
    part[3].second = VsmtValue({{32, 0}, {33, 1}});
    part[3].second[kVsmtRecords + kVsmtRecord + 12] = 0;
    part[3].second[kVsmtRecords + kVsmtRecord + 13] = 0;
    part[3].second[kVsmtRecords + kVsmtRecord + 14] = 0;
    part[3].second[kVsmtRecords + kVsmtRecord + 15] = 0;
    Reject(Part(part), kRefract, kRefractVars, "incorrect barrier variable target", "both tracks at var 0");
  }
  {
    // Swapped GUID tracks: the caller hands the variables in the order the GENP's PVAR
    // table has them, and the track then disagrees with its slots.
    auto part = RefractionPart();
    part[5].second = PvrtValue({{kGain, 0}, {kBreakup, 0}});  // the table's order, reversed
    const std::vector<Guid> vars = {kBreakup, kGain};
    Reject(Part(part), kRefract, vars, "incorrect barrier variable target", "PVRT in the wrong order");
  }
  {
    // The VSMT targets are the shader's slots 32 and 33; 31 and 34 are not read.
    auto part = RefractionPart();
    part[3].second = VsmtValue({{31, 0}, {33, 1}});
    Reject(Part(part), kRefract, kRefractVars, "incorrect barrier variable target", "target 31");
  }
  {
    auto part = RefractionPart();
    part[3].second = VsmtValue({{32, 0}, {34, 1}});
    Reject(Part(part), kRefract, kRefractVars, "incorrect barrier variable target", "target 34");
  }
  {
    // The track's element must be VARF: a CNST real (as many bytes, and it reads) is a
    // constant, not the variable the target names.
    auto part = RefractionPart();
    part[3].second = VsmtValue({{32, 0}, {33, 1}});
    SetWord(part[3].second, kVsmtRecords + 8, Tag("CNST"));
    Reject(Part(part), kRefract, kRefractVars, "incorrect barrier variable target", "CNST track");
  }
  {
    // A track's kind must be Real (0). The VSMT reader always takes a real element, so a
    // kind 1 (vector) record parses and only the validator can catch it.
    auto part = RefractionPart();
    part[3].second = VsmtValue({{32, 0}, {33, 1}});
    SetWord(part[3].second, kVsmtRecords + 4, 1);
    Reject(Part(part), kRefract, kRefractVars, "incorrect barrier variable target", "vector track kind");
  }
  {
    // An int element (kind 2) reads as a CNST int, but the target's slot is a real.
    auto part = RefractionPart();
    part[3].second = VsmtValue({{32, 0}, {33, 1}});
    SetWord(part[3].second, kVsmtRecords + 4, 2);
    SetWord(part[3].second, kVsmtRecords + 8, Tag("CNST"));
    Reject(Part(part), kRefract, kRefractVars, "incorrect barrier variable target", "int track kind");
  }
  {
    auto part = RefractionPart();
    part[3].second = VsmtValue({{32, 0}});  // one track for two variables
    Reject(Part(part), kRefract, kRefractVars, "invalid barrier material track", "one track");
  }
}

// Regressions for the floats and words the draw reads but the first validator did not look
// at: the blend row (PortReadVfxMat drops the whole VMAT when blend > MaxBlend 7, so the
// PART would silently draw as retail), the eleven original sources and the material's own
// modulate/depthSoften (both scale the fragment), a v3 material's opacity gain/bias, the
// barrier's NMAP slot (the verified recipes have none, so -1) and an aliased colour/indirect
// pair (the shader reads TCH0 and TCH1 as different maps).
void TestMaterialRegressions() {
  for (const uint32_t blend : {8u, 0xffffffffu, 0x80000000u}) {
    const Patch patch = [blend](const char* fourcc, Bytes& value) {
      if (std::strcmp(fourcc, "VMAT") == 0) {
        SetWord(value, kVmatBlend, blend);
      }
    };
    Reject(Part(RefractionPart(), patch), kRefract, kRefractVars, "invalid VMAT layout or feature",
           "blend " + std::to_string(blend));
  }
  for (const size_t source : {size_t(0), size_t(10)}) {
    const Patch patch = [source](const char* fourcc, Bytes& value) {
      if (std::strcmp(fourcc, "VMAT") == 0) {
        SetFloat(value, VmatSrcValue(2, source), kNaN);
      }
    };
    Reject(Part(RefractionPart(), patch), kRefract, kRefractVars, "nonfinite material source",
           "NaN Src " + std::to_string(source) + " value");
  }
  {
    const Patch patch = [](const char* fourcc, Bytes& value) {
      if (std::strcmp(fourcc, "VMAT") == 0) {
        SetFloat(value, VmatModulate(2), kNaN);
      }
    };
    Reject(Part(RefractionPart(), patch), kRefract, kRefractVars, "nonfinite material parameter",
           "NaN modulate");
  }
  {
    const Patch patch = [](const char* fourcc, Bytes& value) {
      if (std::strcmp(fourcc, "VMAT") == 0) {
        SetFloat(value, VmatDepthSoften(2), kNaN);
      }
    };
    Reject(Part(RefractionPart(), patch), kRefract, kRefractVars, "nonfinite material parameter",
           "NaN depthSoften");
  }
  for (const bool gain : {true, false}) {
    const Patch patch = [gain](const char* fourcc, Bytes& value) {
      if (std::strcmp(fourcc, "VMAT") == 0) {
        SetFloat(value, gain ? VmatOpacityGain(2) : VmatOpacityBias(2), kNaN);
      }
    };
    auto part = RefractionPart();
    part[0].second = Counted(VmatPayload(3, kRefract, 0, 2));
    Reject(Part(part, patch), kRefract, kRefractVars, "nonfinite material parameter",
           gain ? "NaN v3 opacity gain" : "NaN v3 opacity bias");
  }
  {
    // The verified barrier recipes have no normal map: VPRM's NMAP slot must be -1.
    const Patch patch = [](const char* fourcc, Bytes& value) {
      if (std::strcmp(fourcc, "VPRM") == 0) {
        SetWord(value, kPayload + 4, 0);
      }
    };
    Reject(Part(RefractionPart(), patch), kRefract, kRefractVars, "missing CCH0 row", "VPRM NMAP 0");
  }
  {
    // Both maps at one slot: the pattern and the two noise reads would be one texture.
    const Patch patch = [](const char* fourcc, Bytes& value) {
      if (std::strcmp(fourcc, "VMAT") == 0) {
        SetWord(value, VmatSlot(2, 5), 0);
      }
    };
    Reject(Part(RefractionPart(), patch), kRefract, kRefractVars, "missing barrier texture slot",
           "colour slot == indirect slot");
  }
  {
    // AlreadyExposed (bit 31) is an exposure policy the converter sets per material; neither
    // verified barrier recipe emits it, so a barrier VMAT that carries it is not one of them.
    for (const uint32_t feature : {kRefract, kGlow}) {
      const Patch patch = [feature](const char* fourcc, Bytes& value) {
        if (std::strcmp(fourcc, "VMAT") == 0) {
          SetWord(value, kVmatFeatures, feature | (1u << 31));
        }
      };
      const auto part = feature == kRefract ? RefractionPart() : GlowPart();
      const auto& vars = feature == kRefract ? kRefractVars : kGlowVars;
      Reject(Part(part, patch), feature, vars, "invalid VMAT layout or feature",
             feature == kRefract ? "features bit29|bit31" : "features bit30|bit31");
    }
  }
}

// The barrier's pattern maps are ordinary 2D textures: one layer, one atlas cell, no warp
// and no tiling, sampled with a wrap mode and a filter the sampler has. A conversion that
// writes a flipbook, a tile atlas or a UV set the barrier does not use is not one of the
// verified effects; nor is a mesh with flags the layout does not define, or a variable whose
// default is not a number.
void TestTextureAndDefaultRegressions() {
  struct Field {
    const char* name;
    size_t field;
    uint32_t value;
  };
  const Field fields[] = {
      {"uv set 3", 1, 3},   {"wrap S 3", 2, 3},  {"wrap T 3", 3, 3},      {"filter 2", 4, 2},
      {"cols 2", 5, 2},     {"cols 0", 5, 0},    {"rows 2", 6, 2},        {"rows 0", 6, 0},
      {"layers 2", 7, 2},   {"layers 0", 7, 0},  {"warped 1", 8, 1},
  };
  for (const Field& field : fields) {
    for (const size_t texture : {size_t(0), size_t(1)}) {
      const Patch patch = [field, texture](const char* fourcc, Bytes& value) {
        if (std::strcmp(fourcc, "VMAT") == 0) {
          SetWord(value, VmatTexField(texture, field.field), field.value);
        }
      };
      Reject(Part(RefractionPart(), patch), kRefract, kRefractVars, "unsupported barrier texture layout",
             std::string(field.name) + " on texture " + std::to_string(texture));
    }
  }
  // The glow's single map too: the layout is checked whatever the feature.
  {
    const Patch patch = [](const char* fourcc, Bytes& value) {
      if (std::strcmp(fourcc, "VMAT") == 0) {
        SetWord(value, VmatTexField(0, 5), 4);  // cols 4
      }
    };
    Reject(Part(GlowPart(), patch), kGlow, kGlowVars, "unsupported barrier texture layout",
           "glow cols 4");
  }
  for (const uint32_t flags : {4u, 8u, 0xffffffffu}) {
    const Patch patch = [flags](const char* fourcc, Bytes& value) {
      if (std::strcmp(fourcc, "VMSH") == 0) {
        SetWord(value, kVmshFlags, flags);
      }
    };
    Reject(Part(RefractionPart(), patch), kRefract, kRefractVars, "missing coloured VMSH v2",
           "VMSH flags " + std::to_string(flags));
  }
  for (const size_t record : {size_t(0), size_t(1)}) {
    for (const size_t index : {size_t(0), size_t(3)}) {
      const Patch patch = [record, index](const char* fourcc, Bytes& value) {
        if (std::strcmp(fourcc, "PVRT") == 0) {
          SetFloat(value, PvrtDefault(record, index), kNaN);
        }
      };
      Reject(Part(RefractionPart(), patch), kRefract, kRefractVars, "nonfinite variable default",
             "NaN PVRT record " + std::to_string(record) + " default " + std::to_string(index));
    }
  }
}

// The stream must end where its last property does: retail's reader stops at _END, so a
// staged PART with anything after it is not the file the conversion wrote.
void TestTrailingData() {
  const auto refuse = [](Bytes part, const std::string& what) {
    Refuse(part, kRefract, kRefractVars, what);
  };
  {
    Bytes part = Part(RefractionPart());
    part.push_back(0);
    refuse(part, "one byte after _END");
  }
  {
    Bytes part = Part(RefractionPart());
    PutFourCC(part, "SIZE");
    refuse(part, "a fourcc after _END");
  }
  {
    // A whole property the reader never reaches.
    Bytes part = Part(RefractionPart());
    PutFourCC(part, "SIZE");
    PutFourCC(part, "CNST");
    PutFloat(part, 0.1f);
    refuse(part, "a property after _END");
  }
  {
    // The exact arithmetic: everything before _END counts, so a PART cut inside its last
    // property is a reader failure, not trailing data.
    Bytes part = Part(RefractionPart());
    part.resize(part.size() - 2);
    Refuse(part, kRefract, kRefractVars, "cut inside _END");
  }
}

// The seven slots are -1 or an existing texture (the colour and, for a refraction, the
// indirect slot keep their own, more specific refusals, which the order preserves), and the
// sprite centre is one of the nine the feed indexes.
void TestSlotsAndSpriteCentre() {
  const char* names[7] = {"colour", "opacity", "ramp", "ramp2", "threshold", "indirect", "palette"};
  for (size_t slot = 0; slot < 7; ++slot) {
    for (const bool refract : {true, false}) {
      const uint32_t feature = refract ? kRefract : kGlow;
      const size_t textures = refract ? 2 : 1;
      const Patch patch = [slot, textures](const char* fourcc, Bytes& value) {
        if (std::strcmp(fourcc, "VMAT") == 0) {
          SetWord(value, VmatSlot(textures, slot), uint32_t(textures));  // one past the last
        }
      };
      const auto part = refract ? RefractionPart() : GlowPart();
      const auto& vars = refract ? kRefractVars : kGlowVars;
      // Colour, and the indirect slot of a refraction, are refused by their own check first.
      const bool specific = slot == 0 || (refract && slot == 5);
      Reject(Part(part, patch), feature, vars,
             specific ? "missing barrier texture slot" : "invalid material texture slot",
             std::string(names[slot]) + " slot past the texture count");
    }
  }
  {
    // 0xffffffff is the reader's "no slot"; any other value must exist.
    const Patch patch = [](const char* fourcc, Bytes& value) {
      if (std::strcmp(fourcc, "VMAT") == 0) {
        SetWord(value, VmatSlot(2, 6), 0xfffffffeu);
      }
    };
    Reject(Part(RefractionPart(), patch), kRefract, kRefractVars, "invalid material texture slot",
           "palette slot 0xfffffffe");
  }
  {
    // A palette slot that does exist is structurally fine (the default is -1).
    const Patch patch = [](const char* fourcc, Bytes& value) {
      if (std::strcmp(fourcc, "VMAT") == 0) {
        SetWord(value, VmatSlot(2, 6), 1);
      }
    };
    Accept(Part(RefractionPart(), patch), kRefract, kRefractVars, "palette slot 1 of 2");
  }
  for (const uint32_t centre : {9u, 0xffffffffu}) {
    const Patch patch = [centre](const char* fourcc, Bytes& value) {
      if (std::strcmp(fourcc, "VMAT") == 0) {
        SetWord(value, VmatSpriteCenter(2), centre);
      }
    };
    Reject(Part(RefractionPart(), patch), kRefract, kRefractVars, "invalid sprite centre",
           "sprite centre " + std::to_string(centre));
  }
  {
    // 8 is the last centre the feed has.
    const Patch patch = [](const char* fourcc, Bytes& value) {
      if (std::strcmp(fourcc, "VMAT") == 0) {
        SetWord(value, VmatSpriteCenter(2), 8);
      }
    };
    Accept(Part(RefractionPart(), patch), kRefract, kRefractVars, "sprite centre 8");
  }
}

// The request itself: the two features this validates, their variable counts and a
// dependency to ask.
void TestRequest() {
  const Bytes part = Part(RefractionPart());
  std::string error;
  CHECK(!ValidatePart(part, 1u << 28, kRefractVars, Assets{}, error));
  CHECK_EQ(error, std::string("invalid barrier validation request"));
  CHECK(!ValidatePart(part, kRefract, kGlowVars, Assets{}, error));
  CHECK_EQ(error, std::string("invalid barrier validation request"));
  CHECK(!ValidatePart(Part(GlowPart()), kGlow, kRefractVars, Assets{}, error));
  CHECK_EQ(error, std::string("invalid barrier validation request"));
  CHECK(!ValidatePart(Part(GlowPart()), kGlow, kGlowVars, Check{}, error));
  CHECK_EQ(error, std::string("invalid barrier validation request"));
}

// The five real converted barrier PARTs (a refraction conversion of the room's GENPs) have
// every property but a VMSH: a GENP with no retail PART is converted through no model
// provider, so no mesh is written. They must validate up to that point, which is what shows
// the offsets above are the real files' and not only this test's own.
void TestRealParts(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    std::ifstream in(argv[i], std::ios::binary);
    REQUIRE(in.good());
    const Bytes part{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const std::string name = argv[i];

    std::vector<PortRemastered::RetailPartProperty> properties;
    std::string error;
    REQUIRE_MSG(PortRemastered::SplitRetailPart(part.data(), part.size(), properties, error),
                name + ": reads as a PART: " + error);
    size_t vmat = 0;
    for (const auto& property : properties) {
      vmat += property.fourcc == Tag("VMAT") ? 1 : 0;
      CHECK_MSG(property.fourcc != Tag("VMSH"), name + ": has no VMSH (no model provider)");
    }
    CHECK_EQ(vmat, size_t(1));

    // The variables are the file's own PVRT records; the dependency has every TXTR.
    std::vector<Guid> vars;
    for (const auto& property : properties) {
      if (property.fourcc != Tag("PVRT")) {
        continue;
      }
      REQUIRE(property.value.size() >= 8);
      const uint32_t count = uint32_t(property.value[4]) << 24 | uint32_t(property.value[5]) << 16 |
                             uint32_t(property.value[6]) << 8 | property.value[7];
      CHECK_EQ(property.value.size(), size_t(8) + size_t(count) * 36);
      for (uint32_t v = 0; v < count && 8 + size_t(v) * 36 + 16 <= property.value.size(); ++v) {
        Guid guid{};
        std::copy(property.value.begin() + ptrdiff_t(8 + size_t(v) * 36),
                  property.value.begin() + ptrdiff_t(8 + size_t(v) * 36 + 16), guid.begin());
        vars.push_back(guid);
      }
    }
    CHECK_EQ(vars.size(), size_t(2));

    const bool ok = ValidatePart(part, kRefract, vars, Assets{}, error);
    CHECK_MSG(!ok, name + ": refused (there is no VMSH)");
    CHECK_EQ(error, std::string("missing coloured VMSH v2"));
    std::printf("%s: refused: %s\n", name.c_str(), error.c_str());
  }
}

}  // namespace

int main(int argc, char** argv) {
  TestPositives();
  TestTruncation();
  TestFraming();
  TestMissingProperties();
  TestDuplicateProperties();
  TestVmat();
  TestVmsh();
  TestVprm();
  TestXfmd();
  TestVariables();
  TestMaterialRegressions();
  TestTextureAndDefaultRegressions();
  TestTrailingData();
  TestSlotsAndSpriteCentre();
  TestRequest();
  TestRealParts(argc, argv);
  return PORT_TEST_RESULT();
}
