// How a Remastered mesh is blended: its class (ModelMesh::bits2), not a material flag.
// A synthetic model with three single-mesh materials is run through PortRemastered::Converter
// and the GX blend factors of the CMDL's materials are read back: an opaque one writes
// (One, Zero), a transparent one of class 1 (SrcAlpha, InvSrcAlpha) and one of class 3 (SrcAlpha, One),
// standalone and over a disc model's material whatever that one blends.

#include "port_remastered_convert.h"
#include "port_test.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace PortRemastered;

namespace {

constexpr uint32_t FourCC(char a, char b, char c, char d) {
  return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) | (uint32_t(uint8_t(c)) << 8) | uint8_t(d);
}

ModelMaterial Material(const char* name, uint32_t flags) {
  ModelMaterial material;
  material.name = name;
  material.unk1 = flags;
  material.types.push_back(FourCC('R', 'L', 'T', 'G'));
  ModelMaterialData d;
  d.usage = FourCC('D', 'I', 'F', 'T');
  d.kind = ModelMaterialData::Kind::Texture;
  d.texture.usage = d.usage;
  d.texture.hasUsage = true;
  d.texture.id[3] = 0x11;
  d.texture.wrapX = 1;
  d.texture.wrapY = 1;
  material.data.push_back(d);
  return material;
}

struct Surface {
  uint32_t flags;
  uint8_t cls;
};

Model BuildModel(const std::vector<Surface>& surfaces) {
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
  for (size_t i = 0; i < surfaces.size(); ++i) {
    model.materials.push_back(Material(("blendtest" + std::to_string(i)).c_str(), surfaces[i].flags));
    ModelMesh mesh;
    mesh.material = uint32_t(i);
    mesh.vertexBuffer = 0;
    mesh.bits2 = surfaces[i].cls;
    mesh.indices = {0, 1, 2};
    model.meshes.push_back(mesh);
  }
  return model;
}

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

// A disc model (as port_inventory_visor_tests' visor): one set of one untextured material blending
// (src, dst), and one surface drawing BuildModel's three vertices in the GameCube's frame.
std::vector<uint8_t> RetailModel(uint32_t src, uint32_t dst) {
  std::vector<uint8_t> mat;
  Put32(mat, 0);    // flags
  Put32(mat, 0);    // textures
  Put32(mat, 0x3);  // vertex attributes: a u16 position index
  Put32(mat, 0);    // group
  Put32(mat, (dst << 16) | src);
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
  for (std::vector<uint8_t>& sec : secs) {
    Pad32(sec);
  }
  std::vector<uint8_t> out;
  Put32(out, 0xDEADBABE);
  Put32(out, 2);
  Put32(out, 0);  // flags
  out.resize(0x24, 0);
  Put32(out, uint32_t(secs.size()));
  Put32(out, 1);  // material sets
  for (const std::vector<uint8_t>& sec : secs) {
    Put32(out, uint32_t(sec.size()));
  }
  Pad32(out);
  for (const std::vector<uint8_t>& sec : secs) {
    out.insert(out.end(), sec.begin(), sec.end());
  }
  return out;
}

// Standalone with an empty retail; otherwise against that disc model, as the game imports one.
bool Convert(const Model& model, std::vector<uint8_t>& cmdl, const std::vector<uint8_t>& retail = {},
             const std::string& source = {}) {
  ConvertIO io;
  io.texture = [](const ModelUuid&, Image& out, std::string&) {
    out.width = out.height = 4;
    out.rgba.assign(64, 200);
    return true;
  };
  io.write = [&cmdl](const std::string& name, const std::vector<uint8_t>& data) {
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".CMDL") == 0) {
      cmdl = data;
    }
    return true;
  };
  ConvertOptions opt;
  opt.standalone = retail.empty();
  opt.retail = 0xABCD1234;
  opt.source = source;
  opt.skip.clear();
  if (!opt.standalone) {
    io.retail = [&retail, &opt](uint32_t type, uint32_t id, std::vector<uint8_t>& out) {
      if (type != FourCC('C', 'M', 'D', 'L') || id != opt.retail) {
        return false;
      }
      out = retail;
      return true;
    };
    io.retailId = [](uint32_t) { return false; };
  }
  Converter converter(io);
  std::string error;
  if (!converter.Convert(model, opt, error)) {
    std::fprintf(stderr, "FAIL: the conversion failed: %s\n", error.c_str());
    port_test::CountFailure();
    return false;
  }
  return true;
}

uint32_t Be32(const std::vector<uint8_t>& d, size_t o) {
  return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
}

uint32_t Be16(const std::vector<uint8_t>& d, size_t o) { return (uint32_t(d[o]) << 8) | d[o + 1]; }

// The (source, destination) factors of every material in the CMDL, in order. A material
// is its flags, the texture count and indices, the vertex descriptor and the group id
// (no konst colours here), then the destination and source factors as 16 bit words.
bool Factors(const std::vector<uint8_t>& d, std::vector<std::pair<uint32_t, uint32_t>>& out) {
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
    const uint32_t flags = Be32(d, m);
    if (flags & 0x8) {
      return false;  // konst colours: not a layout this test reads
    }
    const uint32_t nt = Be32(d, m + 4);
    const size_t at = m + 8 + 4 * size_t(nt) + 8;
    if (at + 4 > d.size()) {
      return false;
    }
    out.emplace_back(Be16(d, at + 2), Be16(d, at));
    begin = Be32(d, ends + 4 * size_t(i));
  }
  return true;
}

void TestClasses() {
  // Source identity, not output id or LambertFx, determines null rendering.
  // The output uses an unrelated generated id, as room-model imports do.
  const Model nullFixture = BuildModel({{0, 0}});
  for (const char* source : {"10000000-0000-f000-f000-00003801de98",
                             "100000000000F000F000000012DBB060",
                             "10000000-0000-f000-f000-00001c0b0bef",
                             "10000000-0000-f000-f000-000083dba724"}) {
    for (bool replacement : {false, true}) {
      std::vector<uint8_t> out;
      REQUIRE(Convert(nullFixture, out, replacement ? RetailModel(1, 1) : std::vector<uint8_t>{}, source));
      const uint32_t sections = Be32(out, 36), sets = Be32(out, 40);
      size_t at = (44 + 4 * size_t(sections) + 31) & ~size_t(31);
      for (size_t i = 0; i < sets + 5; ++i) {
        at += Be32(out, 44 + 4 * i);
      }
      REQUIRE(at + 4 <= out.size());
      const bool xray = std::string(source).find("83dba724") != std::string::npos;
      CHECK(Be32(out, at) == (xray ? 1u : 0u));
      CHECK(Be32(out, 12) != Be32(out, 24)); // nonempty bounds survive
    }
  }
  std::vector<uint8_t> cmdl;
  if (!Convert(BuildModel({{0, 0}, {0x1, 1}, {0x1, 3}}), cmdl)) {
    return;
  }
  std::vector<std::pair<uint32_t, uint32_t>> f;
  if (!Factors(cmdl, f)) {
    CHECK_MSG(false, "the converted CMDL's materials are readable");
    return;
  }
  CHECK_MSG(f.size() == 3, "three surfaces, three materials");
  const std::set<std::pair<uint32_t, uint32_t>> got(f.begin(), f.end());
  CHECK_MSG(got.count({1, 0}), "an opaque surface blends (One, Zero)");
  CHECK_MSG(got.count({4, 5}), "a class 1 transparent surface blends (SrcAlpha, InvSrcAlpha)");
  CHECK_MSG(got.count({4, 1}), "a class 3 transparent surface blends (SrcAlpha, One)");
  CHECK_MSG(got.size() == 3, "each class has factors of its own");
}

// Over a disc model's material the class decides too, not what that material blends: an opaque
// disc material under class 1 and 3 meshes, and an effect's (One, One) under a class 0 mesh not
// flagged blended (600D500E's case, drawn opaque).
void TestClassesOverRetail() {
  std::vector<uint8_t> cmdl;
  if (!Convert(BuildModel({{0x1, 0}, {0x1, 1}, {0x1, 3}, {0x20, 2}}), cmdl, RetailModel(1, 0))) {
    return;
  }
  std::vector<std::pair<uint32_t, uint32_t>> f;
  if (!Factors(cmdl, f) || f.size() != 4) {
    CHECK_MSG(false, "the four materials over a disc model are readable");
    return;
  }
  CHECK_MSG((f[0] == std::pair<uint32_t, uint32_t>(1, 0)), "over (One, Zero), class 0 blends (One, Zero)");
  CHECK_MSG((f[1] == std::pair<uint32_t, uint32_t>(4, 5)), "over (One, Zero), class 1 blends (SrcAlpha, InvSrcAlpha)");
  CHECK_MSG((f[2] == std::pair<uint32_t, uint32_t>(4, 1)), "over (One, Zero), class 3 blends (SrcAlpha, One)");
  CHECK_MSG((f[3] == std::pair<uint32_t, uint32_t>(1, 0)), "over (One, Zero), class 2 blends (One, Zero)");
  cmdl.clear();
  f.clear();
  if (!Convert(BuildModel({{0x100010, 0}}), cmdl, RetailModel(1, 1))) {
    return;
  }
  CHECK_MSG(Factors(cmdl, f) && f.size() == 1, "the material over an effect is readable");
  CHECK_MSG((f.size() == 1 && f[0] == std::pair<uint32_t, uint32_t>(1, 0)), "over (One, One), class 0 blends (One, Zero)");
}

// Lit glass of Remastered's premultiplied families (here 11B30369) adds its own colour:
// (One, InvSrcAlpha), however its mesh class says to blend.
void TestPremultipliedGlass() {
  Model model = BuildModel({{0x1, 1}});
  model.materials[0].shaderId[0] = 0x11;
  model.materials[0].shaderId[1] = 0xB3;
  model.materials[0].shaderId[2] = 0x03;
  model.materials[0].shaderId[3] = 0x69;
  std::vector<uint8_t> cmdl;
  if (!Convert(model, cmdl)) {
    return;
  }
  std::vector<std::pair<uint32_t, uint32_t>> f;
  CHECK_MSG(Factors(cmdl, f) && f.size() == 1, "the glass's material is readable");
  CHECK_MSG((f.size() == 1 && f[0] == std::pair<uint32_t, uint32_t>(1, 5)), "premultiplied glass blends (One, InvSrcAlpha)");
}

// Glass_DX11 (03407341, the Waste Disposal tank) with its distortion map and colours is
// drawn over the screen copy, premultiplied, whatever its mesh class says.
void TestHoloGlass() {
  Model model = BuildModel({{0x100053, 1}});
  ModelMaterial& m = model.materials[0];
  m.shaderId[0] = 0x03;
  m.shaderId[1] = 0x40;
  m.shaderId[2] = 0x73;
  m.shaderId[3] = 0x41;
  ModelMaterialData tch2 = m.data[0];
  tch2.usage = tch2.texture.usage = FourCC('T', 'C', 'H', '2');
  tch2.texture.id[3] = 0x22;
  m.data.push_back(tch2);
  for (int i = 0; i < 4; ++i) {
    ModelMaterialData c;
    c.usage = FourCC('C', 'C', 'H', char('0' + i));
    c.kind = ModelMaterialData::Kind::Color;
    c.color[0] = c.color[1] = c.color[2] = c.color[3] = 0.5f;
    m.data.push_back(c);
  }
  std::vector<uint8_t> cmdl;
  if (!Convert(model, cmdl)) {
    return;
  }
  std::vector<std::pair<uint32_t, uint32_t>> f;
  CHECK_MSG(Factors(cmdl, f) && f.size() == 1, "the tank glass's material is readable");
  CHECK_MSG((f.size() == 1 && f[0] == std::pair<uint32_t, uint32_t>(1, 5)), "the tank glass blends (One, InvSrcAlpha)");
}

}  // namespace

int main() {
  TestClasses();
  TestClassesOverRetail();
  TestPremultipliedGlass();
  TestHoloGlass();
  if (port_test::Failures() == 0) {
    std::printf("port_remastered_blend_tests: ok\n");
  }
  return PORT_TEST_RESULT();
}
