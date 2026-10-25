// The dread-samus import (platform/port_dread_samus.cpp): the converter's explicit-weights path
// (ConvertOptions::boneWeights) against its usual nearest-retail-vertex votes, the synthetic Model
// a re-skinned body becomes, the retail skin reader and the mod order rule (ModPrecedence), all
// over synthetic fixtures.
//
// The whole import on real data is opt-in: PORT_DREAD_ROMFS names an extracted Metroid Dread RomFS
// (build/dread/romfs) and PORT_PRIME_SAMUS_DIR a directory of Prime resources dumped from the
// user's disc as <ID>.<TYPE> (CINF, CMDL, CSKR, TXTR of the bodies;
// build/s-dread6-20261024/dump_prime.py). It runs the import into a temporary folder and checks
// that every target's CMDL and CSKRs were written. Nothing of either game is committed.

#include "port_dread_samus.h"
#include "port_dread_samus_mod.h"
#include "port_remastered_convert.h"
#include "port_remastered_storage.h"
#include "port_test.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace PortRemastered;

namespace {

constexpr uint32_t FourCC(char a, char b, char c, char d) {
  return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) | (uint32_t(uint8_t(c)) << 8) | uint8_t(d);
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

uint32_t Be32(const std::vector<uint8_t>& d, size_t o) {
  return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
}

float BeF(const std::vector<uint8_t>& d, size_t o) {
  const uint32_t u = Be32(d, o);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// A disc body (as port_remastered_blend_tests' disc model): one set of one untextured opaque
// material and one surface over three vertices at (-v, 0, v*v).
std::vector<uint8_t> RetailModel() {
  std::vector<uint8_t> mat;
  Put32(mat, 0);    // flags
  Put32(mat, 0);    // textures
  Put32(mat, 0x3);  // vertex attributes: a u16 position index
  Put32(mat, 0);    // group
  Put32(mat, 0);    // blend GX_BL_ZERO / GX_BL_ONE as (dst << 16) | src: one, zero
  mat.back() = 1;
  Put32(mat, 1);  // colour channels
  Put32(mat, 0);
  Put32(mat, 1);  // one TEV stage, no texture
  for (uint32_t v : {0x7A14Fu, 0x21CE7u, 0x100u, 0x100u, 0u}) {
    Put32(mat, v);
  }
  Put32(mat, 0x0000FFFF);
  Put32(mat, 0);  // texgens
  Put32(mat, 4);  // UV animations: none
  Put32(mat, 0);
  std::vector<std::vector<uint8_t>> secs(8);
  Put32(secs[0], 0);
  Put32(secs[0], 1);
  Put32(secs[0], uint32_t(mat.size()));
  secs[0].insert(secs[0].end(), mat.begin(), mat.end());
  for (int v = 0; v < 3; ++v) {
    for (float c : {-float(v), 0.f, float(v * v)}) {
      Put32(secs[1], FloatBits(c));
    }
  }
  std::vector<uint8_t>& surface = secs[7];
  surface.resize(12, 0);
  Put32(surface, 0);
  Put32(surface, 32);
  surface.resize(28, 0);
  Put32(surface, 0);
  surface.resize(64, 0);
  for (uint8_t b : {0x90, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02}) {
    surface.push_back(b);
  }
  Put32(secs[6], 1);
  Put32(secs[6], 96);
  for (std::vector<uint8_t>& sec : secs) {
    Pad32(sec);
  }
  std::vector<uint8_t> out;
  Put32(out, 0xDEADBABE);
  Put32(out, 2);
  Put32(out, 0);
  out.resize(0x24, 0);
  Put32(out, uint32_t(secs.size()));
  Put32(out, 1);
  for (const std::vector<uint8_t>& sec : secs) {
    Put32(out, uint32_t(sec.size()));
  }
  Pad32(out);
  for (const std::vector<uint8_t>& sec : secs) {
    out.insert(out.end(), sec.begin(), sec.end());
  }
  return out;
}

// One group of three vertices, all on bone 5.
std::vector<uint8_t> RetailSkin() {
  std::vector<uint8_t> out;
  Put32(out, 1);
  Put32(out, 1);
  Put32(out, 5);
  Put32(out, FloatBits(1.f));
  Put32(out, 3);
  return out;
}

ModelMaterial Material() {
  ModelMaterial material;
  material.name = "dreadtest";
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

// The retail body's three vertices, already in the GameCube frame (orient identity), one mesh.
Model BodyModel() {
  Model model;
  model.form = FourCC('C', 'M', 'D', 'L');
  model.skinned = true;
  ModelVertexBuffer vb;
  vb.vertexCount = 3;
  for (int v = 0; v < 3; ++v) {
    vb.positions.insert(vb.positions.end(), {-float(v), 0.f, float(v * v)});
    vb.normals.insert(vb.normals.end(), {0.f, -1.f, 0.f});
  }
  vb.uvs.resize(1);
  for (int v = 0; v < 3; ++v) {
    vb.uvs[0].insert(vb.uvs[0].end(), {float(v), 0.f});
  }
  model.vertexBuffers.push_back(vb);
  model.materials.push_back(Material());
  ModelMesh mesh;
  mesh.indices = {0, 1, 2};
  mesh.indexCount = 3;
  model.meshes.push_back(mesh);
  return model;
}

constexpr uint32_t kBody = 0xABCD1234;
constexpr uint32_t kSkin = 0x11112222;

// Converts BodyModel over the disc body, with `weights` as ConvertOptions::boneWeights (none: the
// votes), and returns the CSKR written: per group, its (bone, weight) list and vertex count.
bool ConvertSkin(const std::vector<std::vector<std::vector<std::pair<uint32_t, float>>>>& weights,
                 std::vector<std::pair<std::vector<std::pair<uint32_t, float>>, uint32_t>>& groups) {
  const std::vector<uint8_t> cmdl = RetailModel(), cskr = RetailSkin();
  std::vector<uint8_t> written;
  ConvertIO io;
  io.texture = [](const ModelUuid&, Image& out, std::string&) {
    out.width = out.height = 4;
    out.rgba.assign(64, 200);
    return true;
  };
  io.retail = [&](uint32_t type, uint32_t id, std::vector<uint8_t>& out) {
    if (type == FourCC('C', 'M', 'D', 'L') && id == kBody) {
      out = cmdl;
      return true;
    }
    if (type == FourCC('C', 'S', 'K', 'R') && id == kSkin) {
      out = cskr;
      return true;
    }
    return false;
  };
  io.retailId = [](uint32_t) { return false; };
  io.write = [&](const std::string& name, const std::vector<uint8_t>& data) {
    if (name == "11112222.CSKR") {
      written = data;
    }
    return true;
  };
  ConvertOptions opt;
  opt.retail = kBody;
  opt.skins = {kSkin};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      opt.orient[r][c] = r == c ? 1.0 : 0.0;
    }
  }
  opt.skip.clear();
  opt.boneWeights = weights;
  Converter converter(io);
  std::string error;
  if (!CHECK_MSG(converter.Convert(BodyModel(), opt, error), "conversion: " + error) ||
      !CHECK_MSG(written.size() >= 4, "no CSKR written")) {
    return false;
  }
  groups.clear();
  size_t o = 4;
  for (uint32_t g = Be32(written, 0); g > 0; --g) {
    std::vector<std::pair<uint32_t, float>> ws;
    for (uint32_t n = Be32(written, o), i = (o += 4, 0); i < n; ++i, o += 8) {
      ws.emplace_back(Be32(written, o), BeF(written, o + 4));
    }
    groups.emplace_back(ws, Be32(written, o));
    o += 4;
  }
  return true;
}

void TestBoneWeights() {
  // Unset: the static model takes the nearest retail vertex's weights, bone 5.
  std::vector<std::pair<std::vector<std::pair<uint32_t, float>>, uint32_t>> groups;
  if (ConvertSkin({}, groups)) {
    std::set<uint32_t> bones;
    uint32_t verts = 0;
    for (const auto& [ws, count] : groups) {
      for (const auto& w : ws) {
        bones.insert(w.first);
      }
      verts += count;
    }
    CHECK_MSG(bones == std::set<uint32_t>{5}, "votes: every vertex on bone 5");
    CHECK(verts == 3);
  }
  // Set: written as given (quantised to 1/64), whatever the retail skin says.
  const std::vector<std::vector<std::vector<std::pair<uint32_t, float>>>> given = {
      {{{7, 0.75f}, {9, 0.25f}}, {{7, 1.f}}, {{9, 0.6f}, {7, 0.4f}}}};
  if (ConvertSkin(given, groups)) {
    std::map<std::vector<std::pair<uint32_t, float>>, uint32_t> seen;
    for (const auto& [ws, count] : groups) {
      seen[ws] += count;
    }
    CHECK_MSG(seen.size() == 3, "three distinct weight groups, got " + std::to_string(seen.size()));
    const std::vector<std::pair<uint32_t, float>> a = {{7, 0.75f}, {9, 0.25f}}, b = {{7, 1.f}};
    CHECK_MSG(seen.count(a) == 1 && seen[a] == 1, "vertex 0 keeps (7 .75, 9 .25)");
    CHECK_MSG(seen.count(b) == 1 && seen[b] == 1, "vertex 1 keeps (7 1)");
    bool quantised = false;
    for (const auto& [ws, count] : seen) {
      if (ws.size() == 2 && ws[0].first == 7 && ws[1].first == 9 && std::fabs(ws[0].second - 0.40625f) < 1e-6f &&
          std::fabs(ws[1].second - 0.59375f) < 1e-6f) {
        quantised = true;  // 0.4 and 0.6 to the nearest 1/64, sorted by bone
      }
    }
    CHECK_MSG(quantised, "vertex 2's (9 .6, 7 .4) quantised to 26/64 and 38/64");
    for (const auto& [ws, count] : groups) {
      for (const auto& w : ws) {
        CHECK_MSG(w.first != 5, "no vote weight leaks in");
      }
    }
  }
  // Too few lists for a buffer: refused, not read past.
  {
    ConvertIO io;
    io.retail = [](uint32_t type, uint32_t, std::vector<uint8_t>& out) {
      out = type == FourCC('C', 'M', 'D', 'L') ? RetailModel() : RetailSkin();
      return true;
    };
    io.retailId = [](uint32_t) { return false; };
    io.texture = [](const ModelUuid&, Image& out, std::string&) {
      out.width = out.height = 4;
      out.rgba.assign(64, 200);
      return true;
    };
    io.write = [](const std::string&, const std::vector<uint8_t>&) { return true; };
    ConvertOptions opt;
    opt.retail = kBody;
    opt.skins = {kSkin};
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) {
        opt.orient[r][c] = r == c ? 1.0 : 0.0;
      }
    }
    opt.boneWeights = {{{{7, 1.f}}}};
    Converter converter(io);
    std::string error;
    CHECK(!converter.Convert(BodyModel(), opt, error));
    CHECK(error.find("do not cover") != std::string::npos);
  }
}

void TestRetailSkin() {
  PortDread::RetailSkinRef ref;
  std::string error;
  if (CHECK_MSG(PortDreadSamus::RetailSkinFromCmdl(RetailModel(), RetailSkin(), ref, &error), error)) {
    CHECK(ref.weights.size() == 3 && ref.pos.size() == 9);
    CHECK(ref.pos[3] == -1.f && ref.pos[8] == 4.f);
    CHECK(ref.weights[2].size() == 1 && ref.weights[2][0].first == 5 && ref.weights[2][0].second == 1.f);
  }
  CHECK(!PortDreadSamus::RetailSkinFromCmdl({1, 2, 3}, RetailSkin(), ref, &error));
  std::vector<uint8_t> cut = RetailSkin();
  cut.resize(10);
  CHECK(!PortDreadSamus::RetailSkinFromCmdl(RetailModel(), cut, ref, &error));
}

void TestBuildModel() {
  PortDread::ReskinResult r;
  for (int m = 0; m < 3; ++m) {
    PortDread::ReskinMesh& mesh = r.meshes.emplace_back();
    mesh.name = "mesh" + std::to_string(m);
    mesh.vertex_count = 4;
    for (int v = 0; v < 4; ++v) {
      mesh.pos.insert(mesh.pos.end(), {float(v), float(m), float(v * m)});
      mesh.normal.insert(mesh.normal.end(), {0.f, 0.f, 1.f});
      mesh.weights.push_back({{uint32_t(3 + m), 1.f}});
      mesh.source.push_back(PortDread::WeightSource::Table);
    }
    mesh.uv[1].assign(8, 0.5f);  // a second set only
    mesh.submeshes.push_back({{0, 1, 2}});
    mesh.submeshes.push_back({{2, 3, 0, 1}});  // a stray index past the last triangle is dropped
  }
  PortDread::SuitMaterial a, b;
  a.material.name = "a";
  b.material.name = "b";
  PortRemastered::Model model;
  std::vector<std::vector<std::vector<std::pair<uint32_t, float>>>> weights;
  std::string error;
  if (!CHECK_MSG(PortDreadSamus::BuildModel(r, {&a, nullptr, &a}, model, weights, &error), error)) {
    return;
  }
  CHECK(model.skinned && model.form == FourCC('C', 'M', 'D', 'L'));
  CHECK_MSG(model.vertexBuffers.size() == 2, "the material-less mesh is left out");
  CHECK_MSG(model.materials.size() == 1 && model.materials[0].name == "a", "one material for the shared one");
  CHECK(model.meshes.size() == 4);
  CHECK(model.meshes[2].vertexBuffer == 1 && model.meshes[2].material == 0);
  CHECK(model.meshes[1].indices.size() == 3 && model.meshes[1].indexCount == 3);
  CHECK(weights.size() == 2 && weights[1].size() == 4 && weights[1][0][0].first == 5);
  CHECK_MSG(model.vertexBuffers[0].uvs.size() == 2 && model.vertexBuffers[0].uvs[0].size() == 8 &&
                model.vertexBuffers[0].uvs[1][0] == 0.5f,
            "a missing first uv set is zero-filled");
  CHECK(model.boundsMax[0] == 3.f && model.boundsMax[2] == 6.f && model.boundsMin[1] == 0.f);
  CHECK(!PortDreadSamus::BuildModel(r, {nullptr, nullptr, nullptr}, model, weights, &error));
  r.meshes[0].submeshes[0].indices[1] = 9;
  CHECK(!PortDreadSamus::BuildModel(r, {&b, nullptr, nullptr}, model, weights, &error));
}

void TestModOrder() {
  using PortRemasteredStorage::ModFolder;
  const std::vector<ModFolder> shared = {
      {"zz-last", "/m/zz-last", false}, {"dread-samus", "/m/dread-samus", false}, {"aa", "/m/aa", false}};
  const std::vector<ModFolder> order = PortRemasteredStorage::OrderMods(shared, "/data/remastered-models");
  std::vector<std::string> names;
  for (const ModFolder& m : order) {
    names.push_back(m.name);
  }
  CHECK_MSG((names == std::vector<std::string>{"aa", "remastered-models", "zz-last", "dread-samus"}),
            "dread-samus loads after every other mod");
  CHECK(PortRemasteredStorage::ModPrecedence(PortDreadSamus::kModName) == 1);
}

// Every target's body is known to the run-time tables (port_dread_samus_mod.h): a cutscene body
// by its skin and character, an in-game body by its suit, each with its own new skeleton.
void TestBallFit() {
  PortDread::Bcmdl source;
  source.joints.push_back({});
  PortDread::BcmdlMesh shell;
  shell.enabled = true;
  shell.vertices.count = 3;
  shell.vertices.pos = {-2,-2,0, 2,2,0, 0,0,2};
  shell.vertices.normal = {0,0,1, 0,0,1, 0,0,1};
  shell.indices = {0,1,2};
  PortDread::BcmdlSubmesh sub;
  sub.skin_type = 2;
  sub.index_count = 3;
  shell.submeshes.push_back(sub);
  source.meshes.push_back(shell);
  shell.enabled = false;
  shell.vertices.pos.assign(9, 10000);
  source.meshes.push_back(shell); // disabled sonar cannot affect the fit
  std::vector<uint8_t> cmdl(44,0);
  auto set = [&](size_t at, uint32_t u) { for (int i=0;i<4;++i) cmdl[at+i]=uint8_t(u>>(24-8*i)); };
  set(0,0xDEADBABE);
  for (int a=0;a<3;++a) {
    float lo=-.5f, hi=.5f;
    uint32_t bits;
    std::memcpy(&bits,&lo,4); set(12+4*a,bits);
    std::memcpy(&bits,&hi,4); set(24+4*a,bits);
  }
  PortDread::ReskinResult fit;
  std::string why;
  REQUIRE_MSG(PortDreadSamus::FitBall(source,cmdl,4,fit,&why),why);
  CHECK(fit.meshes.size()==1);
  CHECK(std::fabs(fit.scale-.25f)<1e-6);
  for (const auto& w : fit.meshes[0].weights) CHECK((w == std::vector<std::pair<uint32_t,float>>{{4,1.f}}));
  CHECK(std::fabs(fit.meshes[0].pos[0]-.5f)<1e-6);
  CHECK(std::fabs(fit.meshes[0].normal[1]-1.f)<1e-6);
  source.meshes[0].enabled=false;
  CHECK(!PortDreadSamus::FitBall(source,cmdl,4,fit,&why));
  source.meshes[0].enabled=true;
  source.meshes[1].enabled=true;
  CHECK(!PortDreadSamus::FitBall(source,cmdl,4,fit,&why));
  source.meshes[1].enabled=false;
  // Bone-local shell: rotate 90 degrees about Z and translate its bind pose.
  // The fit removes translation; directions retain the bind rotation and the
  // proper Dread-to-Prime rotation, without flipping tangent handedness.
  auto& rigid=source.meshes[0];
  rigid.submeshes[0].skin_type=1;
  rigid.submeshes[0].joint_map={0};
  rigid.vertices.normal={1,0,0, 1,0,0, 1,0,0};
  rigid.vertices.tangent={0,1,0,-1, 0,1,0,1, 0,1,0,-1};
  rigid.vertices.joints.assign(12,0);
  rigid.vertices.weights={1,0,0,0, 1,0,0,0, 1,0,0,0};
  source.joints[0].euler[2]=1.5707963267948966f;
  source.joints[0].pos[0]=7;
  source.joints[0].pos[1]=-3;
  source.joints[0].pos[2]=2;
  REQUIRE_MSG(PortDreadSamus::FitBall(source,cmdl,4,fit,&why),why);
  CHECK(std::fabs(fit.scale-.25f)<1e-6);
  CHECK(std::fabs(fit.meshes[0].pos[0]+.5f)<1e-6);
  CHECK(std::fabs(fit.meshes[0].pos[1]+.25f)<1e-6);
  CHECK(std::fabs(fit.meshes[0].pos[2]+.5f)<1e-6);
  for (int v=0;v<3;++v) {
    const auto& m=fit.meshes[0];
    CHECK(std::fabs(m.normal[3*v])<1e-6 && std::fabs(m.normal[3*v+1])<1e-6);
    CHECK(std::fabs(m.normal[3*v+2]-1)<1e-6);
    CHECK(std::fabs(m.tangent[4*v]-1)<1e-6);
    CHECK(std::fabs(m.tangent[4*v+1])<1e-6 && std::fabs(m.tangent[4*v+2])<1e-6);
    CHECK(m.tangent[4*v+3]==(v==1 ? 1.f : -1.f));
  }
  rigid.vertices.weights[0]=.5f;
  rigid.vertices.weights[1]=.5f;
  CHECK(!PortDreadSamus::FitBall(source,cmdl,4,fit,&why));
  rigid.vertices.weights[0]=1;
  rigid.vertices.weights[1]=0;
  source.joints[0].scale[0]=2;
  CHECK(!PortDreadSamus::FitBall(source,cmdl,4,fit,&why));
}

// A beam's colour is the glow of its gun texture: the mean of the texels weighted by max(rgb)^4,
// its largest channel scaled to 1; a texture that is all black has no colour.
void TestBeamTint() {
  float tint[3] = {-1, -1, -1};
  Image orange;
  orange.width = 2;
  orange.height = 1;
  orange.rgba = {255, 128, 0, 255, 0, 0, 0, 255};  // one bright orange texel, one black
  REQUIRE(PortDreadSamus::BeamTint(orange, tint));
  CHECK_MSG(std::fabs(tint[0] - 1.f) < 1e-6, "the largest channel is scaled to 1");
  CHECK_MSG(std::fabs(tint[1] - 128.f / 255.f) < 1e-6, "the other channels keep their ratio");
  CHECK_MSG(std::fabs(tint[2]) < 1e-6, "blue stays 0");
  // The dark texel weighs nothing: the weight is the fourth power of the brightest channel.
  Image dim = orange;
  dim.rgba = {255, 128, 0, 255, 32, 32, 32, 255};
  float dimTint[3];
  REQUIRE(PortDreadSamus::BeamTint(dim, dimTint));
  CHECK(std::fabs(dimTint[0] - 1.f) < 1e-6 && std::fabs(dimTint[1] - 128.f / 255.f) < 1e-3);
  Image black;
  black.width = 1;
  black.height = 1;
  black.rgba = {0, 0, 0, 255};
  CHECK_MSG(!PortDreadSamus::BeamTint(black, tint), "an all-black texture has no tint");
  Image empty;
  CHECK(!PortDreadSamus::BeamTint(empty, tint));
}

// The first-person cannon fit: a Dread suit's cannon meshes turned so the barrel runs +Y with the
// body's up on +X, scaled to the retail gun's length and placed with its tip on the muzzle. The
// fixture is a long thin box along +X from just past the elbow (taller in z, so up is well defined)
// with three rings so a vertex lands partway down the barrel too.
void CannonBody(PortDread::ReskinResult& body) {
  PortDread::PrimeJoint root;
  root.id = 1;
  root.name = "Root";
  root.head[0] = 0.f;
  root.head[1] = -0.5f;
  root.head[2] = 0.f;
  PortDread::PrimeJoint shoulder;
  shoulder.id = 2;
  shoulder.parent = 1;
  shoulder.name = "R_shoulder";
  shoulder.head[0] = -0.05f;
  PortDread::PrimeJoint elbow;
  elbow.id = 3;
  elbow.parent = 2;
  elbow.name = "R_elbow";  // the barrel leaves the elbow at the origin
  body.skeleton.joints = {root, shoulder, elbow};

  PortDread::ReskinMesh& cannon = body.meshes.emplace_back();
  cannon.name = "Samus_Cannon";
  cannon.material_name = "cannon";
  const float xs[3] = {0.1f, 0.3f, 0.5f};
  const float yz[4][2] = {{-0.02f, -0.03f}, {0.02f, -0.03f}, {0.02f, 0.03f}, {-0.02f, 0.03f}};
  for (int r = 0; r < 3; ++r) {
    for (int i = 0; i < 4; ++i) {
      cannon.pos.insert(cannon.pos.end(), {xs[r], yz[i][0], yz[i][1]});
      cannon.normal.insert(cannon.normal.end(), {1.f, 0.f, 0.f});
      cannon.weights.push_back({{3, 1.f}});
      cannon.source.push_back(PortDread::WeightSource::Table);
    }
  }
  cannon.vertex_count = 12;
  std::vector<uint32_t>& indices = cannon.submeshes.emplace_back().indices;
  for (int r = 0; r < 2; ++r) {
    for (int i = 0; i < 4; ++i) {
      const uint32_t a = uint32_t(r * 4 + i), b = uint32_t(r * 4 + (i + 1) % 4);
      indices.insert(indices.end(), {a, b, b + 4, a, b + 4, a + 4});
    }
  }
  indices.insert(indices.end(), {0, 2, 1, 0, 3, 2});  // back cap
  indices.insert(indices.end(), {8, 9, 10, 8, 10, 11});  // muzzle cap

  // The arm skin the fit adds: the mesh past the elbow weighted to the shoulder's subtree.
  PortDread::ReskinMesh& arm = body.meshes.emplace_back();
  arm.name = "Samus_Arm";
  arm.vertex_count = 3;
  arm.pos = {-0.1f, -0.02f, 0.f, -0.06f, -0.02f, 0.03f, -0.1f, 0.02f, 0.f};
  arm.normal.assign(9, 0.f);
  for (int v = 0; v < 3; ++v) {
    arm.weights.push_back({{2, 1.f}});
    arm.source.push_back(PortDread::WeightSource::Table);
  }
  arm.submeshes.push_back({{0, 1, 2}});
}

void TestCannonFit() {
  PortDread::ReskinResult body;
  CannonBody(body);
  const float muzzle[3] = {0.02f, 1.0f, -0.05f};
  const float backY = 0.2f;
  PortDread::ReskinResult out;
  float scale = 0.f;
  std::string error;
  REQUIRE_MSG(PortDreadSamus::FitCannon(body, muzzle, backY, out, scale, &error), error);
  REQUIRE(out.meshes.size() == 2);  // the cannon, then the arm skin the fit adds
  CHECK_MSG(out.meshes[0].name == "Samus_Cannon" && out.meshes[1].name == "Samus_Arm", "cannon first");
  CHECK(out.meshes[0].vertex_count == 12 && out.meshes[1].vertex_count == 3);
  CHECK_MSG(std::fabs(scale - (muzzle[1] - backY) / 0.4f) < 1e-3, "the barrel spans muzzle[1] - backY");
  const PortDread::ReskinMesh& m = out.meshes[0];
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  float tip[3] = {0, 0, 0};
  for (uint32_t v = 0; v < m.vertex_count; ++v) {
    for (int a = 0; a < 3; ++a) {
      const float c = m.pos[size_t(v) * 3 + size_t(a)];
      lo[a] = std::min(lo[a], c);
      hi[a] = std::max(hi[a], c);
    }
    if (v >= 8) {  // the far ring
      for (int a = 0; a < 3; ++a) tip[a] += m.pos[size_t(v) * 3 + size_t(a)] / 4.f;
    }
  }
  CHECK_MSG(lo[1] >= backY - 1e-4f && hi[1] <= muzzle[1] + 1e-4f, "the barrel lies between backY and muzzle[1]");
  CHECK_MSG(std::fabs(lo[1] - backY) < 1e-4f && std::fabs(hi[1] - muzzle[1]) < 1e-4f, "back at backY, tip at muzzle");
  for (int a = 0; a < 3; ++a) {
    CHECK_MSG(std::fabs(tip[a] - muzzle[a]) < 1e-4f, "the far end's centre is the muzzle");
  }
  CHECK_MSG(std::fabs(hi[1] - lo[1] - (muzzle[1] - backY)) < 1e-3f, "the barrel is the gun's length");
  // The box's z (the body's up) went to X and its y to Z; x is now the barrel.
  CHECK_MSG(std::fabs(hi[0] - lo[0] - 0.06f * scale) < 1e-3f, "the body's up is across X");
  CHECK_MSG(std::fabs(hi[2] - lo[2] - 0.04f * scale) < 1e-3f, "the box's y is across Z");
  CHECK_MSG(std::fabs(hi[1] - lo[1] - 0.4f * scale) < 1e-3f, "the box's x is now the barrel");
  // Normals and weights ride along untouched.
  CHECK(m.normal.size() == m.pos.size());
  CHECK(std::fabs(m.normal[0]) < 1e-5f && std::fabs(m.normal[1] - 1.f) < 1e-5f && std::fabs(m.normal[2]) < 1e-5f);
  CHECK(m.weights.size() == 12 && m.weights[0].size() == 1 && m.weights[0][0].first == 3);

  // No cannon mesh: nothing to fit.
  PortDread::ReskinResult armless;
  CannonBody(armless);
  armless.meshes.erase(armless.meshes.begin());
  CHECK(!PortDreadSamus::FitCannon(armless, muzzle, backY, out, scale, &error));
  // The arm skin's shoulder and the elbow are both required.
  PortDread::ReskinResult noShoulder;
  CannonBody(noShoulder);
  noShoulder.skeleton.joints.erase(noShoulder.skeleton.joints.begin() + 1);
  noShoulder.skeleton.joints[1].parent = 1;
  CHECK(!PortDreadSamus::FitCannon(noShoulder, muzzle, backY, out, scale, &error));
  CHECK(!error.empty());
  PortDread::ReskinResult noElbow;
  CannonBody(noElbow);
  noElbow.skeleton.joints.erase(noElbow.skeleton.joints.begin() + 2);
  CHECK(!PortDreadSamus::FitCannon(noElbow, muzzle, backY, out, scale, &error));
  // A muzzle that leaves no barrel length is refused.
  PortDread::ReskinResult good;
  CannonBody(good);
  CHECK(!PortDreadSamus::FitCannon(good, muzzle, muzzle[1], out, scale, &error));
}

void TestTables() {
  CHECK(std::size(PortDreadSamus::kBallTargets) == 8);
  std::set<uint32_t> ballIds;
  for (const auto& t : PortDreadSamus::kBallTargets) {
    CHECK(t.suit >= 0 && t.suit < 4);
    CHECK(PortDreadSamus::BallModel(t.suit, t.spider) == PortModelVariant::Id(t.cmdl, t.suit));
    for (uint32_t base : {t.ancs, t.cmdl, t.low}) {
      const uint32_t id = PortModelVariant::Id(base, t.suit);
      CHECK(ballIds.insert(id).second);
      CHECK(!PortDreadSamus::IsBody(id));
    }
  }
  CHECK(PortDreadSamus::BallModel(4, false) == 0);
  CHECK(std::size(PortDreadSamus::kTargets) == 12);
  CHECK(PortDreadSamus::DollBody(3) == 0x5644E7A1);
  CHECK(PortDreadSamus::DreadCinf(0x5644E7A1) == 0xD7EAD00C);
  CHECK(PortDreadSamus::InGameBody(3) == 0x891E00FD);
  CHECK(PortDreadSamus::CutsceneBody(3) == 0x2782648E);
  CHECK(PortDreadSamus::CutsceneSkin(0x2782648E) == 0x4CADCB58);
  CHECK(PortDreadSamus::DreadCinf(0x2782648E) == 0xD7EAD007);
  CHECK(PortDreadSamus::DreadCinf(0x891E00FD) == 0xD7EAD008);
  std::set<uint32_t> cinfs;
  for (const PortDreadSamus::Target& t : PortDreadSamus::kTargets) {
    const uint32_t cinf = PortDreadSamus::DreadCinf(t.cmdl);
    CHECK(cinf != 0 && cinf != t.cinf);
    CHECK(cinfs.insert(cinf).second);
    bool listed = false;
    for (int i = 0; i < 4; ++i) {
      const uint32_t body = t.cinf == 0x19C53902   ? PortDreadSamus::CutsceneBody(i)
                            : t.cinf == 0xF76E70A6 ? PortDreadSamus::DollBody(i)
                                                   : PortDreadSamus::InGameBody(i);
      listed |= body == t.cmdl;
    }
    CHECK_MSG(listed, std::string(t.suit) + " " + t.skeleton + " body is not in the suit table");
    if (t.cinf == 0x19C53902) {
      CHECK(PortDreadSamus::CutsceneSkin(t.cmdl) == t.cskr[0]);
    }
  }
  CHECK(PortDreadSamus::CutsceneSkin(PortDreadSamus::kPhazonCutsceneAlias) ==
        PortDreadSamus::CutsceneSkin(PortDreadSamus::kPhazonCutsceneBody));
  CHECK(PortDreadSamus::DreadCinf(PortDreadSamus::kPhazonCutsceneAlias) ==
        PortDreadSamus::DreadCinf(PortDreadSamus::kPhazonCutsceneBody));
  CHECK(PortDreadSamus::InGameBody(4) == 0 && PortDreadSamus::CutsceneBody(4) == 0 &&
        PortDreadSamus::DollBody(4) == 0);
}

void TestBallOwnership() {
  using namespace PortDreadSamus;
  using Key = std::pair<uint32_t, uint32_t>;
  for (const auto& t : kBallTargets) {
    const Key ancs{FourCC('A','N','C','S'), PortModelVariant::Id(t.ancs, t.suit)};
    const Key high{FourCC('C','M','D','L'), PortModelVariant::Id(t.cmdl, t.suit)};
    const Key low{FourCC('C','M','D','L'), PortModelVariant::Id(t.low, t.suit)};
    std::map<Key, std::string> winners;
    const auto resolve = [&] {
      return ResolveBallOwnership(t.suit, t.spider, [&](uint32_t type, uint32_t id) {
        const auto it = winners.find({type, id});
        return it != winners.end() && it->second == kModName;
      });
    };
    // Disabled/missing variants take the retail fallback, not Dread's draw rules.
    CHECK(!resolve().main && !resolve().low);
    for (const Key& override : {ancs, high, low}) {
      winners = {{ancs, kModName}, {high, kModName}, {low, kModName}};
      CHECK(resolve().main && resolve().low);
      winners[override] = "higher-priority-mod";
      CHECK(resolve().main == (override == low));
      CHECK(resolve().low == (override != low));
      // The same rules apply when a resource is absent: main and low fallback independently.
      winners.erase(override);
      CHECK(resolve().main == (override == low));
      CHECK(resolve().low == (override != low));
    }
  }
  const auto fusion = ResolveBallOwnership(4, true, [](uint32_t, uint32_t) { return true; });
  CHECK(!fusion.main && !fusion.low);
}

// ---- real data ----

void TestRealData() {
  const char* romfs = std::getenv("PORT_DREAD_ROMFS");
  const char* prime = std::getenv("PORT_PRIME_SAMUS_DIR");
  if (romfs == nullptr || *romfs == '\0' || prime == nullptr || *prime == '\0') {
    std::printf("SKIP: real-data Dread Samus import (PORT_DREAD_ROMFS / PORT_PRIME_SAMUS_DIR not set)\n");
    return;
  }
  namespace fs = std::filesystem;
  const fs::path out = fs::temp_directory_path() / ("port_dread_samus_" + std::to_string(std::rand()));
  fs::create_directories(out);
  auto readFile = [](const fs::path& path, std::vector<uint8_t>& data) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      return false;
    }
    data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
  };
  PortDreadSamus::Io io;
  io.dread = [&](const std::string& path, std::vector<uint8_t>& data, std::string& why) {
    if (!readFile(fs::path(romfs) / path, data)) {
      why = "missing";
      return false;
    }
    return true;
  };
  io.retail = [&](uint32_t type, uint32_t id, std::vector<uint8_t>& data) {
    char name[32];
    std::snprintf(name, sizeof(name), "%08X.%c%c%c%c", id, char(type >> 24), char(type >> 16), char(type >> 8),
                  char(type));
    return readFile(fs::path(prime) / name, data);
  };
  io.retailId = [&](uint32_t id) {
    char stem[16];
    std::snprintf(stem, sizeof(stem), "%08X.", id);
    for (const auto& e : fs::directory_iterator(prime)) {
      if (e.path().filename().string().rfind(stem, 0) == 0) {
        return true;
      }
    }
    return false;
  };
  io.write = [&](const std::string& name, const std::vector<uint8_t>& data) {
    std::ofstream f(out / name, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    return bool(f);
  };
  io.log = [](const std::string& line) { std::printf("  %s\n", line.c_str()); };
  io.detail = [](const std::string&) {};
  PortDreadSamus::Result result;
  std::string error;
  const bool ok = PortDreadSamus::Import(io, result, error);
  CHECK_MSG(ok, "import: " + error);
  CHECK_MSG(result.failed == 0, std::to_string(result.failed) + " targets failed");
  for (const PortDreadSamus::Target& t : PortDreadSamus::kTargets) {
    char name[32];
    std::snprintf(name, sizeof(name), "%08X.CMDL", t.cmdl);
    std::error_code ec;
    CHECK_MSG(fs::file_size(out / name, ec) > 1000 && !ec, std::string("missing ") + name);
    for (uint32_t s : t.cskr) {
      if (s != 0) {
        std::snprintf(name, sizeof(name), "%08X.CSKR", s);
        CHECK_MSG(fs::exists(out / name), std::string("missing ") + name);
      }
    }
  }
  {
    std::vector<char> body, alias;
    std::ifstream a(out / "2782648E.CMDL", std::ios::binary), b(out / "B333E1D7.CMDL", std::ios::binary);
    body.assign(std::istreambuf_iterator<char>(a), {});
    alias.assign(std::istreambuf_iterator<char>(b), {});
    CHECK_MSG(!body.empty() && body == alias, "B333E1D7.CMDL is not a copy of the Phazon body");
  }
  size_t textures = 0;
  for (const std::string& f : result.files) {
    textures += f.size() > 5 && f.compare(f.size() - 5, 5, ".TXTR") == 0;
  }
  std::printf("real data: %zu targets, %zu files (%zu TXTR) in %s\n", result.converted, result.files.size(), textures,
              out.string().c_str());
  CHECK(textures > 0);
  CHECK(result.measured);
  CHECK(result.converted == std::size(PortDreadSamus::kTargets) + std::size(PortDreadSamus::kCannonTargets) +
                                std::size(PortDreadSamus::kBallTargets));
  for (const auto& t : PortDreadSamus::kCannonTargets) {
    char name[32];
    std::snprintf(name, sizeof(name), "%08X.CMDL", PortDreadSamus::FirstPersonCannon(t.primeSuit));
    std::vector<uint8_t> cannon;
    REQUIRE(readFile(out / name, cannon));
    REQUIRE(cannon.size() > 0x2c);
    // One material set per beam, as the gun model has.
    CHECK((uint32_t(cannon[0x28]) << 24 | uint32_t(cannon[0x29]) << 16 | uint32_t(cannon[0x2a]) << 8 | cannon[0x2b]) ==
          PortDreadSamus::kBeams);
  }
  for (const auto& t : PortDreadSamus::kBallTargets) {
    for (const auto& key : std::vector<std::pair<uint32_t, const char*>>{
             {t.ancs, "ANCS"}, {t.cmdl, "CMDL"}, {t.low, "CMDL"}}) {
      char name[32];
      std::snprintf(name, sizeof(name), "%08X.%s", PortModelVariant::Id(key.first, t.suit), key.second);
      CHECK_MSG(fs::exists(out / name), std::string("missing ") + name);
    }
    char name[32];
    std::snprintf(name,sizeof(name),"%08X.ANCS",PortModelVariant::Id(t.ancs,t.suit));
    std::vector<uint8_t> patched, original, model, skin;
    REQUIRE(readFile(out/name,patched));
    REQUIRE(io.retail(FourCC('A','N','C','S'),t.ancs,original));
    size_t at=14;
    while (at<original.size() && original[at]) ++at;
    ++at;
    auto be = [](const std::vector<uint8_t>& b,size_t p) { return uint32_t(b[p])<<24 | uint32_t(b[p+1])<<16 | uint32_t(b[p+2])<<8 | b[p+3]; };
    CHECK(be(patched,at)==PortModelVariant::Id(t.cmdl,t.suit));
    const uint32_t skinId=PortModelVariant::Id(be(original,at+4),t.suit);
    CHECK(be(patched,at+4)==skinId);
    // CINF and animation tables must stay byte-identical.
    REQUIRE(patched.size()==original.size());
    for (size_t i=0;i<original.size();++i) if (i<at || i>=at+8) CHECK(original[i]==patched[i]);
    std::snprintf(name,sizeof(name),"%08X.CMDL",PortModelVariant::Id(t.cmdl,t.suit));
    REQUIRE(readFile(out/name,model));
    std::snprintf(name,sizeof(name),"%08X.CSKR",skinId);
    REQUIRE(readFile(out/name,skin));
    PortDread::RetailSkinRef weights;
    REQUIRE_MSG(PortDreadSamus::RetailSkinFromCmdl(model,skin,weights,&error),error);
    for (const auto& w : weights.weights) CHECK((w==std::vector<std::pair<uint32_t,float>>{{4,1.f}}));
  }
  PortDreadSamus::Io failing=io;
  size_t writes=0;
  failing.write=[&](const std::string&,const std::vector<uint8_t>&) { ++writes; return true; };
  const uint32_t collision=PortDreadSamus::BallModel(0,false);
  failing.retailId=[&](uint32_t id) { return id==collision || io.retailId(id); };
  failing.log=[](const std::string&){};
  PortDreadSamus::Result failed;
  CHECK(!PortDreadSamus::Import(failing,failed,error));
  CHECK(error.find("collision")!=std::string::npos);
  CHECK(writes==0 && failed.files.empty());
  failing=io;
  failing.log=[](const std::string&){};
  failing.write=[&](const std::string&,const std::vector<uint8_t>&) { ++writes; return true; };
  failing.dread=[&](const std::string& path,std::vector<uint8_t>& bytes,std::string& why) {
    if (path.find("morphballhyper_bc")!=std::string::npos) { why="injected missing shell texture"; return false; }
    return io.dread(path,bytes,why);
  };
  CHECK(!PortDreadSamus::Import(failing,failed,error));
  CHECK(error.find("shell material")!=std::string::npos);
  CHECK(writes==0 && failed.files.empty());
  failing=io;
  failing.log=[](const std::string&){};
  failing.write=[&](const std::string&,const std::vector<uint8_t>&) { ++writes; return false; };
  CHECK(!PortDreadSamus::Import(failing,failed,error));
  CHECK(error.find("cannot write")!=std::string::npos);
  CHECK(failed.files.empty());
  // A cancellation raised by a successful staging write must stop the next
  // write, including when it arrives in the very last write of the transaction.
  for (size_t cancelAt : {size_t(1), result.files.size() / 2, result.files.size()}) {
    failing=io;
    failing.log=[](const std::string&){};
    bool cancel=false;
    writes=0;
    failing.cancelled=[&] { return cancel; };
    failing.write=[&](const std::string&, const std::vector<uint8_t>&) {
      if (++writes == cancelAt) cancel=true;
      return true;
    };
    CHECK(!PortDreadSamus::Import(failing,failed,error));
    CHECK_MSG(error.find("cancelled")!=std::string::npos,error);
    CHECK(writes==cancelAt);
    CHECK(failed.files.size()==cancelAt);
  }
  if (std::getenv("PORT_DREAD_SAMUS_KEEP") == nullptr) {
    fs::remove_all(out);
  }
}

}  // namespace

int main() {
  TestBeamTint();
  TestBoneWeights();
  TestRetailSkin();
  TestBuildModel();
  TestModOrder();
  TestTables();
  TestBallOwnership();
  TestBallFit();
  TestCannonFit();
  TestRealData();
  return PORT_TEST_RESULT();
}
