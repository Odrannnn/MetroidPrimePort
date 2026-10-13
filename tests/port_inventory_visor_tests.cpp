// The pause screen's Remastered visor (kb material/4c37625c.md, 17dd0a37.md).
// Converter: 4c37625c is kind 36 (SrcAlpha / InvSrcAlpha, the screen copy's stage) and 17dd0a37
// kind 37 (SrcAlpha / One), whatever their mesh class says, and both keep their REFL cube.
// Runtime: CCubeModel::PortInventoryModulation's DIFC, ICMC and C for each of the doll's passes.

#include "port_remastered_convert.h"

#include "Kyoto/Graphics/CCubeModel.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace PortRemastered;

namespace {

int sFailures = 0;

void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++sFailures;
  }
}

constexpr uint32_t FourCC(char a, char b, char c, char d) {
  return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) | (uint32_t(uint8_t(c)) << 8) | uint8_t(d);
}

ModelMaterialData Texture(uint32_t usage, uint8_t id) {
  ModelMaterialData d;
  d.usage = usage;
  d.kind = ModelMaterialData::Kind::Texture;
  d.texture.usage = usage;
  d.texture.hasUsage = true;
  d.texture.id[3] = id;
  d.texture.wrapX = 1;
  d.texture.wrapY = 1;
  return d;
}

ModelMaterial Material(uint32_t shader) {
  ModelMaterial material;
  material.name = "visor" + std::to_string(shader & 0xFF);
  material.unk1 = 0x1;
  material.types.push_back(FourCC('R', 'L', 'T', 'G'));
  for (int i = 0; i < 4; ++i) {
    material.shaderId[i] = uint8_t(shader >> (24 - 8 * i));
  }
  material.data.push_back(Texture(FourCC('B', 'C', 'L', 'R'), 0x11));
  material.data.push_back(Texture(FourCC('R', 'E', 'F', 'L'), 0x33));
  return material;
}

// Both materials on mesh class 1 (SrcAlpha, InvSrcAlpha): the additive one must not take it.
Model BuildModel() {
  Model model;
  ModelVertexBuffer vb;
  vb.vertexCount = 3;
  for (int v = 0; v < 3; ++v) {
    vb.positions.insert(vb.positions.end(), {float(v), float(v * v), 0.f});
    vb.normals.insert(vb.normals.end(), {0.f, 0.f, 1.f});
  }
  vb.uvs.resize(1);
  for (int v = 0; v < 3; ++v) {
    vb.uvs[0].insert(vb.uvs[0].end(), {float(v), 0.f});
  }
  model.vertexBuffers.push_back(vb);
  const uint32_t shaders[] = {0x4C37625C, 0x17DD0A37};
  for (uint32_t i = 0; i < 2; ++i) {
    model.materials.push_back(Material(shaders[i]));
    ModelMesh mesh;
    mesh.material = i;
    mesh.vertexBuffer = 0;
    mesh.bits2 = 1;
    mesh.indices = {0, 1, 2};
    model.meshes.push_back(mesh);
  }
  return model;
}

uint32_t Be32(const std::vector<uint8_t>& d, size_t o) {
  return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
}

uint32_t Be16(const std::vector<uint8_t>& d, size_t o) { return (uint32_t(d[o]) << 8) | d[o + 1]; }

void Put32(std::vector<uint8_t>& out, uint32_t v) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(uint8_t(v >> shift));
  }
}

void Pad32(std::vector<uint8_t>& out) { out.resize((out.size() + 31) & ~size_t(31), 0); }

uint32_t FloatBits(float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, sizeof(bits));
  return bits;
}

// A disc model for the visor (CSamusDoll's, retail blends it SrcAlpha / InvSrcAlpha): one set of
// one material (4, 5) with no texture, and one surface drawing BuildModel's three vertices in
// the GameCube's frame (x, y, z) = (-x, z, y).
std::vector<uint8_t> RetailVisor() {
  std::vector<uint8_t> mat;
  Put32(mat, 0);    // flags
  Put32(mat, 0);    // textures
  Put32(mat, 0x3);  // vertex attributes: a u16 position index
  Put32(mat, 0);    // group
  Put32(mat, (5u << 16) | 4u);  // destination, source: GX_BL_INVSRCALPHA, GX_BL_SRCALPHA
  Put32(mat, 1);    // colour channels
  Put32(mat, 0);
  Put32(mat, 1);    // one TEV stage, no texture
  for (uint32_t v : {0x7A14Fu, 0x21CE7u, 0x100u, 0x100u, 0u}) {
    Put32(mat, v);
  }
  Put32(mat, 0x0000FFFF);
  Put32(mat, 0);  // texgens
  Put32(mat, 4);  // UV animations: none
  Put32(mat, 0);
  std::vector<std::vector<uint8_t>> secs(8);
  Put32(secs[0], 0);  // the set's textures
  Put32(secs[0], 1);
  Put32(secs[0], uint32_t(mat.size()));
  secs[0].insert(secs[0].end(), mat.begin(), mat.end());
  for (int v = 0; v < 3; ++v) {
    for (float c : {-float(v), 0.f, float(v * v)}) {
      Put32(secs[1], FloatBits(c));
    }
  }
  std::vector<uint8_t>& surface = secs[7];
  surface.resize(12, 0);  // centre
  Put32(surface, 0);      // material
  Put32(surface, 32);     // display list size
  surface.resize(28, 0);
  Put32(surface, 0);  // extra
  surface.resize(64, 0);
  for (uint8_t b : {0x90, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02}) {  // GX_TRIANGLES 0, 1, 2
    surface.push_back(b);
  }
  Put32(secs[6], 1);  // surfaces, and where each ends
  Put32(secs[6], 96);
  for (std::vector<uint8_t>& s : secs) {
    Pad32(s);
  }
  std::vector<uint8_t> out;
  Put32(out, 0xDEADBABE);
  Put32(out, 2);
  Put32(out, 0);  // flags
  out.resize(0x24, 0);
  Put32(out, uint32_t(secs.size()));
  Put32(out, 1);  // material sets
  for (const std::vector<uint8_t>& s : secs) {
    Put32(out, uint32_t(s.size()));
  }
  Pad32(out);
  for (const std::vector<uint8_t>& s : secs) {
    out.insert(out.end(), s.begin(), s.end());
  }
  return out;
}

struct OutMaterial {
  uint32_t src, dst;
  bool screen;  // a TEV stage samples map 7, the screen copy
};

// As port_remastered_blend_tests: each material's (source, destination) factors, and whether one
// of its stages binds map 7.
bool Materials(const std::vector<uint8_t>& d, std::vector<OutMaterial>& out) {
  if (d.size() < 48) {
    return false;
  }
  const uint32_t nsec = Be32(d, 36);
  if (nsec < 3 || 44 + 4 * size_t(nsec) > d.size()) {
    return false;
  }
  const size_t base = (44 + 4 * size_t(nsec) + 31) & ~size_t(31);
  const uint32_t ntex = Be32(d, base);
  const size_t countAt = base + 4 + 4 * size_t(ntex);
  const uint32_t n = Be32(d, countAt);
  const size_t ends = countAt + 4;
  const size_t first = ends + 4 * size_t(n);
  if (first > d.size()) {
    return false;
  }
  uint32_t begin = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const size_t m = first + begin;
    if (Be32(d, m) & 0x8) {
      return false;
    }
    const uint32_t nt = Be32(d, m + 4);
    const size_t at = m + 8 + 4 * size_t(nt) + 8;
    if (at + 12 > d.size()) {
      return false;
    }
    OutMaterial mat = {Be16(d, at + 2), Be16(d, at), false};
    size_t o = at + 4 + ((Be32(d, m) & 0x400) ? 4 : 0);
    o += 4 + 4 * size_t(Be32(d, o));
    const uint32_t ntev = Be32(d, o);
    o += 4 + 20 * size_t(ntev);
    if (o + 4 * size_t(ntev) > d.size()) {
      return false;
    }
    for (uint32_t s = 0; s < ntev; ++s) {
      mat.screen = mat.screen || d[o + 4 * s + 2] == 7;
    }
    out.push_back(mat);
    begin = Be32(d, ends + 4 * size_t(i));
  }
  return true;
}

// Standalone (the visor converted on its own) and against the disc's visor model, as the game
// imports it: both give the same two materials.
void TestConverter(bool standalone) {
  const char* const pathName = standalone ? "standalone" : "retail";
  const std::vector<uint8_t> retailCmdl = RetailVisor();
  std::vector<uint8_t> cmdl;
  std::vector<MaterialDecision> decisions;
  ConvertIO io;
  io.texture = [](const ModelUuid&, Image& out, std::string&) {
    out.width = out.height = 4;
    out.rgba.assign(64, 200);
    return true;
  };
  io.cube = [](const ModelUuid&, uint32_t& edge, std::vector<float>& rgba, std::string&) {
    edge = 4;
    rgba.assign(6 * 4 * 4 * 4, 0.5f);
    return true;
  };
  io.write = [&cmdl](const std::string& name, const std::vector<uint8_t>& data) {
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".CMDL") == 0) {
      cmdl = data;
    }
    return true;
  };
  io.decision = [&decisions](const MaterialDecision& d) { decisions.push_back(d); };
  ConvertOptions opt;
  opt.standalone = standalone;
  opt.retail = 0xABCD1234;
  opt.skip.clear();
  if (!standalone) {
    io.retail = [&retailCmdl, &opt](uint32_t type, uint32_t id, std::vector<uint8_t>& out) {
      if (type != FourCC('C', 'M', 'D', 'L') || id != opt.retail) {
        return false;
      }
      out = retailCmdl;
      return true;
    };
    io.retailId = [](uint32_t) { return false; };
  }
  Converter converter(io);
  std::string error;
  if (!converter.Convert(BuildModel(), opt, error)) {
    std::fprintf(stderr, "FAIL: %s: the conversion failed: %s\n", pathName, error.c_str());
    ++sFailures;
    return;
  }
  auto check = [pathName](bool condition, const char* what) {
    if (!condition) {
      std::fprintf(stderr, "FAIL: %s: %s\n", pathName, what);
      ++sFailures;
    }
  };
  check(decisions.size() == 2, "two materials decided");
  for (const MaterialDecision& d : decisions) {
    const bool glass = d.shader == "4C37625C";
    std::fprintf(stderr, "%s %s: kind %d path %s tag %s cube %s retail %s (%s)\n", pathName, d.shader.c_str(), d.kind,
                 d.path.c_str(), d.tag.c_str(), d.cube.c_str(), d.retail.c_str(), d.kindReason.c_str());
    check(d.kind == (glass ? 36 : 37), glass ? "4c37625c is kind 36" : "17dd0a37 is kind 37");
    check(d.path == "pbr", "the visor's materials are PBR");
    check(d.cube != "-" && !d.cube.empty(), "the visor's materials keep their REFL cube");
    check(d.tag == "PBR7", "the cube is in the record (PBR7)");
    check(standalone || d.retail.compare(0, 4, "4,5 ") == 0, "the disc material blends (4, 5)");
  }
  std::vector<OutMaterial> f;
  check(Materials(cmdl, f) && f.size() == 2, "the visor's materials are readable");
  if (f.size() == 2 && decisions.size() == 2) {
    for (size_t i = 0; i < 2; ++i) {
      const bool glass = decisions[i].shader == "4C37625C";
      const OutMaterial& m = f[size_t(decisions[i].index)];
      check(m.src == 4 && m.dst == (glass ? 5u : 1u),
            glass ? "kind 36 blends (SrcAlpha, InvSrcAlpha)" : "kind 37 blends (SrcAlpha, One) on a class 1 mesh");
      check(m.screen == glass, glass ? "kind 36 binds the screen copy (map 7)" : "kind 37 binds no screen copy");
    }
  }
}

// The draw decision under a pass: only kinds 36 and 37 change, and only those under the pass's
// own mode go PBR. Every other kind is outside, whatever the pass, so it keeps its own PBR path.
void TestDrawDecision() {
  typedef CCubeModel M;
  const M::PortInventoryPass base = {false, {1.f, 1.f, 1.f, 1.f}, 0.f, 1.f};
  const M::PortInventoryPass pulse = {true, {1.f, 1.f, 1.f, 1.f}, 0.5f, 1.f};
  const int blend = CModelFlags::kT_Blend;
  const int additive = CModelFlags::kT_Additive;
  const int opaque = CModelFlags::kT_Opaque;
  // No pass: the visor's kinds draw as they would anywhere else.
  Check(M::PortInventoryDraw(false, base, 36.f, blend, true, true) == M::kPID_Outside, "no pass: 36 outside");
  Check(M::PortInventoryDraw(false, pulse, 37.f, additive, true, true) == M::kPID_Outside, "no pass: 37 outside");
  // Under a pass, other kinds are untouched, in either pass and whatever the screen copy.
  for (float kind : {-1.f, 0.f, 8.f, 34.f, 35.f, 38.f}) {
    for (int trans : {opaque, blend, additive}) {
      Check(M::PortInventoryDraw(true, base, kind, trans, true, false) == M::kPID_Outside &&
                M::PortInventoryDraw(true, pulse, kind, trans, false, true) == M::kPID_Outside,
            "a pass leaves other kinds outside");
    }
  }
  // The pass's own mode, the glass with its screen copy: alternating, each decided alone.
  for (int round = 0; round < 2; ++round) {
    Check(M::PortInventoryDraw(true, base, 36.f, blend, true, true) == M::kPID_Pbr, "base pass: 36 PBR");
    Check(M::PortInventoryDraw(true, base, 37.f, blend, true, false) == M::kPID_Pbr, "base pass: 37 PBR");
    Check(M::PortInventoryDraw(true, pulse, 36.f, additive, true, true) == M::kPID_Pbr, "pulse pass: 36 PBR");
    Check(M::PortInventoryDraw(true, pulse, 37.f, additive, true, false) == M::kPID_Pbr, "pulse pass: 37 PBR");
  }
  Check(M::PortInventoryDraw(true, base, 36.f, blend, true, false) == M::kPID_Tev, "36 without the copy: TEV");
  Check(M::PortInventoryDraw(true, base, 37.f, blend, false, true) == M::kPID_Tev, "not PBR-allowed: TEV");
  // Another mode than the pass's falls back.
  for (float kind : {36.f, 37.f}) {
    Check(M::PortInventoryDraw(true, base, kind, opaque, true, true) == M::kPID_Tev, "base pass, opaque: TEV");
    Check(M::PortInventoryDraw(true, base, kind, additive, true, true) == M::kPID_Tev, "base pass, additive: TEV");
    Check(M::PortInventoryDraw(true, pulse, kind, blend, true, true) == M::kPID_Tev, "pulse pass, blended: TEV");
    Check(M::PortInventoryDraw(true, pulse, kind, opaque, true, true) == M::kPID_Tev, "pulse pass, opaque: TEV");
  }
}

// The flags of the visor's PBR draw: depth compared, not written, whatever the doll's, for the
// blended and the additive pass; shader set, colour and the other flag bits are the doll's.
// Every other draw keeps the flags it had.
void TestDrawFlags() {
  typedef CCubeModel M;
  const CColor tint(static_cast< uchar >(64), static_cast< uchar >(128), static_cast< uchar >(191),
                    static_cast< uchar >(153));
  const uint other = CModelFlags::kF_DepthCompare | CModelFlags::kF_DepthUpdate | CModelFlags::kF_NoTextureLock |
                     CModelFlags::kF_DepthNonInclusive;
  for (CModelFlags::ETrans trans : {CModelFlags::kT_Blend, CModelFlags::kT_Additive}) {
    const CModelFlags model(trans, 3, static_cast< CModelFlags::EFlags >(other), tint);
    const CModelFlags opaque(CModelFlags::kT_Opaque, 3, static_cast< CModelFlags::EFlags >(other), tint);
    const CModelFlags flags = M::PortDrawFlags(true, false, opaque, model);
    const uint expected = (other & ~static_cast< uint >(CModelFlags::kF_DepthUpdate));
    Check(flags.GetTrans() == CModelFlags::kT_Opaque, "inventory: shaded as opaque");
    Check((flags.GetOtherFlags() & CModelFlags::kF_DepthCompare) != 0, "inventory: depth compare on");
    Check((flags.GetOtherFlags() & CModelFlags::kF_DepthUpdate) == 0, "inventory: depth write off");
    Check(static_cast< uint >(flags.GetOtherFlags()) == expected, "inventory: other flag bits preserved");
    Check(flags.GetShaderSet() == 3, "inventory: shader set preserved");
    Check(flags.GetColor() == tint, "inventory: colour preserved");
    // A doll that already compares nothing is still compared against.
    const CModelFlags none(trans, 3, CModelFlags::kF_Nothing, tint);
    const CModelFlags noneOpaque(CModelFlags::kT_Opaque, 3, CModelFlags::kF_Nothing, tint);
    const CModelFlags forced = M::PortDrawFlags(true, true, noneOpaque, none);
    Check(forced.GetOtherFlags() == CModelFlags::kF_DepthCompare, "inventory: compare on, write off from none");
    // The other families.
    Check(M::PortDrawFlags(false, false, opaque, model) == model, "outside: the doll's flags");
    Check(M::PortDrawFlags(false, true, opaque, model) == opaque, "solid blend: opaque flags unchanged");
  }
}

bool Near(float a, float b) { return std::fabs(a - b) < 1e-6f; }

bool Eq4(const float* v, float a, float b, float c, float d) {
  return Near(v[0], a) && Near(v[1], b) && Near(v[2], c) && Near(v[3], d);
}

void TestModulation() {
  float diffuse[4], incan[4], coverage = -1.f;
  CCubeModel::PortInventoryPass pass = {false, {0.75f, 0.75f, 0.75f, 0.6f}, 0.5f, 0.6f};
  Check(CCubeModel::PortInventoryModulation(pass, diffuse, incan, coverage), "the base pass draws");
  Check(Eq4(diffuse, 0.75f, 0.75f, 0.75f, 0.6f), "base pass: DIFC is the visor colour");
  Check(Eq4(incan, 0.f, 0.f, 0.f, 1.f), "base pass: ICMC is (0, 0, 0, 1)");
  Check(Near(coverage, 0.6f), "base pass: C is drawAlpha");

  pass.additive = true;
  pass.coverage = 1.f;
  pass.pulse = 0.5f;
  Check(CCubeModel::PortInventoryModulation(pass, diffuse, incan, coverage), "a pulse draws");
  Check(Eq4(diffuse, 1.f, 1.f, 1.f, 1.f), "pulse at full coverage: DIFC white");
  Check(Eq4(incan, 0.2f, 0.05f, 0.f, 1.f), "pulse at full coverage: ICMC is the pulse colour");
  Check(Near(coverage, 1.f), "pulse at full coverage: C 1");

  pass.coverage = 1.f - 5e-6f;
  Check(CCubeModel::PortInventoryModulation(pass, diffuse, incan, coverage) && Eq4(incan, 0.2f, 0.05f, 0.f, 1.f),
        "coverage within 1e-5 of 1 counts as full");

  pass.coverage = 0.5f;
  pass.pulse = 0.25f;
  Check(CCubeModel::PortInventoryModulation(pass, diffuse, incan, coverage), "a partial pulse draws");
  Check(Eq4(diffuse, 0.1f, 0.025f, 0.f, 0.25f), "partial coverage: DIFC is the pulse colour, alpha the pulse");
  Check(Eq4(incan, 0.f, 0.f, 0.f, 1.f), "partial coverage: ICMC neutral");
  Check(Near(coverage, 0.5f), "partial coverage: C is drawAlpha");

  pass.pulse = 0.f;
  Check(!CCubeModel::PortInventoryModulation(pass, diffuse, incan, coverage), "no pulse, no second pass");
  pass.additive = false;
  Check(CCubeModel::PortInventoryModulation(pass, diffuse, incan, coverage), "the base pass draws at no pulse");

  Check(CCubeModel::PortInventoryKind(36.f) && CCubeModel::PortInventoryKind(37.f), "kinds 36 and 37 are the visor's");
  Check(!CCubeModel::PortInventoryKind(34.f) && !CCubeModel::PortInventoryKind(35.f) &&
            !CCubeModel::PortInventoryKind(38.f) && !CCubeModel::PortInventoryKind(-1.f),
        "no other kind is");
}

}  // namespace

int main() {
  TestConverter(true);
  TestConverter(false);
  TestModulation();
  TestDrawDecision();
  TestDrawFlags();
  if (sFailures == 0) {
    std::printf("port_inventory_visor_tests: ok\n");
  }
  return sFailures == 0 ? 0 : 1;
}
