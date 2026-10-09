// Remastered model -> GameCube CMDL/CSKR/TXTR. See port_remastered_convert.h.
//
// The arithmetic follows the reference converter this was ported from step for
// step (double precision geometry, the order sums are taken in, round half to
// even), so that both produce the same bytes for the same model and one can be
// checked against the other. Where a line looks needlessly particular about
// the order of an addition, that is why.

#include "port_env.h"
#include "port_remastered_convert.h"
#include "port_strings.h"
#include "port_remastered_anuv.h"

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
namespace {

struct Fail {
  std::string what;
};

constexpr uint32_t FourCC(char a, char b, char c, char d) {
  return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) | (uint32_t(uint8_t(c)) << 8) |
         uint32_t(uint8_t(d));
}

struct Span {
  const uint8_t* p = nullptr;
  size_t n = 0;
};

uint32_t R32(Span s, size_t o) {
  if (o > s.n || s.n - o < 4) {
    throw Fail{"a retail resource is truncated"};
  }
  return (uint32_t(s.p[o]) << 24) | (uint32_t(s.p[o + 1]) << 16) | (uint32_t(s.p[o + 2]) << 8) | s.p[o + 3];
}

uint16_t R16(Span s, size_t o) {
  if (o > s.n || s.n - o < 2) {
    throw Fail{"a retail resource is truncated"};
  }
  return uint16_t((s.p[o] << 8) | s.p[o + 1]);
}

uint8_t R8(Span s, size_t o) {
  if (o >= s.n) {
    throw Fail{"a retail resource is truncated"};
  }
  return s.p[o];
}

float RF(Span s, size_t o) {
  const uint32_t v = R32(s, o);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

Span Sub(Span s, size_t o, size_t n) {
  if (o > s.n || s.n - o < n) {
    throw Fail{"a retail resource is truncated"};
  }
  return Span{s.p + o, n};
}

// A slice the way the reference takes one: clamped to what is there.
Span Slice(Span s, size_t o, size_t n) {
  if (o > s.n) {
    o = s.n;
  }
  return Span{s.p + o, std::min(n, s.n - o)};
}

using Blob = std::vector<uint8_t>;

void P8(Blob& b, uint8_t v) { b.push_back(v); }
void P16(Blob& b, uint32_t v) {
  b.push_back(uint8_t(v >> 8));
  b.push_back(uint8_t(v));
}
void P32(Blob& b, uint32_t v) {
  b.push_back(uint8_t(v >> 24));
  b.push_back(uint8_t(v >> 16));
  b.push_back(uint8_t(v >> 8));
  b.push_back(uint8_t(v));
}
void PF(Blob& b, double v) {
  const float f = float(v);
  uint32_t u;
  std::memcpy(&u, &f, 4);
  P32(b, u);
}
void Pad(Blob& b, size_t a = 32) { b.resize((b.size() + a - 1) & ~(a - 1), 0); }
void Append(Blob& b, const Blob& o) { b.insert(b.end(), o.begin(), o.end()); }

size_t Align(size_t n, size_t a = 32) { return (n + a - 1) & ~(a - 1); }

using port::Hex8;

std::string FormatG(double v) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%g", v);
  return buf;
}

using port::Lower;

uint32_t Crc32(const std::string& s) {
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      }
      table[i] = c;
    }
    ready = true;
  }
  uint32_t c = 0xFFFFFFFFu;
  for (unsigned char ch : s) {
    c = table[(c ^ ch) & 0xFF] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

// A material parameter as the reference reads it: the float's shortest decimal
// form, read back as a double (it parses them out of a text table).
double ShortestDouble(float f) {
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
double RowSum(const double* a, size_t n) {
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
void Nearest(const double* a, size_t na, const double* b, size_t nb, std::vector<uint32_t>& out) {
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

RetailMaterial ParseMaterial(Span m) {
  RetailMaterial out;
  size_t o = 0;
  out.flags = R32(m, o);
  o += 4;
  const uint32_t nt = R32(m, o);
  o += 4;
  for (uint32_t i = 0; i < nt; ++i, o += 4) {
    out.tex.push_back(R32(m, o));
  }
  out.vtx = R32(m, o);
  o += 4;
  out.group = R32(m, o);
  o += 4;
  if (out.flags & 0x8) {
    const uint32_t nk = R32(m, o);
    if (nk != 0) {
      out.konstAlpha = double(R8(m, o + 4 + 3)) / 255.0;
    }
    o += 4 + size_t(nk) * 4;
  }
  out.blendDst = R16(m, o);
  out.blendSrc = R16(m, o + 2);
  o += 4;
  if (out.flags & 0x400) {
    o += 4;
  }
  const uint32_t nch = R32(m, o);
  o += 4;
  for (uint32_t i = 0; i < nch; ++i, o += 4) {
    out.chans.push_back(R32(m, o));
  }
  const uint32_t ntev = R32(m, o);
  o += 4;
  for (uint32_t i = 0; i < ntev; ++i, o += 20) {
    out.tev.push_back({R32(m, o), R32(m, o + 4), R32(m, o + 8), R32(m, o + 12)});
  }
  for (uint32_t i = 0; i < ntev; ++i, o += 4) {
    out.tevTex.push_back({R8(m, o), R8(m, o + 1), R8(m, o + 2), R8(m, o + 3)});
  }
  const uint32_t ntg = R32(m, o);
  o += 4;
  for (uint32_t i = 0; i < ntg; ++i, o += 4) {
    out.texgen.push_back(R32(m, o));
  }
  // The UV animations load the matrices those texgens name: without them a texgen
  // reads whatever matrix the material drawn before left.
  if (o <= m.n && m.n - o >= 8) {
    const Span anim = Slice(m, o, 4 + size_t(R32(m, o)));
    if (anim.n >= 8 && anim.n == 4 + size_t(R32(m, o))) {
      out.uvAnim.assign(anim.p, anim.p + anim.n);
    }
  }
  return out;
}

struct MaterialSet {
  std::vector<uint32_t> tex;
  std::vector<Span> mats;
};

MaterialSet ParseSet(Span sec) {
  MaterialSet out;
  const uint32_t nt = R32(sec, 0);
  size_t o = 4;
  for (uint32_t i = 0; i < nt; ++i, o += 4) {
    out.tex.push_back(R32(sec, o));
  }
  const uint32_t nm = R32(sec, o);
  o += 4;
  const size_t base = o + size_t(nm) * 4;
  uint32_t start = 0;
  for (uint32_t i = 0; i < nm; ++i, o += 4) {
    const uint32_t end = R32(sec, o);
    if (end < start) {
      throw Fail{"a retail material table is out of order"};
    }
    out.mats.push_back(Sub(sec, base + start, end - start));
    start = end;
  }
  return out;
}

// Bytes per vertex in a display list: matrix indices are u8, attributes u16.
size_t VtxStride(uint32_t vtx) {
  size_t n = 0;
  for (int bit = 24; bit < 32; ++bit) {
    if (vtx & (1u << bit)) {
      ++n;
    }
  }
  for (int sh = 0; sh < 24; sh += 2) {
    if ((vtx >> sh) & 3) {
      n += 2;
    }
  }
  return n;
}

int Popcount(uint32_t v) {
  int n = 0;
  for (; v; v &= v - 1) {
    ++n;
  }
  return n;
}

// Position indices of a display list, as a flat triangle list.
void Triangles(Span dl, size_t stride, size_t mtx, std::vector<uint32_t>& tris) {
  size_t o = 0;
  std::vector<uint32_t> idx;
  while (o + 3 <= dl.n) {
    const uint8_t op = dl.p[o] & 0xF8;
    if (op != 0x80 && op != 0x90 && op != 0x98 && op != 0xA0 && op != 0xA8 && op != 0xB0 && op != 0xB8) {
      break;
    }
    const size_t n = R16(dl, o + 1);
    o += 3;
    if (n * stride > dl.n - o || (n && stride < mtx + 2)) {
      break;
    }
    idx.resize(n);
    for (size_t k = 0; k < n; ++k) {
      idx[k] = R16(dl, o + k * stride + mtx);
    }
    o += n * stride;
    if (op == 0x90) {
      tris.insert(tris.end(), idx.begin(), idx.begin() + n / 3 * 3);
    } else if (op == 0x98) {
      for (size_t k = 0; k + 2 < n; ++k) {
        if (k % 2 == 0) {
          tris.insert(tris.end(), {idx[k], idx[k + 1], idx[k + 2]});
        } else {
          tris.insert(tris.end(), {idx[k + 1], idx[k], idx[k + 2]});
        }
      }
    } else if (op == 0xA0) {
      for (size_t k = 1; k + 1 < n; ++k) {
        tris.insert(tris.end(), {idx[0], idx[k], idx[k + 1]});
      }
    } else if (op == 0x80) {
      for (size_t k = 0; k + 3 < n; k += 4) {
        tris.insert(tris.end(), {idx[k], idx[k + 1], idx[k + 2], idx[k], idx[k + 2], idx[k + 3]});
      }
    }
  }
}

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
bool GunFxParticle(const RetailMaterial& m) { return m.blendSrc == 4 && m.blendDst == 5 && !(m.flags & 0x400); }

bool IsFx(const RetailMaterial& m) { return !(m.blendSrc == 1 && m.blendDst == 0); }

enum class Role { EnvMap, Lightmap, Reflect, Emissive, Diffuse, Keep };

const char* RoleName(Role r) {
  switch (r) {
  case Role::EnvMap:
    return "envmap";
  case Role::Lightmap:
    return "lightmap";
  case Role::Reflect:
    return "reflect";
  case Role::Emissive:
    return "emissive";
  case Role::Diffuse:
    return "diffuse";
  default:
    return "keep";
  }
}

struct SlotRole {
  uint32_t slot;
  uint32_t coord;
  Role role;
  int uvSrc;  // which texcoord attribute the stage's texgen reads, negative for none
};

// What each textured TEV stage of a retail material does with its texture.
std::vector<SlotRole> SlotRoles(const RetailMaterial& m) {
  // TEV colour inputs: CPREV APREV C0 A0 C1 A1 C2 A2 TEXC TEXA RASC RASA ONE HALF KONST ZERO
  enum { CPREV = 0, TEXC = 8, RASC = 10, ONE = 12 };
  std::vector<SlotRole> out;
  const size_t n = std::min(m.tev.size(), m.tevTex.size());
  for (size_t i = 0; i < n; ++i) {
    const uint32_t slot = m.tevTex[i][2], coord = m.tevTex[i][3];
    if (slot == 0xFF || coord >= m.texgen.size()) {
      continue;
    }
    const uint32_t tg = m.texgen[coord];
    const uint32_t src = (tg >> 4) & 31;
    const uint32_t c = m.tev[i].color;
    const uint32_t b = (c >> 5) & 15, cc = (c >> 10) & 15, d = (c >> 15) & 15;
    const uint32_t outReg = (m.tev[i].colorOp >> 9) & 3;
    Role role;
    if (src < 4) {
      role = Role::EnvMap;  // a sphere-mapped texgen over position/normal
    } else if (outReg == 1) {
      role = Role::Lightmap;
    } else if (outReg == 3) {
      role = Role::Reflect;
    } else if ((b == CPREV || d == CPREV) && (b == TEXC || d == TEXC) && cc == ONE) {
      role = Role::Emissive;
    } else if (d == TEXC && b == ONE && cc == RASC) {
      role = Role::Emissive;
    } else {
      role = Role::Diffuse;
    }
    out.push_back({slot, coord, role, int(src) - 4});
  }
  return out;
}

// Retail CSKR: weight groups (bone, weight)... and how many vertices each covers.
struct SkinGroup {
  std::vector<std::pair<uint32_t, float>> weights;
  uint32_t count = 0;
};

std::vector<SkinGroup> ReadCskr(Span d) {
  std::vector<SkinGroup> groups;
  const uint32_t n = R32(d, 0);
  size_t o = 4;
  for (uint32_t i = 0; i < n; ++i) {
    SkinGroup g;
    const uint32_t nw = R32(d, o);
    o += 4;
    for (uint32_t k = 0; k < nw; ++k, o += 8) {
      g.weights.emplace_back(R32(d, o), RF(d, o));
      g.weights.back().second = RF(d, o + 4);
    }
    g.count = R32(d, o);
    o += 4;
    groups.push_back(std::move(g));
  }
  return groups;
}

std::vector<uint32_t> SkinBones(Span d) {
  std::vector<uint32_t> bones;
  for (const SkinGroup& g : ReadCskr(d)) {
    for (const auto& w : g.weights) {
      bones.push_back(w.first);
    }
  }
  std::sort(bones.begin(), bones.end());
  bones.erase(std::unique(bones.begin(), bones.end()), bones.end());
  return bones;
}

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
// PortSetPBRMaterial). kPbrEmissive, 0.10, is what stood in for that exposure before, and
// is the factor the runtime falls back on outside a Remastered room. Here it only sizes a
// ramp's mean, which is not an authored map.
const double kPbrEmissive = 0.10;
// Ceiling on the metalness channel. Remastered applies none (the MR map is read as is);
// it was 0.6 before the BRDF LUT and the room probe cubes, when full metals went near-black.
const double kPbrMetalMax = 1.0;
const double kFlatStd = 3.0;
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
  bool mean = false;  // drawn as its colour times alpha, averaged: one colour
  bool solid = false;  // a flat map that is the surface's colour all the same (the Eyon gloss's black BCLR)
  bool detail = false;  // kind 20's detail map, which rides in the emissive slot (that shader has no glow)
  bool volume = false;  // a 3D texture, kept whole as an 8 x 8 atlas of its 64 slices (ConvertIO::volume), RGBA8, level 0 only
  double metalMax = kPbrMetalMax;  // an MR map's metalness ceiling
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
  bool opaqueFx = false;   // a retail fx material Remastered draws opaque (mesh class 0)
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
};

using WeightKey = std::vector<std::pair<uint32_t, double>>;

bool ContainsNoCase(const std::string& lower, const char* needle) { return lower.find(needle) != std::string::npos; }

// Names of effect materials: glow shells, pickups' halos.
bool IsFxName(const std::string& name) {
  const std::string l = Lower(name);
  return ContainsNoCase(l, "pickup") || ContainsNoCase(l, "vfx") || ContainsNoCase(l, "glow") ||
         ContainsNoCase(l, "additive") || ContainsNoCase(l, "_fx") || ContainsNoCase(l, "fx_");
}

int NextPow2(int v) {
  int p = 1;
  while (p < std::max(1, v)) {
    p <<= 1;
  }
  return p;
}

} // namespace

// ---- textures ----

struct Converter::State {
  ConvertIO io;
  std::map<std::string, std::optional<uint32_t>> ids;  // tag -> texture id, or none where retail's stays
  std::map<uint32_t, std::string> owner;               // texture id -> the tag it was written for
  // The last few textures decoded: a map that was measured is usually written
  // straight after, and a bake reads its neighbours.
  std::vector<std::pair<std::string, Image>> opened;
  int pbr = 0, tev = 0;
  uint32_t model = 0;  // the retail id being converted, for the joint log

  void Log(const std::string& line) const {
    if (io.log) {
      io.log(line);
    }
  }

  const Image& Open(const MapRef& map) {
    for (auto& e : opened) {
      if (e.first == map.src) {
        return e.second;
      }
    }
    Image img;
    std::string error;
    if (!(map.volume ? io.volume && io.volume(map.id, img, error) : io.texture(map.id, img, error))) {
      throw Fail{"texture " + IdToString(map.id) + ": " + error};
    }
    if (img.width <= 0 || img.height <= 0 || img.rgba.size() != size_t(img.width) * size_t(img.height) * 4) {
      throw Fail{"texture " + IdToString(map.id) + " decoded to nothing"};
    }
    if (opened.size() >= 3) {
      opened.erase(opened.begin());
    }
    opened.emplace_back(map.src, std::move(img));
    return opened.back().second;
  }

  // What an earlier import measured of a texture (ConvertIO::recall), else measure() now: a
  // texture whose outputs are reused is then not decoded just to name them.
  template <class F>
  std::string Fact(const std::string& key, F&& measure) {
    std::string value;
    if (io.recall && io.recall(key, value)) {
      return value;
    }
    value = measure();
    if (io.remember) {
      io.remember(key, value, {});
    }
    return value;
  }

  // Whether a map is more than a placeholder texel (over 4 px either way).
  bool Real(const MapRef& map) {
    int w = 0, h = 0;
    const std::string text = Fact("size:" + map.src, [&] {
      const Image& img = Open(map);
      return std::to_string(img.width) + " " + std::to_string(img.height);
    });
    if (std::sscanf(text.c_str(), "%d %d", &w, &h) != 2) {
      const Image& img = Open(map);
      w = img.width;
      h = img.height;
    }
    return w > 4 || h > 4;
  }

  // A flat fill has no surface detail to convert: Remastered keeps such a
  // surface's pattern in its shader.
  static bool FlatBase(const Image& img) {
    if (std::max(img.width, img.height) <= 4) {
      return true;
    }
    const size_t count = img.rgba.size() / 4;
    uint64_t sum = 0, squares = 0;
    for (size_t i = 0; i < count; ++i) {
      for (int c = 0; c < 3; ++c) {
        const uint64_t v = img.rgba[i * 4 + c];
        sum += v;
        squares += v * v;
      }
    }
    const long double n = (long double)(count * 3);
    const long double mean = (long double)sum / n;
    const long double var = (long double)squares / n - mean * mean;
    return std::sqrt(double(var > 0 ? var : 0)) < kFlatStd;
  }

  uint32_t TexId(const std::string& tag) const {
    uint32_t h = Crc32("mprem:" + tag);
    while (io.retailId && io.retailId(h)) {
      ++h;
    }
    return h;
  }

  // How much alpha the retail texture in a slot carries, so the replacement
  // keeps as much: "none", "punch" (CMPR with a transparent texel) or "full".
  std::string RetailAlpha(uint32_t id) const {
    Blob d;
    if (!io.retail(FourCC('T', 'X', 'T', 'R'), id, d) || d.size() < 12) {
      return "none";
    }
    const Span s{d.data(), d.size()};
    const uint32_t fmt = R32(s, 0);
    const uint32_t w = R16(s, 4), h = R16(s, 6);
    if (fmt == 0 || fmt == 1 || fmt == 7) {
      return "none";
    }
    if (fmt != 10) {
      return "full";
    }
    const size_t bytes = std::min(size_t(std::max(8u, w)) * std::max(8u, h) / 2, d.size() - 12) / 8 * 8;
    for (size_t o = 12; o < 12 + bytes; o += 8) {
      const uint16_t c0 = R16(s, o), c1 = R16(s, o + 2);
      if (c0 > c1) {
        continue;
      }
      for (int k = 0; k < 4; ++k) {
        const uint8_t b = d[o + 4 + k];
        if ((b & 0xC0) == 0xC0 || (b & 0x30) == 0x30 || (b & 0x0C) == 0x0C || (b & 0x03) == 0x03) {
          return "punch";
        }
      }
    }
    return "none";
  }

  void Write(const std::string& name, const Blob& data) const {
    if (!io.write(name, data)) {
      throw Fail{"could not write " + name};
    }
    if (written != nullptr) {
      written->push_back(name);
    }
  }

  // The files a texture's conversion writes, for ConvertIO::remember.
  std::vector<std::string>* written = nullptr;
  struct Writing {
    State& c;
    std::vector<std::string> files;
    explicit Writing(State& owner) : c(owner) { c.written = &files; }
    ~Writing() { c.written = nullptr; }
    Writing(const Writing&) = delete;
    Writing& operator=(const Writing&) = delete;
  };

  // Puts the files an earlier import wrote for `key` in the folder, if its value is `value`.
  bool Reuse(const std::string& key, const std::string& value) {
    std::string was;
    return io.recall && io.relink && io.recall(key, was) && was == value && io.relink(key);
  }

  void Remember(const std::string& key, const std::string& value, const std::vector<std::string>& files) {
    if (io.remember) {
      io.remember(key, value, files);
    }
  }

  static Image Solid(uint8_t r, uint8_t g, uint8_t b) {
    Image img;
    img.width = img.height = 8;
    img.rgba.resize(8 * 8 * 4);
    for (size_t i = 0; i < 64; ++i) {
      img.rgba[i * 4] = r;
      img.rgba[i * 4 + 1] = g;
      img.rgba[i * 4 + 2] = b;
      img.rgba[i * 4 + 3] = 255;
    }
    return img;
  }

  // A material's own reflection cube (RemMaterial::refl), written once as <ID>.envcube:
  // 'MPCB', u32 edge, u32 mips, then RGBA16F, every mip of face 0 from the largest down,
  // then face 1 and so on (GXCreatePBRCube's layout), little endian. The faces are
  // Remastered's, in its world (PortRoomEnv's probes map a direction into it the same way).
  // The colours are what the GPU samples (io.cube decodes by format: most REFL cubes are
  // UNORM ASTC and stay as stored, BC6H keeps its range), a box filter down to kCubeEdge
  // and on to 1x1. 0 when the material has none or the import cannot read cubes.
  static constexpr uint32_t kCubeEdge = 128;
  uint32_t Cube(const MapRef& refl) {
    if (!refl.has || !io.cube) {
      return 0;
    }
    const std::string tag = "cube:" + refl.src;
    const auto known = ids.find(tag);
    if (known != ids.end()) {
      return known->second.value_or(0);
    }
    const uint32_t tid = TexId(tag);
    const std::string key = "tex:" + tag;
    std::string was;
    const bool kept = io.recall && io.recall(key, was) && was == Hex8(tid);
    uint32_t edge = 0;
    std::vector<float> faces;
    const auto read = [&] {
      std::string error;
      if (!io.cube(refl.id, edge, faces, error) || edge == 0 || faces.size() != size_t(edge) * edge * 24) {
        Log("  note: cube " + IdToString(refl.id) + " not read" + (error.empty() ? "" : ": " + error));
        ids[tag] = std::nullopt;
        return false;
      }
      return true;
    };
    if (!kept && !read()) {
      return 0;
    }
    const auto owned = owner.find(tid);
    if (owned != owner.end() && owned->second != tag) {
      throw Fail{"texture id clash on " + Hex8(tid) + ": " + owned->second + " and " + tag};
    }
    owner[tid] = tag;
    ids[tag] = tid;
    if (io.claim && !io.claim(tid)) {
      return tid;
    }
    if (kept && (Reuse(key, Hex8(tid)) || !read())) {
      return ids[tag].value_or(0);
    }
    Writing writing(*this);
    const uint32_t top = std::min(edge, kCubeEdge);
    uint32_t mips = 0;
    while ((top >> mips) != 0) {
      ++mips;
    }
    Blob out;
    const auto le32 = [&](uint32_t v) {
      for (int i = 0; i < 4; ++i) {
        out.push_back(uint8_t(v >> (8 * i)));
      }
    };
    out.insert(out.end(), {'M', 'P', 'C', 'B'});
    le32(top);
    le32(mips);
    for (int face = 0; face < 6; ++face) {
      const float* src = faces.data() + size_t(face) * edge * edge * 4;
      // The top mip: each texel the mean of the block of source texels it covers.
      const uint32_t step = edge / top;
      std::vector<float> level(size_t(top) * top * 4);
      for (uint32_t y = 0; y < top; ++y) {
        for (uint32_t x = 0; x < top; ++x) {
          float sum[3] = {0.0f, 0.0f, 0.0f};
          for (uint32_t sy = 0; sy < step; ++sy) {
            for (uint32_t sx = 0; sx < step; ++sx) {
              const float* p = src + (size_t(y * step + sy) * edge + x * step + sx) * 4;
              for (int c = 0; c < 3; ++c) {
                sum[c] += p[c];
              }
            }
          }
          float* d = level.data() + (size_t(y) * top + x) * 4;
          for (int c = 0; c < 3; ++c) {
            d[c] = sum[c] / float(step * step);
          }
          d[3] = 1.0f;
        }
      }
      for (uint32_t mip = 0; mip < mips; ++mip) {
        const uint32_t e = top >> mip;
        for (float v : level) {
          const uint16_t h = HalfBits(v);
          out.push_back(uint8_t(h));
          out.push_back(uint8_t(h >> 8));
        }
        if (e > 1) {
          std::vector<float> next(size_t(e / 2) * (e / 2) * 4);
          for (uint32_t y = 0; y < e / 2; ++y) {
            for (uint32_t x = 0; x < e / 2; ++x) {
              for (int c = 0; c < 4; ++c) {
                const auto at = [&](uint32_t ix, uint32_t iy) { return level[(size_t(iy) * e + ix) * 4 + c]; };
                next[(size_t(y) * (e / 2) + x) * 4 + c] =
                    0.25f * (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1));
              }
            }
          }
          level.swap(next);
        }
      }
    }
    Write(Hex8(tid) + ".envcube", out);
    Remember(key, Hex8(tid), writing.files);
    return tid;
  }

  // A non-negative float as half bits, rounded to nearest and capped at the largest half.
  static uint16_t HalfBits(float v) {
    if (!(v > 0.0f)) {
      return 0;
    }
    if (v >= 65504.0f) {
      return 0x7BFF;
    }
    int e = 0;
    const float m = std::frexp(v, &e);  // v = m * 2^e, m in [0.5, 1)
    if (e < -13) {
      return uint16_t(std::lround(std::ldexp(v, 24)));  // subnormal (rounds up into the normals)
    }
    uint32_t bits = uint32_t(e + 14) << 10;
    const long frac = std::lround((m * 2.0f - 1.0f) * 1024.0f);
    return uint16_t(std::min<uint32_t>(bits + uint32_t(frac), 0x7BFF));
  }

  struct Bake {
    bool on = false;
    const MapRef* mr = nullptr;
    const MapRef* normal = nullptr;
    double ao = 0.6, cavity = 0.5, spec = 0.35;
  };

  // Remastered's shading folded into the base colour, texel-local, for the TEV
  // path: occlusion (MR red), cavities (normal-map tilt) and a sheen on smooth
  // metal (MR blue metal, green roughness).
  Image Baked(const Image& base, const Bake& bake) {
    const size_t count = size_t(base.width) * size_t(base.height);
    std::vector<float> shade(count, 1.0f), lift(count, 0.0f);
    auto load = [&](const MapRef& map, MapKind kind) {
      const Image& img = Open(map);
      return img.width == base.width && img.height == base.height ? img : Resize(img, base.width, base.height, kind);
    };
    if (bake.mr) {
      const Image t = load(*bake.mr, MapKind::Data);
      for (size_t i = 0; i < count; ++i) {
        const float r = t.rgba[i * 4] / 255.0f, g = t.rgba[i * 4 + 1] / 255.0f, b = t.rgba[i * 4 + 2] / 255.0f;
        shade[i] *= 1.0f - float(bake.ao) * (1.0f - r);
        lift[i] = float(bake.spec) * b * (1.0f - g) * (1.0f - g);
      }
    }
    if (bake.normal) {
      const Image t = load(*bake.normal, MapKind::Normal);
      for (size_t i = 0; i < count; ++i) {
        const float nx = t.rgba[i * 4] / 255.0f * 2.0f - 1.0f, ny = t.rgba[i * 4 + 1] / 255.0f * 2.0f - 1.0f;
        const float nz = std::sqrt(std::clamp(1.0f - nx * nx - ny * ny, 0.0f, 1.0f));
        shade[i] *= 1.0f - float(bake.cavity) * (1.0f - nz);
      }
    }
    Image out = base;
    for (size_t i = 0; i < count; ++i) {
      for (int c = 0; c < 3; ++c) {
        float v = base.rgba[i * 4 + c] / 255.0f;
        v *= shade[i];
        v += (1.0f - v) * lift[i] * shade[i];  // screen-style, stays under white
        out.rgba[i * 4 + c] = uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
      }
    }
    return out;
  }

  // The texture id for a role of a Remastered material, converting and writing
  // the texture the first time its tag is seen. `role` is "pbr:<map>" or a TEV
  // slot role; `alpha` is RetailAlpha() of the texture a TEV slot replaces.
  // No value means the retail texture stays.
  std::optional<uint32_t> Get(const std::string& role, const MapRef* rt, std::string alpha,
                              const ConvertOptions& opt) {
    const MapRef* src = nullptr;
    std::string tag;
    int ncap = 0;  // largest edge of the native .dds, 0 for none
    int k = -1;    // the PBR map, on that path
    bool surface = true;  // the PBR base's normal map has a surface to it
    // A PBR colour texture (base, glow, a shader's ramp) follows its source format, as
    // Remastered's GPU does: an sRGB one is filtered in linear light and written sRGB, so
    // the GPU decodes it; a UNORM one is data, filtered and sampled raw. MR and normal
    // maps are never decoded.
    bool colourTex = false, srgbTex = false;
    Bake bake;
    if (role.rfind("pbr:", 0) == 0) {
      for (int i = 0; i < kMaps; ++i) {
        if (role.compare(4, std::string::npos, kMapName[i]) == 0) {
          k = i;
        }
      }
      if (rt[k].has) {
        src = &rt[k];
        tag = std::string("pbr:") + kMapName[k] + ":" + src->src + (src->raw ? ":raw" : src->mean ? ":mean" : "") +
              (src->volume ? ":vol" : "");
        colourTex = k != kNormal && (k != kMr || src->raw);
        if (colourTex || k == kNormal) {
          srgbTex = Fact("srgb:" + src->src, [&] { return std::string(Open(*src).srgb ? "1" : "0"); }) == "1";
          tag += srgbTex ? ":srgb" : ":unorm";
        }
        // Named fields, not positional: the size and the alpha follow them.
        if (k == kMr && !src->raw) {
          tag += ":mmax=" + FormatG(src->metalMax);
        }
        // Only a retail model has a retail texture to fall back on.
        if (k == kBase && !opt.standalone) {
          // A map that is more than a placeholder texel (not a mean, over 4 px).
          auto real = [&](int m) { return rt[m].has && !rt[m].mean && Real(rt[m]); };
          // The normal map is the surface. Failing that, the material's own MR or
          // glow map still makes Remastered's draw its own (a flat suit light, the
          // eye's shadow): a solid base under them is what it draws. Nothing but
          // placeholders (the Eyon's) gives Remastered nothing to draw.
          surface = real(kNormal) || real(kMr) || (rt[kEmissive].has && real(kEmissive)) || real(kBase) || rt[kBase].solid;
          tag += surface ? "" : ":bare";
        }
      } else {
        tag = std::string("const:") + kMapNeutral[k];
      }
    } else if (role == "diffuse" && rt[kBase].has) {
      src = &rt[kBase];
      tag = "base:" + src->src;
      // Only maps sharing the base's texcoord can be baked texel for texel.
      const uint32_t uv = rt[kBase].coord;
      bake.mr = rt[kMr].has && rt[kMr].coord == uv ? &rt[kMr] : nullptr;
      bake.normal = rt[kNormal].has && rt[kNormal].coord == uv ? &rt[kNormal] : nullptr;
      if (!bake.mr) {
        bake.ao = bake.spec = 0.0;
      }
      if (!bake.normal) {
        bake.cavity = 0.0;
      }
      bake.on = bake.ao != 0.0 || bake.cavity != 0.0 || bake.spec != 0.0;
      if (bake.on) {
        tag += ":bake:" + (bake.mr ? bake.mr->src : std::string("-")) + ":" +
               (bake.normal ? bake.normal->src : std::string("-")) + ":" + FormatG(bake.ao) + ":" +
               FormatG(bake.cavity) + ":" + FormatG(bake.spec) + ":0:0";
      }
    } else if (role == "emissive" && rt[kEmissive].has) {
      src = &rt[kEmissive];
      tag = "emis:" + src->src + ":1";
    } else if (role == "reflect" && rt[kMr].has) {
      src = &rt[kMr];
      tag = "refl:" + src->src + ":0.6";
      // A stage writing C2 is not always a reflectivity mask: the Metroid's
      // dome multiplies its colour by it. A map with no metal anywhere would
      // turn black and blank the stage, so the retail texture stays.
      const std::string any = Fact("metal:" + src->src, [&] {
        const Image& mr = Open(*src);
        for (size_t i = 0; i + 3 < mr.rgba.size(); i += 4) {
          const float g = mr.rgba[i + 1] / 255.0f, b = mr.rgba[i + 2] / 255.0f;
          if (uint8_t(std::clamp(b * (1.0f - 0.7f * g) * 0.6f, 0.0f, 1.0f) * 255.0f) != 0) {
            return std::string("1");
          }
        }
        return std::string("0");
      });
      if (any != "1") {
        return std::nullopt;
      }
    } else if (role == "envmap") {
      // A sphere-mapped reflection: its texture is a gradient, not surface
      // detail, so there is nothing to convert from Remastered.
      return std::nullopt;
    } else if (role == "lightmap") {
      tag = "const:c0c0c0";
    } else {
      tag = "const:000000";
    }
    if (role == "reflect" && alpha == "punch") {
      alpha = "none";  // the reflectivity map's alpha is 0 by construction
    }
    const bool raw = src && src->raw;
    int cap;
    if (k >= 0) {
      // The albedo is what shows and can be CMPR; AO/roughness/metal would
      // blotch and the normals would tilt, so the normal map stays RGBA8.
      // A cutout's base is the exception: its alpha is the shape.
      // A blended surface's base keeps its whole alpha, which is its opacity.
      alpha = k == kNormal || raw ? "rgba"
              : k == kBase && (alpha == "punch" || alpha == "cut" || alpha == "blend" || alpha == "mask" || alpha == "mask2") ? alpha
                                                                                                             : "none";
      cap = kPbrMax[k];
      if (src) {
        ncap = opt.nativeMax > 0 ? std::min(kPbrNative[k], opt.nativeMax) : kPbrNative[k];
        cap = std::min(cap, kPbrStub[k]);
        if (src->volume) {
          ncap = 0;  // every texel exact: the atlas is the TXTR, never a .dds
          cap = 512;
        }
      }
    } else if (alpha == "full") {
      cap = 1024;
    } else {
      cap = opt.maxTexture;
    }
    tag += ":" + std::to_string(cap);
    if (ncap) {
      tag += ":dds" + std::to_string(ncap);
    }
    if (src && alpha != "rgba") {
      tag += ":" + alpha;
    }
    const auto known = ids.find(tag);
    if (known != ids.end()) {
      return known->second;
    }
    const uint32_t tid = TexId(tag);
    const auto owned = owner.find(tid);
    if (owned != owner.end() && owned->second != tag) {
      throw Fail{"texture id clash on " + Hex8(tid) + ": " + owned->second + " and " + tag};
    }
    owner[tid] = tag;
    ids[tag] = tid;
    const std::string name = Hex8(tid);
    if (!src) {
      const unsigned long rgb = std::strtoul(tag.substr(6, 6).c_str(), nullptr, 16);
      Write(name + ".TXTR", EncodeTxtrRgba8(Solid(uint8_t(rgb >> 16), uint8_t(rgb >> 8), uint8_t(rgb)), 8));
      return tid;
    }
    // Converted by an earlier import: neither decoded nor encoded again.
    const std::string key = "tex:" + tag;
    bool claimed = false;
    if (std::string was; io.recall && io.recall(key, was)) {
      if (was == "-") {
        ids[tag] = std::nullopt;
        return std::nullopt;
      }
      if (was == name) {
        if (k >= 0 && io.claim && !io.claim(tid)) {
          return tid;
        }
        claimed = k >= 0;
        if (Reuse(key, name)) {
          return tid;
        }
      }
    }
    Writing writing(*this);
    Image img = Open(*src);
    if (src->mean) {
      // An sRGB texture is averaged in linear light, then encoded again.
      double sum[3] = {0.0, 0.0, 0.0};
      const size_t n = img.rgba.size() / 4;
      for (size_t i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
          const double v = srgbTex ? double(SrgbByteToLinear(img.rgba[i * 4 + c])) : double(img.rgba[i * 4 + c]);
          sum[c] += v * double(img.rgba[i * 4 + 3]) / 255.0;
        }
      }
      uint8_t mean[3];
      for (int c = 0; c < 3; ++c) {
        const double m = sum[c] / double(n);
        mean[c] = srgbTex ? LinearToSrgbByteExact(float(m)) : uint8_t(std::nearbyint(m));
      }
      img = Solid(mean[0], mean[1], mean[2]);
    }
    const size_t count = img.rgba.size() / 4;
    const bool isBase = k == kBase || role == "diffuse";
    // How the image is filtered when it is resized and mipped: base, emissive
    // and the TEV colour slots are sRGB colour, the normal map a direction,
    // and the MR and reflect maps are plain data.
    const MapKind mapKind = k == kNormal                  ? MapKind::Normal
                            : k >= 0                          ? (srgbTex ? MapKind::Colour : MapKind::Data)
                            : role == "reflect"               ? MapKind::Data
                                                              : MapKind::Colour;
    if (k == kNormal) {
      // Two-channel normal maps leave B at 0; the shader rebuilds z. Keep A opaque.
      // A few are stored in an sRGB format (ASTC 8x5), which Remastered's sampler
      // decodes before the shader unpacks them; BC5 has no sRGB form, so decode here.
      for (size_t i = 0; i < count; ++i) {
        if (srgbTex) {
          for (int c = 0; c < 3; ++c) {
            img.rgba[i * 4 + c] = SrgbToLinearByte(img.rgba[i * 4 + c]);
          }
        }
        img.rgba[i * 4 + 3] = 255;
      }
      srgbTex = false;  // decoded: from here on it is data
    }
    if (alpha == "blend" || alpha == "mask2") {
      // Remastered's shader squares the base map's alpha into the opacity, or
      // into the glow mask ("mask2", the port's mask being the alpha as is).
      for (size_t i = 0; i < count; ++i) {
        const unsigned a = img.rgba[i * 4 + 3];
        img.rgba[i * 4 + 3] = uint8_t((a * a + 127) / 255);
      }
    }
    const bool keepAlpha = alpha == "punch" || alpha == "cut" || alpha == "blend" || alpha == "mask" || alpha == "mask2";
    if (isBase && !raw && !keepAlpha && FlatBase(img)) {
      // On the TEV path the retail texture is kept, which beats writing a flat
      // grey over a textured model. On the PBR path a flat albedo is fine: the
      // normal and MR maps carry the surface, so the material gets a solid base.
      // Unless the normal map is a placeholder texel too (the Eyon's 9E5BDD71,
      // every map 1x1): then Remastered gives nothing, and it drew a grey ball.
      if (k != kBase || !surface) {
        ids[tag] = std::nullopt;
        Remember(key, "-", {});
        return std::nullopt;
      }
      uint64_t sum[3] = {0, 0, 0};
      for (size_t i = 0; i < count; ++i) {
        for (int c = 0; c < 3; ++c) {
          sum[c] += img.rgba[i * 4 + c];
        }
      }
      uint8_t col[3];
      for (int c = 0; c < 3; ++c) {
        col[c] = uint8_t(std::nearbyint(double(sum[c]) / double(count)));
      }
      if (srgbTex) {
        // Average in linear light. The .dds holds the sRGB bytes (the GPU decodes
        // them); the TXTR stub, sampled raw, holds the linear ones.
        double lin[3] = {0.0, 0.0, 0.0};
        for (size_t i = 0; i < count; ++i) {
          for (int c = 0; c < 3; ++c) {
            lin[c] += double(SrgbByteToLinear(img.rgba[i * 4 + c]));
          }
        }
        for (int c = 0; c < 3; ++c) {
          col[c] = LinearToSrgbByteExact(float(lin[c] / double(count)));
        }
        Write(name + ".dds", EncodeDds(Solid(col[0], col[1], col[2]), ColourDdsFormat(), false, MapKind::Data, true));
        Write(name + ".TXTR",
              EncodeTxtrRgba8(Solid(SrgbToLinearByte(col[0]), SrgbToLinearByte(col[1]), SrgbToLinearByte(col[2])), 8));
      } else {
        Write(name + ".TXTR", EncodeTxtrRgba8(Solid(col[0], col[1], col[2]), 8));
      }
      Remember(key, name, writing.files);
      return tid;
    }
    // A PBR map's id is its tag's alone, so whoever wrote it wrote the same file.
    // Asked only now: up to here the tag alone decides whether there is a
    // texture at all (none, a solid, or a failed decode), so every converter
    // that gets this far would write one, and the costly part below runs once.
    if (k >= 0 && !claimed && io.claim && !io.claim(tid)) {
      return tid;
    }
    if (bake.on) {
      img = Baked(img, bake);
    }
    if (k == kMr && !raw) {
      const uint8_t ceiling = uint8_t(std::nearbyint(float(src->metalMax * 255.0)));
      for (size_t i = 0; i < count; ++i) {
        img.rgba[i * 4 + 2] = std::min(img.rgba[i * 4 + 2], ceiling);
      }
    }
    if (role == "reflect") {
      // glTF order: G roughness, B metal.
      for (size_t i = 0; i < count; ++i) {
        const float g = img.rgba[i * 4 + 1] / 255.0f, b = img.rgba[i * 4 + 2] / 255.0f;
        const uint8_t v = uint8_t(std::clamp(b * (1.0f - 0.7f * g) * 0.6f, 0.0f, 1.0f) * 255.0f);
        img.rgba[i * 4] = img.rgba[i * 4 + 1] = img.rgba[i * 4 + 2] = v;
        img.rgba[i * 4 + 3] = 0;
      }
    }
    if (ncap) {
      const int w = std::max(8, std::min(ncap, NextPow2(img.width)));
      const int h = std::max(8, std::min(ncap, NextPow2(img.height)));
      if (k == kBase && !raw && !keepAlpha) {
        for (size_t i = 0; i < count; ++i) {
          img.rgba[i * 4 + 3] = 255;  // alpha "none", as the stub's CMPR
        }
      }
      // otherwise the stub already holds every texel (an sRGB one is always drawn from its
      // .dds: the stub holds 8-bit linear, which is coarse in the darks)
      if (srgbTex || std::max(w, h) > cap) {
        const DdsFormat format = k == kNormal ? NormalDdsFormat() : ColourDdsFormat();
        const bool punch = alpha == "punch";
        if (w == img.width && h == img.height) {
          Write(name + ".dds", EncodeDds(img, format, punch, mapKind, srgbTex));
        } else {
          Write(name + ".dds", EncodeDds(Resize(img, w, h, mapKind), format, punch, mapKind, srgbTex));
        }
      }
    }
    const int w = std::max(8, std::min(cap, NextPow2(img.width)));
    const int h = std::max(8, std::min(cap, NextPow2(img.height)));
    if (w != img.width || h != img.height) {
      img = Resize(img, w, h, mapKind);
    }
    // The stub is sampled raw: an sRGB texture's holds linear bytes, mipped as data.
    MapKind stubKind = mapKind;
    if (srgbTex) {
      for (size_t i = 0; i < img.rgba.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
          img.rgba[i + c] = SrgbToLinearByte(img.rgba[i + c]);
        }
      }
      stubKind = MapKind::Data;
    }
    if (alpha == "none" || alpha == "punch") {
      Write(name + ".TXTR", EncodeTxtrCmpr(img, alpha == "punch", stubKind));
    } else {
      Write(name + ".TXTR", EncodeTxtrRgba8(img, role == "emissive" ? 4 : 8, stubKind));
    }
    Remember(key, name, writing.files);
    return tid;
  }

  void Convert(const Model& model, const ConvertOptions& opt);
};

namespace {

// Bits of a Remastered material's feature word.
constexpr uint32_t kTransparentFlag = 0x1;   // blended over what is behind it
constexpr uint32_t kVertexColorFlag = 0x10;  // the vertex colour tints it
constexpr uint32_t kCutoutFlag = 0x20;       // one-bit alpha: ground leaves, grates, foliage
constexpr uint32_t kIncanMaskFlag = 0x200;   // the base map's alpha masks the glow
// The first four bytes of the id of a shader whose alpha is read below.
constexpr uint32_t kShaderHeightBlend = 0xCA10C453;  // snow and ice over rock
// Shaders with maps and parameters of their own (TCHn, CCHn), read from their code.
constexpr uint32_t kShaderUpLayer = 0x9EFE0D2E;   // TCH0-2 are a second layer on what faces up, CCH0.x its edge
constexpr uint32_t kShaderProjBlend = 0xD6AA2A3A;    // 9EFE0D2E's blend of TCH0-2 on what faces up (CCH0.x its edge), alpha lerp(BCLR.a, TCH0.a, w)
constexpr uint32_t kShaderDecalBlend = 0x17E458CD;   // a blended decal whose alpha is a smoothstep of TCH0.x about CCH0.x
constexpr uint32_t kShaderVertexBlend = 0xE9DF2188;  // TCH0-2 a second layer by the vertex alpha, CCH0.x its edge, TCH3 a detail map
constexpr uint32_t kShaderDetail = 0x9AB899E7;    // TCH0 is a detail map, on a texcoord of its own
constexpr uint32_t kShaderDetailTinted = 0x41A12C9E;  // the same with the vertex colour tint
constexpr uint32_t kShaderLava = 0x023388CD;      // the glow is CCH0.x times the vertex alpha
// Falling water, unlit: TCH0's three channels are sheets scrolling at CCH1's and CCH2's speeds
// (times CCH0.x), weighted by the vertex colour, and their sum picks the colour from TCH1, a
// ramp whose row is the vertex alpha. CCH0's y, z and w are the sum's softness and the ramp's
// offsets, CCH3 the colour.
constexpr uint32_t kShaderWaterfall = 0x50412FE7;
// 963E469B folds to the same output as 50412FE7 (same samplers, same params; kb material/963e469b.md): kind 7 too.
constexpr uint32_t kShaderWaterfall2 = 0x963E469B;
constexpr bool IsWaterfall(uint32_t shader) { return shader == kShaderWaterfall || shader == kShaderWaterfall2; }
constexpr uint32_t kShaderParallax = 0x2F3FB02B;  // TCH0 is seen inside the surface; CCH0 and CCH1.x say how
// Glass: the frame behind it, offset by TCH1's RG noise times CCH0.x and tinted by
// CCH1 x CCH3 x CCH4.x x CCH4.z, where TCH0's R (and the vertex blue) lets it through;
// TCH0's G plus the vertex green glows in CCH2 x CCH4.y, its B is the roughness and its
// A (times the vertex alpha) the opacity of a reflection CCH0.y and CCH1.w scale.
constexpr uint32_t kShaderGlass = 0x231F8383;
// A lava pool's surface (a LavaRenderVolume's model): BCLR is a colour ramp, TCH0 a pattern
// carried along TCH2's flow map in two phases that TCH1's noise offsets, CCH0 the flow's
// strength, its period in seconds and the brightness, CCH1 the maps' scales.
constexpr uint32_t kShaderLavaPool[] = {0x3ADE58B7, 0xB9C24545, 0xBB4CD79B};
// The arm cannon's beam glow (Wave, Plasma), over a lit surface: TCH0's three
// channels scroll at CCH1's and CCH2.xy's speeds (times CCH0.y) and, less twice
// the vertex colour, plus CCH0.w, pick a colour from TCH1, a ramp whose row is
// the vertex alpha and whose alpha scales it, times CCH0.z.
constexpr uint32_t kShaderGunGlow[] = {0xA13D6235, 0x62F671E0};
// The Ice Beam cannon's frost shell (kind 12), a dissolve: CCH1.y (PEMI) is the
// charge (CGunWeaponMP1::UpdateChargeEffects drives it from 0 to 1 and shows the
// shell only while it is above 0), TCH1's noise against it decides what is drawn,
// and the edge glows CCH1.x x CCH2. Without its maps it is not drawn: as a plain
// surface it froze the gun for good.
constexpr uint32_t kShaderFrozenShell = 0x2FC554A2;
// Ice (Model_IceSpreader: the Ice charge's shards, frozen nozzles; PBR kind 28): one
// light slot over 1/pi, its own REFL cube (x CCH5.x, tinted CCH2) with a light-luma
// specular occlusion, TCH0 as a frost layer seen a little below the surface (parallax
// CCH0.w) times CCH1.x, the base alpha squared and the vertex mask, plus a CCH3 rim.
constexpr uint32_t kShaderIceSpreader = 0x088E025E;
// A Metroid's dome: a matcap shell (PBR kind 13). Its glTF calls it
// opaque, so the vote put it on an opaque retail material, a solid white blob;
// it keeps retail's blended dome instead.
constexpr uint32_t kShaderMatcapShell = 0xC83E6FCD;
// LayerBaseNormal_2TangentStream: each layer's normal is built in its own tangent frame,
// TANGENT_0 for the first and TANGENT_1 for the second. Their models carry a second frame
// (CMDL flag 0x10, 15-float NBT entries, DL opcode 0x94).
constexpr uint32_t kShaderTwoTangent[] = {0xA978D507, 0xD363D694, 0xA3C367BE, 0x72B34E42, 0xB9E899F3,
                                           0x7FEB9E94, 0xF84AC32D, 0x3C66AAEF};
// The macro-normal shaders (a3c367be, 72b34e42, b9e899f3, 7feb9e94 without vertex colour, f84ac32d
// with MNMP on texcoord set 1; same whiteout fold, kb material/7feb9e94.md): NMAP in TANGENT_0's frame, then MNMP
// (kept in layer[kNormal], map 6) added to it in TANGENT_1's frame (mode bit 16384).
// AreaLight unlit (69edcc3c, c2b36795; kb material/69edcc3c.md): DIFT x DIFC x vertex colour + ICNC + ICMC. Their DIFT
// (and ICAN) is the 1x1 white default a6cc3300, which is the whole base, not a missing one: the colour comes from the
// vertices and constants, so the flat base must not send the surface back to retail's TEV material.
constexpr uint32_t kShaderAreaLight[] = {0x69EDCC3C, 0xC2B36795};
// 3c66aaef is d37039f7's two layers (both in TANGENT_0's frame) plus MNMP (kb material/3c66aaef.md): a layered
// material whose macro normal is an 8th map.
constexpr uint32_t kShaderLayeredMacro = 0x3C66AAEF;
constexpr uint32_t kShaderMacroNormal[] = {0xA3C367BE, 0x72B34E42, 0xB9E899F3, 0x7FEB9E94, 0xF84AC32D};
// The shaders whose fragment code multiplies ICAN x ICNC x INCI by the global system
// values' inverse tonemap exposure (c4[0].z; USE_INVERSEEXPOSURE, MFC4, which no
// material sets as a bit): heads, eyes, suits, pirates, creatures, the Metroid's body
// (b6268b63). The glow shows at that strength whatever the room's exposure, and the
// maps are authored dim for it (build/mpr/mtrl-tr/{b6268b63,ae819893}/NOTES.md). Read
// from the code (`re.sh fgrep`, build/mpr/invexp/), not from the pack's strings: 15
// more packs carry the define and never compile it in. 3392509e, dd387a18 and e1979471
// (Jellyzap, Flaahgra's lower body) match too but use c4[0] as their TransmissionColor and
// draw no glow, so they are left out. Sorted for binary_search.
constexpr uint32_t kShaderInverseExposure[] = {
    0x0A714D54, 0x17DD0A37, 0x1E462D99, 0x2835AB3B, 0x2E7A70CC, 0x2F540B27, 0x31E53C70,
    0x46CB52A7, 0x47908924, 0x50DB912E, 0x5104B751, 0x53AD3B78, 0x7A8C93DB, 0x7DFCA4A1,
    0x86B48EF1, 0x8A2049D9, 0x8C121639, 0x94DC56A5, 0x99370071, 0x9C023C70, 0x9DB310D1,
    0x9FD5E413, 0xA6D80F61, 0xAB87F3D5, 0xAE819893, 0xB6268B63, 0xBEDB1948, 0xC371A140,
    0xC72CA0EC, 0xC80BC2C1, 0xCB9736B8, 0xCE685F8A, 0xD2D3ACCA, 0xD6E5D629, 0xE5CA8683,
    0xEC024BAA, 0xEE64B773, 0xEFF78D93, 0xF0302441, 0xF55B835A, 0xFB2DE3A0,
};
// Of those, the ones compiled with USE_DIFFUSE_AS_INCAN_MASK: the glow is masked by the
// base map's alpha squared (the pirate trooper's limbs, bedb1948), whatever the
// material's own 0x200 flag says, and the base alpha is no opacity. Sorted.
constexpr uint32_t kShaderIncanMaskSquared[] = {
    0x0A714D54, 0x31E53C70, 0x46CB52A7, 0x53AD3B78, 0x86B48EF1, 0x8A2049D9, 0x99370071,
    0x9DB310D1, 0xA6D80F61, 0xBEDB1948, 0xC80BC2C1, 0xD6E5D629, 0xE5CA8683, 0xF0302441,
    0xF55B835A,
};
// The arm cannon's body: lit PBR that reflects its REFL cube (a Tallon forest, LDR) along
// the reflection vector, at the roughness's mip, instead of a room probe. Drawn with the
// room's cube the metal took the room's colours and read pale. The default REFL (black)
// that most materials carry keeps the room's.
constexpr uint32_t kShaderGunBody[] = {0x547E64E5, 0xD43DE005, 0x917F1415, 0xF495B260};
// The beam panels inside the Wave, Plasma and Phazon guns (lightning, magma, phazon): a
// black base whose alpha masks an ICAN glow of ICNC 4-7. Only those three use the shader.
// Under kPbrEmissive the magma read as faint dark red.
constexpr uint32_t kShaderGunPanel = 0x9A4515CE;
constexpr const char* kDefaultRefl = "7b98170f";
// Lit glass that Remastered draws premultiplied (EBlendMode 5, One and InvSrcAlpha):
// phazon and plasma glass, soot_translucent. Its opacity scales the diffuse light
// only; the reflection and the glow show at full strength on the clearest pane.
constexpr uint32_t kShaderPremulGlass[] = {0x11B30369, 0x941068BF, 0xBCC73459};
// Of those, 941068bf and bcc73459 read the vertex colour raw (no log2/exp2 in their vertex shaders); 11b30369
// decodes it as 2 |c|^2.2 like every other MFVC shader (mode bit 524288, kb material/11b30369.md).
// 788360de, lit glass with coloured translucency (dual-source blend One / Src1Color, kb material/788360de.md).
constexpr uint32_t kShaderDualGlass = 0x788360DE;
constexpr uint32_t kShaderRawVertex[] = {0x941068BF, 0xBCC73459, kShaderDualGlass};
// Glass_DX11 (HoloGlass; only the Waste Disposal tank): the scene behind it, bent by TCH2
// and tinted, plus the reflection, BCLR and the vertex colour, blended with a second
// colour output (build/mpr/glass/NOTES.md). The port draws it from the screen copy.
constexpr uint32_t kShaderHoloGlass = 0x03407341;
// BoundaryShield_Ship1_DX11 (the Frigate's orange force fields; build/mpr/shield/NOTES.md): unlit,
// animated noise through two maps, the screen copy bent by it. PBR kind 14, with its CCH0..6 and DIFC
// carried in the record's PBR8 trailer.
constexpr uint32_t kShaderBoundaryShield = 0x6C1A4768;
// ForceField (6F53C775: the cyan Exterior Docking Hangar barrier 982D9D75, the Mines' orange fields):
// the same pixel and vertex code as 6C1A4768 (perms 002_0/002_1/004_0 fold identically), only a
// different shader id, so it takes the same kind.
constexpr uint32_t kShaderForceField = 0x6F53C775;
// PickUp (PickUp_Blue_Mat / PickUp_Orange_Mat, build/mpr/artifact/NOTES.md): unlit, a normal-map
// fresnel pair over a gradient that scrolls through world space. PBR kind 15, constants in the
// same PBR8 trailer (rows 0-3 CCH0..CCH3, row 7 DIFC, rows 4-5 filled at run time).
constexpr uint32_t kShaderPickUp = 0x3E95A9FE;
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
constexpr uint32_t kShaderIndirect = 0x8CE05ED0;
constexpr uint32_t kShaderGunFx = 0x98F0556D;
// dda64c97 (stored id 974ca6dd), the Eyon's eyeball_gloss: bfb300b6's actor lighting (normal map, GGX) with a
// matcap term, REFV x REFS(N'.xy) x luminance(L), in place of the reflection. Kind 25 (kb material/dda64c97.md).
constexpr uint32_t kShaderEyeGloss = 0x974CA6DD;
// 7cf91c66 (variasuit Eyeball_Exterior) is the same lighting and matcap. Its glow is DMGI(CCH2.y) x ICMC + ICNC (c4[0].y x
// c5[1] + c4[1]; c5[0] is DIFC), which is zero in all 11 materials whatever DMGI is: nothing to drive (kb material/7cf91c66.md).
constexpr uint32_t kShaderEyeGloss2 = 0x7CF91C66;
constexpr bool IsEyeGloss(uint32_t shader) { return shader == kShaderEyeGloss || shader == kShaderEyeGloss2; }
// Unlit, the vertex colour times the base map (a door shield's noise), which
// scrolls at (CCH0.y, -CCH0.z) a second over texcoords scaled by CCH1.yz. Its
// vertex shader linearises the colour and doubles it (2 pow(|c|, 2.2)), the base
// map is sRGB, and the pixel shader scales the product by CCH0.x x CCH1.x; the
// alpha is the base map's times the vertex's, not squared.
constexpr uint32_t kShaderColorUnlit = 0x992941B7;
// Phazon3 (cc96c27d, the Phazon Mines' stone): a lit surface whose albedo and glow are a gradient ramp (TCH0)
// read at the normal map's rim, where the vertex alpha and BCLR's alpha say, pulsed by a mask (TCH1) and the
// time (kind 21). The vertex colour is raw data (alpha and red weights, blue a cavity mask), no tint.
constexpr uint32_t kShaderPhazon = 0xCC96C27D;
// The same family's other member (9e52aa74, 0x100010 RLTG stone of the Phazon Mines and the crater's flesh;
// kind 22): no mask scale, the phase is sin(mask + time), a second ramp read gives the cavity glow's alpha term,
// and the glow takes no exposure factor.
constexpr uint32_t kShaderPhazonB = 0x9E52AA74;
// PhazonPool (07acff46, the Phazon Mines' blisters, FNLI/EMSI/FNLS; kind 32): a lit PBR surface whose normal
// strength and roughness follow an animated 3D noise (TCH0, a 64^3 volume) and whose glow is a fresnel-driven ramp
// (TCH1), see kb material/07acff46.md.
constexpr uint32_t kShaderPhazonPool = 0x07ACFF46;
// Distortion2 (24670bf0, the Phendrana ice walls, the crater's flesh glass; kind 23): refracts a mipped copy of
// the frame by its normal map and a fresnel term, tinted by the base map and the vertex colour (kb material/24670bf0.md).
constexpr uint32_t kShaderRefractGlass = 0x24670BF0;
// The same body from the baked-probe pack (cc8afd0f, 31 materials: Phendrana ice walls and waterfalls; kind 30):
// the scene term is v3.w x scene, without the base map and vertex colour tint (kb material/cc8afd0f.md).
constexpr uint32_t kShaderHoloGlassC = 0x2F95A061;  // HoloGlass's twin: the bent room tinted by CCH1, dual-source blended
constexpr uint32_t kShaderHoloGlassB = 0x3991DA00;  // HoloGlass: scrolling indirect layers over a distorted screen copy
constexpr uint32_t kShaderRefractGlassB = 0xCC8AFD0F;
// FB2BC671 and DF3E3423 (the ids as the reports print them), the ChozoGhost's X-ray materials, static and skinned
// (kind 31, kb material/fb2bc671.md, df3e3423.md).
constexpr uint32_t kShaderXrayStatic = 0xFB2BC671;
constexpr uint32_t kShaderXraySkinned = 0xDF3E3423;
// Shaders that are verbatim copies of one the port already handles: every permutation, vertex and fragment, has the
// same code (build/mpr/twins/twins.py, exact_twins; kb topic/shader-twins.md). They take the twin's kind and mode.
struct ShaderTwin {
  uint32_t copy;
  uint32_t of;
};
constexpr ShaderTwin kShaderTwins[] = {
    {0x88E65647, kShaderIndirect},  {0x1DD39C60, kShaderRefractGlassB}, {0x94438883, kShaderXrayStatic},
    {0x7126CA03, kShaderLambertFx}, {0x0A23705E, kShaderPickUp},        {0x24F4B1FE, kShaderPickUp},
    {0x9B6E0881, kShaderIceSpreader}, {0xCD6C8057, kShaderHoloGlassB},
    // The skinned variants: identical fragment shaders; the vertex shaders add the bone-matrix fetch (vp_c5, indices in
    // v5, weights in v6) and renormalise the skinned N and T (T1 too), nothing else. The port skins on its side
    // (DolphinCSkinRules.cpp, FinishTangentFrame, incl. the second frame).
    {0xC36C5166, kShaderIceSpreader}, {0xB55635D2, 0xA978D507}, {0xEF94ACCF, kShaderLava},
};
uint32_t ShaderFamily(uint32_t shader) {
  for (const ShaderTwin& t : kShaderTwins)
    if (t.copy == shader) return t.of;
  return shader;
}
// DFC2A7DC folds to exactly DF3E3423's perm 002_0 (same registers, same outputs; kb material/dfc2a7dc.md).
constexpr uint32_t kShaderXraySkinned2 = 0xDFC2A7DC;
constexpr bool IsXraySkinned(uint32_t shader) { return shader == kShaderXraySkinned || shader == kShaderXraySkinned2; }
constexpr uint32_t kShaderXrayMap = 0x65E90E82;
constexpr bool IsXrayGhost(uint32_t shader) { return shader == kShaderXrayStatic || IsXraySkinned(shader); }
// The decal cutouts (kind 24; kb material/e538b757.md, 42fe2ed0.md): a bfb300b6 surface cut by a smoothstep of an
// alpha A about CCH0.x = c (0.5 on all of them): discard where t²(3-2t) < 0.25, t = (A - 0.5 + c) / 2c; output
// alpha 1, no vertex colour. e538b757 cuts on BCLR.a², 42fe2ed0 on TCH0.r (its alpha map, in the emissive slot).
constexpr uint32_t kShaderDecalCut = 0xE538B757;
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
constexpr uint32_t kShaderSurfaceUnlit[] = {0x67135A0B, 0x6FC4D540};
// A shader with parameters of its own (TCHn, CCHn) reads the vertex colour as
// it likes: masks for its extra maps, a colour seen through ice. These are the
// ones read that multiply the albedo by it, as the standard shader does
// (992941B7 is ColorUnlit: a door shield's blue is all vertex colour).
constexpr uint32_t kShaderTints[] = {0x9EFE0D2E, 0xCA10C453, 0x17E458CD, 0xE9DF2188,
                                     0x41A12C9E, 0xD6AA2A3A, 0x992941B7};

// Which of a Remastered material's parameters feed the four maps, and its two
// strengths. Later parameters replace earlier ones, as in the reference.
// The names of the shader constants and lists a shader is in ("-" for none), for the materials report.
std::string ShaderRole(uint32_t shader) {
  std::string out;
  auto add = [&](bool in, const char* name) {
    if (in) {
      out += (out.empty() ? "" : "+") + std::string(name);
    }
  };
  auto in = [&](const auto& list) { return std::find(std::begin(list), std::end(list), shader) != std::end(list); };
  add(shader == kShaderHeightBlend, "height-blend");
  add(shader == kShaderUpLayer, "up-layer");
  add(shader == kShaderProjBlend, "projected-blend");
  add(shader == kShaderDecalBlend, "decal-alpha");
  add(shader == kShaderDetail || shader == kShaderDetailTinted, "detail");
  add(shader == kShaderVertexBlend, "vertex-blend");
  add(shader == kShaderLava, "lava");
  add(IsWaterfall(shader), "waterfall");
  add(shader == kShaderParallax, "parallax");
  add(shader == kShaderGlass, "glass");
  add(in(kShaderLavaPool), "lava-pool");
  add(in(kShaderGunGlow), "gun-glow");
  add(shader == kShaderFrozenShell, "frozen-shell");
  add(shader == kShaderIceSpreader, "ice");
  add(shader == kShaderMatcapShell, "matcap-shell");
  add(in(kShaderInverseExposure), "inverse-exposure");
  add(in(kShaderIncanMaskSquared), "incan-mask-squared");
  add(in(kShaderGunBody), "gun-body");
  add(shader == kShaderGunPanel, "gun-panel");
  add(in(kShaderPremulGlass), "premul-glass");
  add(shader == kShaderBoundaryShield || shader == kShaderForceField, "boundary-shield");
  add(shader == kShaderPickUp, "pickup");
  add(shader == kShaderHolo, "holo");
  add(shader == kShaderHoloRefl, "holo-refl");
  add(shader == kShaderHologram, "hologram");
  add(shader == kShaderLambertFx, "lambert-fx");
  add(shader == kShaderGunFx, "gun-fx");
  add(IsEyeGloss(shader), "eye-gloss");
  add(shader == kShaderColorUnlit, "color-unlit");
  add(shader == kShaderPhazon, "phazon");
  add(shader == kShaderPhazonB, "phazon-b");
  add(shader == kShaderPhazonPool, "phazon-pool");
  add(shader == kShaderRefractGlass || shader == kShaderRefractGlassB, "refract-glass");
  add(shader == kShaderHoloGlassB || shader == kShaderHoloGlassC, "holo-glass-b");
  add(shader == kShaderDualGlass, "dual-glass");
  add(IsXrayGhost(shader), "xray-ghost");
  add(shader == kShaderXrayMap, "xray-map");
  add(shader == kShaderDecalCut || shader == kShaderDecalAlphaMap, "decal-cut");
  add(in(kShaderTints), "tinted");
  return out.empty() ? "-" : out;
}

const char* KindName(int kind) {
  switch (kind) {
  case 0: return "standard";
  case 1: return "up-layer";
  case 2: return "detail";
  case 3: return "lava";
  case 4: return "parallax";
  case 6: return "lava-pool";
  case 7: return "waterfall";
  case 8: return "glass";
  case 9: return "gun-glow";
  case 10: return "premul-glass";
  case 11: return "holo-glass";
  case 12: return "frozen-shell";
  case 13: return "matcap-shell";
  case 14: return "boundary-shield";
  case 15: return "pickup";
  case 16: return "holo";
  case 17: return "holo-refl";
  case 18: return "hologram";
  case 19: return "gun-fx";
  case 20: return "vertex-blend";
  case 21: return "phazon";
  case 22: return "phazon-b";
  case 23: return "refract-glass";
  case 24: return "decal-cut";
  case 25: return "eye-gloss";
  case 26: return "projected-blend";
  case 27: return "decal-alpha";
  case 28: return "ice";
  case 29: return "holo-glass-b";
  case 30: return "refract-glass-b";
  case 31: return "xray-ghost";
  case 32: return "phazon-pool";
  case 33: return "xray-map";
  case 34: return "dual-glass";
  default: return "kind?";
  }
}

RemMaterial ReadMaterial(const ModelMaterial& mat, const ConvertOptions& opt) {
  RemMaterial out;
  out.name = mat.name;
  out.flags = mat.unk1;
  out.cutout = (mat.unk1 & kCutoutFlag) != 0;
  out.blended = (mat.unk1 & kTransparentFlag) != 0 && !out.cutout;
  uint8_t sid[4];
  std::memcpy(sid, &mat.shaderId, 4);
  const uint32_t rawShader = uint32_t(sid[0]) << 24 | uint32_t(sid[1]) << 16 | uint32_t(sid[2]) << 8 | sid[3];
  const uint32_t shader = ShaderFamily(rawShader);
  out.shader = rawShader;
  out.family = shader;
  out.role = ShaderRole(shader);
  out.shell = shader == kShaderMatcapShell;
  out.shield = shader == kShaderBoundaryShield || shader == kShaderForceField || shader == kShaderPickUp || shader == kShaderHolo ||
               shader == kShaderHoloRefl || shader == kShaderHologram || shader == kShaderGunFx || IsEyeGloss(shader) ||
               IsXrayGhost(shader) || shader == kShaderXrayMap;
  bool custom = false;
  for (const ModelMaterialData& d : mat.data) {
    const uint32_t family = d.usage & 0xFFFFFF00u;
    custom = custom || family == (FourCC('T', 'C', 'H', '0') & 0xFFFFFF00u) ||
             family == (FourCC('C', 'C', 'H', '0') & 0xFFFFFF00u);
  }
  // Premultiplied glass multiplies its albedo by the vertex colour and its alpha by the
  // vertex alpha, which is all the opacity a pane has (its base map is a flat grey, alpha 1).
  const bool premulGlass =
      std::find(std::begin(kShaderPremulGlass), std::end(kShaderPremulGlass), shader) != std::end(kShaderPremulGlass);
  // Plain MFVC shaders (882014EE, F22FEB5B, the layered ones) multiply only the diffuse;
  // every kShaderTints shader multiplies the albedo, so its F0 is tinted too.
  out.tintF0 = std::find(std::begin(kShaderTints), std::end(kShaderTints), shader) != std::end(kShaderTints);
  out.tinted = (mat.unk1 & kVertexColorFlag) != 0 && (!custom || premulGlass || out.tintF0);
  // What the alpha a shader writes is made of is only in its code. Every one
  // read writes the base map's alpha squared, times the vertex alpha, bar these:
  // with the mask flag the base alpha scales the glow instead and the vertex
  // alpha alone is the opacity, a material in no lit pass (screens, holograms)
  // is its own colour and glow, a framebuffer one (ice, glass) writes alpha 1
  // and refracts what is behind it (the port does that for glass, kind 8), and
  // the snow shader cuts its edge by height.
  out.maskSquared = std::binary_search(std::begin(kShaderIncanMaskSquared), std::end(kShaderIncanMaskSquared), shader);
  out.mask = (mat.unk1 & kIncanMaskFlag) != 0 || out.maskSquared;
  out.unlit = mat.types.empty();
  bool lit = false, framebuffer = false;
  for (uint32_t type : mat.types) {
    lit = lit || type == FourCC('R', 'L', 'T', 'G');
    framebuffer = framebuffer || type == FourCC('R', 'F', 'B', 'P');
  }
  if (out.mask) {
    out.blended = out.blended && out.tinted;
  }
  if (framebuffer && lit) {
    out.blended = false;
  }
  if (shader == kShaderHeightBlend) {
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind == ModelMaterialData::Kind::Color && d.usage == FourCC('C', 'C', 'H', '0')) {
        out.height = std::clamp(ShortestDouble(d.color[0]), 0.01, 4.0);
      }
    }
  }
  auto set = [&](int k, const ModelTextureRef& t, MapRef* into = nullptr) {
    if (!t.hasUsage) {
      return;
    }
    MapRef& m = into ? *into : out.maps[k];
    m.has = true;
    // A model stores the id with its first three groups little endian; a pak,
    // and the printed form, have them the other way round.
    m.id = t.id;
    std::swap(m.id[0], m.id[3]);
    std::swap(m.id[1], m.id[2]);
    std::swap(m.id[4], m.id[5]);
    std::swap(m.id[6], m.id[7]);
    m.coord = t.texCoord;
    m.wrap[0] = t.wrapX;
    m.wrap[1] = t.wrapY;
    m.src = opt.texturePrefix + IdToString(m.id) + opt.textureSuffix;
  };
  bool bclr = false;
  bool refl = false;
  const ModelMaterialData* icnc = nullptr;
  const ModelMaterialData* bklt = nullptr;
  const ModelMaterialData* bkla = nullptr;
  for (const ModelMaterialData& d : mat.data) {
    if (shader == kShaderIndirect && d.usage == FourCC('I', 'N', 'D', 'S')) {
      out.indStrength = d.kind == ModelMaterialData::Kind::Scalar ? d.scalar : d.color[0];
    }
    if (shader == kShaderIndirect && d.usage == FourCC('I', 'N', 'D', 'I') && d.kind == ModelMaterialData::Kind::Texture &&
        d.texture.hasUsage) {
      // The offset map is data (its xy are the offset), on its own texcoord set.
      set(kMr, d.texture);
      out.maps[kMr].raw = true;
      out.indirect = out.unlit;
    }
    const bool texture = d.kind == ModelMaterialData::Kind::Texture;
    const bool layered = d.kind == ModelMaterialData::Kind::LayeredTexture;
    switch (d.usage) {
    case FourCC('D', 'I', 'F', 'T'):
      // A material with a base colour map keeps a legacy diffuse slot beside
      // it, holding a 1x1 white default: the base colour map is the albedo.
      if (texture && !bclr) {
        set(kBase, d.texture);
      }
      break;
    case FourCC('B', 'C', 'L', 'R'):
      if (texture) {
        set(kBase, d.texture);
        bclr = d.texture.hasUsage;
      }
      break;
    case FourCC('R', 'E', 'F', 'L'):
      refl = refl || texture;
      if (texture && (shader == kShaderIceSpreader || shader == kShaderHoloRefl)) {
        set(kBase, d.texture, &out.refl);
        if (Lower(IdToString(out.refl.id)).rfind(kDefaultRefl, 0) == 0) {
          out.refl = MapRef{};
        }
      } else if (texture && std::find(std::begin(kShaderGunBody), std::end(kShaderGunBody), shader) != std::end(kShaderGunBody)) {
        set(kBase, d.texture, &out.refl);
        if (Lower(IdToString(out.refl.id)).rfind(kDefaultRefl, 0) == 0) {
          out.refl = MapRef{};
        }
        // The metalness keeps its full range, whether the metal shows its own cube or
        // the room's: under the ceiling the pale albedo was lit as diffuse by the bright
        // ambient around the gun and read washed-out teal, where Remastered's is charcoal.
        out.maps[kMr].metalMax = 1.0;
      }
      break;
    case FourCC('R', 'E', 'F', 'S'):
    case FourCC('R', 'E', 'F', 'V'):
      // 98F0556D's sphere map and reflectivity map: the second layer's base and MR (kind 19).
      if (texture && (shader == kShaderGunFx || IsEyeGloss(shader))) {
        const bool sphere = d.usage == FourCC('R', 'E', 'F', 'S');
        MapRef& m = sphere ? out.layer[kBase] : out.layer[kMr];
        set(sphere ? kBase : kMr, d.texture, &m);
        m.raw = true;
      }
      break;
    case FourCC('B', 'C', 'R', 'L'):
      if (layered) {
        set(kBase, d.layeredTextures[0]);
        set(kBase, d.layeredTextures[1], &out.layer[kBase]);
      }
      break;
    case FourCC('L', 'I', 'T', 'S'):
      if (d.kind == ModelMaterialData::Kind::Scalar) {
        out.hasLits = true;
        out.lits = ShortestDouble(d.scalar);
      }
      break;
    case FourCC('B', 'L', 'S', 'M'):
      if (d.kind == ModelMaterialData::Kind::Scalar) {
        out.layerSmooth = ShortestDouble(d.scalar);
      }
      break;
    case FourCC('B', 'S', 'A', 'O'):
      if (d.kind == ModelMaterialData::Kind::Color) {
        for (int i = 0; i < 4; ++i) {
          out.layerHeight[i] = ShortestDouble(d.color[i]);
        }
      }
      break;
    case FourCC('M', 'E', 'T', 'L'):
      if (texture) {
        set(kMr, d.texture);
      }
      break;
    case FourCC('M', 'T', 'L', 'L'):
      if (layered) {
        set(kMr, d.layeredTextures[0]);
        set(kMr, d.layeredTextures[1], &out.layer[kMr]);
      }
      break;
    case FourCC('N', 'M', 'A', 'P'):
      if (texture) {
        set(kNormal, d.texture);
      }
      break;
    case FourCC('M', 'N', 'M', 'P'):
      if (texture && shader == kShaderLayeredMacro) {
        set(kNormal, d.texture, &out.macroMap);
        out.macroLayered = out.macroMap.has;
      } else if (texture && std::find(std::begin(kShaderMacroNormal), std::end(kShaderMacroNormal), shader) !=
                                std::end(kShaderMacroNormal)) {
        set(kNormal, d.texture, &out.layer[kNormal]);
        out.macro = out.layer[kNormal].has;
      }
      break;
    case FourCC('N', 'R', 'M', 'L'):
      if (layered) {
        set(kNormal, d.layeredTextures[0]);
        set(kNormal, d.layeredTextures[1], &out.layer[kNormal]);
      }
      break;
    case FourCC('I', 'C', 'A', 'N'):
      if (texture) {
        set(kEmissive, d.texture);
      }
      break;
    case FourCC('I', 'C', 'N', 'C'):
      if (d.kind == ModelMaterialData::Kind::Color) {
        icnc = &d;
      }
      break;
    case FourCC('B', 'K', 'L', 'T'):
      if (d.kind == ModelMaterialData::Kind::Color) {
        bklt = &d;
      }
      break;
    case FourCC('B', 'K', 'L', 'A'):
      if (d.kind == ModelMaterialData::Kind::Color) {
        bkla = &d;
      }
      break;
    default:
      break;
    }
  }
  out.noRefl = !refl;
  // The strength is ICNC, the incandescence colour (grey in every material
  // seen, 0.15 to 400), times the shader's INCI parameter where it has one. A
  // render type row names the colour that holds a parameter and the component:
  // INCI / CCH2 / flag 0 is CCH2.x.
  double s = 1.0;
  if (icnc) {
    s = std::max({ShortestDouble(icnc->color[0]), ShortestDouble(icnc->color[1]), ShortestDouble(icnc->color[2])});
  }
  const ModelRenderType* inci = nullptr;
  for (const ModelRenderType& r : mat.renderTypes) {
    if (r.dataId == FourCC('I', 'N', 'C', 'I')) {
      inci = &r;
    }
  }
  if (inci && inci->flag2 < 4) {
    const ModelMaterialData* colour = nullptr;
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind == ModelMaterialData::Kind::Color && d.usage == inci->dataType) {
        colour = &d;
      }
    }
    if (colour) {
      s *= ShortestDouble(colour->color[inci->flag2]);
    }
  }
  out.emissive = s;
  if (out.maps[kEmissive].has &&
      std::find(std::begin(kShaderGunBody), std::end(kShaderGunBody), shader) != std::end(kShaderGunBody)) {
    // The arm cannon's lit stripes. Remastered reads the ramp (dark, red, orange,
    // yellow along U; the stripe's soft edges down V) at a texcoord the model's ANUV
    // animates: the set's U plus a 0..0.5 hump, looping every 1.033 s, so the stripes
    // pulse along the ramp (the converter turns the curve into a texture matrix). Both
    // stripe shaders (917f1415, f495b260) multiply that glow by c4[0].z in every perm,
    // so it is inverse-exposed: ICAN x INCI (2) on screen, whatever the room. The old
    // compressed strength (0.1 x sqrt(s / 0.1), 0.45) left them dim orange-brown where
    // Remastered shows them bright yellow.
    out.glowLinear = true;
    out.emissive = s;
    out.reason += "gun-body list: stripes ramp at inverse exposure; ";
  } else if (out.maps[kEmissive].has && shader == kShaderGunPanel) {
    // The beam panels: ICAN x ICNC x base alpha with no exposure factor (only the lit
    // part is exposed), so like the inverse-exposed glows it is kept linear.
    out.glowLinear = true;
    out.emissive = s;
    out.reason += "gun-panel: glow at inverse exposure; ";
  } else if (out.maps[kEmissive].has &&
             std::binary_search(std::begin(kShaderInverseExposure),
                                std::end(kShaderInverseExposure), shader)) {
    // Inverse-exposed: on screen the glow is ICAN x INCI with no exposure factor, and
    // the maps are dim (the Metroid body's peaks at 33/255), so a room's exposure would
    // leave them unlit in a dark room. Nor is it compressed: it is already what the
    // screen shows, not an HDR value for bloom.
    out.glowLinear = true;
    out.emissive = s;
    out.reason += "inverse-exposure list: glow at inverse exposure; ";
  }
  // The shaders of their own. ICNC is 1 in every lava material and the strength
  // is CCH0.x instead.
  const ModelMaterialData* tch[4] = {nullptr, nullptr, nullptr, nullptr};
  const ModelMaterialData* cch[7] = {};
  for (const ModelMaterialData& d : mat.data) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (d.kind == ModelMaterialData::Kind::Texture && d.usage == FourCC('T', 'C', 'H', char('0' + i))) {
        tch[i] = &d;
      }
    }
    for (uint32_t i = 0; i < 7; ++i) {
      if (d.kind == ModelMaterialData::Kind::Color && d.usage == FourCC('C', 'C', 'H', char('0' + i))) {
        cch[i] = &d;
      }
    }
  }
  if (shader == kShaderUpLayer && tch[0] && cch[0]) {
    out.kind = 1;
    static const int slot[3] = {kBase, kMr, kNormal};
    for (int i = 0; i < 3; ++i) {
      if (tch[i]) {
        set(slot[i], tch[i]->texture, &out.layer[slot[i]]);
      }
    }
    out.layerSmooth = ShortestDouble(cch[0]->color[0]);
  } else if (shader == kShaderProjBlend && tch[0] && cch[0]) {
    // 9EFE0D2E's blend (the up-facing weight of the bottom layer's normal, the heights in the alphas), whose TCH0-2
    // are read on a texcoord set of their own. Its alpha is the plain lerp of the two base alphas (kind 26).
    out.kind = 26;
    static const int slot[3] = {kBase, kMr, kNormal};
    for (int i = 0; i < 3; ++i) {
      if (tch[i]) {
        set(slot[i], tch[i]->texture, &out.layer[slot[i]]);
      }
    }
    out.layerSmooth = ShortestDouble(cch[0]->color[0]);
  } else if (shader == kShaderDecalBlend && tch[0] && cch[0] && !out.maps[kEmissive].has) {
    // The alpha map TCH0 rides in the emissive slot as kind 24's does; CCH0.x is the edge width (kind 27).
    out.kind = 27;
    set(kEmissive, tch[0]->texture);
    out.maps[kEmissive].detail = out.maps[kEmissive].has;
    out.layerSmooth = ShortestDouble(cch[0]->color[0]);
  } else if (shader == kShaderVertexBlend && tch[0] && cch[0]) {
    // Bottom layer BCLR/METL/NMAP, top layer TCH0-2 by the vertex alpha (CCH0.x the edge). TCH3 multiplies
    // the blended albedo: it rides in the emissive slot, which this shader has no use for.
    out.kind = 20;
    static const int slot[3] = {kBase, kMr, kNormal};
    for (int i = 0; i < 3; ++i) {
      if (tch[i]) {
        set(slot[i], tch[i]->texture, &out.layer[slot[i]]);
      }
    }
    if (tch[3] && !out.maps[kEmissive].has) {
      set(kEmissive, tch[3]->texture);
      out.maps[kEmissive].detail = out.maps[kEmissive].has;
    }
    out.layerSmooth = ShortestDouble(cch[0]->color[0]);
  } else if (out.cutout && cch[0] && cch[0]->color[0] == 0.5 &&
             (shader == kShaderDecalCut || (shader == kShaderDecalAlphaMap && tch[0] && !out.maps[kEmissive].has))) {
    // c = 0.5 (t = A) is the only value they use, and the one the shader assumes.
    out.kind = 24;
    if (shader == kShaderDecalAlphaMap) {
      set(kEmissive, tch[0]->texture);
      out.maps[kEmissive].detail = out.maps[kEmissive].has;
    }
  } else if ((shader == kShaderDetail || shader == kShaderDetailTinted) && tch[0]) {
    out.kind = 2;
    set(kBase, tch[0]->texture, &out.layer[kBase]);
  } else if (shader == kShaderLava && cch[0]) {
    out.kind = 3;
    out.vcolor = true;
    out.emissive = ShortestDouble(cch[0]->color[0]);
  } else if (shader == kShaderParallax && tch[0] && cch[0]) {
    out.kind = 4;
    out.vcolor = true;
    set(kBase, tch[0]->texture, &out.layer[kBase]);
    for (int i = 0; i < 4; ++i) {
      out.kindParam[i] = ShortestDouble(cch[0]->color[i]);
    }
    out.kindStrength = cch[1] ? ShortestDouble(cch[1]->color[0]) : 0.0;
  } else if (std::find(std::begin(kShaderLavaPool), std::end(kShaderLavaPool), shader) != std::end(kShaderLavaPool) &&
             out.maps[kBase].has && tch[0] && tch[1] && tch[2] && cch[0] && cch[1]) {
    out.kind = 6;
    out.vcolor = true;
    static const int slot[3] = {kBase, kMr, kNormal};
    for (int i = 0; i < 3; ++i) {
      set(slot[i], tch[i]->texture, &out.layer[slot[i]]);
      // The shader reads every map on the first texcoord, whatever the material says.
      out.layer[slot[i]].coord = 0;
    }
    out.maps[kBase].coord = 0;
    // The pool's LavaRenderVolume overrides CCH0 and CCH1 (x, y, z of each).
    double c[6];
    for (int i = 0; i < 6; ++i) {
      c[i] = opt.hasLava ? ShortestDouble(opt.lava[i]) : ShortestDouble(cch[i / 3]->color[i % 3]);
    }
    const double period = c[1];
    out.kindParam[0] = period > 1e-3 ? 1.0 / period : 0.0;  // the game multiplies it by the time
    out.kindParam[1] = c[0];
    out.kindParam[2] = c[3];
    out.kindParam[3] = c[5];
    out.layerHeight[0] = c[4];  // the noise map's scale
    out.layerHeight[1] = 2.4;   // the heat's gain, a literal of the shader
    out.layerHeight[2] = period;  // takes the phase back to seconds for the shimmer
    out.kindStrength = c[2];
  } else if (IsWaterfall(shader) && tch[0] && tch[1] && cch[0] && cch[1] && cch[2]) {
    out.kind = 7;
    out.vcolor = true;
    set(kBase, tch[1]->texture);
    set(kBase, tch[0]->texture, &out.layer[kBase]);
    out.maps[kBase].coord = out.layer[kBase].coord = 0;
    out.maps[kMr].has = out.maps[kNormal].has = out.maps[kEmissive].has = false;
    const double rate = ShortestDouble(cch[0]->color[0]);
    for (int i = 0; i < 4; ++i) {
      out.layerHeight[i] = rate * ShortestDouble(cch[1]->color[i]);
    }
    out.layerSmooth = rate * ShortestDouble(cch[2]->color[0]);
    out.kindParam[0] = 1.0;  // the game multiplies it by the time
    out.kindParam[1] = rate * ShortestDouble(cch[2]->color[1]);
    out.kindParam[2] = ShortestDouble(cch[0]->color[1]);
    out.kindParam[3] = ShortestDouble(cch[0]->color[2]);
    out.kindStrength = ShortestDouble(cch[0]->color[3]);
    // The fold's output is ramp x c5[0] + c5[1]: c5 follows the CCH block (c4[0..2] are CCH0-2, CCH3 is never read),
    // so c5[0] = DIFC and c5[1] = ICMC. DIFC.w = 1 and ICMC.rgb = 0 on all 20 materials, so only the rgb tint is carried.
    for (int i = 0; i < 3; ++i) {
      out.tint[i] = 1.0;
    }
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind == ModelMaterialData::Kind::Color && d.usage == FourCC('D', 'I', 'F', 'C')) {
        for (int i = 0; i < 3; ++i) {
          out.tint[i] = ShortestDouble(d.color[i]);
        }
      }
    }
  } else if (shader == kShaderGlass && out.maps[kBase].has && tch[0] && tch[1] && cch[0] && cch[1] && cch[2] &&
             cch[3] && cch[4]) {
    out.kind = 8;
    out.vcolor = true;
    // The mask and the noise are bound as the second layer's base and MR. CCH4.w
    // (8 in every glass material) is no uv scale the maps are read with.
    set(kBase, tch[0]->texture, &out.layer[kBase]);
    set(kMr, tch[1]->texture, &out.layer[kMr]);
    const double glow = ShortestDouble(cch[4]->color[1]);
    for (int i = 0; i < 3; ++i) {
      out.layerHeight[i] = ShortestDouble(cch[2]->color[i]) * glow;
      out.tint[i] = ShortestDouble(cch[1]->color[i]) * ShortestDouble(cch[3]->color[i]) *
                    ShortestDouble(cch[4]->color[0]) * ShortestDouble(cch[4]->color[2]);
    }
    out.layerHeight[3] = ShortestDouble(cch[1]->color[3]);
    for (int i = 0; i < 3; ++i) {
      out.kindParam[i] = ShortestDouble(cch[0]->color[i]);
    }
  } else if (shader == kShaderHoloGlass && out.maps[kBase].has && tch[2] && cch[0] && cch[1] && cch[2] && cch[3]) {
    out.kind = 11;
    out.vcolor = true;
    // The distortion map is bound as the second layer's base; it scrolls CCH3.y a second
    // down and a quarter of that across. The scene is tinted by CCH1 (where the second
    // output lets the room through), and BCLR, the vertex colour and the reflection's
    // fresnel per vertex alpha are weighted by CCH0.w, CCH2.y and CCH0.y.
    set(kBase, tch[2]->texture, &out.layer[kBase]);
    out.layer[kBase].raw = true;
    out.kindParam[0] = 1.0;  // the game multiplies it by the time
    out.kindParam[1] = ShortestDouble(cch[3]->color[1]);
    out.kindParam[2] = ShortestDouble(cch[3]->color[0]);
    out.kindParam[3] = ShortestDouble(cch[3]->color[2]);
    for (int i = 0; i < 3; ++i) {
      out.tint[i] = ShortestDouble(cch[1]->color[i]);
    }
    out.layerHeight[0] = ShortestDouble(cch[0]->color[3]);
    out.layerHeight[1] = ShortestDouble(cch[2]->color[1]);
    out.layerHeight[2] = ShortestDouble(cch[0]->color[1]);
    out.layerHeight[3] = 0.0;
  } else if (shader == kShaderFrozenShell && out.maps[kBase].has && tch[1] && cch[0] && cch[1] && cch[2]) {
    out.kind = 12;
    // The dissolve noise is bound as the second layer's base. The edge is
    // clamp(TCH1 - charge (CCH1.z + 2) DIFC.w + 1); the shell shows where it is under
    // about 0.9 (or where the base alpha squared times DIFC.w lets it), and the edge
    // glows CCH1.x x CCH2 at inverse exposure, so linear. The runtime puts the charge
    // in the first parameter. The incandescence is CCH0.w (IINT), not INCI.
    set(kBase, tch[1]->texture, &out.layer[kBase]);
    out.layer[kBase].raw = true;
    double difc = 1.0;
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind == ModelMaterialData::Kind::Color && d.usage == FourCC('D', 'I', 'F', 'C')) {
        difc = ShortestDouble(d.color[3]);
      }
    }
    out.kindParam[0] = 0.0;
    out.kindParam[1] = (ShortestDouble(cch[1]->color[2]) + 2.0) * difc;
    out.kindParam[2] = difc;
    // The rim: F0 x AO x (1 - n.v)^CCH0.z on faces turned up, times CCH0.y x LINT
    // (CCH0.x, which nothing in the exe sets at run time), at inverse exposure.
    out.kindParam[3] = ShortestDouble(cch[0]->color[1]) * ShortestDouble(cch[0]->color[0]);
    out.kindStrength = ShortestDouble(cch[1]->color[0]);
    for (int i = 0; i < 3; ++i) {
      out.layerHeight[i] = ShortestDouble(cch[2]->color[i]);
    }
    out.layerHeight[3] = ShortestDouble(cch[0]->color[2]);
    // ICAN x CCH0.w (IINT), exposed like other glows: drawn linear, the stored 10 turned
    // the shell a saturated cyan, where Remastered's white is the white base under the
    // probe light and the rim. CIceBeamMP1::PreRenderGunFx sets IINT to 3 + 14 v, v
    // going 0..0.5..0 once a second; the runtime pulses the stored peak to match.
    out.emissive = s * ShortestDouble(cch[0]->color[3]);
  } else if (shader == kShaderMatcapShell && out.maps[kNormal].has && tch[0] && cch[0] && cch[1]) {
    out.kind = 13;
    out.vcolor = true;
    // The matcap is bound as the second layer's base (colour: the shader decodes it, then
    // scales it by CCH0.z). CCH0 is rim power, rim gain, albedo scale, lit-normal tilt;
    // CCH1 the matcap normal's tilt, the rim's bias and the probe reflection's scale. DIFC
    // tints the albedo and its alpha scales the vertex alpha.
    set(kBase, tch[0]->texture, &out.layer[kBase]);
    out.layer[kBase].coord = 0;
    out.kindParam[0] = ShortestDouble(cch[0]->color[0]);
    out.kindParam[1] = ShortestDouble(cch[0]->color[1]);
    out.kindParam[2] = ShortestDouble(cch[1]->color[0]);
    out.kindParam[3] = ShortestDouble(cch[1]->color[1]);
    out.kindStrength = std::max(ShortestDouble(cch[0]->color[2]), 0.0);
    out.layerHeight[2] = ShortestDouble(cch[1]->color[2]);
    out.layerHeight[1] = 1.0;
    for (int i = 0; i < 3; ++i) {
      out.tint[i] = 1.0;
    }
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind == ModelMaterialData::Kind::Color && d.usage == FourCC('D', 'I', 'F', 'C')) {
        for (int i = 0; i < 3; ++i) {
          out.tint[i] = ShortestDouble(d.color[i]);
        }
        out.layerHeight[1] = ShortestDouble(d.color[3]);
      }
    }
  } else if (shader == kShaderPhazon && out.maps[kBase].has && out.maps[kNormal].has && tch[0] && tch[1] &&
             cch[0] && cch[1] && cch[2] && cch[3]) {
    out.kind = 21;
    out.vcolor = true;
    // BCLR, METL and NMAP are maps 0-2 as in the standard shader. TCH0 (the gradient ramp, read at
    // a computed coordinate) and TCH1 (the pulse's mask, read at uv x CCH0.w) are bound as the
    // second layer's base and MR; the sampler modes they were authored with are kept. The shader's
    // constants go whole to the runtime in GXSetPBRShield's rows: CCH0..CCH3 in rows 0-3, DIFC in
    // row 7 (the game multiplies CCH2.x by the time).
    set(kBase, tch[0]->texture, &out.layer[kBase]);
    set(kMr, tch[1]->texture, &out.layer[kMr]);
    out.layer[kBase].raw = out.layer[kMr].raw = true;
    out.kindParam[0] = 1.0;  // the game multiplies it by the time
    for (int r = 0; r < 4; ++r) {
      for (int i = 0; i < 4; ++i) {
        out.shieldRows[r * 4 + i] = ShortestDouble(cch[r]->color[i]);
      }
    }
    for (int i = 0; i < 4; ++i) {
      out.shieldRows[28 + i] = 1.0;
    }
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind == ModelMaterialData::Kind::Color && d.usage == FourCC('D', 'I', 'F', 'C')) {
        for (int i = 0; i < 4; ++i) {
          out.shieldRows[28 + i] = ShortestDouble(d.color[i]);
        }
      }
    }
  } else if (shader == kShaderPhazonB && out.maps[kBase].has && out.maps[kNormal].has && tch[0] && tch[1] &&
             cch[0] && cch[1] && cch[2] && cch[3]) {
    out.kind = 22;
    out.vcolor = true;
    // As kind 21: TCH0 (the ramp) and TCH1 (the mask, here read at the base UV) are the second layer's
    // base and MR, CCH0..CCH3 are rows 0-3. This shader has no DIFC (the output alpha is 1).
    set(kBase, tch[0]->texture, &out.layer[kBase]);
    set(kMr, tch[1]->texture, &out.layer[kMr]);
    out.layer[kBase].raw = out.layer[kMr].raw = true;
    out.kindParam[0] = 1.0;  // the game multiplies it by the time
    for (int r = 0; r < 4; ++r) {
      for (int i = 0; i < 4; ++i) {
        out.shieldRows[r * 4 + i] = ShortestDouble(cch[r]->color[i]);
      }
    }
    for (int i = 0; i < 4; ++i) {
      out.shieldRows[28 + i] = 1.0;
    }
  } else if (shader == kShaderPhazonPool && out.maps[kBase].has && out.maps[kNormal].has && tch[0] && tch[1] &&
             cch[0] && cch[1]) {
    out.kind = 32;
    // TCH0 (the noise volume, stacked into one 2D atlas by the importer) and TCH1 (the emission ramp) are the second
    // layer's base and MR, on TCH0's texcoord set. CCH0 (fresnel power, gain, noise scale along the view, emission
    // gain) and CCH1 (noise speed, depth along BCLR.a) are rows 0 and 1, ICMC row 6 and DIFC row 7.
    set(kBase, tch[0]->texture, &out.layer[kBase]);
    set(kMr, tch[1]->texture, &out.layer[kMr]);
    out.layer[kBase].raw = out.layer[kMr].raw = true;
    out.layer[kBase].volume = true;
    out.kindParam[0] = 1.0;  // the game multiplies it by the time
    for (int r = 0; r < 2; ++r) {
      for (int i = 0; i < 4; ++i) {
        out.shieldRows[r * 4 + i] = ShortestDouble(cch[r]->color[i]);
      }
    }
    for (int i = 0; i < 4; ++i) {
      out.shieldRows[28 + i] = 1.0;
    }
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind != ModelMaterialData::Kind::Color) {
        continue;
      }
      if (d.usage == FourCC('D', 'I', 'F', 'C')) {
        for (int i = 0; i < 4; ++i) {
          out.shieldRows[28 + i] = ShortestDouble(d.color[i]);
        }
      } else if (d.usage == FourCC('I', 'C', 'M', 'C')) {
        for (int i = 0; i < 3; ++i) {
          out.shieldRows[24 + i] = ShortestDouble(d.color[i]);
        }
      }
    }
  } else if (shader == kShaderXrayMap && out.maps[kBase].has && out.maps[kNormal].has && tch[0] && cch[0] && cch[1]) {
    out.kind = 33;
    // 65e90e82, the X-ray visor's map material: CCH0/CCH1 are rows 0 and 1 as kind 31's, DIFC row 2 (rgba, default 1),
    // ICMC row 3 (rgb, default 0). TCH0 (subtracted twice from the colour and averaged into the alpha) rides in the MR slot.
    for (int r = 0; r < 2; ++r) {
      for (int i = 0; i < 4; ++i) {
        out.shieldRows[r * 4 + i] = ShortestDouble(cch[r]->color[i]);
      }
    }
    for (int i = 0; i < 4; ++i) {
      out.shieldRows[8 + i] = 1.0;
    }
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind != ModelMaterialData::Kind::Color) {
        continue;
      }
      if (d.usage == FourCC('D', 'I', 'F', 'C')) {
        for (int i = 0; i < 4; ++i) {
          out.shieldRows[8 + i] = ShortestDouble(d.color[i]);
        }
      } else if (d.usage == FourCC('I', 'C', 'M', 'C')) {
        for (int i = 0; i < 3; ++i) {
          out.shieldRows[12 + i] = ShortestDouble(d.color[i]);
        }
      }
    }
    out.shieldRows[15] = 1.0;
    set(kMr, tch[0]->texture);
    out.maps[kMr].raw = true;
  } else if ((IsXrayGhost(shader)) && out.maps[kBase].has &&
             out.maps[kNormal].has && cch[0] && cch[1]) {
    out.kind = 31;
    // CCH0 (fresnel power, ICAN scale, alpha at the near and far depth) and CCH1 (the depth range, the alpha-by-fresnel
    // and rim switches) are rows 0 and 1, ICNC row 2 (rgba), ICMC row 3 with w = 1 where the shader reads ICAN (the
    // skinned one).
    for (int r = 0; r < 2; ++r) {
      for (int i = 0; i < 4; ++i) {
        out.shieldRows[r * 4 + i] = ShortestDouble(cch[r]->color[i]);
      }
    }
    for (int i = 0; i < 4; ++i) {
      out.shieldRows[8 + i] = 1.0;
    }
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind != ModelMaterialData::Kind::Color) {
        continue;
      }
      if (d.usage == FourCC('I', 'C', 'N', 'C')) {
        for (int i = 0; i < 4; ++i) {
          out.shieldRows[8 + i] = ShortestDouble(d.color[i]);
        }
      } else if (d.usage == FourCC('I', 'C', 'M', 'C')) {
        for (int i = 0; i < 3; ++i) {
          out.shieldRows[12 + i] = ShortestDouble(d.color[i]);
        }
      }
    }
    out.shieldRows[15] = IsXraySkinned(shader) && out.maps[kEmissive].has ? 1.0 : 0.0;
  } else if ((shader == kShaderRefractGlass || shader == kShaderRefractGlassB) && out.maps[kBase].has &&
             out.maps[kNormal].has && cch[0]) {
    out.kind = shader == kShaderRefractGlassB ? 30 : 23;
    out.vcolor = true;
    // BCLR (alpha = roughness, squared in the shader) and NMAP are maps 0 and 2 as in the standard
    // shader. CCH0 (the refraction, the normal's share, the fresnel power and scale) goes whole to
    // kindParam. REFP and BLCM are the room's, which the runtime already has.
    for (int i = 0; i < 4; ++i) {
      out.kindParam[i] = ShortestDouble(cch[0]->color[i]);
    }
  } else if ((shader == kShaderHoloGlassB || shader == kShaderHoloGlassC) && out.maps[kBase].has && tch[0] && tch[1] && tch[2] && cch[0] && cch[1] &&
             cch[2] && cch[3]) {
    out.kind = 29;
    out.vcolor = true;
    // BCLR is the base map (its alpha the roughness); TCH0 and TCH2 are indirect offsets (raw) and TCH1
    // a colour (sRGB), bound as the second layer's base, MR and normal on their own AUVI texcoords.
    // The shader's constants go to the runtime whole (rows 0-3 CCH0..CCH3, row 7 DIFC); row 4 x is the
    // per-object phase the game fills, row 6 w says whether the material has a cube of its own.
    set(kBase, tch[0]->texture, &out.layer[kBase]);
    set(kMr, tch[1]->texture, &out.layer[kMr]);
    set(kNormal, tch[2]->texture, &out.layer[kNormal]);
    out.layer[kBase].raw = out.layer[kNormal].raw = true;
    out.kindParam[0] = 1.0;  // the game multiplies it by the time
    out.tint[0] = out.tint[1] = out.tint[2] = 0.0;
    double difc[4] = {1.0, 1.0, 1.0, 1.0};
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind == ModelMaterialData::Kind::Color && d.usage == FourCC('D', 'I', 'F', 'C')) {
        for (int i = 0; i < 4; ++i) {
          difc[i] = ShortestDouble(d.color[i]);
        }
      }
    }
    for (int r = 0; r < 4; ++r) {
      for (int i = 0; i < 4; ++i) {
        out.shieldRows[r * 4 + i] = ShortestDouble(cch[r]->color[i]);
      }
    }
    out.shieldRows[20] = shader == kShaderHoloGlassC ? 1.0 : 0.0;  // row 5 x: tint and dual-source terms
    out.shieldRows[27] = out.refl.has ? 1.0 : 0.0;
    for (int i = 0; i < 4; ++i) {
      out.shieldRows[28 + i] = difc[i];
    }
  } else if ((shader == kShaderBoundaryShield || shader == kShaderForceField) && out.maps[kBase].has && tch[0] && tch[1] && cch[0] && cch[1] &&
             cch[2] && cch[3] && cch[4] && cch[5] && cch[6]) {
    out.kind = 14;
    out.vcolor = true;
    // BCLR is the base map; TCH0 (the glow's mask) and TCH1 (the noise that bends everything) are
    // bound as the second layer's base and MR, each on the texcoord its own AUVI entry names. The
    // shader's constants go to the runtime whole (rows 0-6 CCH0..CCH6, row 7 DIFC): the scroll
    // speeds, pulse rates, colours and the screen bend are all in them.
    set(kBase, tch[0]->texture, &out.layer[kBase]);
    set(kMr, tch[1]->texture, &out.layer[kMr]);
    out.layer[kBase].raw = out.layer[kMr].raw = true;
    out.kindParam[0] = 1.0;  // the game multiplies it by the time
    out.tint[0] = out.tint[1] = out.tint[2] = 0.0;
    double difc[4] = {1.0, 1.0, 1.0, 1.0};
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind == ModelMaterialData::Kind::Color && d.usage == FourCC('D', 'I', 'F', 'C')) {
        for (int i = 0; i < 4; ++i) {
          difc[i] = ShortestDouble(d.color[i]);
        }
      }
    }
    for (int r = 0; r < 7; ++r) {
      for (int i = 0; i < 4; ++i) {
        out.shieldRows[r * 4 + i] = ShortestDouble(cch[r]->color[i]);
      }
    }
    for (int i = 0; i < 4; ++i) {
      out.shieldRows[28 + i] = difc[i];
    }
  } else if (shader == kShaderPickUp && out.maps[kBase].has && out.maps[kNormal].has && tch[0] && cch[0] &&
             cch[1] && cch[2] && cch[3]) {
    out.kind = 15;
    out.vcolor = true;
    // BCLR (the base map) is a three-channel mask and NMAP (the normal map) bends the rim's
    // normal. TCH0, the gradient, is bound as the second layer's base; it is read at the world
    // position, so its texcoord is unused. The constants go to the runtime whole.
    set(kBase, tch[0]->texture, &out.layer[kBase]);
    out.layer[kBase].coord = 0;
    out.kindParam[0] = 1.0;  // the game multiplies it by the time
    out.tint[0] = out.tint[1] = out.tint[2] = 0.0;
    double difc[4] = {1.0, 1.0, 1.0, 1.0};
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind == ModelMaterialData::Kind::Color && d.usage == FourCC('D', 'I', 'F', 'C')) {
        for (int i = 0; i < 4; ++i) {
          difc[i] = ShortestDouble(d.color[i]);
        }
      }
    }
    for (int r = 0; r < 4; ++r) {
      for (int i = 0; i < 4; ++i) {
        out.shieldRows[r * 4 + i] = ShortestDouble(cch[r]->color[i]);
      }
    }
    for (int i = 0; i < 4; ++i) {
      out.shieldRows[28 + i] = difc[i];
    }
  } else if ((shader == kShaderHolo || shader == kShaderHoloRefl || shader == kShaderHologram) &&
             out.maps[kBase].has && (shader != kShaderHologram || (cch[0] && cch[1] && cch[2] && cch[3]))) {
    out.kind = shader == kShaderHolo ? 16 : shader == kShaderHoloRefl ? 17 : 18;
    out.vcolor = out.kind == 18;
    // DIFT (or BCLR for the hologram) is the base map. The constants go to the runtime in the PBR8
    // trailer: row 7 DIFC; row 6 the incandescence ICNC + ICMC (summed, rgb) for 16 and 17, with w
    // the gain of the material's own cube (17); ICMC alone for 18, whose rows 0-3 are CCH0..CCH3 and
    // rows 4-5 are filled at run time (world x and y).
    out.kindParam[0] = 1.0;  // the game multiplies it by the time
    out.tint[0] = out.tint[1] = out.tint[2] = 0.0;
    double difc[4] = {1.0, 1.0, 1.0, 1.0};
    double incan[3] = {0.0, 0.0, 0.0};
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind != ModelMaterialData::Kind::Color) {
        continue;
      }
      if (d.usage == FourCC('D', 'I', 'F', 'C')) {
        for (int i = 0; i < 4; ++i) {
          difc[i] = ShortestDouble(d.color[i]);
        }
      } else if (d.usage == FourCC('I', 'C', 'M', 'C') || (d.usage == FourCC('I', 'C', 'N', 'C') && out.kind != 18)) {
        for (int i = 0; i < 3; ++i) {
          incan[i] += ShortestDouble(d.color[i]);
        }
      }
    }
    if (out.kind == 18) {
      for (int r = 0; r < 4; ++r) {
        for (int i = 0; i < 4; ++i) {
          out.shieldRows[r * 4 + i] = ShortestDouble(cch[r]->color[i]);
        }
      }
    }
    for (int i = 0; i < 3; ++i) {
      out.shieldRows[24 + i] = incan[i];
    }
    // The cube gain: only a cube of the material's own (not the black default) adds anything.
    out.shieldRows[27] = out.kind == 17 && out.refl.has ? 1.0 : 0.0;
    for (int i = 0; i < 4; ++i) {
      out.shieldRows[28 + i] = difc[i];
    }
  }
  if (std::find(std::begin(kShaderAreaLight), std::end(kShaderAreaLight), shader) != std::end(kShaderAreaLight)) {
    out.maps[kBase].solid = true;
  }
  const bool eyeGloss = IsEyeGloss(shader);
  if ((shader == kShaderGunFx || eyeGloss) && out.maps[kBase].has && out.layer[kBase].has && out.layer[kMr].has) {
    out.kind = eyeGloss ? 25 : 19;
    // The Eyon's BCLR is a 1x1 black that is the base all the same (diffuse 0, F0 from METL): no TEV fallback.
    out.maps[kBase].solid = eyeGloss;
    // Lit: REFV (map 5) x REFS (map 4, read at the view-space normal) x luminance(L) + DIFT x DIFC x L
    // + ICNC + ICMC. Row 6 = ICNC + ICMC, row 7 = DIFC; Remastered's mesh has no colour stream.
    out.layer[kMr].coord = out.layer[kBase].coord = 0;
    out.tint[0] = out.tint[1] = out.tint[2] = 1.0;
    double difc[4] = {1.0, 1.0, 1.0, 1.0};
    double incan[3] = {0.0, 0.0, 0.0};
    for (const ModelMaterialData& d : mat.data) {
      if (d.kind != ModelMaterialData::Kind::Color) {
        continue;
      }
      if (d.usage == FourCC('D', 'I', 'F', 'C')) {
        for (int i = 0; i < 4; ++i) {
          difc[i] = ShortestDouble(d.color[i]);
        }
      } else if (d.usage == FourCC('I', 'C', 'M', 'C') || d.usage == FourCC('I', 'C', 'N', 'C')) {
        for (int i = 0; i < 3; ++i) {
          incan[i] += ShortestDouble(d.color[i]);
        }
      }
    }
    for (int i = 0; i < 3; ++i) {
      out.shieldRows[24 + i] = incan[i];
    }
    for (int i = 0; i < 4; ++i) {
      out.shieldRows[28 + i] = difc[i];
    }
  }
  if (std::find(std::begin(kShaderGunGlow), std::end(kShaderGunGlow), shader) != std::end(kShaderGunGlow) &&
      out.maps[kBase].has && tch[0] && tch[1] && cch[0] && cch[1] && cch[2]) {
    out.kind = 9;
    out.vcolor = true;
    // The noise and the ramp are bound as the second layer's base and MR. The ramp is
    // read where the noise says, so its own texcoord is never used.
    set(kBase, tch[0]->texture, &out.layer[kBase]);
    set(kMr, tch[1]->texture, &out.layer[kMr]);
    out.layer[kBase].raw = out.layer[kMr].raw = true;
    out.layer[kMr].coord = out.layer[kBase].coord;
    for (int i = 0; i < 4; ++i) {
      out.layerHeight[i] = ShortestDouble(cch[1]->color[i]);
    }
    out.kindParam[0] = ShortestDouble(cch[0]->color[1]);  // the game multiplies it by the time
    out.kindParam[1] = ShortestDouble(cch[2]->color[0]);
    out.kindParam[2] = ShortestDouble(cch[2]->color[1]);
    out.kindParam[3] = ShortestDouble(cch[0]->color[3]);
    // The ramp times CCH0.z (10-15) at inverse exposure (c4[0].z in both shaders), so like
    // the inverse-exposed glows it is kept linear. The square root left the Wave gun's
    // lines a dull violet.
    out.kindStrength = std::max(ShortestDouble(cch[0]->color[2]), 0.0);
  }
  if (std::find(std::begin(kShaderPremulGlass), std::end(kShaderPremulGlass), shader) !=
          std::end(kShaderPremulGlass) &&
      out.blended && !out.cutout) {
    out.kind = 10;
  }
  if (shader == kShaderDualGlass && out.blended && !out.cutout) {
    out.kind = 34;
  }
  if (shader == kShaderIceSpreader && tch[0] && !out.maps[kEmissive].has && cch[0] && cch[1] && cch[2] && cch[3]) {
    // Ice (088e025e, kind 28): TCH0 is the frost, shifted by a parallax along the view and drawn
    // over the lit surface with a rim. Rows: CCH0 (normal strength z, parallax scale w), CCH1
    // (frost gain x, rim gain y, rim power z), CCH2 rgb with CCH5.x in w (cube tint and gain),
    // CCH3 (rim colour).
    set(kEmissive, tch[0]->texture);
    out.kind = 28;
    const ModelMaterialData* rows[4] = {cch[0], cch[1], cch[2], cch[3]};
    for (int r = 0; r < 4; ++r) {
      for (int i = 0; i < 4; ++i) {
        out.shieldRows[r * 4 + i] = ShortestDouble(rows[r]->color[i]);
      }
    }
    out.shieldRows[11] = cch[5] ? ShortestDouble(cch[5]->color[0]) : 1.0;
    out.emissive = 1.0;
    out.reason += "ice: kind 28, own cube; ";
  }
  // The texture matrix a scroll loads has no scale, so CCH1.yz (1 on every door
  // shield seen) is not drawn.
  if (shader == kShaderColorUnlit && cch[0]) {
    out.scroll[0] = ShortestDouble(cch[0]->color[1]);
    out.scroll[1] = -ShortestDouble(cch[0]->color[2]);
  }
  // CharacterBacklight (shader ae819893, build/mpr/mtrl-tr/ae819893/NOTES.md): BKLT is a
  // parameter vector, x the strength of the light from behind and y of the one from above.
  // BKLA is the cubic of the fade along the model's y (t 0 to 1 over its bounds), x t^3 + y t^2 + z t + w; every
  // material seen has a single term of 1 (t^3 in 358 of 375), so its power is kept and the
  // largest term picks it otherwise. No BKLA is t^3, the common case.
  out.backlight = bklt ? ShortestDouble(bklt->color[0]) : 0.0;
  out.backlightTop = bklt ? ShortestDouble(bklt->color[1]) : 0.0;
  out.backlightFalloff = bklt ? 4.0 : 0.0;
  if (bklt && bkla) {
    int term = -1;
    for (int i = 0; i < 4; ++i) {
      if (std::abs(bkla->color[i]) > 1e-6f && (term < 0 || std::abs(bkla->color[i]) > std::abs(bkla->color[term]))) {
        term = i;
      }
    }
    out.backlightFalloff = term < 0 ? 0.0 : 4.0 - term;
  }
  if (shader == kShaderColorUnlit) {
    out.colorUnlit = true;
    out.backlight = (cch[0] ? ShortestDouble(cch[0]->color[0]) : 1.0) * (cch[1] ? ShortestDouble(cch[1]->color[0]) : 1.0);
  }
  // Both layers' base maps and an edge width make a blend; the shader's own
  // floor on the width is not known, so a zero one is the thinnest edge.
  out.layered = out.maps[kBase].has && out.layer[kBase].has && !out.cutout && !out.blended;
  if (out.kind != 7) {
    out.layerSmooth = std::clamp(out.layerSmooth, 1e-3, 16.0);
  }
  if (out.kind == 6) {
    // Its own colour, and the extra maps are no second layer but must be bound like one.
    out.layered = out.unlit = true;
    out.cutout = out.blended = out.tinted = out.mask = false;
    out.height = 0.0;
  }
  if (out.kind == 7) {
    // The ramp's colour and alpha are all of it; the sheets' map is bound as a second layer.
    out.layered = out.unlit = out.blended = true;
    out.cutout = out.tinted = out.mask = false;
    out.height = 0.0;
    out.emissive = 1.0;
    out.backlight = out.backlightTop = 0.0;
  }
  if (out.kind == 8) {
    // Lit, but only by its reflection: what shows through is the frame behind it.
    out.layered = out.blended = true;
    out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
    out.emissive = 1.0;
    out.backlight = out.backlightTop = 0.0;
    out.maps[kMr].has = out.maps[kEmissive].has = false;
  }
  if (out.kind == 11) {
    // All of it is the shader's, and it covers what is behind it with the screen copy.
    out.layered = out.blended = true;
    out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
    out.emissive = 1.0;
    out.backlight = out.backlightTop = 0.0;
    out.maps[kMr].has = out.maps[kNormal].has = out.maps[kEmissive].has = false;
  }
  if (out.kind == 14 || out.kind == 29) {
    // Unlit and alpha blended; every colour is the shader's, and it reads the screen copy.
    out.layered = out.blended = true;
    out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
    out.emissive = 0.0;
    out.backlight = out.backlightTop = 0.0;
    out.maps[kMr].has = out.maps[kNormal].has = out.maps[kEmissive].has = false;
  }
  if (out.kind == 15) {
    // Unlit and blended; every colour is the shader's, and the normal map is kept (it bends the rim).
    out.layered = out.blended = true;
    out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
    out.emissive = 0.0;
    out.backlight = out.backlightTop = 0.0;
    out.maps[kMr].has = out.maps[kEmissive].has = false;
  }
  if (out.kind >= 16 && out.kind <= 18) {
    // Unlit and additive; every colour is the shader's, and the base map's alpha is its own.
    // No second layer: the one map is the base.
    out.blended = true;
    out.layered = false;
    out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
    out.emissive = 0.0;
    out.backlight = out.backlightTop = 0.0;
    out.maps[kMr].has = out.maps[kNormal].has = out.maps[kEmissive].has = false;
  }
  if (out.kind == 28) {
    // Lit, with the frost map in the emissive slot and its own opacity (the base alpha).
    out.tinted = out.mask = out.maskSquared = out.unlit = false;
    out.height = 0.0;
    out.backlight = out.backlightTop = 0.0;
  }
  if (out.kind == 33) {
    // As kind 31, but TCH0 stays in the MR slot and the ICAN map is always read.
    out.layered = out.blended = true;
    out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
    out.emissive = 0.0;
    out.backlight = out.backlightTop = 0.0;
  }
  if (out.kind == 31) {
    // Alpha blended and lit by the lights' specular alone; the shader makes the rest (ICAN is kept for the skinned one).
    out.layered = out.blended = true;
    out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
    out.emissive = 0.0;
    out.backlight = out.backlightTop = 0.0;
    out.maps[kMr].has = false;
    if (out.shieldRows[15] == 0.0) {
      out.maps[kEmissive].has = false;
    }
  }
  if (out.kind == 25) {
    // As kind 19, but the normal map, MR and the light's spec stay: only the reflection is the matcap.
    out.layered = true;
    out.blended = out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
    out.emissive = 0.0;
    out.backlight = out.backlightTop = 0.0;
    out.maps[kEmissive].has = false;
  }
  if (out.kind == 19) {
    // Lit and opaque (Remastered's mesh class 0; a fade is the model flags'). Maps 4 and 5 are
    // REFS and REFV; the lit base is DIFT alone.
    out.layered = true;
    out.blended = out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
    out.emissive = 0.0;
    out.backlight = out.backlightTop = 0.0;
    out.maps[kMr].has = out.maps[kNormal].has = out.maps[kEmissive].has = false;
  }
  if (out.kind == 13) {
    // Lit by the standard path and alpha blended; the shader makes its own albedo and rim.
    out.layered = out.blended = true;
    out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
    out.emissive = 0.0;
    out.backlight = out.backlightTop = 0.0;
    out.maps[kEmissive].has = false;
  }
  if (out.kind == 12) {
    // Opaque and lit; the shader discards what the dissolve leaves out.
    out.layered = true;
    out.blended = out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
  }
  if (out.kind == 23 || out.kind == 30) {
    // Opaque: the shader writes alpha 1, and what is behind it is its own copy of the frame. The
    // vertex alpha is the transmission weight and the base alpha the roughness, neither an opacity.
    out.layered = true; // keeps the kind (the demotion below), drawn opaque all the same
    out.blended = out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
    out.emissive = 0.0;
    out.backlight = out.backlightTop = 0.0;
    out.maps[kMr].has = out.maps[kEmissive].has = false;
  }
  if (out.kind == 21 || out.kind == 22 || out.kind == 32) {
    // Opaque and lit; the base alpha is a weight in the shader and no opacity, the glow is the
    // shader's (ICNC is 0 on every material) and the vertex alpha picks no layer.
    out.layered = true;
    out.blended = out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
    out.emissive = 0.0;
    out.maps[kEmissive].has = false;
  }
  if (out.kind == 9) {
    // The glow is all the shader's; the vertex alpha picks the ramp's row and is no
    // opacity, and there is no edge between layers.
    out.layered = true;
    out.blended = out.cutout = out.tinted = out.mask = out.unlit = false;
    out.height = 0.0;
  }
  // The kind the shader asked for: a role that names a kind but left it 0 lacked its maps or colours.
  if (out.kind == 0) {
    static const char* const kKindRoles[] = {"up-layer", "detail", "lava", "parallax", "waterfall",
                                             "glass",    "lava-pool", "gun-glow", "premul-glass", "holo-glass",
                                             "frozen-shell", "matcap-shell", "boundary-shield", "pickup",
                                             "holo",         "holo-refl",    "hologram",     "gun-fx", "vertex-blend", "phazon", "projected-blend", "decal-alpha", "phazon-pool"};
    for (const char* name : kKindRoles) {
      if (out.role.find(name) != std::string::npos) {
        out.reason += std::string("fallback: ") + name + " shader without its maps/params (or blend); ";
      }
    }
  } else {
    out.reason += std::string("kind ") + KindName(out.kind) + " from the shader (" + out.role + "); ";
  }
  // All but lava, premultiplied glass, the holograms (kinds 16-18, one map) and the decal cutouts draw
  // with the second layer's maps.
  if (out.kind != 3 && out.kind != 10 && !(out.kind >= 16 && out.kind <= 18) && out.kind != 24 && out.kind != 27 && out.kind != 34 && !out.layered) {
    if (out.kind != 0) {
      out.reason += std::string("demoted ") + KindName(out.kind) + " to standard: not layered; ";
    }
    if (out.maps[kEmissive].detail) {
      out.maps[kEmissive] = MapRef();
    }
    out.kind = 0;
    out.vcolor = false;
  }
  // A frost shell without its dissolve would cover the gun for good.
  out.hidden = shader == kShaderFrozenShell && out.kind != 12;
  if (out.hidden) {
    out.reason += "frozen-shell: hidden; ";
  }
  if (out.shell) {
    out.reason += "matcap-shell; ";
  }
  if (out.maskSquared) {
    out.reason += "incan-mask-squared: glow masked by base alpha squared; ";
  } else if (out.mask) {
    out.reason += "incan-mask flag: glow masked by base alpha; ";
  }
  if (out.colorUnlit) {
    out.reason += "color-unlit shader; ";
  }
  if (out.kind == 10) {
    out.reason += "premul-glass list: lit premultiplied; ";
  }
  // A material with an AUVI names, per UV output channel, which of the model's
  // texcoord sets feeds it, so a map's texcoord no longer names a set directly.
  // port_remastered_uv.h holds the rule and what the tests drive.
  //
  // Lava pools (kind 6) and waterfalls (kind 7) keep their existing forced UV0.
  // No surveyed material of either kind has AUVI; combined behavior is unverified.
  if (out.kind == 32) {
    // 07acff46 reads AUVI per sampler slot, not by the authored texcoord: x feeds BCLR, y the noise volume, z METL and
    // w NMAP (kb material/07acff46.md), so the set a map uses is its slot's component whatever `@k` it named.
    if (const int32_t* auvi = Auvi(mat)) {
      MapRef* slots[] = {&out.maps[kBase], &out.layer[kBase], &out.maps[kMr], &out.maps[kNormal]};
      for (size_t i = 0; i < std::size(slots); ++i) {
        slots[i]->authored = slots[i]->coord;
        if (auvi[i] >= 0) {
          slots[i]->coord = uint32_t(auvi[i]);
        }
      }
    }
  } else if (out.kind != 6 && out.kind != 7) {
    MapRef* mapRefs[] = {&out.maps[kBase], &out.maps[kMr], &out.maps[kNormal], &out.maps[kEmissive],
                         &out.layer[kBase], &out.layer[kMr], &out.layer[kNormal]};
    uint32_t coords[std::size(mapRefs)];
    for (size_t i = 0; i < std::size(mapRefs); ++i) {
      coords[i] = mapRefs[i]->coord;
      mapRefs[i]->authored = coords[i];
    }
    ApplyAuvi(mat, coords, std::size(coords));
    for (size_t i = 0; i < std::size(mapRefs); ++i) {
      mapRefs[i]->coord = coords[i];
    }
  }
  return out;
}

// The port's material record (CCubeModel::PortSetPBRMaterial), appended to a
// PBR material: emissive multiplier rgb, backlight weight rgb, 'PBRM'; or,
// for a material with a height blend or no lighting, those, the blend's
// threshold, the mode (1 unlit, 2 glow masked by the base alpha, 4 tinted by
// the vertex colour, summed), and 'PBR2'; or, for a layered one, those, the blend's edge width, the scale and offset of
// each layer's height, and 'PBR3'; or, for a shader of its own, those, the kind, its strength
// (linear like the emissive one) and its four parameters, and 'PBR4'; or, where a
// map clamps or mirrors, all nineteen, the maps' wrap modes (one word) and 'PBR5'; or, for the
// back copy of a material with a LITS, those and the diffuse and F0 factors, and 'PBR6'; or,
// for a material with a reflection cube of its own (State::Cube), all of that, the cube's id
// and 'PBR7'. The boundary shield (kind 14) adds a trailer after any of these: its shader's
// constants, CCH0..CCH6 then DIFC (32 floats), and 'PBR8'.
// Glow strengths are stored as Remastered has them, linear and uncapped (they were a capped
// square root while the port had no exposure or bloom to take HDR values). The runtime
// applies the room's exposure to the ones flagged in the mode word (32, 64); an
// inverse-exposed glow (RemMaterial::glowLinear) is what Remastered's screen shows.
// A ColorUnlit surface drawn by its own rule (mode 8): one whose vertex colour
// the port keeps and lights nothing with.
// Remastered's lit PBR shader (60989bc1, USE_TWO_SIDED_MATERIAL) takes s = LITS on a back
// face and multiplies the normal frame by sign(s), the diffuse light by |s| and F0 by 0 where
// s > 0, the frame being the mesh's own. A back copy of a lit material with a positive LITS
// is therefore its own primitive and material, diffuse |s| and F0 0 (record 'PBR6'). Its
// vertices keep the older back copy's turned normals, so the TEV fallback and every other
// reader light it as before; the F0 factor 0 is the marker by which the PBR shader turns
// the normal back (and keeps the front's tangent frame, see shader.cpp). Only the
// ordinary lit material qualifies: unlit, ColorUnlit and the special shader kinds are other
// shaders. LITS of -1 (the flipped normal of the older back copy, factors 1 and 1), no LITS
// and the zero or other negatives the survey did not find keep the older copy.
bool BackLightScale(const RemMaterial& m) {
  return m.hasLits && std::isfinite(m.lits) && m.lits > 0.0 && m.kind == 0 && !m.unlit &&
         !m.colorUnlit;
}

bool ColorUnlitDraw(const RemMaterial& m) { return m.colorUnlit && m.unlit && m.tinted; }

// The PBR record's mode word: 1 unlit, 2 glow masked by the base alpha, 4 tinted by the vertex
// colour, 8 ColorUnlit's draw, 32 a glow the runtime exposes (ExposedGlow), 64 a kind's
// glow strength it exposes (ExposedStrength), 128 the vertex colour tints F0 too.
int PbrMode(const RemMaterial& m);

// Whether the glow is Remastered's bare ICAN x ICNC in scene radiance, which the frame's
// exposure multiplies: the port's runtime scales it (bit 32). Not the inverse-exposed
// glows (glowLinear: they are drawn as the screen shows them), a ramp's mean, nor a
// liquid or glass, whose first three floats are a tint.
bool ExposedGlow(const RemMaterial& m) {
  return !m.glowLinear && m.maps[kEmissive].has && !m.maps[kEmissive].mean && m.kind != 7 && m.kind != 20 &&
         m.kind != 8 && m.kind != 11 && m.kind != 14 && m.kind != 21 && m.kind != 24;
}

// A glow strength of a kind below 5 (the parallax's inside), exposed at run time like the
// emissive map's (mode bit 64).
bool ExposedStrength(const RemMaterial& m) { return m.kind > 0 && m.kind < 5 && m.kindStrength > 0.0; }

// A lit plain (kind 0) surface without REFL reflects nothing around it: every Remastered
// shader that samples a cube has REFL, and no other does (mode bit 256).
// VFX_Model_Base (29D9FDFB, and 1982DB27 without the vertex colour) and LambertFx (4BC890C1) are
// pure Lambert: no specular lobe and no cube (kb material/{29d9fdfb,1982db27,4bc890c1}.md).
bool VfxBase(const RemMaterial& m) {
  return (m.shader == kShaderVfxBase || m.shader == kShaderVfxBase2) && !m.unlit && !ColorUnlitDraw(m) && m.kind == 0;
}
bool PureLambert(const RemMaterial& m) {
  return (VfxBase(m) || (m.family == kShaderLambertFx && !m.unlit && !ColorUnlitDraw(m) && m.kind == 0));
}
// LambertFx's fragment perms (042-057) sample no lightmap, probe volume or BLCM: the light sum starts
// from the flat ambient constant alone (mode bit 262144; kb material/4bc890c1.md, perms 000/001 have no
// fragment shader, they are the depth pass).
bool FlatAmbient(const RemMaterial& m) { return m.family == kShaderLambertFx && PureLambert(m); }
bool NoEnvSpec(const RemMaterial& m) { return (m.noRefl || PureLambert(m)) && !m.unlit && !ColorUnlitDraw(m) && m.kind == 0; }

bool SurfaceUnlit(const RemMaterial& m) {
  return m.unlit && m.kind == 0 && !ColorUnlitDraw(m) &&
         std::find(std::begin(kShaderSurfaceUnlit), std::end(kShaderSurfaceUnlit), m.shader) != std::end(kShaderSurfaceUnlit);
}

int PbrMode(const RemMaterial& m) {
  return (m.unlit ? 1 : 0) + (m.mask ? 2 : 0) + (m.tinted ? 4 : 0) + (ColorUnlitDraw(m) ? 8 : 0) +
         (ExposedGlow(m) ? 32 : 0) + (ExposedStrength(m) ? 64 : 0) +
         (m.tinted && m.tintF0 && !ColorUnlitDraw(m) ? 128 : 0) + (NoEnvSpec(m) ? 256 : 0) +
         (m.cutExact ? 512 : 0) + (PureLambert(m) ? 1024 : 0) + (VfxBase(m) ? 2048 + 4096 : 0) +
         (m.indirect ? 8192 : 0) + (m.macro || m.macroLayered ? 16384 : 0) +
         (m.wind ? 32768 : 0) + (m.wind && m.shader == kShaderVertexAlpha ? 65536 : 0) +
         (SurfaceUnlit(m) ? 131072 : 0) + (FlatAmbient(m) ? 262144 : 0) +
         (std::find(std::begin(kShaderRawVertex), std::end(kShaderRawVertex), m.shader) != std::end(kShaderRawVertex) ? 524288 : 0);
}

void PbrRecord(Blob& b, const RemMaterial& m, uint32_t wrap, uint32_t cube) {
  const double e = std::max(m.emissive, 0.0);
  // ColorUnlit's backlight is its gain, and no backlight at all where drawn otherwise. A lit
  // surface's is the strength from behind, from above, and the fade's power plus 1.
  double k[3] = {0.0, 0.0, 0.0};
  if (ColorUnlitDraw(m)) {
    k[0] = k[1] = k[2] = std::max(m.backlight, 0.0);
  } else if (!m.colorUnlit && !m.unlit && (m.backlight > 0.0 || m.backlightTop > 0.0)) {
    k[0] = std::max(m.backlight, 0.0);
    k[1] = std::max(m.backlightTop, 0.0);
    k[2] = m.backlightFalloff;
  } else if (m.indirect) {
    k[0] = m.indStrength;  // INDS: an unlit surface has no use for the backlight
  }
  std::vector<double> f;
  for (int i = 0; i < 3; ++i) {
    // A liquid has no glow of its own, and its colour goes where the glow's would.
    f.push_back(m.kind == 7 || m.kind == 8 || m.kind == 11 || m.kind == 13 ? m.tint[i] : e);
  }
  for (int i = 0; i < 3; ++i) {
    f.push_back(k[i]);
  }
  const char* tag = "PBRM";
  if (m.height > 0.0 || m.unlit || m.mask || m.layered || m.tinted || m.kind || ExposedGlow(m) || NoEnvSpec(m) || m.cutExact || m.indirect || m.macro || m.macroLayered || m.wind) {
    f.push_back(m.height);
    f.push_back(double(PbrMode(m)));
    tag = "PBR2";
    if (m.layered || m.kind) {
      // Only a blend of two layers has an edge.
      f.push_back(m.kind == 7 || m.kind == 20 || m.kind == 26 || m.kind == 27 || (m.layered && m.kind <= 1) ? m.layerSmooth : 0.0);
      for (double h : m.layerHeight) {
        f.push_back(h);
      }
      tag = "PBR3";
      if (m.kind) {
        f.push_back(double(m.kind));
        f.push_back(m.kind >= 5 ? m.kindStrength : std::max(m.kindStrength, 0.0));
        for (double v : m.kindParam) {
          f.push_back(v);
        }
        tag = "PBR4";
      }
    }
  }
  // Only a material with a map that does not repeat needs the long form: every
  // field, the ones the short form leaves out at the reader's neutral 0.
  const bool scaled = m.lightScale[0] != 1.0 || m.lightScale[1] != 1.0;
  const bool wraps = wrap != 0x55555555u || scaled || cube != 0;
  if (wraps) {
    f.resize(19, 0.0);
    tag = cube != 0 ? "PBR7" : scaled ? "PBR6" : "PBR5";
  }
  for (double v : f) {
    PF(b, v);
  }
  if (wraps) {
    P32(b, wrap);
  }
  if (scaled || cube != 0) {
    PF(b, m.lightScale[0]);
    PF(b, m.lightScale[1]);
  }
  if (cube != 0) {
    P32(b, cube);
  }
  b.insert(b.end(), tag, tag + 4);
  if ((m.kind >= 14 && m.kind <= 19) || m.kind == 21 || m.kind == 22 || m.kind == 32 || m.kind == 25 || m.kind == 28 || m.kind == 29 || m.kind == 31 || m.kind == 33 || m.wind) {
    // The boundary shield's (or pickup's) constants follow the record, as a trailer the reader strips first.
    // A swaying leaf's is the model's SWindSet (v1, v2, rate, b, c) in the first nine.
    for (double v : m.shieldRows) {
      PF(b, v);
    }
    static constexpr char kPbr8[] = "PBR8";
    b.insert(b.end(), kPbr8, kPbr8 + 4);
  }
}

// A retail material rebuilt for the port's PBR path: maps 0-3 are base, MR,
// normal and emissive, each on the texcoord its Remastered map used, lit
// channel 0 kept from retail. The TEV is the fallback the black, shadow,
// thermal and blended paths still draw with (base x lighting + emissive), and
// it samples every map so all are bound. A layered material has three more,
// maps 4-6: the second layer's base, MR and normal.
Blob PbrMaterial(const RetailMaterial& pm, uint32_t vtx, const uint32_t* texIdx, uint32_t group,
                 const uint32_t* coords, const RemMaterial& rem, uint32_t wrap, uint32_t cube,
                 const uint32_t* authored, const AnuvEntry* anim, AnuvCounts& counts) {
  const int nmaps = rem.macroLayered ? kMacroMaps : rem.layered || rem.macro ? kLayeredMaps : kMaps;
  Blob b;
  // ColorUnlit's colour, out of linear light, is (2 x gain)^(1/2.2) times the vertex
  // colour times the base map, both as stored: a stage multiplies by half that
  // (konst 0) and doubles. Exact bar the tone curve, which TEV has none of.
  const bool colorUnlit = ColorUnlitDraw(rem);
  // The emissive map is stored as authored (the PBR path scales it at run time), but the
  // fallback adds it as it is: it took the 0.10 the converter used to bake in, as a konst
  // (in the gamma domain TEV works in, that is 0.10^(1/2.2) of the byte).
  const bool emissiveKonst = rem.maps[kEmissive].has && !rem.maps[kEmissive].mean && !rem.maps[kEmissive].raw &&
                             !rem.maps[kEmissive].detail;
  const uint32_t emissiveSel = colorUnlit ? 0x0Du : 0x0Cu;  // GX_TEV_KCSEL_K1 / K0
  const uint32_t flags = (pm.flags & 0xFFFF & ~uint32_t(0x8 | 0x40 | 0x100 | 0x400 | 0x800 | 0x2000)) |
                         0xF0000 | kPbrFlag | (colorUnlit || emissiveKonst ? 0x8u : 0u);
  P32(b, flags);
  P32(b, uint32_t(nmaps));
  for (int i = 0; i < nmaps; ++i) {
    P32(b, texIdx[i]);
  }
  // The vertex descriptor is retail's: it fixes how many texcoord attributes,
  // and so how many indices per vertex, the display list carries.
  P32(b, vtx);
  // The material's cache id: CCubeMaterial::SetCurrent skips the vertex layout
  // and TEV when it matches the previous draw's, so it must differ from retail's.
  P32(b, group);
  if (colorUnlit || emissiveKonst) {
    P32(b, (colorUnlit ? 1u : 0u) + (emissiveKonst ? 1u : 0u));
    if (colorUnlit) {
      const double half = 0.5 * std::pow(2.0 * std::max(rem.backlight, 0.0), 1.0 / 2.2);
      const uint32_t k = uint32_t(std::lround(std::clamp(half, 0.0, 1.0) * 255.0));
      P32(b, k << 24 | k << 16 | k << 8 | 0xFF);
    }
    if (emissiveKonst) {
      const uint32_t k = uint32_t(std::lround(std::pow(kPbrEmissive, 1.0 / 2.2) * 255.0));
      P32(b, k << 24 | k << 16 | k << 8 | 0xFF);
    }
  }
  // The matcap shell (kind 13) is drawn premultiplied, GX_BL_ONE and GX_BL_INVSRCALPHA: its
  // shader scales the diffuse by the opacity, and the rim and reflection are added unscaled.
  // The boundary shield (kind 14) and cracked glass (kind 8, class 1 with the scene in rgb) are plainly alpha blended, GX_BL_SRCALPHA and GX_BL_INVSRCALPHA.
  // The pickup (kind 15) is SrcAlpha with the mesh class's destination: One where it is additive.
  // 4BC890C1 (a lit Lambert over a retail effect) is opaque, as Remastered's mesh class 0 draws it.
  const bool lambertFx = rem.family == kShaderLambertFx || rem.opaqueFx;
  P16(b, rem.kind == 8 || rem.kind == 13 || rem.kind == 14 || rem.kind == 29 || rem.kind == 34 ? 5 : (rem.kind >= 15 && rem.kind <= 18) || rem.kind == 31 || rem.kind == 33 ? (rem.additive ? 1 : 5) : rem.kind == 19 ? (GunFxParticle(pm) ? 5 : 0) : lambertFx ? 0 : pm.blendDst);
  P16(b, rem.kind == 8 || rem.kind == 14 || rem.kind == 29 || rem.kind == 34 || (rem.kind >= 15 && rem.kind <= 18) || rem.kind == 31 || rem.kind == 33 ? 4 : rem.kind == 19 ? (GunFxParticle(pm) ? 4 : 1) : rem.kind == 13 || lambertFx ? 1 : pm.blendSrc);
  // An unlit surface coloured by its vertices (a door shield) keeps that in the
  // fallback too, which is what draws it whenever the model is not opaque: channel
  // 0 unlit with the vertex colour as its material colour (bit 2), and the alpha
  // taken from it as well.
  const bool vertexGlow = rem.unlit && rem.tinted;
  P32(b, vertexGlow ? 1u : uint32_t(pm.chans.size()));
  if (vertexGlow) {
    P32(b, 0x4);
  } else {
    for (uint32_t c : pm.chans) {
      P32(b, c);
    }
  }
  static const uint32_t tev[kLayeredMaps][3] = {
      {0x7A14F, 0x21CE7, 4},  // ZERO, RASC, TEXC, ZERO: base x channel 0; alpha = base
      {0x3D0F, 0x1CE7, 255},  // ZERO, TEXC, ZERO, CPREV: MR, sampled only
      {0x3D0F, 0x1CE7, 255},  // normal, sampled only
      {0x310F, 0x1CE7, 255},  // ZERO, TEXC, ONE, CPREV: + emissive
      {0x3D0F, 0x1CE7, 255},  // the second layer, sampled only
      {0x3D0F, 0x1CE7, 255},
      {0x3D0F, 0x1CE7, 255},
  };
  // Glass samples the frame behind it, which the game copies into map 7 (the
  // spare buffer's) before it draws one: a stage of its own binds it.
  const int nstages = rem.kind == 8 || rem.kind == 11 || rem.kind == 14 || rem.kind == 23 || rem.kind == 29 || rem.kind == 30 || rem.kind == 34 ? nmaps + 1 : nmaps;
  P32(b, uint32_t(nstages));
  for (int i = 0; i < nstages; ++i) {
    // Kind 20's map 3 is a detail map, sampled only (the table's stage adds it as glow).
    const uint32_t* t = tev[rem.maps[kEmissive].detail && i == kEmissive ? 2 : std::min(i, kLayeredMaps - 1)];
    const bool gain = colorUnlit && i == 1;
    const bool emissiveStage = emissiveKonst && i == kEmissive;
    P32(b, gain ? 0x7B80Fu : emissiveStage ? 0x390Fu : t[0]);  // gain: ZERO, CPREV, KONST, ZERO, then x2; emissive: ZERO, TEXC, KONST, CPREV
    P32(b, i == 0 && vertexGlow ? 0x39487u : t[1]);  // ZERO, TEXA, RASA, ZERO: base x vertex alpha
    P32(b, gain ? 0x140u : 0x100u);
    P32(b, 0x100);
    P8(b, 0);
    P8(b, 0);
    P8(b, gain ? 0x0C : emissiveStage ? emissiveSel : 0);  // GX_TEV_KCSEL_K0 (K1)
    P8(b, uint8_t(t[2]));
  }
  for (int i = 0; i < nstages; ++i) {
    P8(b, 0);
    P8(b, 0);
    // A macro-normal material has no second layer: its maps 4 and 5 are only placeholders, sampled
    // from map 0 so that the shader sees map 6 (the macro normal) without a layer base.
    P8(b, uint8_t(rem.macro && !rem.layered && (i == 4 || i == 5) ? 0 : i < nmaps ? i : 7));
    P8(b, uint8_t(i < nmaps ? coords[i] : 0));
  }
  // One texgen per texcoord the maps use. Each reads its own texcoord through the
  // identity (GX_TG_MTX3x4, TEXi, GX_IDENTITY, GX_PTIDENTITY): texgen word 0 would be
  // position through GX_TEXMTX0, a projection by whatever matrix the last animated
  // material loaded, which slid Magmoor's room rock along with the lava. Where
  // retail's texgen for the coord reads that same texcoord it is kept, with retail's
  // UV animations, so an animated coordinate still drives the maps; one over position
  // or normal (an env map) says nothing about a Remastered texcoord.
  const uint32_t n = 1 + *std::max_element(coords, coords + nmaps);
  std::vector<uint32_t> gens(n);
  for (uint32_t i = 0; i < n; ++i) {
    gens[i] = 0x1EBC00 | ((4 + i) << 4);
  }
  // A scroll of Remastered's own replaces retail's animations: the base map's
  // texcoord goes through GX_TEXMTX0, which a UV scroll (mode 2: offset, then
  // speed per second) loads.
  // The model's own ANUV animations win over both: each texcoord slot a map reads
  // through a transform that moves goes through a matrix of its own (GX_TEXMTXn, n
  // the animation's place in the list, as SetCurrent loads them), from the set the
  // AUVI picked. `authored` is the texcoord index the material named, which is what
  // picks the entry's transform.
  std::vector<const AnuvTransform*> moving;
  std::vector<int> matrixOf(n, -1);
  for (int i = 0; anim != nullptr && i < nmaps; ++i) {
    const uint32_t a = authored[i];
    if (a >= 3 || anim->xf[a].Identity()) {
      continue;
    }
    if (matrixOf[coords[i]] < 0) {
      matrixOf[coords[i]] = int(moving.size());
      moving.push_back(&anim->xf[a]);
    } else if (moving[size_t(matrixOf[coords[i]])] != &anim->xf[a]) {
      ++counts.sharedSlots;  // two transforms on one slot: the first drives both
    }
  }
  const bool animated = !moving.empty();
  counts.animated += animated ? 1 : 0;
  const bool scroll = !animated && (rem.scroll[0] != 0.0 || rem.scroll[1] != 0.0);
  counts.scrollOverridden += animated && (rem.scroll[0] != 0.0 || rem.scroll[1] != 0.0) ? 1 : 0;
  bool retailGens = false;
  for (int i = 0; i < nmaps && !scroll && !animated; ++i) {
    const uint32_t c = coords[i];
    if (c < pm.texgen.size() && ((pm.texgen[c] >> 4) & 31) == 4 + c) {
      gens[c] = pm.texgen[c];
      retailGens = true;
    }
  }
  if (scroll) {
    gens[coords[kBase]] = 0x1E8000 | ((4 + coords[kBase]) << 4);  // GX_TG_MTX3x4, TEXc, GX_TEXMTX0, GX_PTIDENTITY
  }
  for (uint32_t c = 0; c < n; ++c) {
    if (matrixOf[c] >= 0) {
      gens[c] = 0x1E8000 | (uint32_t(3 * matrixOf[c]) << 9) | ((4 + c) << 4);  // TEXMTX0 + 3 per matrix
    }
  }
  P32(b, n);
  for (uint32_t g : gens) {
    P32(b, g);
  }
  if (animated) {
    std::vector<uint32_t> words;
    for (const AnuvTransform* xf : moving) {
      const std::vector<uint32_t> w = AnuvAnimWords(*xf);
      words.insert(words.end(), w.begin(), w.end());
    }
    P32(b, uint32_t(2 + words.size()) * 4);  // the section's size, the count, then the animations
    P32(b, uint32_t(moving.size()));
    for (uint32_t w : words) {
      P32(b, w);
    }
  } else if (scroll) {
    P32(b, 24);  // the count and one animation of five words
    P32(b, 1);
    P32(b, 2);
    PF(b, 0.0);
    PF(b, 0.0);
    PF(b, rem.scroll[0]);
    PF(b, rem.scroll[1]);
  } else if (retailGens && !pm.uvAnim.empty()) {
    b.insert(b.end(), pm.uvAnim.begin(), pm.uvAnim.end());
  } else {
    P32(b, 4);  // no UV animations
    P32(b, 0);
  }
  PbrRecord(b, rem, wrap, cube);
  return b;
}

// The column given to each row of an n x m cost matrix (n <= m, row-major)
// that minimises the total cost, each column used at most once (Hungarian).
static std::vector<size_t> AssignRows(const std::vector<double>& cost, size_t n, size_t m) {
  const double inf = std::numeric_limits<double>::infinity();
  std::vector<double> u(n + 1, 0.0), v(m + 1, 0.0);
  std::vector<size_t> p(m + 1, 0), way(m + 1, 0);
  for (size_t i = 1; i <= n; ++i) {
    p[0] = i;
    size_t j0 = 0;
    std::vector<double> minv(m + 1, inf);
    std::vector<bool> used(m + 1, false);
    do {
      used[j0] = true;
      const size_t i0 = p[j0];
      double delta = inf;
      size_t j1 = 0;
      for (size_t j = 1; j <= m; ++j) {
        if (used[j]) {
          continue;
        }
        const double cur = cost[(i0 - 1) * m + (j - 1)] - u[i0] - v[j];
        if (cur < minv[j]) {
          minv[j] = cur;
          way[j] = j0;
        }
        if (minv[j] < delta) {
          delta = minv[j];
          j1 = j;
        }
      }
      for (size_t j = 0; j <= m; ++j) {
        if (used[j]) {
          u[p[j]] += delta;
          v[j] -= delta;
        } else {
          minv[j] -= delta;
        }
      }
      j0 = j1;
    } while (p[j0] != 0);
    do {
      const size_t j1 = way[j0];
      p[j0] = p[j1];
      j0 = j1;
    } while (j0 != 0);
  }
  std::vector<size_t> col(n, 0);
  for (size_t j = 1; j <= m; ++j) {
    if (p[j] != 0) {
      col[p[j] - 1] = j - 1;
    }
  }
  return col;
}

// One weight per vertex and bone, from the Remastered joints or, for a static
// Remastered model, from the nearest retail vertex.
//
// Each vertex votes its weight on a joint for the bones of the nearest retail
// vertex. A joint takes the bone with the most votes, plus any other with a
// kJointSplit share: several joints sharing a bone is the normal case, and a
// joint covering two retail bones is split between them per vertex, so the
// seam falls where retail has it.
std::vector<WeightKey> SkinWeights(const std::vector<double>& P, size_t n, const std::vector<uint16_t>* J,
                                   const std::vector<float>* W, const Retail& retail, Span cskr,
                                   const std::function<void(const std::string&)>& log) {
  const std::vector<SkinGroup> groups = ReadCskr(cskr);
  std::vector<uint32_t> gid;
  for (size_t i = 0; i < groups.size(); ++i) {
    gid.insert(gid.end(), groups[i].count, uint32_t(i));
  }
  const size_t nr = std::min(gid.size(), retail.Count());
  if (nr == 0) {
    throw Fail{"the retail skin covers no vertices"};
  }
  const double* rp = retail.P.data();
  std::vector<uint32_t> nn;
  Nearest(P.data(), n, rp, nr, nn);
  std::vector<std::vector<std::pair<uint32_t, double>>> weights(n);
  if (J && W) {
    std::vector<uint32_t> bones;
    for (const SkinGroup& g : groups) {
      for (const auto& w : g.weights) {
        bones.push_back(w.first);
      }
    }
    std::sort(bones.begin(), bones.end());
    bones.erase(std::unique(bones.begin(), bones.end()), bones.end());
    const size_t nb = bones.size();
    // The retail vertices' weights, a row per vertex and a column per bone.
    std::vector<double> GW(groups.size() * nb, 0.0);
    for (size_t i = 0; i < groups.size(); ++i) {
      for (const auto& w : groups[i].weights) {
        const size_t col = size_t(std::lower_bound(bones.begin(), bones.end(), w.first) - bones.begin());
        GW[i * nb + col] += double(w.second);
      }
    }
    auto RW = [&](size_t v) { return &GW[size_t(gid[v]) * nb]; };
    uint32_t maxJoint = 0;
    for (size_t i = 0; i < n * 4; ++i) {
      maxJoint = std::max<uint32_t>(maxJoint, (*J)[i]);
    }
    const size_t nj = size_t(maxJoint) + 1;
    std::vector<double> V(nj * nb, 0.0);
    for (int k = 0; k < 4; ++k) {
      for (size_t v = 0; v < n; ++v) {
        const double w = double((*W)[v * 4 + k]);
        const double* row = RW(nn[v]);
        double* dst = &V[size_t((*J)[v * 4 + k]) * nb];
        for (size_t c = 0; c < nb; ++c) {
          dst[c] += w * row[c];
        }
      }
    }
    std::vector<std::vector<uint32_t>> assign(nj);
    size_t mapped = 0, split = 0;
    std::vector<double> share(nb);
    for (size_t j = 0; j < nj; ++j) {
      const double total = RowSum(&V[j * nb], nb);
      if (!(total > 0.0)) {
        continue;
      }
      for (size_t c = 0; c < nb; ++c) {
        share[c] = V[j * nb + c] / total;
      }
      std::vector<uint32_t> order(nb);
      for (size_t c = 0; c < nb; ++c) {
        order[c] = uint32_t(c);
      }
      std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return share[a] > share[b]; });
      for (uint32_t c : order) {
        if (share[c] >= kJointSplit) {
          assign[j].push_back(c);
        }
      }
      if (assign[j].empty()) {
        assign[j].push_back(order[0]);
      }
      ++mapped;
    }
    // A rig cut finer than retail's, both of rigid pieces (the iris doors: each
    // of six leaves is a stack of layers and plates there, one piece here): a
    // piece split between bones tears, and the vote is no guide, since retail's
    // few vertices on a wide leaf are nearer the next leaf's edge than its own.
    // Such a piece rides the bone whose piece is nearest its own, whole.
    //
    // A rig as fine as retail's (the Frigate's big doors: two rings of ten
    // pieces each side) has the same tear wherever the vote splits a rigid
    // piece, but nearest alone can send two pieces to one bone and leave
    // another bone, and what it moves, empty. There every bone takes the piece
    // that the cheapest one-to-one pairing of centres gives it, and any piece
    // left over the nearest bone.
    size_t rehomed = 0;
    bool paired = false;
    const bool rigidRetail = std::all_of(groups.begin(), groups.end(),
                                         [](const SkinGroup& g) { return g.weights.size() == 1; });
    if (rigidRetail && mapped * 10 >= nb * 9) {
      std::vector<double> bc(nb * 3, 0.0), bn(nb, 0.0), jc(nj * 3, 0.0), jn(nj, 0.0);
      std::vector<bool> loose(nj, false);
      for (size_t v = 0; v < nr; ++v) {
        const double* row = RW(v);
        const size_t c = size_t(std::max_element(row, row + nb) - row);
        for (int a = 0; a < 3; ++a) {
          bc[c * 3 + a] += rp[v * 3 + a];
        }
        bn[c] += 1.0;
      }
      for (size_t v = 0; v < n; ++v) {
        for (int k = 0; k < 4; ++k) {
          const double w = double((*W)[v * 4 + k]);
          const size_t j = (*J)[v * 4 + k];
          if (w >= 0.999) {
            for (int a = 0; a < 3; ++a) {
              jc[j * 3 + a] += P[v * 3 + a];
            }
            jn[j] += 1.0;
          } else if (w >= 1e-3) {
            loose[j] = true;
          }
        }
      }
      auto dist = [&](size_t j, size_t c) {
        double d = 0.0;
        for (int a = 0; a < 3; ++a) {
          const double e = jc[j * 3 + a] / jn[j] - bc[c * 3 + a] / bn[c];
          d += e * e;
        }
        return d;
      };
      auto nearest = [&](size_t j) {
        double best = -1.0;
        for (size_t c = 0; c < nb; ++c) {
          if (bn[c] == 0.0) {
            continue;
          }
          const double d = dist(j, c);
          if (best < 0.0 || d < best) {
            assign[j] = {uint32_t(c)};
            best = d;
          }
        }
      };
      if (mapped >= 2 * nb) {
        for (size_t j = 0; j < nj; ++j) {
          if (assign[j].empty() || loose[j] || jn[j] == 0.0) {
            continue;
          }
          nearest(j);
          ++rehomed;
        }
      } else {
        std::vector<size_t> pieces;
        bool rigid = true, torn = false;
        for (size_t j = 0; j < nj; ++j) {
          if (assign[j].empty()) {
            continue;
          }
          rigid = rigid && !loose[j] && jn[j] > 0.0;
          torn = torn || assign[j].size() > 1;
          pieces.push_back(j);
        }
        const bool everyBone = std::all_of(bn.begin(), bn.end(), [](double c) { return c > 0.0; });
        const size_t m = pieces.size();
        if (rigid && torn && everyBone && m >= nb) {
          std::vector<double> cost(nb * m);
          for (size_t c = 0; c < nb; ++c) {
            for (size_t i = 0; i < m; ++i) {
              cost[c * m + i] = dist(pieces[i], c);
            }
          }
          const std::vector<size_t> col = AssignRows(cost, nb, m);
          std::vector<bool> taken(m, false);
          for (size_t c = 0; c < nb; ++c) {
            assign[pieces[col[c]]] = {uint32_t(c)};
            taken[col[c]] = true;
          }
          for (size_t i = 0; i < m; ++i) {
            if (!taken[i]) {
              nearest(pieces[i]);
            }
          }
          rehomed = m;
          paired = true;
        } else if (torn && everyBone && m <= nb &&
                   std::all_of(pieces.begin(), pieces.end(), [&](size_t j) { return jn[j] > 0.0; })) {
          // A rig as fine as retail's or a piece short, not quite rigid (the
          // arm cannon: 34 or 35 joints on 35 bones, the four muzzle petals
          // among them, split between each other by the vote, so the open
          // petals tore into a box). Each joint takes its own bone, and the
          // few vertices blended between joints stay blended, between their
          // bones. The pairing maximises the vote, not closeness: the gun's
          // stack of three rings has a bone between them, and by centres
          // alone every ring slid a step onto its neighbour's bone.
          std::vector<double> cost(m * nb);
          for (size_t i = 0; i < m; ++i) {
            const double* row = &V[pieces[i] * nb];
            const double total = RowSum(row, nb);
            for (size_t c = 0; c < nb; ++c) {
              cost[i * nb + c] = 1.0 - row[c] / total;
            }
          }
          const std::vector<size_t> col = AssignRows(cost, m, nb);
          for (size_t i = 0; i < m; ++i) {
            assign[pieces[i]] = {uint32_t(col[i])};
          }
          rehomed = m;
          paired = true;
        }
      }
    }
    for (size_t j = 0; j < nj; ++j) {
      split += assign[j].size() > 1;
    }
    // MP_REMASTERED_JOINTS=1: each joint's vote and the bones it got, with the
    // centres of its vertices and of each retail bone's.
    if (port::EnvFlag("MP_REMASTERED_JOINTS")) {
      char buf[256];
      std::vector<double> bc(nb * 3, 0.0), bn(nb, 0.0), jc(nj * 3, 0.0), jn(nj, 0.0);
      for (size_t v = 0; v < nr; ++v) {
        const double* row = RW(v);
        const size_t c = size_t(std::max_element(row, row + nb) - row);
        for (int a = 0; a < 3; ++a) {
          bc[c * 3 + a] += rp[v * 3 + a];
        }
        bn[c] += 1.0;
      }
      for (size_t v = 0; v < n; ++v) {
        const size_t k = size_t(std::max_element(W->begin() + v * 4, W->begin() + v * 4 + 4) - (W->begin() + v * 4));
        const size_t j = (*J)[v * 4 + k];
        for (int a = 0; a < 3; ++a) {
          jc[j * 3 + a] += P[v * 3 + a];
        }
        jn[j] += 1.0;
      }
      size_t looseVerts = 0, multiGroups = 0;
      for (size_t v = 0; v < n * 4; ++v) {
        looseVerts += (*W)[v] >= 1e-3 && (*W)[v] < 0.999;
      }
      for (const SkinGroup& g : groups) {
        multiGroups += g.weights.size() > 1;
      }
      log("  rigid retail " + std::to_string(rigidRetail) + " (" + std::to_string(multiGroups) + " of " +
          std::to_string(groups.size()) + " groups blended), " + std::to_string(looseVerts) + " partial weights of " +
          std::to_string(n));
      for (size_t c = 0; c < nb; ++c) {
        const double d = std::max(bn[c], 1.0);
        std::snprintf(buf, sizeof(buf), "  bone %u: %d verts at (%.3f %.3f %.3f)", bones[c], int(bn[c]),
                      bc[c * 3] / d, bc[c * 3 + 1] / d, bc[c * 3 + 2] / d);
        log(buf);
      }
      for (size_t j = 0; j < nj; ++j) {
        const double total = RowSum(&V[j * nb], nb);
        if (!(total > 0.0)) {
          continue;
        }
        const double d = std::max(jn[j], 1.0);
        std::string line;
        std::snprintf(buf, sizeof(buf), "  joint %zu: %d verts at (%.3f %.3f %.3f) ->", j, int(jn[j]), jc[j * 3] / d,
                      jc[j * 3 + 1] / d, jc[j * 3 + 2] / d);
        line = buf;
        for (uint32_t c : assign[j]) {
          std::snprintf(buf, sizeof(buf), " %u(%.2f)", bones[c], V[j * nb + c] / total);
          line += buf;
        }
        line += ", vote";
        std::vector<uint32_t> order(nb);
        for (size_t c = 0; c < nb; ++c) {
          order[c] = uint32_t(c);
        }
        std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return V[j * nb + a] > V[j * nb + b]; });
        for (size_t i = 0; i < std::min<size_t>(3, nb) && V[j * nb + order[i]] > 0.0; ++i) {
          std::snprintf(buf, sizeof(buf), " %u(%.2f)", bones[order[i]], V[j * nb + order[i]] / total);
          line += buf;
        }
        log(line);
      }
    }
    log("  joints mapped " + std::to_string(mapped) + ", " + std::to_string(split) + " split" +
        (rehomed ? ", " + std::to_string(rehomed) + (paired ? " rigid paired by piece" : " rigid by nearest piece")
                 : std::string()));
    std::vector<double> VW(n * nb, 0.0);
    std::vector<uint32_t> sel, on, near;
    std::vector<double> selP, onP;
    for (size_t j = 0; j < nj; ++j) {
      const std::vector<uint32_t>& cs = assign[j];
      if (cs.empty()) {
        continue;
      }
      for (int k = 0; k < 4; ++k) {
        sel.clear();
        for (size_t v = 0; v < n; ++v) {
          if ((*J)[v * 4 + k] == j && double((*W)[v * 4 + k]) >= 1e-3) {
            sel.push_back(uint32_t(v));
          }
        }
        if (sel.empty()) {
          continue;
        }
        if (cs.size() == 1) {
          for (uint32_t v : sel) {
            VW[size_t(v) * nb + cs[0]] += double((*W)[size_t(v) * 4 + k]);
          }
          continue;
        }
        // A split joint hands each vertex the weights of the nearest retail
        // vertex that is on one of the joint's bones, restricted to those
        // bones, so a vertex can never leave the joint's own bones.
        on.clear();
        onP.clear();
        std::vector<double> part(cs.size());
        for (size_t r = 0; r < nr; ++r) {
          for (size_t c = 0; c < cs.size(); ++c) {
            part[c] = RW(r)[cs[c]];
          }
          if (RowSum(part.data(), part.size()) > 0.0) {
            on.push_back(uint32_t(r));
            onP.insert(onP.end(), rp + 3 * r, rp + 3 * r + 3);
          }
        }
        selP.clear();
        for (uint32_t v : sel) {
          selP.insert(selP.end(), P.begin() + 3 * size_t(v), P.begin() + 3 * size_t(v) + 3);
        }
        Nearest(selP.data(), sel.size(), onP.data(), on.size(), near);
        for (size_t i = 0; i < sel.size(); ++i) {
          const double* row = RW(on[near[i]]);
          for (size_t c = 0; c < cs.size(); ++c) {
            part[c] = row[cs[c]];
          }
          const double total = RowSum(part.data(), part.size());
          const double w = double((*W)[size_t(sel[i]) * 4 + k]);
          for (size_t c = 0; c < cs.size(); ++c) {
            VW[size_t(sel[i]) * nb + cs[c]] += w * part[c] / total;
          }
        }
      }
    }
    size_t lost = 0;
    for (size_t v = 0; v < n; ++v) {
      double* row = &VW[v * nb];
      if (RowSum(row, nb) <= 0.0) {
        // No weighted joint of this vertex has a bone: take the nearest retail
        // vertex's weights rather than pin it to an arbitrary bone.
        std::copy(RW(nn[v]), RW(nn[v]) + nb, row);
        ++lost;
      }
      for (size_t c = 0; c < nb; ++c) {
        if (row[c] > 0.0) {
          weights[v].emplace_back(bones[c], row[c]);
        }
      }
    }
    if (lost) {
      log("  " + std::to_string(lost) + " vertices with no mapped joint: weights from the nearest retail vertex");
    }
  } else {
    log("  static Remastered model on a skinned retail one: weights from the nearest retail vertex");
    for (size_t v = 0; v < n; ++v) {
      for (const auto& w : groups[gid[nn[v]]].weights) {
        bool found = false;
        for (auto& have : weights[v]) {
          if (have.first == w.first) {
            have.second = double(w.second);
            found = true;
          }
        }
        if (!found) {
          weights[v].emplace_back(w.first, double(w.second));
        }
      }
    }
  }
  // Quantise to 1/64 and renormalise, so vertices that differ by noise share a
  // weight group.
  std::vector<WeightKey> out(n);
  for (size_t v = 0; v < n; ++v) {
    auto& ws = weights[v];
    if (ws.size() > kMaxSkinWeights) {
      std::stable_sort(ws.begin(), ws.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
      ws.resize(kMaxSkinWeights);
    }
    double total = 0.0;
    for (const auto& w : ws) {
      total += w.second;
    }
    WeightKey q;
    if (total <= 0.0) {
      if (!ws.empty()) {
        q.emplace_back(ws[0].first, 1.0);  // no usable weights: the strongest (first) bone alone
      }
      out[v] = std::move(q);
      continue;
    }
    for (const auto& w : ws) {
      const double r = std::nearbyint(w.second / total * 64.0);
      if (r != 0.0) {
        q.emplace_back(w.first, r / 64.0);
      }
    }
    std::sort(q.begin(), q.end());
    double s = 0.0;
    for (const auto& w : q) {
      s += w.second;
    }
    if (s > 0.0) {
      for (auto& w : q) {
        w.second /= s;
      }
    }
    out[v] = std::move(q);
  }
  return out;
}

Blob CskrBytes(const std::vector<std::pair<const WeightKey*, uint32_t>>& runs, size_t total) {
  Blob b;
  P32(b, uint32_t(runs.size()));
  for (const auto& run : runs) {
    P32(b, uint32_t(run.first->size()));
    for (const auto& w : *run.first) {
      P32(b, w.first);
      PF(b, w.second);
    }
    P32(b, run.second);
  }
  P32(b, 0xFFFFFFFFu);
  P32(b, uint32_t(total));
  P32(b, 0xFFFFFFFFu);
  P32(b, uint32_t(total));
  return b;
}

void SurfaceHeader(Blob& s, const double* centre, uint32_t mat, size_t dlSize, uint32_t extra) {
  for (int c = 0; c < 3; ++c) {
    PF(s, centre[c]);
  }
  P32(s, mat);
  P32(s, uint32_t(dlSize) | 0x80000000u);
  P32(s, 0);
  P32(s, 0);
  P32(s, extra);
  P32(s, 0);
  P32(s, 0);
  PF(s, 1.0);
}

} // namespace

void Converter::State::Convert(const Model& model, const ConvertOptions& opt) {
  Retail retail;
  if (opt.standalone) {
    // What retail's own opaque lit world materials carry (measured over every
    // retail CMDL: flags 0x1083 and one channel word 0x3001 are the common case).
    retail.nmat = 1;
    retail.flags = 0x4;  // the packed texcoord section is there, empty
    RetailMaterial lit;
    lit.flags = 0x1083;
    lit.vtx = 0xF;  // position and normal; the texcoord slots follow the model
    lit.blendSrc = 1;
    lit.blendDst = 0;
    lit.chans = {0x3001};
    retail.mats.push_back(lit);
    // And the same with retail's alpha test, for a Remastered cutout.
    lit.flags |= 0x20;
    retail.mats.push_back(lit);
    // And as retail's blended surfaces are: drawn in the sorted pass over what
    // is behind them, tested against the depth buffer without writing to it.
    lit.flags = 0x1013;
    lit.blendSrc = 4;  // GX_BL_SRCALPHA, GX_BL_INVSRCALPHA
    lit.blendDst = 5;
    retail.mats.push_back(lit);
    // And glass, whose colour is premultiplied: GX_BL_ONE, GX_BL_INVSRCALPHA.
    lit.blendSrc = 1;
    retail.mats.push_back(lit);
    // And an additive surface (mesh class 3): GX_BL_SRCALPHA, GX_BL_ONE.
    lit.blendSrc = 4;
    lit.blendDst = 1;
    retail.mats.push_back(lit);
  } else {
    if (!io.retail(FourCC('C', 'M', 'D', 'L'), opt.retail, retail.data)) {
      throw Fail{"retail model " + Hex8(opt.retail) + " is not on the disc"};
    }
    retail.Parse();
  }

  const double (*M)[3] = opt.orient;
  const double det = M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) -
                     M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
                     M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);

  // Primitives in GameCube space; a vertex buffer is shared by many meshes and
  // is transformed once.
  std::vector<RemMaterial> mats;
  for (const ModelMaterial& m : model.materials) {
    mats.push_back(ReadMaterial(m, opt));
    // The wind a material sways with: its index into the model's WIND sets (0xFF none). Only the plain lit
    // foliage (kind 0) of a standalone model, whose vertices can carry the weight in their colour alpha.
    RemMaterial& rem = mats.back();
    const size_t at = mats.size() - 1;
    if (opt.standalone && rem.kind == 0 && !rem.unlit && !rem.colorUnlit && !rem.blended && !rem.layered &&
        std::find(std::begin(kShaderWindFoliage), std::end(kShaderWindFoliage), rem.shader) !=
            std::end(kShaderWindFoliage) &&
        at < model.windMaterialSet.size() && model.windMaterialSet[at] < model.windSets.size()) {
      rem.wind = true;
      rem.vcolor = true;
      for (int i = 0; i < 9; ++i) {
        rem.shieldRows[i] = ShortestDouble(model.windSets[model.windMaterialSet[at]][size_t(i)]);
      }
    }
  }
  // How Remastered blends a mesh is its class (the two-bit map after the meshes),
  // not a material flag: 0 opaque, 2 alpha tested, 1 sorted and blended straight
  // (SrcA, InvSrcA), 3 sorted and added (SrcA, One). Every model gives all of a
  // material's meshes the same class, and 1 and 3 only to materials flagged 0x1.
  // An additive surface's alpha is its opacity unless the base alpha masks the glow.
  for (const ModelMesh& mesh : model.meshes) {
    if (mesh.bits2 == 3 && mesh.material < mats.size()) {
      RemMaterial& m = mats[mesh.material];
      m.additive = true;
      m.blended = m.blended || (!m.mask && !m.cutout);
    }
  }
  // The model's animated UVs: the map names, per mesh, the entry whose transforms move
  // the material's texcoords. Meshes sharing a material are expected to agree; if they
  // don't, the first one's entry is used.
  Anuv anuv;
  AnuvCounts anuvCounts;
  if (!model.anuv.empty()) {
    std::string anuvError;
    if (!ParseAnuv(model.anuv.data(), model.anuv.size(), anuv, anuvError)) {
      Log("  note: ANUV not read: " + anuvError);
    }
    std::vector<int> entryOf(mats.size(), -2);
    for (size_t mi = 0; mi < model.meshes.size() && mi < anuv.matmap.size(); ++mi) {
      const uint32_t mat = model.meshes[mi].material;
      const int entry = anuv.matmap[mi] == 0xff || anuv.matmap[mi] >= anuv.entries.size() ? -1 : anuv.matmap[mi];
      if (mat >= mats.size()) {
        continue;
      }
      if (entryOf[mat] == -2) {
        entryOf[mat] = entry;
      } else if (entryOf[mat] != entry) {
        ++anuvCounts.entryDisagree;
      }
    }
    for (size_t i = 0; i < mats.size(); ++i) {
      if (entryOf[i] >= 0 && anuv.entries[size_t(entryOf[i])].skip == AnuvSkip::None) {
        mats[i].anuv = entryOf[i];
      } else if (entryOf[i] >= 0) {
        ++anuvCounts.notFlattened;
      }
    }
  }
  std::vector<Buffer> buffers(model.vertexBuffers.size());
  std::vector<Prim> prims;
  std::vector<uint32_t> bufOrder;  // buffers in the order the primitives reach them
  // Each level of detail is a set of meshes of its own, so drawing every mesh
  // stacks the coarse copies on the fine one. Keep the chosen level's.
  std::vector<bool> finest(model.meshes.size(), false);
  bool anyFinest = false;
  std::unordered_map<uint64_t, uint32_t> vmshIndex;  // (buffer, vertex) -> its place in vmshV
  std::vector<float> vmshV;                          // pos, nrm, uv per vertex
  std::vector<uint32_t> vmshI;
  const size_t lodBase = size_t(std::max(opt.lod, 0)) * 5;
  if (opt.lod > 0 && lodBase >= model.lods.size()) {
    throw Fail{"the model has no level of detail " + std::to_string(opt.lod)};
  }
  for (size_t r = lodBase; r < lodBase + 5 && r < model.lods.size(); ++r) {
    const ModelLod& range = model.lods[r];
    for (uint64_t i = range.indexOffset; i < uint64_t(range.indexOffset) + range.indexCount; ++i) {
      if (i < model.lodMeshes.size() && model.lodMeshes[i] < finest.size()) {
        finest[model.lodMeshes[i]] = true;
        anyFinest = true;
      }
    }
  }
  for (size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex) {
    const ModelMesh& mesh = model.meshes[meshIndex];
    if (anyFinest && !finest[meshIndex]) {
      continue;
    }
    if (mesh.material >= mats.size() || mesh.vertexBuffer >= buffers.size()) {
      throw Fail{"a Remastered mesh names a material or vertex buffer that does not exist"};
    }
    const std::string& name = mats[mesh.material].name;
    bool skip = false;
    for (const std::string& s : opt.skip) {
      skip = skip || name.find(s) != std::string::npos;
    }
    if (skip || mats[mesh.material].hidden || (opt.standalone && !mats[mesh.material].maps[kBase].has)) {
      continue;
    }
    Buffer& b = buffers[mesh.vertexBuffer];
    if (!b.loaded) {
      const ModelVertexBuffer& vb = model.vertexBuffers[mesh.vertexBuffer];
      b.loaded = true;
      b.src = &vb;
      b.n = vb.vertexCount;
      if (vb.positions.size() != b.n * 3 || vb.normals.size() != b.n * 3) {
        throw Fail{"a Remastered vertex buffer has no positions or normals"};
      }
      if (vb.uvs.empty() || vb.uvs[0].size() != b.n * 2) {
        throw Fail{"a Remastered vertex buffer has no texture coordinates"};
      }
      b.P.resize(b.n * 3);
      b.N.resize(b.n * 3);
      for (size_t v = 0; v < b.n; ++v) {
        double nrm[3];
        for (int r = 0; r < 3; ++r) {
          double p = 0.0, q = 0.0;
          for (int c = 0; c < 3; ++c) {
            p += double(vb.positions[v * 3 + c]) * M[r][c];
            q += double(vb.normals[v * 3 + c]) * M[r][c];
          }
          b.P[v * 3 + r] = p + opt.offset[r];
          nrm[r] = q;
        }
        const double len = std::max(std::sqrt((nrm[0] * nrm[0] + nrm[1] * nrm[1]) + nrm[2] * nrm[2]), 1e-9);
        for (int r = 0; r < 3; ++r) {
          b.N[v * 3 + r] = nrm[r] / len;
        }
      }
      if (vb.tangents.size() == b.n * 4) {
        // Moved with the positions; a mirror turns the handedness over.
        b.T.resize(b.n * 4);
        for (size_t v = 0; v < b.n; ++v) {
          double t[3];
          for (int r = 0; r < 3; ++r) {
            t[r] = 0.0;
            for (int c = 0; c < 3; ++c) {
              t[r] += double(vb.tangents[v * 4 + c]) * M[r][c];
            }
          }
          const double len = std::max(std::sqrt((t[0] * t[0] + t[1] * t[1]) + t[2] * t[2]), 1e-9);
          for (int r = 0; r < 3; ++r) {
            b.T[v * 4 + r] = t[r] / len;
          }
          b.T[v * 4 + 3] = (vb.tangents[v * 4 + 3] < 0.0f) != (det < 0.0) ? -1.0 : 1.0;
        }
      }
      // (not vb.Find: the converter tests don't link the CMDL parser)
      const ModelAttribute* second = nullptr;
      for (const ModelAttribute& attribute : vb.attributes) {
        if (attribute.name == "TANGENT_1") {
          second = &attribute;
          break;
        }
      }
      if (second != nullptr && !b.T.empty() && !second->isInteger && second->components == 4 &&
          second->data.size() == b.n * 4) {
        b.T1.resize(b.n * 4);
        for (size_t v = 0; v < b.n; ++v) {
          double t[3];
          for (int r = 0; r < 3; ++r) {
            t[r] = 0.0;
            for (int c = 0; c < 3; ++c) {
              t[r] += double(second->data[v * 4 + c]) * M[r][c];
            }
          }
          const double len = std::max(std::sqrt((t[0] * t[0] + t[1] * t[1]) + t[2] * t[2]), 1e-9);
          for (int r = 0; r < 3; ++r) {
            b.T1[v * 4 + r] = t[r] / len;
          }
          b.T1[v * 4 + 3] = (second->data[v * 4 + 3] < 0.0f) != (det < 0.0) ? -1.0 : 1.0;
        }
      }
      // Indexed by a material's texcoord, which takes two from each attribute
      // (TEXCOORD_n.xy, then its zw): the vertex shaders hand varying 2 + c to texcoord c.
      for (size_t c = 0; c < vb.uvs.size() * 2; ++c) {
        const std::vector<float>* uv = c % 2 == 0 ? &vb.uvs[c / 2] : c / 2 < vb.uvsZw.size() ? &vb.uvsZw[c / 2] : nullptr;
        b.uv.emplace_back(uv && uv->size() == b.n * 2 ? std::vector<double>(uv->begin(), uv->end()) : std::vector<double>());
      }
      b.skinned = vb.joints.size() == b.n * 4 && vb.weights.size() == b.n * 4;
      b.C.assign(b.n * 4, 255);
      if (vb.colors.size() == b.n * 4) {
        for (size_t i = 0; i < b.n * 4; ++i) {
          b.C[i] = uint8_t(std::clamp(vb.colors[i], 0.0f, 1.0f) * 255.0f + 0.5f);
        }
        b.colored = true;
      }
    }
    Prim p;
    Prim backPrim;
    p.buffer = mesh.vertexBuffer;
    p.mat = mesh.material;
    p.I = mesh.indices;
    p.I.resize(p.I.size() / 3 * 3);
    for (uint32_t i : p.I) {
      if (i >= b.src->vertexCount) {
        throw Fail{"a Remastered mesh indexes past its vertex buffer"};
      }
    }
    if (opt.joint >= 0) {
      size_t kept = 0;
      for (size_t t = 0; t + 2 < p.I.size(); t += 3) {
        if (TriangleJoint(*b.src, &p.I[t]) == opt.joint) {
          std::copy_n(p.I.begin() + t, 3, p.I.begin() + kept);
          kept += 3;
        }
      }
      p.I.resize(kept);
      if (p.I.empty()) {
        continue;
      }
    }
    if (det < 0) {
      for (size_t t = 0; t + 2 < p.I.size(); t += 3) {
        std::swap(p.I[t], p.I[t + 2]);
      }
    }
    if (opt.vmsh != nullptr && !b.uv.empty() && b.uv[0].size() >= b.n * 2) {
      // The front faces only, before the back copy below adds vertices.
      for (uint32_t i : p.I) {
        const uint64_t key = uint64_t(mesh.vertexBuffer) << 32 | i;
        auto [at, fresh] = vmshIndex.try_emplace(key, uint32_t(vmshV.size() / 8));
        if (fresh) {
          for (int r = 0; r < 3; ++r) {
            vmshV.push_back(float(b.P[size_t(i) * 3 + r]));
          }
          for (int r = 0; r < 3; ++r) {
            vmshV.push_back(float(b.N[size_t(i) * 3 + r]));
          }
          vmshV.push_back(float(b.uv[0][size_t(i) * 2]));
          vmshV.push_back(float(b.uv[0][size_t(i) * 2 + 1]));
        }
        vmshI.push_back(at->second);
      }
    }
    if (mesh.twoSided) {
      // Seen from behind too: Remastered draws the mesh with culling off (its MESH
      // chunk's one-bit map; every material flagged 0x400 plus holograms and glow
      // planes), the game culls back faces, so the back is drawn as a copy: each
      // vertex again with its normal turned round, each triangle wound the other way.
      // The back copy always has the normal turned round (what the TEV path lights by). A
      // material with a positive LITS gets it as a primitive of its own (BackLightScale),
      // whose record's F0 factor 0 tells the PBR shader to turn the normal back.
      const bool lits = BackLightScale(mats[mesh.material]);
      if (mats[mesh.material].hasLits && !lits && mats[mesh.material].lits != -1.0 &&
          mats[mesh.material].lits != 1.0) {
        Log("  note: material " + name + " has LITS " + std::to_string(mats[mesh.material].lits) +
            ", drawn as the older back copy");
      }
      std::unordered_map<uint32_t, uint32_t> back;
      for (uint32_t i : p.I) {
        if (back.count(i) != 0) {
          continue;
        }
        back[i] = uint32_t(b.n++);
        for (int r = 0; r < 3; ++r) {
          b.P.push_back(b.P[size_t(i) * 3 + r]);
          b.N.push_back(-b.N[size_t(i) * 3 + r]);
        }
        if (!b.T.empty()) {
          // The normal turns round and B = w * cross(N, T) stays, so w turns with it.
          for (int r = 0; r < 4; ++r) {
            b.T.push_back(r == 3 ? -b.T[size_t(i) * 4 + r] : b.T[size_t(i) * 4 + r]);
          }
        }
        if (!b.T1.empty()) {
          for (int r = 0; r < 4; ++r) {
            b.T1.push_back(r == 3 ? -b.T1[size_t(i) * 4 + r] : b.T1[size_t(i) * 4 + r]);
          }
        }
        for (std::vector<double>& uv : b.uv) {
          if (!uv.empty()) {
            uv.push_back(uv[size_t(i) * 2]);
            uv.push_back(uv[size_t(i) * 2 + 1]);
          }
        }
        for (int c = 0; c < 4; ++c) {
          b.C.push_back(b.C[size_t(i) * 4 + c]);
        }
        b.copyOf.push_back(i);
      }
      const size_t front = p.I.size();
      for (size_t t = 0; t < front; t += 3) {
        p.I.push_back(back[p.I[t + 2]]);
        p.I.push_back(back[p.I[t + 1]]);
        p.I.push_back(back[p.I[t]]);
      }
      if (lits) {
        backPrim.buffer = p.buffer;
        backPrim.mat = p.mat;
        backPrim.litsBack = true;
        backPrim.I.assign(p.I.begin() + front, p.I.end());
        p.I.resize(front);
      }
    }
    if (!b.used) {
      b.used = true;
      bufOrder.push_back(mesh.vertexBuffer);
    }
    prims.push_back(std::move(p));
    if (backPrim.litsBack) {
      prims.push_back(std::move(backPrim));
    }
  }
  if (prims.empty()) {
    throw Fail{"no primitives left"};
  }
  if (opt.vmsh != nullptr) {
    opt.vmsh->clear();
    if (!vmshI.empty()) {
      const uint32_t nv = uint32_t(vmshV.size() / 8);
      const bool wide = nv > 65535;
      auto put = [&](uint32_t v, int bytes) {
        for (int k = bytes - 1; k >= 0; --k) {
          opt.vmsh->push_back(uint8_t(v >> (8 * k)));
        }
      };
      put(1, 4);
      put(nv, 4);
      put(uint32_t(vmshI.size() / 3), 4);
      for (float f : vmshV) {
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        put(bits, 4);
      }
      for (uint32_t i : vmshI) {
        put(i, wide ? 4 : 2);
      }
    }
  }
  Log(Hex8(opt.retail) + ": " + std::to_string(prims.size()) + " primitives");
  // The levels of detail share their vertex buffers, so a buffer carries the other
  // levels' vertices (and those of skipped meshes) too: keep only what is indexed.
  for (uint32_t bi : bufOrder) {
    Buffer& b = buffers[bi];
    constexpr uint32_t kDropped = UINT32_MAX;
    std::vector<uint32_t> remap(b.n, kDropped);
    for (const Prim& p : prims) {
      if (p.buffer == bi) {
        for (uint32_t i : p.I) {
          remap[i] = 0;
        }
      }
    }
    size_t kept = 0;
    for (size_t v = 0; v < b.n; ++v) {
      if (remap[v] == kDropped) {
        continue;
      }
      remap[v] = uint32_t(kept);
      b.srcOf.push_back(v < b.src->vertexCount ? uint32_t(v) : b.copyOf[v - b.src->vertexCount]);
      std::copy_n(b.P.begin() + v * 3, 3, b.P.begin() + kept * 3);
      std::copy_n(b.N.begin() + v * 3, 3, b.N.begin() + kept * 3);
      if (!b.T.empty()) {
        std::copy_n(b.T.begin() + v * 4, 4, b.T.begin() + kept * 4);
      }
      if (!b.T1.empty()) {
        std::copy_n(b.T1.begin() + v * 4, 4, b.T1.begin() + kept * 4);
      }
      std::copy_n(b.C.begin() + v * 4, 4, b.C.begin() + kept * 4);
      for (std::vector<double>& uv : b.uv) {
        if (!uv.empty()) {
          std::copy_n(uv.begin() + v * 2, 2, uv.begin() + kept * 2);
        }
      }
      ++kept;
    }
    b.n = kept;
    b.P.resize(kept * 3);
    b.N.resize(kept * 3);
    b.T.resize(b.T.empty() ? 0 : kept * 4);
    b.T1.resize(b.T1.empty() ? 0 : kept * 4);
    b.C.resize(kept * 4);
    for (std::vector<double>& uv : b.uv) {
      if (!uv.empty()) {
        uv.resize(kept * 2);
      }
    }
    for (Prim& p : prims) {
      if (p.buffer == bi) {
        for (uint32_t& i : p.I) {
          i = remap[i];
        }
      }
    }
  }

  // Retail material per primitive: forced, or by a vote of the nearest retail
  // vertices among materials of the same kind (blended effect or opaque surface).
  std::vector<double> up;
  std::vector<int> upm;
  for (size_t v = 0; v < retail.Count(); ++v) {
    if (retail.vmat[v] >= 0) {
      up.insert(up.end(), retail.P.begin() + v * 3, retail.P.begin() + v * 3 + 3);
      upm.push_back(retail.vmat[v]);
    }
  }
  for (Prim& p : prims) {
    if (opt.standalone) {
      const RemMaterial& m = mats[p.mat];
      p.rmat = m.kind == 8 || m.kind == 10 || m.kind == 11 || m.kind == 14 || m.kind == 23 || m.kind == 29 || m.kind == 30 || m.kind == 34 ? 3 : m.cutout ? 1 : m.additive ? 4 : m.blended ? 2 : 0;
      continue;
    }
    if (opt.material >= 0) {
      if (size_t(opt.material) >= retail.mats.size()) {
        throw Fail{"the forced retail material does not exist"};
      }
      p.rmat = opt.material;
      continue;
    }
    if (upm.empty()) {
      throw Fail{"the retail model draws nothing"};
    }
    const bool fx = IsFxName(mats[p.mat].name) || mats[p.mat].additive || mats[p.mat].shell || mats[p.mat].shield;
    std::vector<double> cand;
    std::vector<int> candMat;
    for (int pass = 0; pass < 2 && cand.empty(); ++pass) {
      for (size_t i = 0; i < upm.size(); ++i) {
        if (pass == 1 || IsFx(retail.mats[upm[i]]) == fx) {
          cand.insert(cand.end(), up.begin() + i * 3, up.begin() + i * 3 + 3);
          candMat.push_back(upm[i]);
        }
      }
    }
    std::vector<uint32_t> vs = p.I;
    std::sort(vs.begin(), vs.end());
    vs.erase(std::unique(vs.begin(), vs.end()), vs.end());
    if (vs.size() > 4096) {
      std::vector<uint32_t> sample(4096);
      const double step = double(vs.size() - 1) / 4095.0;
      for (size_t i = 0; i < 4096; ++i) {
        sample[i] = vs[i == 4095 ? vs.size() - 1 : size_t(double(i) * step)];
      }
      vs = std::move(sample);
    }
    const Buffer& b = buffers[p.buffer];
    std::vector<double> pts(vs.size() * 3);
    for (size_t i = 0; i < vs.size(); ++i) {
      std::copy(b.P.begin() + size_t(vs[i]) * 3, b.P.begin() + size_t(vs[i]) * 3 + 3, pts.begin() + i * 3);
    }
    std::vector<uint32_t> nn;
    Nearest(pts.data(), vs.size(), cand.data(), candMat.size(), nn);
    std::vector<std::pair<int, size_t>> votes;  // in the order first seen, which decides a tie
    for (uint32_t i : nn) {
      auto it = std::find_if(votes.begin(), votes.end(), [&](const auto& e) { return e.first == candMat[i]; });
      if (it == votes.end()) {
        votes.emplace_back(candMat[i], 1);
      } else {
        ++it->second;
      }
    }
    size_t best = 0;
    for (size_t i = 1; i < votes.size(); ++i) {
      if (votes[i].second > votes[best].second) {
        best = i;
      }
    }
    p.rmat = votes[best].first;
  }

  // Unified vertex list: every distinct buffer once.
  size_t n = 0;
  size_t maxuv = 0;
  bool skinned = true;
  for (uint32_t bi : bufOrder) {
    Buffer& b = buffers[bi];
    b.offset = n;
    n += b.n;
    maxuv = std::max(maxuv, b.uv.size() - 1);
    skinned = skinned && b.skinned;
  }
  if (opt.standalone) {
    // A texcoord slot for every set, so each map keeps the one it was made on.
    for (RetailMaterial& m : retail.mats) {
      for (size_t k = 0; k <= std::min<size_t>(maxuv, 7); ++k) {
        m.vtx |= 3u << (8 + 2 * k);
      }
    }
  }
  // Whether any buffer carries UV0.zw (the lightmap's coordinates, material texcoord 1).
  bool hasLightmapUv = false;
  for (uint32_t bi : bufOrder) {
    hasLightmapUv = hasLightmapUv || (buffers[bi].uv.size() > 1 && !buffers[bi].uv[1].empty());
  }
  // Vertex tangents (the NBT normal section, CMDL flag 0x8) only when every buffer has them.
  bool useTan = !bufOrder.empty();
  for (uint32_t bi : bufOrder) {
    useTan = useTan && !buffers[bi].T.empty();
  }
  // A second tangent frame (flag 0x10) only for the models that draw a two-tangent shader
  // with TANGENT_1 on every buffer; its other surfaces keep the first nine floats (0x93).
  auto twoTangent = [&](const Prim& p) {
    for (uint32_t id : kShaderTwoTangent) {
      if (mats[p.mat].shader == id) {
        return true;
      }
    }
    return false;
  };
  bool useTan2 = false;
  for (const Prim& p : prims) {
    useTan2 = useTan2 || twoTangent(p);
  }
  for (uint32_t bi : bufOrder) {
    useTan2 = useTan2 && useTan && !buffers[bi].T1.empty();
  }
  std::vector<double> P, N, T, T1;
  P.reserve(n * 3);
  N.reserve(n * 3);
  for (uint32_t bi : bufOrder) {
    P.insert(P.end(), buffers[bi].P.begin(), buffers[bi].P.end());
    N.insert(N.end(), buffers[bi].N.begin(), buffers[bi].N.end());
    if (useTan) {
      T.insert(T.end(), buffers[bi].T.begin(), buffers[bi].T.end());
    }
    if (useTan2) {
      T1.insert(T1.end(), buffers[bi].T1.begin(), buffers[bi].T1.end());
    }
  }
  auto uvSet = [&](size_t i) {
    std::vector<double> out;
    out.reserve(n * 2);
    for (uint32_t bi : bufOrder) {
      const Buffer& b = buffers[bi];
      const std::vector<double>& uv = i < b.uv.size() && !b.uv[i].empty() ? b.uv[i] : b.uv[0];
      out.insert(out.end(), uv.begin(), uv.end());
    }
    return out;
  };
  for (Prim& p : prims) {
    for (uint32_t& i : p.I) {
      i += uint32_t(buffers[p.buffer].offset);
    }
  }
  // Vertex colours, for a room's own materials that are tinted by them. The
  // alpha is an opacity only on a blended surface (on an opaque one it weighs
  // the material's layers), so a vertex no blended surface uses is opaque.
  // On a retail model, only a glow that is all vertex colour reads them: an
  // unlit Remastered surface over a retail effect (a door shield's cyan rim),
  // and the arm cannon's beam glow (kind 9), which is all vertex colour and ramp.
  auto ownGlow = [&](const RemMaterial& m, const RetailMaterial& pm) {
    return !opt.standalone && m.unlit && m.tinted && IsFx(pm);
  };
  std::vector<uint8_t> C;
  bool useColor = false;
  for (const Prim& p : prims) {
    const bool reads = opt.standalone ? mats[p.mat].tinted || mats[p.mat].vcolor
                                      : ownGlow(mats[p.mat], retail.mats[p.rmat]) || mats[p.mat].kind == 9 || mats[p.mat].kind == 13 || mats[p.mat].kind == 14 || mats[p.mat].kind == 29 || mats[p.mat].kind == 15 || mats[p.mat].kind == 18;
    useColor = useColor || (reads && buffers[p.buffer].colored);
  }
  if (useColor) {
    C.reserve(n * 4);
    for (uint32_t bi : bufOrder) {
      C.insert(C.end(), buffers[bi].C.begin(), buffers[bi].C.end());
    }
    std::vector<bool> keepAlpha(n, false);
    for (const Prim& p : prims) {
      if (((mats[p.mat].blended || mats[p.mat].layered) && mats[p.mat].tinted) || mats[p.mat].vcolor ||
          ownGlow(mats[p.mat], retail.mats[p.rmat]) || mats[p.mat].kind == 13 || mats[p.mat].kind == 14 || mats[p.mat].kind == 29 || mats[p.mat].kind == 15 || mats[p.mat].kind == 18) {
        for (uint32_t i : p.I) {
          keepAlpha[i] = true;
        }
      }
    }
    for (size_t v = 0; v < n; ++v) {
      if (!keepAlpha[v]) {
        C[v * 4 + 3] = 255;
      }
    }
  }

  // Output materials: one per (retail material, Remastered material) pair, in
  // every material set.
  using Key = std::tuple<int, uint32_t, bool>;  // retail material, Remastered material, LITS back
  std::vector<Key> keys;
  for (Prim& p : prims) {
    const Key key(p.rmat, p.mat, p.litsBack);
    auto it = std::find(keys.begin(), keys.end(), key);
    if (it == keys.end()) {
      keys.push_back(key);
      it = keys.end() - 1;
    }
    p.omat = int(it - keys.begin());
  }

  // The texcoord arrays the output carries, each n long. A display list
  // attribute names one by its place in this list.
  std::vector<std::vector<double>> uvArrays;
  std::vector<std::tuple<size_t, bool>> uvKeys;
  auto uvIndex = [&](size_t uvi, const char* role) -> uint32_t {
    const bool squeezed = role && opt.squeeze && opt.squeezeRole == role;
    const std::tuple<size_t, bool> key(uvi, squeezed);
    const auto it = std::find(uvKeys.begin(), uvKeys.end(), key);
    if (it != uvKeys.end()) {
      return uint32_t(it - uvKeys.begin());
    }
    std::vector<double> u = uvSet(uvi);
    if (squeezed) {
      // A deliberate artistic remap, so it runs on the raw coordinates and the
      // result is left alone.
      const double u0 = opt.squeezeFrom[0], u1 = opt.squeezeFrom[1];
      const double lo = opt.squeezeTo[0], hi = opt.squeezeTo[1];
      for (size_t v = 0; v < n; ++v) {
        u[v * 2] = lo + (hi - lo) * std::clamp((u[v * 2] - u0) / (u1 - u0), 0.0, 1.0);
      }
    }
    // Otherwise the coordinates stay as Remastered authored them (U can span
    // -3.9..5.0, or sit wholly outside 0..1): each map samples them with its own
    // wrap modes, which the PBR record carries. Remapping a set onto 0..1 stretched
    // offset or partial tiles (-4.97..-4.77 drew the whole texture).
    uvKeys.push_back(key);
    uvArrays.push_back(std::move(u));
    return uint32_t(uvArrays.size() - 1);
  };

  std::vector<std::vector<Blob>> setBlobs(retail.nmat);
  std::vector<std::vector<uint32_t>> setTex(retail.nmat);
  auto texIndex = [&](size_t si, uint32_t tid) {
    auto it = std::find(setTex[si].begin(), setTex[si].end(), tid);
    if (it == setTex[si].end()) {
      setTex[si].push_back(tid);
      return uint32_t(setTex[si].size() - 1);
    }
    return uint32_t(it - setTex[si].begin());
  };
  std::vector<std::vector<uint32_t>> dlAttrs;  // per output material: the uv array of each texcoord attribute
  std::vector<bool> dlColor;                   // and whether a colour index comes before them
  // The materials report: one row per output material, as it is pushed.
  auto decide = [&](const RemMaterial& rem, uint32_t sourceIndex, const char* path, const std::string& pathReason,
                    const std::string& notes, const std::string& tag, uint32_t cube, const RetailMaterial& pm) {
    if (!io.decision) {
      return;
    }
    MaterialDecision d;
    d.cmdl = Hex8(opt.outputModel != 0 ? opt.outputModel : opt.retail);
    d.index = int(dlAttrs.size()) - 1;
    d.source = opt.source;
    d.sourceIndex = int(sourceIndex);
    d.shader = Hex8(rem.shader);
    d.role = rem.role;
    d.flags = rem.flags;
    d.tag = tag;
    d.kind = rem.kind;
    d.mode = PbrMode(rem);
    d.path = path;
    d.pathReason = pathReason;
    d.kindReason = notes + rem.reason;
    d.emissive = rem.emissive;
    d.backlight = rem.backlight;
    d.strength = rem.kindStrength;
    for (int i = 0; i < 4; ++i) {
      d.p[i] = rem.kindParam[i];
    }
    d.cube = cube != 0 ? Hex8(cube) : "-";
    char retailBlend[48];
    std::snprintf(retailBlend, sizeof(retailBlend), "%u,%u 0x%X", pm.blendSrc, pm.blendDst, pm.flags);
    d.retail = retailBlend;
    io.decision(d);
  };
  for (const auto& key : keys) {
    std::string loopNotes;  // what this loop changed on the material, for the report
    const int rmat = std::get<0>(key);
    const RetailMaterial& pm = retail.mats[rmat];
    RemMaterial rem = mats[std::get<1>(key)];
    if (std::get<2>(key)) {
      rem.lightScale[0] = std::abs(rem.lits);
      rem.lightScale[1] = 0.0;
    }
    const bool glow = ownGlow(rem, pm) && useColor;
    // Remastered's missile lock-on highlight is a runtime effect: its map is
    // solid red, so baked in it turns grey shards red.
    if (Lower(rem.name).find("missilelock") != std::string::npos) {
      loopNotes += "missilelock: emissive map dropped; ";
      rem.maps[kEmissive].has = false;
    }
    // A retail material's vertices have no layer weight.
    // A shader of its own needs none: without colours the up-facing one goes by
    // the normal alone.
    // The beam glow is the exception: it is the vertex colour's, on the gun's own vertices.
    // Without the colours (the inventory and cinematic guns) it is drawn as the ramp's mean,
    // a glow that does not move.
    const bool gunGlow = rem.kind == 9 && useColor;
    if (rem.kind == 9 && !gunGlow) {
      if (!rem.maps[kEmissive].has) {
        rem.maps[kEmissive] = rem.layer[kMr];
        rem.maps[kEmissive].raw = false;
        rem.maps[kEmissive].mean = true;
        rem.maps[kEmissive].coord = 0;  // one colour: any texcoord does
        rem.emissive = rem.kindStrength / kPbrEmissive;
        rem.glowLinear = true;
      }
      loopNotes += "gun-glow without vertex colours: drawn as the ramp's mean; ";
      rem.kind = 0;
      rem.vcolor = false;
      rem.layered = false;
    }
    // So is the Ice Beam cannon's frost shell, which needs no vertex colours: drawn as a
    // plain surface it would freeze the gun for good.
    const bool frostShell = rem.kind == 12;
    // The Metroid dome's matcap shell (kind 13) is a retail model's blended surface drawn by
    // the shader: its alpha, rim and colours are all the Remastered material's.
    const bool matcapShell = rem.kind == 13;
    // And the Frigate's force fields (kind 14), a retail model's fx surface drawn by the shader.
    const bool shield = (rem.kind >= 14 && rem.kind <= 19) || rem.kind == 25 || rem.kind == 28 || rem.kind == 29 || rem.kind == 31 || rem.kind == 33 || rem.kind == 34;
    // 4BC890C1 is a plain lit Lambert that Remastered draws opaque (mesh class 0) where retail
    // used a blended effect: it takes the standard path, so it leaves the retail-fx gate.
    // Likewise any Remastered material without the blend or cutout flag: its mesh is class 0,
    // which Remastered draws opaque whatever retail's blend (the missile pickup's top,
    // 274D21BE; kb topic/mesh-blend-class.md).
    const bool lambertFx = rem.family == kShaderLambertFx || rem.shader == kShaderVfxBase || rem.shader == kShaderVfxBase2 ||
                           (!opt.standalone && !(rem.flags & (kTransparentFlag | kCutoutFlag)));
    rem.opaqueFx = !opt.standalone && IsFx(pm) && !(rem.flags & (kTransparentFlag | kCutoutFlag));
    if (rem.kind == 19 || rem.kind == 25) {
      // Row 6 w (unused by this kind otherwise): the retail konst alpha, a factor of the alpha of a particle model.
      rem.shieldRows[27] = GunFxParticle(pm) ? pm.konstAlpha : 1.0;
    }
    if (!opt.standalone && !gunGlow && !frostShell && !matcapShell && !shield) {
      if (rem.kind != 0) {
        loopNotes += std::string("kind ") + KindName(rem.kind) + " dropped: a retail model, not standalone; ";
      }
      rem.kind = 0;
      rem.vcolor = false;
    }
    rem.macro = rem.macro && opt.standalone;
    rem.layered = rem.layered && (gunGlow || frostShell || matcapShell || shield || (opt.standalone && (rem.kind != 0 || (useColor && rem.tinted))));
    rem.macroLayered = rem.macroLayered && rem.layered && opt.standalone;
    // Nor do the alpha and shading modes belong on one: what they say is about the
    // Remastered surface, and a retail model keeps the retail material's
    // (bar a glow of its own, whose colour and fade are all in it).
    // The glow mask is the exception: a glow map is often a few solid blocks the
    // base map's alpha cuts down to lamps (the save station's arms went yellow
    // all over), and the opacity stays the retail draw's either way.
    if (!opt.standalone) {
      rem.unlit = glow;
      rem.mask = rem.mask && rem.maps[kEmissive].has;
      rem.height = 0.0;
    }
    const MapRef* rt = rem.maps;
    // A retail descriptor with no texcoord slot at all cannot feed a material
    // that samples texcoord 0, so one is declared: bits 8..23 are the eight
    // texcoord slots, two bits each, and 3 is the encoding retail uses.
    uint32_t vtx = pm.vtx;
    size_t ntexattr = 0;
    for (int k = 0; k < 8; ++k) {
      ntexattr += ((vtx >> (8 + 2 * k)) & 3) ? 1 : 0;
    }
    if (ntexattr == 0) {
      vtx |= 3u << 8;
      ntexattr = 1;
    }
    // The beam glow's noise is on a texcoord of its own, which a retail gun lacks.
    if (gunGlow) {
      const size_t want = std::min<size_t>(std::min<size_t>(rem.layer[kBase].coord, maxuv) + 1, 8);
      for (; ntexattr < want; ++ntexattr) {
        vtx |= 3u << (8 + 2 * ntexattr);
      }
    }
    // The frost shell's glow map (ICAN) is on texcoord 1.
    if (frostShell) {
      size_t coord = rem.layer[kBase].coord;
      for (int k = 0; k < kMaps; ++k) {
        coord = std::max<size_t>(coord, rem.maps[k].has ? rem.maps[k].coord : 0);
      }
      const size_t want = std::min<size_t>(std::min<size_t>(coord, maxuv) + 1, 8);
      for (; ntexattr < want; ++ntexattr) {
        vtx |= 3u << (8 + 2 * ntexattr);
      }
    }
    // Bits 4 and 5 are colour 0. Only a material that reads it declares it.
    const bool colored = useColor && (opt.standalone ? rem.tinted || rem.vcolor : glow || gunGlow || matcapShell || shield);
    rem.tinted = colored && rem.tinted;
    if (colored) {
      vtx |= 3u << 4;
    }
    dlColor.push_back(colored);
    // PBR needs a base map and an opaque retail material: blended effects keep
    // their TEV.
    // Only a room's own material asks for a cutout: a retail material's alpha
    // test says nothing about what the Remastered map's alpha holds.
    // A plain lit cutout (no vertex colour) is Remastered's 1-bit alpha shader: full alpha,
    // cut by the shader. Any other keeps the CMPR punch and retail's alpha compare.
    const bool exactCut = opt.standalone && rem.cutout && (rem.kind == 0 || rem.kind == 24) && !rem.unlit && !rem.layered;
    const char* const baseAlpha =
        glow ? "blend" : !opt.standalone ? (frostShell || rem.mask || rem.shader == kShaderVfxBase || rem.shader == kShaderVfxBase2 || (rem.kind >= 16 && rem.kind <= 18) ? (!frostShell && rem.maskSquared ? "mask2" : "mask") : "") : rem.cutout ? (exactCut ? "cut" : "punch") : rem.mask || rem.layered || rem.height > 0.0 ? "mask" : rem.blended ? "blend" : "";
    const bool usePbr = opt.pbr && rt[kBase].has && (opt.standalone || glow || matcapShell || shield || lambertFx || !IsFx(pm)) &&
                        Get("pbr:base", rt, baseAlpha, opt).has_value();
    // The base map keeps its whole alpha, and the shader cuts it as Remastered's does.
    rem.cutExact = usePbr && exactCut;
    if (usePbr) {
      ++pbr;
      std::string recordTag;
      // A layered material's base alphas are the two heights the blend compares.
      const int nmaps = rem.macroLayered ? kMacroMaps : rem.layered || rem.macro ? kLayeredMaps : kMaps;
      MapRef both[kMacroMaps];
      std::copy(rt, rt + kMaps, both);
      std::copy(rem.layer, rem.layer + 3, both + kMaps);
      both[kLayeredMaps] = rem.macroMap;
      uint32_t tids[kMacroMaps];
      for (int k = 0; k < nmaps; ++k) {
        const int m = k == kLayeredMaps ? int(kNormal) : k % kMaps;
        if (rem.macro && !rem.layered && (k == 4 || k == 5)) {
          tids[k] = tids[0];
          continue;
        }
        tids[k] = *Get(std::string("pbr:") + kMapName[m], both + (k - m), m == kBase ? baseAlpha : "", opt);
      }
      // Each map keeps the texcoord set it was authored on. The descriptor
      // fixes how many texcoord attributes exist, and a coord past it cannot be
      // sampled at all, so it is folded onto the base's; and if the base's own
      // is past it too, onto 0, which the descriptor always has.
      size_t bset = std::min<size_t>(rt[kBase].coord, maxuv);
      bset = bset < ntexattr ? bset : 0;
      uint32_t coords[kMacroMaps];
      for (int k = 0; k < nmaps; ++k) {
        const size_t c = std::min<size_t>(both[k].has ? both[k].coord : bset, maxuv);
        coords[k] = uint32_t(c < ntexattr ? c : bset);
      }
      uint32_t authored[kMacroMaps];
      for (int k = 0; k < nmaps; ++k) {
        authored[k] = both[k].has ? both[k].authored : 0xFFFFFFFFu;
      }
      const AnuvEntry* entry = rem.anuv >= 0 ? &anuv.entries[size_t(rem.anuv)] : nullptr;
      // Two maps on one source set through different transforms cannot share a
      // texgen, since the texgen's matrix moves every map that reads it. The later
      // one is given a texcoord attribute of its own, a second copy of the set (the
      // descriptor then declares one more), as long as one of the eight is free; if
      // none is, the maps share and the first transform drives both.
      std::vector<uint32_t> attrs(ntexattr, 0xFFFFFFFFu);
      if (entry != nullptr) {
        struct Slot {
          uint32_t source, transform, slot;
        };
        std::vector<Slot> slots;
        bool split = false, starved = false;
        for (int k = 0; k < nmaps; ++k) {
          if (!both[k].has) {
            continue;
          }
          const uint32_t a = authored[k];
          const uint32_t transform = a < 3 && !entry->xf[a].Identity() ? a : 3;  // 3: no motion
          const uint32_t source = coords[k];
          const auto same = std::find_if(slots.begin(), slots.end(), [&](const Slot& o) {
            return o.source == source && o.transform == transform;
          });
          if (same != slots.end()) {
            coords[k] = same->slot;
            continue;
          }
          const bool taken = std::any_of(slots.begin(), slots.end(), [&](const Slot& o) { return o.source == source; });
          uint32_t slot = source;
          if (taken && ntexattr < 8) {
            slot = uint32_t(ntexattr);
            vtx |= 3u << (8 + 2 * ntexattr);
            attrs.push_back(uvIndex(source, nullptr));
            ++ntexattr;
            split = true;
          } else if (taken) {
            starved = true;
          }
          slots.push_back({source, transform, slot});
          coords[k] = slot;
        }
        anuvCounts.slotSplit += split ? 1 : 0;
        anuvCounts.outOfSlots += starved ? 1 : 0;
      }
      for (int k = 0; k < nmaps; ++k) {
        if (attrs[coords[k]] == 0xFFFFFFFFu) {
          attrs[coords[k]] = uvIndex(coords[k], nullptr);
        }
      }
      // Each map's sampler, 2 bits an axis (GX: 0 clamp, 1 repeat, 2 mirror); a
      // map the material lacks, or one without a sampler, repeats.
      uint32_t wrap = 0x55555555u;
      for (int k = 0; k < nmaps; ++k) {
        for (int c = 0; c < 2 && both[k].has; ++c) {
          const int32_t w = both[k].wrap[c];
          const uint32_t shift = uint32_t(k * 4 + c * 2);
          wrap = (wrap & ~(3u << shift)) | (uint32_t(w >= 0 && w <= 2 ? w : 1) << shift);
        }
      }
      const uint32_t zero = uvIndex(0, nullptr);
      for (uint32_t& a : attrs) {
        a = a == 0xFFFFFFFFu ? zero : a;
      }
      // The lightmap's coordinates (texcoord 1: UV0.zw) ride on one more attribute, if the
      // model has them and a slot is free; the material's 'LMUV' trailer names the slot.
      int lightmapSlot = -1;
      if (opt.lightmapUv && hasLightmapUv && ntexattr < 8) {
        lightmapSlot = int(ntexattr);
        vtx |= 3u << (8 + 2 * ntexattr);
        attrs.push_back(uvIndex(1, nullptr));
        ++ntexattr;
      }
      dlAttrs.push_back(attrs);
      const uint32_t group = 0x40000000u | uint32_t(dlAttrs.size() - 1);
      const uint32_t cube = Cube(rem.refl);
      for (size_t si = 0; si < retail.nmat; ++si) {
        uint32_t idx[kMacroMaps];
        for (int k = 0; k < nmaps; ++k) {
          idx[k] = texIndex(si, tids[k]);
        }
        AnuvCounts scratch;  // counted once, not once per material set
        setBlobs[si].push_back(PbrMaterial(pm, vtx, idx, group, coords, rem, wrap, cube, authored, entry,
                                           si == 0 ? anuvCounts : scratch));
        if (si == 0) {
          recordTag.assign(setBlobs[si].back().end() - 4, setBlobs[si].back().end());
        }
        if (lightmapSlot >= 0) {
          P32(setBlobs[si].back(), uint32_t(lightmapSlot));
          setBlobs[si].back().insert(setBlobs[si].back().end(), {'L', 'M', 'U', 'V'});
        }
      }
      decide(rem, std::get<1>(key), "pbr", "ok", loopNotes, recordTag, cube, pm);
      continue;
    }
    ++tev;
    const std::string tevReason =
        !opt.pbr ? "pbr off"
        : !rt[kBase].has ? "no base map"
        : !(opt.standalone || glow || matcapShell || shield || lambertFx || !IsFx(pm)) ? "retail fx material keeps its TEV"
                                                   : "the base map did not resolve";
    // The TEV path: the retail material with each texture slot refilled from
    // the Remastered map that matches what its stage does.
    const std::vector<SlotRole> roles = SlotRoles(pm);
    std::vector<uint32_t> attrs(ntexattr, 0xFFFFFFFFu);
    std::map<uint32_t, uint32_t> slotIds;
    // Each slot's sampler, as the PBR record's wrap word (slot i: S in bits 4i..4i+1, T in
    // 4i+2..4i+3): a slot refilled from a Remastered map takes that map's modes; a retail
    // texture, a constant or a slot past 7 repeats, as in retail.
    uint32_t wrap = 0x55555555u;
    for (const SlotRole& r : roles) {
      Role role = r.role;
      if (r.slot >= pm.tex.size() || pm.tex[r.slot] >= retail.sets[0].tex.size()) {
        throw Fail{"a retail material samples a texture it does not list"};
      }
      const std::optional<uint32_t> tid =
          Get(RoleName(role), rt, RetailAlpha(retail.sets[0].tex[pm.tex[r.slot]]), opt);
      if (tid) {
        slotIds[r.slot] = *tid;
      } else {
        role = Role::Keep;
      }
      const int rk = role == Role::Emissive ? kEmissive : role == Role::Reflect || role == Role::EnvMap ? kMr : kBase;
      // The maps Get draws from (a constant has no sampler of its own).
      const bool mapped = (role == Role::Diffuse || role == Role::Emissive || role == Role::Reflect) && rt[rk].has;
      for (int c = 0; c < 2 && r.slot < 8; ++c) {
        const int32_t w = mapped ? rt[rk].wrap[c] : 1;
        const uint32_t shift = r.slot * 4 + uint32_t(c) * 2;
        wrap = (wrap & ~(3u << shift)) | (uint32_t(w >= 0 && w <= 2 ? w : 1) << shift);
      }
      const uint32_t uvi = rt[rk].has ? rt[rk].coord : rt[kBase].has ? rt[kBase].coord : 0;
      if (r.uvSrc >= 0 && size_t(r.uvSrc) < ntexattr && attrs[r.uvSrc] == 0xFFFFFFFFu) {
        attrs[r.uvSrc] = uvIndex(std::min<size_t>(uvi, maxuv), RoleName(role));
      }
    }
    for (uint32_t& a : attrs) {
      if (a == 0xFFFFFFFFu) {
        a = uvIndex(0, nullptr);
      }
    }
    dlAttrs.push_back(attrs);
    decide(rem, std::get<1>(key), "tev", tevReason, loopNotes, wrap != 0x55555555u ? "WRAP" : "TEV", 0, pm);
    for (size_t si = 0; si < retail.nmat; ++si) {
      const MaterialSet& set = retail.sets[si];
      if (size_t(rmat) >= set.mats.size()) {
        throw Fail{"the retail material sets differ in length"};
      }
      Blob blob(set.mats[rmat].p, set.mats[rmat].p + set.mats[rmat].n);
      const RetailMaterial q = ParseMaterial(set.mats[rmat]);
      for (size_t i = 0; i < q.tex.size(); ++i) {
        if (q.tex[i] >= set.tex.size()) {
          throw Fail{"a retail material samples a texture it does not list"};
        }
        const auto it = slotIds.find(uint32_t(i));
        const uint32_t idx = texIndex(si, it != slotIds.end() ? it->second : set.tex[q.tex[i]]);
        for (int k = 0; k < 4; ++k) {
          blob[8 + i * 4 + k] = uint8_t(idx >> (24 - 8 * k));
        }
      }
      if (vtx != q.vtx) {
        // A texcoord slot added above has to reach the material as well: the
        // display list now sends that index.
        const size_t at = 8 + q.tex.size() * 4;
        for (int k = 0; k < 4; ++k) {
          blob[at + k] = uint8_t(vtx >> (24 - 8 * k));
        }
      }
      // Read back from the material's end (PortPbrRecord::Read), so nothing retail parses moves.
      if (wrap != 0x55555555u) {
        P32(blob, wrap);
        blob.insert(blob.end(), {'W', 'R', 'A', 'P'});
      }
      setBlobs[si].push_back(std::move(blob));
    }
  }
  if (uvArrays.empty()) {  // no material samples a texture, but the section must exist
    uvIndex(0, nullptr);
  }

  // Skinning: the weight group of every vertex. A model's skin is not named by
  // the CMDL: the character and its CINF decide, so a model can appear under
  // several skins, and the weights are written under every one whose skeleton
  // has the bones of the one they were mapped against. A replaced CMDL left
  // with retail's CSKR, laid out for retail's vertex count, explodes.
  std::vector<int> weights;       // per vertex: index into weightKeys
  std::vector<WeightKey> weightKeys;
  std::vector<uint32_t> skins;
  if (!opt.skins.empty()) {
    std::vector<uint32_t> sids = opt.skins;
    std::sort(sids.begin(), sids.end());
    sids.erase(std::unique(sids.begin(), sids.end()), sids.end());
    Blob ref;
    if (!io.retail(FourCC('C', 'S', 'K', 'R'), sids[0], ref)) {
      throw Fail{"retail skin " + Hex8(sids[0]) + " is not on the disc"};
    }
    const Span refSpan{ref.data(), ref.size()};
    const std::vector<uint32_t> refBones = SkinBones(refSpan);
    skins.push_back(sids[0]);
    for (size_t i = 1; i < sids.size(); ++i) {
      Blob other;
      if (io.retail(FourCC('C', 'S', 'K', 'R'), sids[i], other)) {
        const std::vector<uint32_t> bones = SkinBones(Span{other.data(), other.size()});
        if (std::includes(bones.begin(), bones.end(), refBones.begin(), refBones.end())) {
          skins.push_back(sids[i]);
          continue;
        }
      }
      Log("  note: skin " + Hex8(sids[i]) + " has a different skeleton, not writing weights for it");
    }
    std::vector<uint16_t> J;
    std::vector<float> W;
    if (skinned) {
      for (uint32_t bi : bufOrder) {
        const ModelVertexBuffer& src = *buffers[bi].src;
        for (uint32_t v : buffers[bi].srcOf) {
          J.insert(J.end(), src.joints.begin() + size_t(v) * 4, src.joints.begin() + size_t(v) * 4 + 4);
          W.insert(W.end(), src.weights.begin() + size_t(v) * 4, src.weights.begin() + size_t(v) * 4 + 4);
        }
      }
    }
    const std::vector<WeightKey> perVertex =
        SkinWeights(P, n, skinned ? &J : nullptr, skinned ? &W : nullptr, retail, refSpan,
                    [this](const std::string& line) { Log(Hex8(this->model) + line); });
    std::map<WeightKey, int> intern;
    weights.resize(n);
    for (size_t v = 0; v < n; ++v) {
      const auto it = intern.emplace(perVertex[v], int(weightKeys.size()));
      if (it.second) {
        weightKeys.push_back(perVertex[v]);
      }
      weights[v] = it.first->second;
    }
  }
  const bool hasSkin = !skins.empty();

  // Weld: Remastered splits vertices far more than the written attributes
  // need. Vertices equal in every written value (float32 position, normal, all
  // UV arrays, weight group) become one, so this is lossless; the first of
  // each keeps its place.
  {
    const size_t width = (6 + (useTan ? 4 : 0) + (useTan2 ? 4 : 0) + uvArrays.size() * 2) * 4 + 4 + 4;
    std::unordered_map<std::string, uint32_t> seen;
    seen.reserve(n * 2);
    std::vector<uint32_t> vmap(n), rep;
    std::string keyBytes(width, '\0');
    for (size_t v = 0; v < n; ++v) {
      char* out = keyBytes.data();
      auto put = [&](double d) {
        const float f = float(d);
        std::memcpy(out, &f, 4);
        out += 4;
      };
      for (int c = 0; c < 3; ++c) {
        put(P[v * 3 + c]);
      }
      for (int c = 0; c < 3; ++c) {
        put(N[v * 3 + c]);
      }
      if (useTan) {
        for (int c = 0; c < 4; ++c) {
          put(T[v * 4 + c]);
        }
      }
      if (useTan2) {
        for (int c = 0; c < 4; ++c) {
          put(T1[v * 4 + c]);
        }
      }
      for (const auto& a : uvArrays) {
        put(a[v * 2]);
        put(a[v * 2 + 1]);
      }
      const int32_t wid = hasSkin ? weights[v] : 0;
      std::memcpy(out, &wid, 4);
      if (useColor) {
        std::memcpy(out + 4, &C[v * 4], 4);
      }
      const auto it = seen.emplace(keyBytes, uint32_t(rep.size()));
      if (it.second) {
        rep.push_back(uint32_t(v));
      }
      vmap[v] = it.first->second;
    }
    for (Prim& p : prims) {
      for (uint32_t& i : p.I) {
        i = vmap[i];
      }
    }
    auto take = [&](std::vector<double>& a, size_t per) {
      std::vector<double> out(rep.size() * per);
      for (size_t i = 0; i < rep.size(); ++i) {
        std::copy(a.begin() + size_t(rep[i]) * per, a.begin() + size_t(rep[i]) * per + per, out.begin() + i * per);
      }
      a = std::move(out);
    };
    take(P, 3);
    take(N, 3);
    if (useTan) {
      take(T, 4);
    }
    if (useTan2) {
      take(T1, 4);
    }
    for (auto& a : uvArrays) {
      take(a, 2);
    }
    if (useColor) {
      std::vector<uint8_t> c(rep.size() * 4);
      for (size_t i = 0; i < rep.size(); ++i) {
        std::copy(C.begin() + size_t(rep[i]) * 4, C.begin() + size_t(rep[i]) * 4 + 4, c.begin() + i * 4);
      }
      C = std::move(c);
    }
    if (hasSkin) {
      std::vector<int> w(rep.size());
      for (size_t i = 0; i < rep.size(); ++i) {
        w[i] = weights[rep[i]];
      }
      weights = std::move(w);
    }
    Log("  welded " + std::to_string(n) + " -> " + std::to_string(rep.size()) + " verts");
    n = rep.size();
  }
  const size_t nuv = n * uvArrays.size();
  // Past 16-bit indices, write windowed surfaces (the port's own 'PBIX' base
  // indices).
  const bool big = n >= 0xFFFF || nuv >= 0x10000;

  auto writeCskr = [&](const std::vector<std::pair<const WeightKey*, uint32_t>>& runs, size_t total) {
    const Blob cskr = CskrBytes(runs, total);
    for (uint32_t s : skins) {
      uint32_t id = s;
      for (size_t i = 0; i < opt.outputSkins.size() && i < opt.skins.size(); ++i) {
        if (opt.skins[i] == s) {
          id = opt.outputSkins[i];
        }
      }
      Write(Hex8(id) + ".CSKR", cskr);
    }
  };

  // One NBT entry of the normal section: N, B, T, with B = w * cross(N, T) so the shader can
  // read the handedness back.
  auto putNormal = [&](Blob& out, size_t v) {
    for (int c = 0; c < 3; ++c) {
      PF(out, N[v * 3 + c]);
    }
    if (useTan) {
      const double* nn = &N[v * 3];
      const double* tt = &T[v * 4];
      const double cr[3] = {nn[1] * tt[2] - nn[2] * tt[1], nn[2] * tt[0] - nn[0] * tt[2],
                            nn[0] * tt[1] - nn[1] * tt[0]};
      for (int c = 0; c < 3; ++c) {
        PF(out, cr[c] * tt[3]);
      }
      for (int c = 0; c < 3; ++c) {
        PF(out, tt[c]);
      }
      if (useTan2) {
        const double* t1 = &T1[v * 4];
        const double c1[3] = {nn[1] * t1[2] - nn[2] * t1[1], nn[2] * t1[0] - nn[0] * t1[2],
                              nn[0] * t1[1] - nn[1] * t1[0]};
        for (int c = 0; c < 3; ++c) {
          PF(out, c1[c] * t1[3]);
        }
        for (int c = 0; c < 3; ++c) {
          PF(out, t1[c]);
        }
      }
    }
  };

  std::vector<Blob> surfs;
  Blob secP, secN, secC, secUV;
  double lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
  bool haveBounds = false;
  auto bound = [&](const double* p) {
    for (int c = 0; c < 3; ++c) {
      lo[c] = haveBounds ? std::min(lo[c], p[c]) : p[c];
      hi[c] = haveBounds ? std::max(hi[c], p[c]) : p[c];
    }
    haveBounds = true;
  };
  const size_t kDlChunk = 65535 / 3 * 3;
  if (big) {
    // Each surface covers a run of at most `window` vertices (duplicated where
    // windows meet) and stores base indices after its bounds, which the port
    // adds to every display list index. UVs are interleaved per vertex so one
    // base serves every texture attribute. Skin runs are per window, so weight
    // groups may repeat.
    const size_t k = uvArrays.size();
    const size_t window = 0xFFFF / k;
    std::vector<std::pair<const WeightKey*, uint32_t>> runs;
    size_t start = 0;
    std::vector<int> local(n, -1);
    for (const Prim& p : prims) {
      const std::vector<uint32_t>& attrs = dlAttrs[p.omat];
      std::vector<std::pair<std::vector<uint32_t>, std::vector<uint32_t>>> chunks;  // vertices, triangles
      std::vector<uint32_t> verts, cur;
      auto flush = [&]() {
        for (uint32_t v : verts) {
          local[v] = -1;
        }
        chunks.emplace_back(std::move(verts), std::move(cur));
        verts.clear();
        cur.clear();
      };
      for (size_t t = 0; t + 2 < p.I.size(); t += 3) {
        const uint32_t a = p.I[t], b = p.I[t + 1], c = p.I[t + 2];
        size_t fresh = local[a] < 0 ? 1 : 0;
        fresh += local[b] < 0 && b != a ? 1 : 0;
        fresh += local[c] < 0 && c != a && c != b ? 1 : 0;
        if (verts.size() + fresh > window) {
          flush();
        }
        for (uint32_t v : {a, b, c}) {
          if (local[v] < 0) {
            local[v] = int(verts.size());
            verts.push_back(v);
          }
          cur.push_back(v);
        }
      }
      if (!cur.empty()) {
        flush();
      }
      for (auto& chunk : chunks) {
        std::vector<uint32_t>& cv = chunk.first;
        if (hasSkin) {
          std::vector<int> keyOrder(weightKeys.size(), -1);
          std::vector<int> order;
          std::vector<uint32_t> counts;
          for (uint32_t v : cv) {
            if (keyOrder[weights[v]] < 0) {
              keyOrder[weights[v]] = int(order.size());
              order.push_back(weights[v]);
              counts.push_back(0);
            }
            ++counts[keyOrder[weights[v]]];
          }
          std::stable_sort(cv.begin(), cv.end(),
                           [&](uint32_t x, uint32_t y) { return keyOrder[weights[x]] < keyOrder[weights[y]]; });
          for (size_t i = 0; i < order.size(); ++i) {
            runs.emplace_back(&weightKeys[order[i]], counts[i]);
          }
        }
        for (size_t i = 0; i < cv.size(); ++i) {
          local[cv[i]] = int(i);
        }
        double centre[3] = {0, 0, 0}, clo[3], chi[3];
        for (size_t i = 0; i < cv.size(); ++i) {
          const double* pos = &P[size_t(cv[i]) * 3];
          for (int c = 0; c < 3; ++c) {
            PF(secP, pos[c]);
            centre[c] += pos[c];
            clo[c] = i ? std::min(clo[c], pos[c]) : pos[c];
            chi[c] = i ? std::max(chi[c], pos[c]) : pos[c];
          }
          bound(pos);
          putNormal(secN, size_t(cv[i]));
          if (useColor) {
            secC.insert(secC.end(), C.begin() + size_t(cv[i]) * 4, C.begin() + size_t(cv[i]) * 4 + 4);
          }
          for (const auto& a : uvArrays) {
            PF(secUV, a[size_t(cv[i]) * 2]);
            PF(secUV, a[size_t(cv[i]) * 2 + 1]);
          }
        }
        for (double& c : centre) {
          c /= double(cv.size());
        }
        Blob dl;
        const std::vector<uint32_t>& tris = chunk.second;
        for (size_t s = 0; s < tris.size(); s += kDlChunk) {
          const size_t count = std::min(kDlChunk, tris.size() - s);
          P8(dl, useTan2 && twoTangent(p) ? 0x94 : useTan ? 0x93 : 0x90);
          P16(dl, uint32_t(count));
          for (size_t i = s; i < s + count; ++i) {
            const uint32_t j = uint32_t(local[tris[i]]);
            P16(dl, j);
            P16(dl, j);
            if (dlColor[p.omat]) {
              P16(dl, j);
            }
            for (uint32_t a : attrs) {
              P16(dl, uint32_t(j * k + a));
            }
          }
        }
        Pad(dl);
        for (uint32_t v : cv) {
          local[v] = -1;
        }
        Blob s;
        SurfaceHeader(s, centre, uint32_t(p.omat), dl.size(), 64);
        for (double v : clo) {
          PF(s, v);
        }
        for (double v : chi) {
          PF(s, v);
        }
        P32(s, 0x50424958);  // 'PBIX': position, normal, colour, uv, packed uv bases
        P32(s, uint32_t(start));
        P32(s, uint32_t(start));
        P32(s, useColor ? uint32_t(start) : 0);
        P32(s, uint32_t(start * k));
        P32(s, 0);
        s.resize(s.size() + 16, 0);
        Pad(s);
        Append(s, dl);
        surfs.push_back(std::move(s));
        start += cv.size();
      }
    }
    if (hasSkin) {
      writeCskr(runs, start);
    }
    Log("  windowed: " + std::to_string(surfs.size()) + " surfaces, " + std::to_string(start) + " verts");
    n = start;
  } else {
    // A skin is a list of weight groups, each covering a run of vertices, so
    // the vertices are ordered by group.
    std::vector<uint32_t> order(n);
    for (size_t v = 0; v < n; ++v) {
      order[v] = uint32_t(v);
    }
    if (hasSkin) {
      std::vector<int> keyOrder(weightKeys.size(), -1);
      std::vector<int> first;
      std::vector<uint32_t> counts;
      for (size_t v = 0; v < n; ++v) {
        if (keyOrder[weights[v]] < 0) {
          keyOrder[weights[v]] = int(first.size());
          first.push_back(weights[v]);
          counts.push_back(0);
        }
        ++counts[keyOrder[weights[v]]];
      }
      std::stable_sort(order.begin(), order.end(),
                       [&](uint32_t x, uint32_t y) { return keyOrder[weights[x]] < keyOrder[weights[y]]; });
      std::vector<std::pair<const WeightKey*, uint32_t>> runs;
      for (size_t i = 0; i < first.size(); ++i) {
        runs.emplace_back(&weightKeys[first[i]], counts[i]);
      }
      writeCskr(runs, n);
    }
    std::vector<uint32_t> remap(n);
    for (size_t i = 0; i < n; ++i) {
      remap[order[i]] = uint32_t(i);
    }
    for (size_t i = 0; i < n; ++i) {
      const double* pos = &P[size_t(order[i]) * 3];
      bound(pos);
      for (int c = 0; c < 3; ++c) {
        PF(secP, pos[c]);
      }
      putNormal(secN, size_t(order[i]));
      if (useColor) {
        secC.insert(secC.end(), C.begin() + size_t(order[i]) * 4, C.begin() + size_t(order[i]) * 4 + 4);
      }
    }
    for (const auto& a : uvArrays) {
      for (size_t i = 0; i < n; ++i) {
        PF(secUV, a[size_t(order[i]) * 2]);
        PF(secUV, a[size_t(order[i]) * 2 + 1]);
      }
    }
    std::vector<uint8_t> mark(n, 0);
    for (const Prim& p : prims) {
      const std::vector<uint32_t>& attrs = dlAttrs[p.omat];
      Blob dl;
      for (size_t s = 0; s < p.I.size(); s += kDlChunk) {
        const size_t count = std::min(kDlChunk, p.I.size() - s);
        P8(dl, useTan2 && twoTangent(p) ? 0x94 : useTan ? 0x93 : 0x90);
        P16(dl, uint32_t(count));
        for (size_t i = s; i < s + count; ++i) {
          const uint32_t j = remap[p.I[i]];
          mark[j] = 1;
          P16(dl, j);
          P16(dl, j);
          if (dlColor[p.omat]) {
            P16(dl, j);
          }
          for (uint32_t a : attrs) {
            P16(dl, uint32_t(j + a * n));
          }
        }
      }
      Pad(dl);
      // The mean of the surface's vertices, each once, in index order.
      double centre[3] = {0, 0, 0};
      size_t count = 0;
      for (size_t j = 0; j < n; ++j) {
        if (mark[j]) {
          mark[j] = 0;
          const double* pos = &P[size_t(order[j]) * 3];
          for (int c = 0; c < 3; ++c) {
            centre[c] += pos[c];
          }
          ++count;
        }
      }
      for (double& c : centre) {
        c /= double(count);
      }
      Blob s;
      SurfaceHeader(s, centre, uint32_t(p.omat), dl.size(), 0);
      Pad(s);
      Append(s, dl);
      surfs.push_back(std::move(s));
    }
  }
  Pad(secP);
  Pad(secN);
  Pad(secC);
  Pad(secUV);

  std::vector<Blob> secs;
  for (size_t si = 0; si < retail.nmat; ++si) {
    Blob b;
    P32(b, uint32_t(setTex[si].size()));
    for (uint32_t t : setTex[si]) {
      P32(b, t);
    }
    P32(b, uint32_t(setBlobs[si].size()));
    uint32_t end = 0;
    for (const Blob& m : setBlobs[si]) {
      end += uint32_t(m.size());
      P32(b, end);
    }
    for (const Blob& m : setBlobs[si]) {
      Append(b, m);
    }
    Pad(b);
    secs.push_back(std::move(b));
  }
  secs.push_back(std::move(secP));
  secs.push_back(std::move(secN));
  secs.push_back(std::move(secC));  // colours
  secs.push_back(std::move(secUV));
  secs.emplace_back();  // packed texcoords
  {
    Blob table;
    P32(table, uint32_t(surfs.size()));
    for (const Blob& s : surfs) {
      P32(table, uint32_t(s.size()));
    }
    Pad(table);
    secs.push_back(std::move(table));
  }
  size_t tris = 0;
  for (const Prim& p : prims) {
    tris += p.I.size() / 3;
  }
  const size_t nsurf = surfs.size();
  for (Blob& s : surfs) {
    secs.push_back(std::move(s));
  }
  Blob out;
  P32(out, 0xDEADBABE);
  P32(out, 2);
  // Float normals, and the packed texcoord section is always written (empty): a retail
  // model without it (the Frigate's B8CB941D) would have the game take that empty
  // section for the surface table.
  P32(out, ((retail.flags | 0x4) & ~uint32_t(0x2)) | (useTan ? 0x8u : 0u) | (useTan2 ? 0x10u : 0u));
  for (double v : lo) {
    PF(out, v);
  }
  for (double v : hi) {
    PF(out, v);
  }
  P32(out, uint32_t(secs.size()));
  P32(out, retail.nmat);
  for (const Blob& s : secs) {
    P32(out, uint32_t(s.size()));
  }
  Pad(out);
  for (const Blob& s : secs) {
    Append(out, s);
  }
  const uint32_t outputModel = opt.outputModel != 0 ? opt.outputModel : opt.retail;
  Write(Hex8(outputModel) + ".CMDL", out);
  if (!model.anuv.empty()) {
    Log("  ANUV: " + std::to_string(anuvCounts.animated) + " materials animated, " +
        std::to_string(anuvCounts.notFlattened) + " not flattened, " + std::to_string(anuvCounts.entryDisagree) +
        " mesh entries disagree, " + std::to_string(anuvCounts.sharedSlots) + " shared slots, " +
        std::to_string(anuvCounts.scrollOverridden) + " scrolls overridden, " + std::to_string(anuvCounts.slotSplit) +
        " texcoord copies, " + std::to_string(anuvCounts.outOfSlots) + " out of texcoords");
  }
  Log("  wrote " + Hex8(outputModel) + ".CMDL: " + std::to_string(n) + " verts, " + std::to_string(tris) + " tris, " +
      std::to_string(keys.size()) + " materials, " + std::to_string(nsurf) + " surfaces");
}

Converter::Converter(ConvertIO io) : m_state(new State) { m_state->io = std::move(io); }

Converter::~Converter() { delete m_state; }

bool Converter::Convert(const Model& model, const ConvertOptions& options, std::string& error) {
  if ((!m_state->io.retail && !options.standalone) || !m_state->io.texture || !m_state->io.write) {
    error = "the converter has no way to read or write";
    return false;
  }
  try {
    m_state->model = options.retail;
    m_state->Convert(model, options);
  } catch (const Fail& f) {
    error = Hex8(options.retail) + ": " + f.what;
    return false;
  } catch (const std::exception& e) {
    error = Hex8(options.retail) + ": " + e.what();
    return false;
  }
  return true;
}

int Converter::PbrMaterials() const { return m_state->pbr; }
int Converter::TevMaterials() const { return m_state->tev; }

} // namespace PortRemastered
