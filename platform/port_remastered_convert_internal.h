// Internal to the Remastered model converter (port_remastered_convert.h): what its parts
// share, in PortRemastered::ConvertDetail. The reader and writer primitives, the retail and
// Remastered side types and the material constants the converter reads too are here;
// the parts themselves are
//
//   port_remastered_convert_retail.cpp    the retail CMDL/CSKR parse
//   port_remastered_convert_material.cpp  the shader tables and ReadMaterial
//   port_remastered_convert_pbr.cpp       the port's PBR material record
//   port_remastered_convert_skin.cpp      skin weights and the CSKR and surface writers
//   port_remastered_convert.cpp           textures (Converter::State) and the conversion
//
// Nothing outside those files should include this.

#pragma once

#include "port_env.h"
#include "port_hash.h"
#include "port_bytes.h"
#include "port_remastered_convert.h"
#include "port_strings.h"
#include "port_remastered_anuv.h"
#include "port_visor_mesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_map>

#include "port_remastered_pak.h"
#include "port_remastered_uv.h"

namespace PortRemastered {
namespace ConvertDetail {

struct Fail {
  std::string what;
};

using port::FourCC;

struct Span {
  const uint8_t* p = nullptr;
  size_t n = 0;
};

inline uint32_t R32(Span s, size_t o) {
  if (o > s.n || s.n - o < 4) {
    throw Fail{"a retail resource is truncated"};
  }
  return port::ReadBE32(s.p + o);
}

inline uint16_t R16(Span s, size_t o) {
  if (o > s.n || s.n - o < 2) {
    throw Fail{"a retail resource is truncated"};
  }
  return port::ReadBE16(s.p + o);
}

inline uint8_t R8(Span s, size_t o) {
  if (o >= s.n) {
    throw Fail{"a retail resource is truncated"};
  }
  return s.p[o];
}

inline float RF(Span s, size_t o) {
  const uint32_t v = R32(s, o);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

inline Span Sub(Span s, size_t o, size_t n) {
  if (o > s.n || s.n - o < n) {
    throw Fail{"a retail resource is truncated"};
  }
  return Span{s.p + o, n};
}

// A slice the way the reference takes one: clamped to what is there.
inline Span Slice(Span s, size_t o, size_t n) {
  if (o > s.n) {
    o = s.n;
  }
  return Span{s.p + o, std::min(n, s.n - o)};
}

using Blob = std::vector<uint8_t>;

inline void P8(Blob& b, uint8_t v) { b.push_back(v); }
inline void P16(Blob& b, uint32_t v) {
  b.push_back(uint8_t(v >> 8));
  b.push_back(uint8_t(v));
}
inline void P32(Blob& b, uint32_t v) {
  b.push_back(uint8_t(v >> 24));
  b.push_back(uint8_t(v >> 16));
  b.push_back(uint8_t(v >> 8));
  b.push_back(uint8_t(v));
}
inline void PF(Blob& b, double v) {
  const float f = float(v);
  uint32_t u;
  std::memcpy(&u, &f, 4);
  P32(b, u);
}
inline void Pad(Blob& b, size_t a = 32) { b.resize((b.size() + a - 1) & ~(a - 1), 0); }
inline void Append(Blob& b, const Blob& o) { b.insert(b.end(), o.begin(), o.end()); }

inline size_t Align(size_t n, size_t a = 32) { return (n + a - 1) & ~(a - 1); }

using port::Hex8;

inline std::string FormatG(double v) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%g", v);
  return buf;
}

using port::Lower;

using port::Crc32;

// A material parameter as the reference reads it: the float's shortest decimal
// form, read back as a double (it parses them out of a text table).
inline double ShortestDouble(float f) {
  char buf[40];
  for (int precision = 1; precision <= 9; ++precision) {
    std::snprintf(buf, sizeof(buf), "%.*g", precision, double(f));
    if (std::strtof(buf, nullptr) == f) {
      break;
    }
  }
  return std::strtod(buf, nullptr);
}

// A sum over a row the way numpy takes it (eight running sums past seven
// elements, halves past 128), since a share that sits on a threshold must fall
// on the same side here.
inline double RowSum(const double* a, size_t n) {
  if (n < 8) {
    double r = 0.0;
    for (size_t i = 0; i < n; ++i) {
      r += a[i];
    }
    return r;
  }
  if (n <= 128) {
    double r[8];
    for (int k = 0; k < 8; ++k) {
      r[k] = a[k];
    }
    size_t i = 8;
    for (; i < n - (n % 8); i += 8) {
      for (int k = 0; k < 8; ++k) {
        r[k] += a[i + k];
      }
    }
    double res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
    for (; i < n; ++i) {
      res += a[i];
    }
    return res;
  }
  size_t half = n / 2;
  half -= half % 8;
  return RowSum(a, half) + RowSum(a + half, n - half);
}

// For each of the na points in a, the nearest of the nb points in b (the first
// on a tie).
inline void Nearest(const double* a, size_t na, const double* b, size_t nb, std::vector<uint32_t>& out) {
  out.resize(na);
  for (size_t i = 0; i < na; ++i) {
    const double x = a[3 * i], y = a[3 * i + 1], z = a[3 * i + 2];
    double best = 0.0;
    uint32_t at = 0;
    for (size_t j = 0; j < nb; ++j) {
      const double dx = x - b[3 * j], dy = y - b[3 * j + 1], dz = z - b[3 * j + 2];
      const double d = (dx * dx + dy * dy) + dz * dz;
      if (j == 0 || d < best) {
        best = d;
        at = uint32_t(j);
      }
    }
    out[i] = at;
  }
}

// ---- retail side ----

struct RetailMaterial {
  uint32_t flags = 0;
  std::vector<uint32_t> tex;
  uint32_t vtx = 0;
  uint32_t group = 0;
  uint16_t blendSrc = 0, blendDst = 0;
  double konstAlpha = 1.0;  // the first konst colour's alpha (1 with none)
  std::vector<uint32_t> chans;
  struct Tev {
    uint32_t color, alpha, colorOp, alphaOp;
  };
  std::vector<Tev> tev;
  std::vector<std::array<uint8_t, 4>> tevTex;
  std::vector<uint32_t> texgen;
  std::vector<uint8_t> uvAnim;  // the section as it is: its size, the count and the animations
};

RetailMaterial ParseMaterial(Span m);

struct MaterialSet {
  std::vector<uint32_t> tex;
  std::vector<Span> mats;
};

MaterialSet ParseSet(Span sec);
size_t VtxStride(uint32_t vtx);
int Popcount(uint32_t v);
void Triangles(Span dl, size_t stride, size_t mtx, std::vector<uint32_t>& tris);

struct Retail {
  Blob data;
  uint32_t flags = 0;
  uint32_t nmat = 0;
  std::vector<Span> secs;
  std::vector<MaterialSet> sets;
  std::vector<RetailMaterial> mats;  // of set 0
  std::vector<double> P;             // every position, xyz
  std::vector<int> vmat;             // the material drawing each position, -1 for none

  size_t Count() const { return vmat.size(); }

  void Parse() {
    const Span d{data.data(), data.size()};
    flags = R32(d, 8);
    const uint32_t nsec = R32(d, 0x24);
    nmat = R32(d, 0x28);
    if (nsec > 0x100000 || nmat == 0 || size_t(nmat) + 6 > nsec) {
      throw Fail{"the retail model has an unexpected layout"};
    }
    size_t o = Align(0x2c + size_t(nsec) * 4);
    for (uint32_t i = 0; i < nsec; ++i) {
      const uint32_t size = R32(d, 0x2c + size_t(i) * 4);
      secs.push_back(Sub(d, o, size));
      o += size;
    }
    for (uint32_t i = 0; i < nmat; ++i) {
      sets.push_back(ParseSet(secs[i]));
    }
    for (const Span& m : sets[0].mats) {
      mats.push_back(ParseMaterial(m));
    }
    // The section is padded to 32 bytes, so the padding reads as a few more
    // vertices at the origin; nothing draws them, and they keep their place.
    const Span ps = secs[nmat];
    const size_t np = ps.n / 4 / 3;
    P.resize(np * 3);
    for (size_t i = 0; i < np * 3; ++i) {
      P[i] = RF(ps, i * 4);
    }
    vmat.assign(np, -1);
    const uint32_t nsurf = R32(secs[nmat + 5], 0);
    std::vector<uint32_t> tris;
    for (size_t si = nmat + 6; si < secs.size() && si < size_t(nmat) + 6 + nsurf; ++si) {
      const Span s = secs[si];
      const uint32_t mat = R32(s, 12);
      const uint32_t dlSize = R32(s, 16) & 0x7FFFFFFF;
      const uint32_t extra = R32(s, 28);
      if (mat >= mats.size()) {
        throw Fail{"a retail surface names a material that does not exist"};
      }
      const uint32_t vtx = mats[mat].vtx;
      tris.clear();
      Triangles(Slice(s, Align(44 + size_t(extra)), dlSize), VtxStride(vtx), size_t(Popcount(vtx >> 24)), tris);
      for (uint32_t t : tris) {
        if (t < np && vmat[t] < 0) {
          vmat[t] = int(mat);
        }
      }
    }
  }
};

// A gun-fx particle model (kind 19): retail blends it (4,5) with its konst alpha, where
// Remastered's mesh is opaque. A retail material with an indirect texture (flag 0x400, the
// Metroid3 ice platforms) is drawn opaque as Remastered does.
inline bool GunFxParticle(const RetailMaterial& m) { return m.blendSrc == 4 && m.blendDst == 5 && !(m.flags & 0x400); }

inline bool IsFx(const RetailMaterial& m) { return !(m.blendSrc == 1 && m.blendDst == 0); }

// The (source, destination) factors Remastered draws a mesh class with: opaque and cutout
// GX_BL_ONE / GX_BL_ZERO, blended GX_BL_SRCALPHA / GX_BL_INVSRCALPHA, additive GX_BL_SRCALPHA
// / GX_BL_ONE. False for a material no mesh uses.
inline bool ClassBlend(int cls, uint32_t& src, uint32_t& dst) {
  if (cls < 0 || cls > 3) {
    return false;
  }
  src = cls == 1 || cls == 3 ? 4 : 1;
  dst = cls == 1 ? 5 : cls == 3 ? 1 : 0;
  return true;
}

enum class Role { EnvMap, Lightmap, Reflect, Emissive, Diffuse, Keep };

const char* RoleName(Role r);

struct SlotRole {
  uint32_t slot;
  uint32_t coord;
  Role role;
  int uvSrc;  // which texcoord attribute the stage's texgen reads, negative for none
};

std::vector<SlotRole> SlotRoles(const RetailMaterial& m);

// Retail CSKR: weight groups (bone, weight)... and how many vertices each covers.
struct SkinGroup {
  std::vector<std::pair<uint32_t, float>> weights;
  uint32_t count = 0;
};

std::vector<SkinGroup> ReadCskr(Span d);
std::vector<uint32_t> SkinBones(Span d);

// ---- Remastered side ----

enum { kBase = 0, kMr = 1, kNormal = 2, kEmissive = 3, kMaps = 4, kLayeredMaps = 7, kMacroMaps = 8 };
const char* const kMapName[kMaps] = {"base", "mr", "normal", "emissive"};
// The texel a missing map gets: white, no occlusion / mid roughness / no metal,
// a normal pointing straight out, black.
const char* const kMapNeutral[kMaps] = {"ffffff", "ff9900", "8080ff", "000000"};
const int kPbrMax[kMaps] = {1024, 256, 256, 256};
// Each PBR map is drawn from its .dds, outside the game heap, at up to
// kPbrNative; the TXTR beside it only supplies the sampler state (and is what
// shows where BC textures are unsupported), so it is a stub.
const int kPbrNative[kMaps] = {2048, 1024, 1024, 1024};
const int kPbrStub[kMaps] = {128, 32, 32, 32};
// The PBR emissive map is stored as authored. Remastered's glow is the bare ICAN x ICNC
// in scene radiance, which the post tonemap then multiplies by 2^(3 - EV) (0.03-0.09 in
// the rooms seen); the port does the same at run time (record mode bit 32, see
// PortSetPBRMaterial). Without tone data Remastered's exposure is 1 (STonemapParams::
// BuildLinear(3.0), kb topic/post-output.md), so the glow is left as authored: 1, which is
// also what the TEV fallback adds and what sizes a ramp's mean (not an authored map).
const double kPbrEmissive = 1.0;
// Ceiling on the metalness channel. Remastered applies none (the MR map is read as is);
// it was 0.6 before the BRDF LUT and the room probe cubes, when full metals went near-black.
const double kPbrMetalMax = 1.0;
const double kJointSplit = 0.05;
// The PC skin reader (CVirtualBone, SKIN_MAX_WEIGHTS) keeps four weights a vertex
// and drops the rest without renormalising, so more would leave the vertex pulled
// towards the origin. Retail data has at most three.
const size_t kMaxSkinWeights = 4;
const uint32_t kPbrFlag = 0x4000;  // kStateFlag_PortPBR

struct AnuvCounts {
  int animated = 0;          // materials given a texture matrix from an ANUV entry
  int sharedSlots = 0;       // transforms dropped because a slot already had one
  int scrollOverridden = 0;  // animated materials that also had a scroll of their own
  int entryDisagree = 0;     // materials whose meshes name different entries
  int notFlattened = 0;      // materials whose entry did not flatten
  int slotSplit = 0;         // materials given a texcoord copy so two transforms on one set could differ
  int outOfSlots = 0;        // materials that needed a copy and had no texcoord left
};

struct MapRef {
  bool has = false;
  ModelUuid id{};
  uint32_t coord = 0;
  uint32_t authored = 0;  // the texcoord index the material named, before the AUVI chose its set
  int32_t wrap[2] = {1, 1};  // the sampler's U and V modes (0 clamp, 1 repeat, 2 mirror)
  std::string src;  // how the texture is named in a tag
  bool raw = false;  // a shader's own data (a ramp, noise): every texel and channel kept as it is
  bool detail = false;  // kind 20's detail map, which rides in the emissive slot (that shader has no glow)
  bool volume = false;  // a 3D texture, kept whole as an 8 x 8 atlas of its 64 slices (ConvertIO::volume), RGBA8, level 0 only
  double metalMax = kPbrMetalMax;  // an MR map's metalness ceiling
  // A base map's: at most one map on its texcoord whose red becomes its alpha (the gun
  // body's IncanMask, TCH0), which the "mask" alpha then lays over the glow.
  std::vector<MapRef> mask;
};

struct RemMaterial {
  std::string name;
  uint32_t shader = 0;      // the Remastered shader id (big-endian first word), for the report
  uint32_t family = 0;      // the id of the shader whose code this one repeats exactly (kShaderTwins), else shader
  uint32_t flags = 0;       // the material's feature flags (unk1)
  std::string role;         // the shader lists it is in
  std::string reason;       // which rule chose the kind, for the report
  MapRef maps[kMaps];
  double emissive = 1.0;   // Remastered's emissive strength
  double backlight = 0.0;  // and its backlight strength (from behind; see BacklightRecord)
  double backlightTop = 0.0;    // the backlight's strength from above
  double backlightFalloff = 0.0;  // the power of its fade along the model's y, plus 1 (1 no fade, 0 no backlight)
  bool cutout = false;     // the base map's alpha cuts holes: leaves, grates
  bool blended = false;    // drawn over what is behind it: glass, decals, ice
  bool additive = false;   // its meshes are of class 3: added to what is behind it (SrcA, One)
  int blendClass = -1;     // its meshes' class (ModelMesh::bits2): 0 opaque, 1 blended, 2 cutout, 3 additive; -1 none
  bool classBlend = false; // a retail model's: it blends as its class does, not as the retail material it took
  bool tinted = false;     // its vertices carry a colour
  bool tintF0 = false;     // which tints the albedo before F0 too (kShaderTints), not just the diffuse
  bool wind = false;       // procedural wind sway (mode bit 32768): the model's SWindSet rides in shieldRows[0..8]
  bool cutExact = false;   // drawn as Remastered's 1-bit cutout: full-alpha base, discard at alpha^2 < 0.25 (mode bit 512)
  bool indirect = false;   // 8CE05ED0: map 1 is INDI, the base map's uv offset (mode bit 8192); indStrength is INDS
  double indStrength = 0.0;
  bool noRefl = false;     // no REFL: its shader samples no cube, so it reflects nothing around it
  bool unlit = false;      // a screen: its own colour and glow, no lighting
  bool glowLinear = false; // inverse-exposed: the emissive strength is drawn as is, uncompressed
  int anuv = -1;  // the model's ANUV entry its meshes use, when it flattens
  bool mask = false;       // the base map's alpha masks the glow and is no opacity
  bool maskSquared = false; // ... squared (USE_DIFFUSE_AS_INCAN_MASK): kept squared in the map
  bool shell = false;      // a matcap shell: drawn as PBR kind 13 over the retail blended slot
  bool shield = false;     // a boundary shield or a pickup: PBR kind 14 or 15 over the retail fx slot
  // Kind 14's shader constants: CCH0..CCH6 then DIFC, four each (the PBR8 trailer).
  double shieldRows[32] = {};
  double height = 0.0;     // above 0: the threshold of a height-blended alpha
  // A second layer (base, MR, normal) the vertex alpha blends over the first
  // by the two base maps' heights: snow on rock, moss on stone.
  bool layered = false;
  bool macro = false;  // a macro normal map (MNMP, layer[kNormal]) on a single-layer material
  MapRef layer[3];
  // A macro normal on a layered material (3c66aaef): its own map 7, whiteout-blended over the mixed
  // layer normals in TANGENT_1's frame (mode bit 16384, as the single-layer macro).
  bool macroLayered = false;
  MapRef macroMap;
  double layerSmooth = 0.0;                      // BLSM: the width of the blend's edge
  double layerHeight[4] = {1.0, 0.0, 1.0, 0.0};  // BSAO: scale and offset of each layer's height
  // A shader of its own the port draws (GXSetPBRMaterial's kinds): 1 a second
  // layer on what faces up, 2 a detail map, 3 lava, 4 ice seen into, 5 unused (water
  // is the room's own mesh now), 6 a lava pool's, 7 falling water, 8 glass.
  int kind = 0;
  double kindStrength = 0.0;
  double kindParam[4] = {0.0, 0.0, 0.0, 0.0};
  bool vcolor = false;  // it reads the vertex colour, which is no tint
  double tint[3] = {0.0, 0.0, 0.0};  // kind 7: the liquid's colour; 8: what is seen through it
  bool hidden = false;  // not drawn: the game has its own
  double scroll[2] = {0.0, 0.0};  // the base map's texcoord, per second
  // ColorUnlit's colour: twice the vertex colour linearised, times the base map
  // and CCH0.x x CCH1.x (kept in backlight, which an unlit surface has no use for).
  bool colorUnlit = false;
  // LITS, the shader's LightBleedScale: what a back-facing pixel's light is scaled by (see
  // BackLightScale). hasLits is false for a material that carries none.
  bool hasLits = false;
  double lits = 1.0;
  // What the back copy of a LITS material draws with (diffuse, F0); the front's stay 1, 1.
  double lightScale[2] = {1.0, 1.0};
  // REFL, the cube the arm cannon's shaders reflect in place of the room's (kShaderGunBody).
  MapRef refl;
};

struct Buffer {
  size_t n = 0;
  size_t offset = 0;  // of its first vertex in the unified list
  bool loaded = false;
  bool used = false;
  std::vector<double> P, N;
  std::vector<double> T;  // xyzw per vertex, empty where the buffer has no tangents
  std::vector<double> T1; // TANGENT_1, the same, empty where the buffer has none
  std::vector<std::vector<double>> uv;
  std::vector<uint8_t> C;  // rgba per vertex, white where the buffer has no colours
  bool colored = false;
  const ModelVertexBuffer* src = nullptr;
  bool skinned = false;
  std::vector<uint32_t> copyOf;  // vertices past src's: the src vertex each was copied from
  std::vector<uint32_t> srcOf;   // once compacted: the src vertex each kept one comes from
};

struct Prim {
  uint32_t buffer = 0;
  uint32_t mat = 0;
  std::vector<uint32_t> I;
  int rmat = 0;
  int omat = 0;
  bool litsBack = false;  // the back copy of a material with a positive LITS (see BackLightScale)
  uint32_t visor = 0;     // PortVisorMesh::FromFlags of the mesh's flag word; 0 for most meshes
};

using WeightKey = std::vector<std::pair<uint32_t, double>>;

inline bool ContainsNoCase(const std::string& lower, const char* needle) { return lower.find(needle) != std::string::npos; }

// Names of effect materials: glow shells, pickups' halos.
inline bool IsFxName(const std::string& name) {
  const std::string l = Lower(name);
  return ContainsNoCase(l, "pickup") || ContainsNoCase(l, "vfx") || ContainsNoCase(l, "glow") ||
         ContainsNoCase(l, "additive") || ContainsNoCase(l, "_fx") || ContainsNoCase(l, "fx_");
}

inline int NextPow2(int v) {
  int p = 1;
  while (p < std::max(1, v)) {
    p <<= 1;
  }
  return p;
}

// ---- materials (port_remastered_convert_material.cpp, _pbr.cpp, _skin.cpp) ----

// Bits of a Remastered material's feature word.
constexpr uint32_t kTransparentFlag = 0x1;   // blended over what is behind it
constexpr uint32_t kVertexColorFlag = 0x10;  // the vertex colour tints it
constexpr uint32_t kCutoutFlag = 0x20;       // one-bit alpha: ground leaves, grates, foliage
constexpr uint32_t kIncanMaskFlag = 0x200;   // the base map's alpha masks the glow
// LayerBaseNormal_2TangentStream: each layer's normal is built in its own tangent frame,
// TANGENT_0 for the first and TANGENT_1 for the second. Their models carry a second frame
// (CMDL flag 0x10, 15-float NBT entries, DL opcode 0x94).
constexpr uint32_t kShaderTwoTangent[] = {0xA978D507, 0xD363D694, 0xA3C367BE, 0x72B34E42, 0xB9E899F3,
                                           0x7FEB9E94, 0xF84AC32D, 0x3C66AAEF};
// Lit glass that Remastered draws premultiplied (EBlendMode 5, One and InvSrcAlpha):
// phazon and plasma glass, soot_translucent. Its opacity scales the diffuse light
// only; the reflection and the glow show at full strength on the clearest pane.
constexpr uint32_t kShaderPremulGlass[] = {0x11B30369, 0x941068BF, 0xBCC73459};
// Of those, 941068bf and bcc73459 read the vertex colour raw (no log2/exp2 in their vertex shaders); 11b30369
// decodes it as 2 |c|^2.2 like every other MFVC shader (mode bit 524288, kb material/11b30369.md).
// 788360de, lit glass with coloured translucency (dual-source blend One / Src1Color, kb material/788360de.md).
constexpr uint32_t kShaderDualGlass = 0x788360DE;
constexpr uint32_t kShaderRawVertex[] = {0x941068BF, 0xBCC73459, kShaderDualGlass};
// Retail fx shaders of the Metroid test models and holograms (build/mpr/holo86, holo63, hologram
// NOTES.md): 86CD1703 = unlit DIFT x DIFC + ICNC + ICMC, alpha DIFT.a^2 x DIFC.a, additive (kind 16);
// 6344950D = the same plus the material's own REFL cube (kind 17); 4CA0017C = the scrolling
// hologram (kind 18, CCH0..3, DIFC, ICMC in the PBR8 trailer). 4BC890C1 is a plain lit Lambert
// with no kind: it only leaves the retail-fx gate.
constexpr uint32_t kShaderHolo = 0x86CD1703;
constexpr uint32_t kShaderHoloRefl = 0x6344950D;
constexpr uint32_t kShaderHologram = 0x4CA0017C;
constexpr uint32_t kShaderLambertFx = 0x4BC890C1;
// 29D9FDFB, VFX_Model_Base: pure Lambert, BCLR x vertex colour x (baked light + lights) / pi, the
// opacity the base alpha as it is. No specular from the lights, no reflection, no AO (kb
// material/29d9fdfb.md). Retail models; the retail blend stays.
constexpr uint32_t kShaderVfxBase = 0x29D9FDFB;
// 1982DB27: the same without the vertex colour (alpha BCLR.a raw, BLCM on the whole sum).
constexpr uint32_t kShaderVfxBase2 = 0x1982DB27;
// The foliage shaders with USE_PROCEDURAL_WIND_ANIMATION whose vertex stage the port reproduces (kb material/2ef20548.md,
// f7a0a891.md): the sway of the model's 'WIND' set, weighted by the vertex colour's alpha.
// 2ef20548/f7a0a891 plus the shaders whose vertex stage is the same sway: 83bded1d (2ef's FS), a3c14cce and
// f63182ce (2ef + an emissive map), 73ea9e03 (f7a0 + an emissive map), 5f7e93a7 and 8195894e (2ef/f7a0 with a
// push pre-stage that moves y by a runtime sphere term; its default push is zero, so the sway is the same).
constexpr uint32_t kShaderWindFoliage[] = {0x2EF20548, 0xF7A0A891, 0x83BDED1D, 0xA3C14CCE,
                                           0xF63182CE, 0x73EA9E03, 0x5F7E93A7, 0x8195894E, 0x495899E7};
// 495899e7 (TendrilLights): a3c14cce's lighting with the opacity the vertex alpha alone (mode bit 65536).
constexpr uint32_t kShaderVertexAlpha = 0x495899E7;
constexpr uint32_t kShaderDecalAlphaMap = 0x42FE2ED0;
// The unlit "Surface" shaders (byte-identical code): BCLR x AO x (1 - metal), no exposure factor
// (mode bit 131072: the runtime exposes them with the frame's, as the tonemap does).
// 7159ecc8, 6f1f0438 (CAdd), 2d32d109 (sky) and 1350d65d are the same formula (kb material/7159ecc8.md etc.).
constexpr uint32_t kShaderSurfaceUnlit[] = {0x67135A0B, 0x6FC4D540, 0x7159ECC8, 0x6F1F0438, 0x2D32D109, 0x1350D65D};
// The port kinds of unlit shaders with no exposure factor of their own, exposed by the frame as kShaderSurfaceUnlit
// (mode bit 131072): 38 (5854e91b, bda080c0).
constexpr bool BareUnlitKind(int kind) { return kind == 38; }

size_t LightmapSourceCoordinate(uint32_t family);
const char* KindName(int kind);
RemMaterial ReadMaterial(const ModelMaterial& mat, const ConvertOptions& opt);

bool BackLightScale(const RemMaterial& m);
int PbrMode(const RemMaterial& m);
// The blend factors PbrMaterial writes for rem over pm.
void PbrBlend(const RemMaterial& rem, const RetailMaterial& pm, uint32_t& src, uint32_t& dst);
Blob PbrMaterial(const RetailMaterial& pm, uint32_t vtx, const uint32_t* texIdx, uint32_t group,
                 const uint32_t* coords, const RemMaterial& rem, uint32_t wrap, uint32_t cube,
                 const uint32_t* authored, const AnuvEntry* anim, AnuvCounts& counts);

std::vector<WeightKey> SkinWeights(const std::vector<double>& P, size_t n, const std::vector<uint16_t>* J,
                                   const std::vector<float>* W, const Retail& retail, Span cskr,
                                   const std::function<void(const std::string&)>& log,
                                   const std::vector<std::array<double, 3>>* pivots = nullptr,
                                   const std::vector<ConvertOptions::RetailBone>* skeleton = nullptr,
                                   const std::vector<uint32_t>* tris = nullptr);
// Each vertex's weights cut to kMaxSkinWeights, quantised to 1/64 and renormalised (the votes' last step).
std::vector<WeightKey> QuantiseWeights(std::vector<std::vector<std::pair<uint32_t, double>>>& weights);
Blob CskrBytes(const std::vector<std::pair<const WeightKey*, uint32_t>>& runs, size_t total);
void SurfaceHeader(Blob& s, const double* centre, uint32_t mat, size_t dlSize, uint32_t extra);

}  // namespace ConvertDetail
}  // namespace PortRemastered
