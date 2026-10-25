// Remastered's LITS (LightBleedScale), the scale of a back-facing pixel's light, as the
// converter and the port's material record carry it. Checked as the record's reader alone
// (every older trailer keeps its meaning and neutral factors) and through real conversions:
// a two-sided synthetic model is run through PortRemastered::Converter and what it wrote is
// read back, the back copy's normals and the factors of its 'PBR6' record included.

#include "port_pbr_record.h"
#include "port_remastered_convert.h"
#include "port_test.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace PortRemastered;

namespace {

constexpr uint32_t FourCC(char a, char b, char c, char d) {
  return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) | (uint32_t(uint8_t(c)) << 8) | uint8_t(d);
}

void PutBe(std::vector<uint8_t>& out, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) {
    out.push_back(uint8_t(v >> s));
  }
}

void PutFloat(std::vector<uint8_t>& out, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  PutBe(out, bits);
}

// A material's tail: `floats` floats counting up from 1, the extra words, then the tag.
std::vector<uint8_t> Record(int floats, bool wrap, bool scale, const char* tag, float diffuse = 1.f,
                            float f0 = 1.f) {
  std::vector<uint8_t> r;
  for (int i = 0; i < floats; ++i) {
    PutFloat(r, float(i + 1));
  }
  if (wrap) {
    PutBe(r, 0x11223344);
  }
  if (scale) {
    PutFloat(r, diffuse);
    PutFloat(r, f0);
  }
  r.insert(r.end(), tag, tag + 4);
  return r;
}

void TestReader() {
  float v[19], s[2];
  uint32_t wrap = 0;
  {
    const std::vector<uint8_t> r = Record(19, true, true, "PBR6", 0.4f, 0.f);
    CHECK_MSG(r.size() == 92, "a PBR6 record is 92 bytes");
    CHECK_MSG(PortPbrRecord::Read(r.data() + r.size(), r.size(), v, &wrap, s) == 19, "PBR6 holds 19 floats");
    CHECK_MSG(v[0] == 1.f && v[18] == 19.f, "PBR6 floats are read before the wrap word");
    CHECK_MSG(wrap == 0x11223344, "PBR6 wrap word");
    CHECK_MSG(s[0] == 0.4f && s[1] == 0.f, "PBR6 diffuse and F0 factors");
    uint32_t cube = 7;
    CHECK_MSG(PortPbrRecord::Read(r.data() + r.size(), r.size(), v, &wrap, s, &cube) == 19 && cube == 0,
              "PBR6 has no cube");
  }
  {  // 'PBR7': 'PBR6' with the cube's id before the tag.
    std::vector<uint8_t> r = Record(19, true, true, "PBR6", 0.4f, 0.5f);
    r.resize(r.size() - 4);
    PutBe(r, 0xCAFEF00D);
    r.insert(r.end(), {'P', 'B', 'R', '7'});
    uint32_t cube = 0;
    CHECK_MSG(PortPbrRecord::Read(r.data() + r.size(), r.size(), v, &wrap, s, &cube) == 19, "PBR7 holds 19 floats");
    CHECK_MSG(v[18] == 19.f && wrap == 0x11223344 && s[0] == 0.4f && s[1] == 0.5f, "PBR7 reads as PBR6 before the cube");
    CHECK_MSG(cube == 0xCAFEF00D, "PBR7 cube id");
  }
  {  // 'PBR8': 32 shield floats and the tag after any record; the rest reads as before.
    std::vector<uint8_t> r = Record(19, true, true, "PBR6", 0.4f, 0.5f);
    for (int i = 0; i < 32; ++i) {
      PutBe(r, std::bit_cast<uint32_t>(100.f + float(i)));
    }
    r.insert(r.end(), {'P', 'B', 'R', '8'});
    float shield[32] = {};
    uint32_t cube = 0;
    CHECK_MSG(PortPbrRecord::Read(r.data() + r.size(), r.size(), v, &wrap, s, &cube, shield) == 19, "PBR8 holds 19 floats");
    CHECK_MSG(v[18] == 19.f && wrap == 0x11223344 && s[0] == 0.4f && s[1] == 0.5f, "PBR8 reads the record before it");
    CHECK_MSG(shield[0] == 100.f && shield[31] == 131.f, "PBR8 shield floats");
    // Kind 15 (PickUp) keeps CCH0..3 in rows 0-3, the view-to-world rows in 4-5 and DIFC in row 7.
    CHECK_MSG(shield[12] == 112.f && shield[16] == 116.f && shield[23] == 123.f && shield[28] == 128.f,
              "PBR8 pickup rows (CCH3, world x/y, DIFC)");
    // Kinds 16 and 17 (the holograms) keep ICNC + ICMC and the cube gain in row 6, DIFC in row 7;
    // kind 18 keeps CCH0..3 in rows 0-3, world x/y in 4-5, ICMC in row 6 and DIFC in row 7.
    CHECK_MSG(shield[24] == 124.f && shield[26] == 126.f && shield[27] == 127.f && shield[28] == 128.f && shield[31] == 131.f,
              "PBR8 hologram rows (ICNC+ICMC, cube gain, DIFC)");
    CHECK_MSG(shield[8] == 108.f && shield[15] == 115.f && shield[16] == 116.f,
              "PBR8 hologram CCH0..3 rows");
    // Kind 19 (98F0556D, the lit sphere-map fx) keeps ICNC + ICMC in row 6 and DIFC in row 7, as the
    // holograms do; its REFS and REFV are maps 4 and 5, so the trailer carries nothing else.
    CHECK_MSG(shield[24] == 124.f && shield[25] == 125.f && shield[28] == 128.f && shield[30] == 130.f,
              "PBR8 gun-fx rows (ICNC+ICMC, DIFC)");
    CHECK_MSG(PortPbrRecord::Read(r.data() + r.size(), r.size(), v, &wrap, s) == 19, "PBR8 reads without a shield out");
    const std::vector<uint8_t> plain = Record(19, true, true, "PBR6", 0.4f, 0.5f);
    shield[5] = 9.f;
    CHECK_MSG(PortPbrRecord::Read(plain.data() + plain.size(), plain.size(), v, &wrap, s, nullptr, shield) == 19 &&
                  shield[5] == 0.f,
              "a record with no PBR8 clears the shield floats");
  }
  {  // 'LMUV': slot word and tag after everything; the record before it reads as before.
    std::vector<uint8_t> r = Record(19, true, true, "PBR6", 0.4f, 0.5f);
    for (int i = 0; i < 32; ++i) {
      PutBe(r, std::bit_cast<uint32_t>(100.f + float(i)));
    }
    r.insert(r.end(), {'P', 'B', 'R', '8'});
    CHECK_MSG(PortPbrRecord::LightmapSlot(r.data() + r.size(), r.size()) == -1, "no LMUV gives no slot");
    PutBe(r, 5);
    r.insert(r.end(), {'L', 'M', 'U', 'V'});
    float shield[32] = {};
    uint32_t cube = 7;
    CHECK_MSG(PortPbrRecord::Read(r.data() + r.size(), r.size(), v, &wrap, s, &cube, shield) == 19, "LMUV holds 19 floats");
    CHECK_MSG(v[18] == 19.f && wrap == 0x11223344 && s[0] == 0.4f && s[1] == 0.5f && cube == 0, "LMUV reads the record before it");
    CHECK_MSG(shield[0] == 100.f && shield[31] == 131.f, "LMUV keeps the PBR8 shield floats");
    CHECK_MSG(PortPbrRecord::LightmapSlot(r.data() + r.size(), r.size()) == 5, "LMUV slot");
    std::vector<uint8_t> bad = Record(19, true, true, "PBR6");
    PutBe(bad, 9);
    bad.insert(bad.end(), {'L', 'M', 'U', 'V'});
    CHECK_MSG(PortPbrRecord::LightmapSlot(bad.data() + bad.size(), bad.size()) == -1, "LMUV slot past 7 is none");
  }
  struct Old {
    int floats;
    bool wrap;
    const char* tag;
  };
  const Old older[] = {{19, true, "PBR5"}, {19, false, "PBR4"}, {13, false, "PBR3"}, {8, false, "PBR2"}, {6, false, "PBRM"}};
  for (const Old& o : older) {
    const std::vector<uint8_t> r = Record(o.floats, o.wrap, false, o.tag);
    // A longer material around it, as in a CMDL: the reader looks at the end only.
    std::vector<uint8_t> m(40, 0xAA);
    m.insert(m.end(), r.begin(), r.end());
    s[0] = s[1] = 7.f;
    CHECK_MSG(PortPbrRecord::Read(m.data() + m.size(), m.size(), v, &wrap, s) == o.floats, o.tag);
    CHECK_MSG(v[o.floats - 1] == float(o.floats), "the last float of an older record is unmoved");
    CHECK_MSG(s[0] == 1.f && s[1] == 1.f, "an older record has neutral light factors");
    CHECK_MSG(wrap == (o.wrap ? 0x11223344u : 0x55555555u), "an older record's wrap is unchanged");
  }
  {
    const std::vector<uint8_t> none(40, 0x55);
    s[0] = s[1] = 7.f;
    CHECK_MSG(PortPbrRecord::Read(none.data() + none.size(), none.size(), v, &wrap, s) == 0, "no record");
    CHECK_MSG(s[0] == 1.f && s[1] == 1.f && v[0] == 1.f && v[3] == 0.f, "no record is neutral");
  }
  {  // A TEV material's wrap word alone.
    std::vector<uint8_t> m(40, 0xAA);
    const uint8_t word[] = {0x11, 0x22, 0x33, 0x44, 'W', 'R', 'A', 'P'};
    m.insert(m.end(), word, word + 8);
    s[0] = s[1] = 7.f;
    CHECK_MSG(PortPbrRecord::Read(m.data() + m.size(), m.size(), v, &wrap, s) == 0, "WRAP holds no floats");
    CHECK_MSG(wrap == 0x11223344u && v[0] == 1.f && s[0] == 1.f, "WRAP word, the rest neutral");
  }
  {  // A tag the material is too short to hold is not read.
    const std::vector<uint8_t> r = Record(0, false, false, "PBR6");
    s[0] = s[1] = 7.f;
    CHECK_MSG(PortPbrRecord::Read(r.data() + r.size(), r.size(), v, &wrap, s) == 0 && s[0] == 1.f,
              "a PBR6 tag with no room for the record is ignored");
  }
}

ModelMaterial Material(double lits, bool hasLits) {
  ModelMaterial material;
  material.name = "litstest";
  material.unk1 = 0;
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
  if (hasLits) {
    ModelMaterialData l;
    l.usage = FourCC('L', 'I', 'T', 'S');
    l.kind = ModelMaterialData::Kind::Scalar;
    l.scalar = float(lits);
    material.data.push_back(l);
  }
  return material;
}

Model BuildModel(const ModelMaterial& material, bool twoSided, bool colored = false) {
  Model model;
  model.materials.push_back(material);
  ModelVertexBuffer vb;
  vb.vertexCount = 3;
  for (int v = 0; v < 3; ++v) {
    vb.positions.insert(vb.positions.end(), {float(v), float(v * v), 0.f});
    vb.normals.insert(vb.normals.end(), {0.f, 0.f, 1.f});
  }
  if (colored) {
    for (int v = 0; v < 3; ++v) {
      vb.colors.insert(vb.colors.end(), {1.f, 0.5f, 0.25f, 1.f});
    }
  }
  vb.uvs.resize(1);
  for (int v = 0; v < 3; ++v) {
    vb.uvs[0].insert(vb.uvs[0].end(), {float(v), 0.f});
  }
  model.vertexBuffers.push_back(vb);
  ModelMesh mesh;
  mesh.material = 0;
  mesh.vertexBuffer = 0;
  mesh.twoSided = twoSided;
  mesh.indices = {0, 1, 2};
  model.meshes.push_back(mesh);
  return model;
}

bool Convert(const Model& model, std::vector<uint8_t>& cmdl, std::vector<MaterialDecision>* decisions = nullptr,
             bool withCube = false) {
  ConvertIO io;
  io.texture = [](const ModelUuid&, Image& out, std::string&) {
    out.width = out.height = 4;
    out.rgba.assign(64, 200);
    return true;
  };
  if (withCube) {
    io.cube = [](const ModelUuid&, uint32_t& edge, std::vector<float>& rgba, std::string&) {
      edge = 1;
      rgba.assign(24, 0.5f);
      return true;
    };
  }
  if (decisions != nullptr) {
    io.decision = [decisions](const MaterialDecision& d) { decisions->push_back(d); };
  }
  io.write = [&cmdl](const std::string& name, const std::vector<uint8_t>& data) {
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".CMDL") == 0) {
      cmdl = data;
    }
    return true;
  };
  ConvertOptions opt;
  opt.standalone = true;
  opt.retail = 0xABCD1234;
  opt.skip.clear();
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

float BeFloat(const std::vector<uint8_t>& d, size_t o) {
  const uint32_t bits = Be32(d, o);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

struct Result {
  uint32_t materials = 0;
  int towardFront = 0;  // normals pointing +z, the mesh's own
  int turned = 0;       // normals pointing -z, the older back copy's
  std::vector<std::pair<float, float>> scales;  // diffuse, F0 of every 'PBR6' record
  bool ok = false;
};

// What a conversion wrote: the material count, the normals section's directions and the
// factors of each PBR6 record, found by its tag.
Result Read(const std::vector<uint8_t>& d) {
  Result r;
  if (d.size() < 48) {
    return r;
  }
  const uint32_t nsec = Be32(d, 36);
  if (nsec < 3 || 44 + 4 * size_t(nsec) > d.size()) {
    return r;
  }
  size_t base = (44 + 4 * size_t(nsec) + 31) & ~size_t(31);
  const uint32_t ntex = Be32(d, base);
  r.materials = Be32(d, base + 4 + 4 * size_t(ntex));
  const size_t normals = base + Be32(d, 44) + Be32(d, 48);
  const size_t normalBytes = Be32(d, 52);

  if (normals + normalBytes > d.size()) {
    return r;
  }
  for (size_t o = normals; o + 12 <= normals + normalBytes; o += 12) {
    const float z = BeFloat(d, o + 4);  // the converter writes Y up: the mesh's +z is +y
    r.towardFront += z > 0.5f;
    r.turned += z < -0.5f;
  }
  for (size_t o = 8; o + 4 <= d.size(); ++o) {
    if (std::memcmp(&d[o], "PBR6", 4) == 0) {
      r.scales.emplace_back(BeFloat(d, o - 8), BeFloat(d, o - 4));
    }
  }
  r.ok = true;
  return r;
}

Result Run(const ModelMaterial& material, bool twoSided) {
  std::vector<uint8_t> cmdl;
  if (!Convert(BuildModel(material, twoSided), cmdl)) {
    return {};
  }
  Result r = Read(cmdl);
  CHECK_MSG(r.ok, "the converted CMDL is readable");
  return r;
}

void TestConverter() {
  {  // No LITS: the older back copy, one material, no factors.
    const Result r = Run(Material(0, false), true);
    CHECK_MSG(r.materials == 1 && r.scales.empty(), "no LITS: one material, no PBR6");
    CHECK_MSG(r.turned > 0 && r.towardFront > 0, "no LITS: the back copy's normals are turned round");
  }
  {  // -1: the same as no LITS (the flipped normal is what the older copy has).
    const Result r = Run(Material(-1.0, true), true);
    CHECK_MSG(r.materials == 1 && r.scales.empty(), "LITS -1: one material, no PBR6");
    CHECK_MSG(r.turned > 0, "LITS -1: the older back copy's turned normals");
  }
  {  // 0 and a negative other than -1 are not implemented: the older copy, unchanged.
    for (const double lits : {0.0, -0.5}) {
      const Result r = Run(Material(lits, true), true);
      CHECK_MSG(r.materials == 1 && r.scales.empty() && r.turned > 0, "LITS 0 or another negative keeps the older copy");
    }
  }
  {  // A positive LITS: a back primitive and material of its own, diffuse |LITS| and F0 zero;
     // its vertices keep the older copy's turned normals (the shader turns them back).
    const Result r = Run(Material(0.4, true), true);
    CHECK_MSG(r.materials == 2, "LITS 0.4: the back copy is its own material");
    CHECK_MSG(r.turned > 0 && r.towardFront > 0, "LITS 0.4: the back copy has the turned normals, as legacy");
    CHECK_MSG(r.scales.size() == 1, "LITS 0.4: one PBR6 record, the back's");
    if (r.scales.size() == 1) {
      CHECK_MSG(std::fabs(r.scales[0].first - 0.4f) < 1e-6f && r.scales[0].second == 0.f,
                "LITS 0.4: diffuse 0.4, F0 factor 0");
    }
  }
  {  // 1 is a positive LITS too: diffuse 1, F0 0 on the back.
    const Result r = Run(Material(1.0, true), true);
    CHECK_MSG(r.materials == 2 && r.scales.size() == 1 && r.turned > 0, "LITS 1: a split with a PBR6 record");
    if (!r.scales.empty()) {
      CHECK_MSG(r.scales[0].first == 1.f && r.scales[0].second == 0.f, "LITS 1: factors 1 and 0");
    }
  }
  {  // One-sided meshes have no back copy, whatever LITS says.
    const Result r = Run(Material(0.4, true), false);
    CHECK_MSG(r.materials == 1 && r.scales.empty() && r.turned == 0, "a one-sided mesh ignores LITS");
  }
}

// A holo shader (86CD1703, kind 16) through the whole converter: the kind must survive the
// "not layered" demotion, which leaves it a PBR8 trailer behind (a kind 0 record has none).
void TestHoloKind() {
  ModelMaterial material = Material(0, false);
  material.shaderId[0] = 0x86;
  material.shaderId[1] = 0xCD;
  material.shaderId[2] = 0x17;
  material.shaderId[3] = 0x03;
  std::vector<uint8_t> cmdl;
  if (!Convert(BuildModel(material, false), cmdl)) {
    return;
  }
  bool trailer = false;
  for (size_t o = 0; o + 4 <= cmdl.size(); ++o) {
    trailer = trailer || std::memcmp(&cmdl[o], "PBR8", 4) == 0;
  }
  CHECK_MSG(trailer, "a holo shader keeps kind 16 (PBR8 trailer) through the converter");
}

// The converter's shader ids, as port_remastered_convert_material.cpp reads them.
void SetShader(ModelMaterial& material, uint32_t shader) {
  material.shaderId[0] = uint8_t(shader >> 24);
  material.shaderId[1] = uint8_t(shader >> 16);
  material.shaderId[2] = uint8_t(shader >> 8);
  material.shaderId[3] = uint8_t(shader);
}

ModelMaterialData ColorParam(uint32_t usage, float r, float g, float b, float a) {
  ModelMaterialData d;
  d.usage = usage;
  d.kind = ModelMaterialData::Kind::Color;
  d.color[0] = r;
  d.color[1] = g;
  d.color[2] = b;
  d.color[3] = a;
  return d;
}

ModelMaterialData TexParam(uint32_t usage, uint8_t id3) {
  ModelMaterialData d;
  d.usage = usage;
  d.kind = ModelMaterialData::Kind::Texture;
  d.texture.usage = usage;
  d.texture.hasUsage = true;
  d.texture.id[3] = id3;
  d.texture.wrapX = 1;
  d.texture.wrapY = 1;
  return d;
}

bool Near(float a, float b) { return std::fabs(a - b) < 1e-6f; }

// The first output material's blob: secs[0] holds [ntex][tex ids][nblobs][ends][blobs].
bool FirstBlob(const std::vector<uint8_t>& d, size_t& start, size_t& size) {
  if (d.size() < 48) {
    return false;
  }
  const uint32_t nsec = Be32(d, 36);
  if (nsec < 3 || 44 + 4 * size_t(nsec) > d.size()) {
    return false;
  }
  const size_t base = (44 + 4 * size_t(nsec) + 31) & ~size_t(31);
  if (base + 12 > d.size()) {
    return false;
  }
  const uint32_t ntex = Be32(d, base);
  const size_t nmats = Be32(d, base + 4 + 4 * size_t(ntex));
  if (nmats < 1 || base + 12 + 4 * size_t(ntex) + 4 * nmats > d.size()) {
    return false;
  }
  size = Be32(d, base + 8 + 4 * size_t(ntex));
  start = base + 8 + 4 * size_t(ntex) + 4 * nmats;
  return start + size <= d.size() && size >= 36;
}

// flags, nmaps, tex indices, then the vertex descriptor word.
uint32_t BlobNmaps(const std::vector<uint8_t>& d, size_t start) { return Be32(d, start + 4); }

uint32_t BlobVtx(const std::vector<uint8_t>& d, size_t start) {
  return Be32(d, start + 8 + 4 * size_t(BlobNmaps(d, start)));
}

// The two blend-mode half-words PbrMaterial writes after the group word (dst, then src).
void BlobBlend(const std::vector<uint8_t>& d, size_t start, uint16_t& dst, uint16_t& src) {
  const size_t o = start + 8 + 4 * size_t(BlobNmaps(d, start)) + 8;
  dst = uint16_t((uint16_t(d[o]) << 8) | d[o + 1]);
  src = uint16_t((uint16_t(d[o + 2]) << 8) | d[o + 3]);
}

std::string BlobTag(const std::vector<uint8_t>& d, size_t start, size_t size) {
  return std::string(reinterpret_cast<const char*>(&d[start + size - 4]), 4);
}

struct ShieldTrailer {
  bool has = false;  // a PBR8 trailer: kinds 14-19, the ones with shieldRows
  float row[32] = {};
  std::string record;  // the tag before it: PBR4, or PBR7 with a cube
  float kind = 0.f;    // the record's kind float, when the tag carries one
  bool hasKind = false;
};

ShieldTrailer BlobShield(const std::vector<uint8_t>& d, size_t start, size_t size) {
  ShieldTrailer t;
  if (BlobTag(d, start, size) != "PBR8") {
    return t;
  }
  t.has = true;
  const size_t end = start + size;
  for (int i = 0; i < 32; ++i) {
    t.row[i] = BeFloat(d, end - 132 + 4 * size_t(i));
  }
  const size_t tag = end - 136;
  t.record.assign(reinterpret_cast<const char*>(&d[tag]), 4);
  // 19 record floats, then the wrap word and, for PBR6/PBR7, the light scales and the cube.
  const size_t extra = t.record == "PBR7" ? 16 : t.record == "PBR6" ? 12 : t.record == "PBR5" ? 4 : 0;
  if (t.record == "PBR4" || t.record == "PBR7") {
    t.kind = BeFloat(d, tag - extra - 76 + 13 * 4);
    t.hasKind = true;
  }
  return t;
}

std::vector<uint8_t> ConvertDecisions(const ModelMaterial& material, std::vector<MaterialDecision>& decisions,
                                      bool colored = false, bool withCube = false) {
  std::vector<uint8_t> cmdl;
  if (!Convert(BuildModel(material, false, colored), cmdl, &decisions, withCube)) {
    return {};
  }
  return cmdl;
}

void CheckTrailerRows(const float row[32], const float rgb[3], float w, const float difc[4], const char* what,
                      int zeroFrom = 0, int zeroTo = 24) {
  char msg[160];
  bool zero = true;
  for (int i = zeroFrom; i < zeroTo; ++i) {
    zero = zero && row[i] == 0.f;
  }
  std::snprintf(msg, sizeof(msg), "%s: the rows before 6 are zero", what);
  CHECK_MSG(zero, msg);
  if (rgb != nullptr) {
    std::snprintf(msg, sizeof(msg), "%s: row 6 rgb", what);
    CHECK_MSG(Near(row[24], rgb[0]) && Near(row[25], rgb[1]) && Near(row[26], rgb[2]), msg);
  }
  std::snprintf(msg, sizeof(msg), "%s: row 6 w", what);
  CHECK_MSG(Near(row[27], w), msg);
  std::snprintf(msg, sizeof(msg), "%s: row 7 DIFC", what);
  CHECK_MSG(Near(row[28], difc[0]) && Near(row[29], difc[1]) && Near(row[30], difc[2]) && Near(row[31], difc[3]), msg);
}

// Kind 17 (6344950D): the holo plus the material's own REFL cube. Row 6 is ICNC + ICMC
// with the cube gain in w; like kinds 16 and 18 it is not demoted for having no second layer.
void TestHoloReflKind() {
  ModelMaterial material = Material(0, false);
  SetShader(material, 0x6344950D);
  material.data.push_back(ColorParam(FourCC('D', 'I', 'F', 'C'), 0.5f, 0.25f, 0.125f, 0.75f));
  material.data.push_back(ColorParam(FourCC('I', 'C', 'N', 'C'), 1.f, 2.f, 4.f, 0.f));
  material.data.push_back(ColorParam(FourCC('I', 'C', 'M', 'C'), 8.f, 16.f, 32.f, 0.f));
  material.data.push_back(TexParam(FourCC('R', 'E', 'F', 'L'), 0x44));
  std::vector<MaterialDecision> decisions;
  const std::vector<uint8_t> cmdl = ConvertDecisions(material, decisions);
  CHECK_MSG(!cmdl.empty(), "kind 17 converts");
  if (cmdl.empty()) {
    return;
  }
  CHECK_MSG(decisions.size() == 1, "kind 17: one output material");
  if (!decisions.empty()) {
    CHECK_MSG(decisions[0].kind == 17, "kind 17: the decision keeps kind 17 (not demoted)");
    CHECK_MSG(decisions[0].path == "pbr", "kind 17: the PBR path");
    CHECK_MSG(decisions[0].tag == "PBR8", "kind 17: a PBR8 trailer");
    CHECK_MSG(decisions[0].cube == "-", "kind 17: no cube without a cube reader");
  }
  size_t start = 0, size = 0;
  CHECK_MSG(FirstBlob(cmdl, start, size), "kind 17: the material blob is readable");
  const ShieldTrailer t = BlobShield(cmdl, start, size);
  CHECK_MSG(t.has && t.record == "PBR4", "kind 17: a PBR4 record with a PBR8 trailer");
  CHECK_MSG(t.hasKind && t.kind == 17.f, "kind 17: the record's kind float");
  const float rgb[3] = {9.f, 18.f, 36.f}, difc[4] = {0.5f, 0.25f, 0.125f, 0.75f};
  CheckTrailerRows(t.row, rgb, 1.f, difc, "kind 17");
  {  // Without REFL the incandescence stays but the cube gain is 0.
    ModelMaterial bare = material;
    bare.data.pop_back();
    std::vector<MaterialDecision> bareDecisions;
    const std::vector<uint8_t> bareCmdl = ConvertDecisions(bare, bareDecisions);
    size_t bstart = 0, bsize = 0;
    const bool readable = !bareCmdl.empty() && FirstBlob(bareCmdl, bstart, bsize);
    CHECK_MSG(readable, "kind 17 without REFL converts");
    if (readable) {
      const ShieldTrailer b = BlobShield(bareCmdl, bstart, bsize);
      CHECK_MSG(b.has && b.hasKind && b.kind == 17.f, "kind 17 without REFL: still kind 17");
      CheckTrailerRows(b.row, rgb, 0.f, difc, "kind 17 without REFL");
    }
  }
  {  // With a cube reader the material's own REFL is read: the record carries its id (PBR7).
    std::vector<MaterialDecision> cubeDecisions;
    const std::vector<uint8_t> cubeCmdl = ConvertDecisions(material, cubeDecisions, false, true);
    size_t cstart = 0, csize = 0;
    const bool readable = !cubeCmdl.empty() && FirstBlob(cubeCmdl, cstart, csize);
    CHECK_MSG(readable, "kind 17 with a cube reader converts");
    if (readable) {
      const ShieldTrailer c = BlobShield(cubeCmdl, cstart, csize);
      CHECK_MSG(c.has && c.record == "PBR7" && c.hasKind && c.kind == 17.f,
                "kind 17 with a cube reader: REFL read, PBR7 record, kind 17");
      CHECK_MSG(!cubeDecisions.empty() && cubeDecisions[0].cube != "-", "kind 17 with a cube reader: a cube id");
    }
  }
}

// Kind 18 (4CA0017C): the scrolling hologram. It needs CCH0..3 (without them it is no kind),
// rows 0-3 are CCH0..3, row 6 is ICMC alone (ICNC ignored), row 7 DIFC, and it reads the
// vertex colour.
void TestHologramKind() {
  {
    ModelMaterial energy = Material(0, false);
    SetShader(energy, 0x9C867F3C);
    energy.unk1 = 0x411; // as shipped: transparent, vertex colour, 0x400
    energy.data.push_back(TexParam(FourCC('T', 'C', 'H', '0'), 0x55));
    energy.data.push_back(ColorParam(FourCC('C', 'C', 'H', '0'), 6.f, 20.f, 0.35f, 0.95f));
    energy.data.push_back(ColorParam(FourCC('C', 'C', 'H', '1'), 2.f, 1.5f, 1.f, 1.f));
    energy.data.push_back(ColorParam(FourCC('D', 'I', 'F', 'C'), 0.5f, 0.25f, 0.125f, 0.75f));
    std::vector<MaterialDecision> decisions;
    const auto cmdl = ConvertDecisions(energy, decisions, true);
    REQUIRE(!decisions.empty());
    CHECK(decisions[0].kind == 44 && decisions[0].path == "pbr");
    size_t at = 0, size = 0;
    REQUIRE(FirstBlob(cmdl, at, size));
    const auto trailer = BlobShield(cmdl, at, size);
    CHECK(trailer.has && trailer.hasKind && trailer.kind == 44.f);
    CHECK(Near(trailer.row[0], 6.f) && Near(trailer.row[1], 20.f));
    CHECK(Near(trailer.row[4], 2.f) && Near(trailer.row[5], 1.5f));
    CHECK(Near(trailer.row[28], 0.5f) && Near(trailer.row[31], 0.75f));
    CHECK((BlobVtx(cmdl, at) & 0x30u) == 0x30u);
  }
  ModelMaterial material = Material(0, false);
  SetShader(material, 0x4CA0017C);
  material.data.push_back(ColorParam(FourCC('C', 'C', 'H', '0'), 1.f, 2.f, 3.f, 4.f));
  material.data.push_back(ColorParam(FourCC('C', 'C', 'H', '1'), 5.f, 6.f, 7.f, 8.f));
  material.data.push_back(ColorParam(FourCC('C', 'C', 'H', '2'), 9.f, 10.f, 11.f, 12.f));
  material.data.push_back(ColorParam(FourCC('C', 'C', 'H', '3'), 13.f, 14.f, 15.f, 16.f));
  material.data.push_back(ColorParam(FourCC('I', 'C', 'M', 'C'), 7.f, 8.f, 9.f, 0.f));
  material.data.push_back(ColorParam(FourCC('I', 'C', 'N', 'C'), 100.f, 200.f, 300.f, 0.f));
  material.data.push_back(ColorParam(FourCC('D', 'I', 'F', 'C'), 0.5f, 0.25f, 0.125f, 0.75f));
  std::vector<MaterialDecision> decisions;
  const std::vector<uint8_t> cmdl = ConvertDecisions(material, decisions, true);
  CHECK_MSG(!cmdl.empty(), "kind 18 converts");
  if (cmdl.empty()) {
    return;
  }
  if (!decisions.empty()) {
    CHECK_MSG(decisions[0].kind == 18, "kind 18: the decision keeps kind 18 (not demoted)");
    CHECK_MSG(decisions[0].path == "pbr", "kind 18: the PBR path");
  }
  size_t start = 0, size = 0;
  CHECK_MSG(FirstBlob(cmdl, start, size), "kind 18: the material blob is readable");
  const ShieldTrailer t = BlobShield(cmdl, start, size);
  CHECK_MSG(t.has && t.record == "PBR4" && t.hasKind && t.kind == 18.f, "kind 18: a PBR4 record with kind 18");
  bool cch = true;
  for (int r = 0; r < 4; ++r) {
    for (int i = 0; i < 4; ++i) {
      cch = cch && Near(t.row[r * 4 + i], float(r * 4 + i + 1));
    }
  }
  CHECK_MSG(cch, "kind 18: rows 0-3 are CCH0..CCH3");
  const float rgb[3] = {7.f, 8.f, 9.f}, difc[4] = {0.5f, 0.25f, 0.125f, 0.75f};
  // Rows 0-3 are the CCHs above; rows 4-5 (run-time world x/y) stay zero.
  CheckTrailerRows(t.row, rgb, 0.f, difc, "kind 18", 16, 24);
  CHECK_MSG((BlobVtx(cmdl, start) & 0x30u) == 0x30u, "kind 18: the vertex colour is read (colour bits)");
  {  // Without CCH0..3 it is no kind: the standard record, no trailer, a fallback note.
    ModelMaterial bare = Material(0, false);
    SetShader(bare, 0x4CA0017C);
    bare.data.push_back(ColorParam(FourCC('D', 'I', 'F', 'C'), 0.5f, 0.25f, 0.125f, 0.75f));
    std::vector<MaterialDecision> bareDecisions;
    const std::vector<uint8_t> bareCmdl = ConvertDecisions(bare, bareDecisions);
    size_t bstart = 0, bsize = 0;
    const bool readable = !bareCmdl.empty() && FirstBlob(bareCmdl, bstart, bsize);
    CHECK_MSG(readable, "kind 18 without CCH0..3 converts");
    if (readable) {
      CHECK_MSG(!BlobShield(bareCmdl, bstart, bsize).has, "kind 18 without CCH0..3: no PBR8 trailer");
      CHECK_MSG(BlobTag(bareCmdl, bstart, bsize) == "PBR2", "kind 18 without CCH0..3: the standard record (PBR2: no REFL, mode bit 256)");
    }
    CHECK_MSG(!bareDecisions.empty() && bareDecisions[0].kind == 0, "kind 18 without CCH0..3: kind 0");
    CHECK_MSG(!bareDecisions.empty() && bareDecisions[0].kindReason.find("fallback: hologram") != std::string::npos,
              "kind 18 without CCH0..3: the hologram fallback note");
  }
  {  // The colour bits come from vcolor: kind 16 on the same coloured model has none, and
    // kind 18 with no vertex colours has none either (there is nothing to read).
    ModelMaterial holo = Material(0, false);
    SetShader(holo, 0x86CD1703);
    std::vector<MaterialDecision> holoDecisions;
    const std::vector<uint8_t> holoCmdl = ConvertDecisions(holo, holoDecisions, true);
    size_t hstart = 0, hsize = 0;
    CHECK_MSG(!holoCmdl.empty() && FirstBlob(holoCmdl, hstart, hsize), "kind 16 on a coloured model converts");
    if (!holoCmdl.empty() && hsize != 0) {
      CHECK_MSG((BlobVtx(holoCmdl, hstart) & 0x30u) == 0u, "kind 16: no colour bits without vcolor");
    }
    std::vector<MaterialDecision> plainDecisions;
    const std::vector<uint8_t> plainCmdl = ConvertDecisions(material, plainDecisions, false);
    size_t pstart = 0, psize = 0;
    CHECK_MSG(!plainCmdl.empty() && FirstBlob(plainCmdl, pstart, psize), "kind 18 with no colours converts");
    if (!plainCmdl.empty() && psize != 0) {
      const ShieldTrailer p = BlobShield(plainCmdl, pstart, psize);
      CHECK_MSG(p.has && p.hasKind && p.kind == 18.f, "kind 18 with no colours: still kind 18");
      CHECK_MSG((BlobVtx(plainCmdl, pstart) & 0x30u) == 0u, "kind 18 with no colours: no colour bits");
    }
  }
}

// Kind 19 (98F0556D): the lit sphere-map fx. REFS and REFV go to the second layer's base and
// MR (raw), so the blob has all seven maps; the trailer rows are row 6 ICNC + ICMC (w the
// retail konst alpha, 1 on an opaque surface) and row 7 DIFC.
void TestGunFxKind() {
  ModelMaterial material = Material(0, false);
  SetShader(material, 0x98F0556D);
  material.data.push_back(TexParam(FourCC('R', 'E', 'F', 'S'), 0x55));
  material.data.push_back(TexParam(FourCC('R', 'E', 'F', 'V'), 0x66));
  material.data.push_back(ColorParam(FourCC('D', 'I', 'F', 'C'), 0.5f, 0.25f, 0.125f, 0.75f));
  material.data.push_back(ColorParam(FourCC('I', 'C', 'N', 'C'), 1.f, 1.f, 2.f, 0.f));
  material.data.push_back(ColorParam(FourCC('I', 'C', 'M', 'C'), 4.f, 8.f, 16.f, 0.f));
  std::vector<MaterialDecision> decisions;
  const std::vector<uint8_t> cmdl = ConvertDecisions(material, decisions);
  CHECK_MSG(!cmdl.empty(), "kind 19 converts");
  if (cmdl.empty()) {
    return;
  }
  if (!decisions.empty()) {
    CHECK_MSG(decisions[0].kind == 19, "kind 19: the decision keeps kind 19");
    CHECK_MSG(decisions[0].path == "pbr", "kind 19: the PBR path");
  }
  size_t start = 0, size = 0;
  CHECK_MSG(FirstBlob(cmdl, start, size), "kind 19: the material blob is readable");
  CHECK_MSG(BlobNmaps(cmdl, start) == 7, "kind 19: all seven maps, the second layer bound");
  const ShieldTrailer t = BlobShield(cmdl, start, size);
  CHECK_MSG(t.has && t.record == "PBR4" && t.hasKind && t.kind == 19.f, "kind 19: a PBR4 record with kind 19");
  const float rgb[3] = {5.f, 9.f, 18.f}, difc[4] = {0.5f, 0.25f, 0.125f, 0.75f};
  CheckTrailerRows(t.row, rgb, 1.f, difc, "kind 19");
  {  // Without REFV the second layer's MR is missing and it is no kind: the plain
  // unlit-less standard record (the unused layer is dropped with the kind).
    ModelMaterial bare = material;
    bare.data.erase(bare.data.begin() + 2);
    std::vector<MaterialDecision> bareDecisions;
    const std::vector<uint8_t> bareCmdl = ConvertDecisions(bare, bareDecisions);
    size_t bstart = 0, bsize = 0;
    const bool readable = !bareCmdl.empty() && FirstBlob(bareCmdl, bstart, bsize);
    CHECK_MSG(readable, "kind 19 without REFV converts");
    if (readable) {
      CHECK_MSG(!BlobShield(bareCmdl, bstart, bsize).has, "kind 19 without REFV: no PBR8 trailer");
      CHECK_MSG(BlobTag(bareCmdl, bstart, bsize) == "PBR2", "kind 19 without REFV: the standard record (PBR2: no REFL, mode bit 256)");
    }
    CHECK_MSG(!bareDecisions.empty() && bareDecisions[0].kind == 0, "kind 19 without REFV: kind 0");
  }
}

// Shader 4BC890C1 ("lambert4b"): a plain lit Lambert, no kind. It leaves the retail-fx gate
// (IsFx), so it takes the lit opaque PBR path: kind 0, the standard record, no trailer. Over
// a blended retail surface its blend words stay opaque, where another shader keeps retail's.
void TestLambertFx() {
  ModelMaterial material = Material(0, false);
  SetShader(material, 0x4BC890C1);
  std::vector<MaterialDecision> decisions;
  const std::vector<uint8_t> cmdl = ConvertDecisions(material, decisions);
  CHECK_MSG(!cmdl.empty(), "lambert-fx converts");
  if (cmdl.empty()) {
    return;
  }
  if (!decisions.empty()) {
    CHECK_MSG(decisions[0].kind == 0, "lambert-fx: kind 0");
    CHECK_MSG(decisions[0].path == "pbr", "lambert-fx: the PBR path, not the retail-fx TEV fallback");
    CHECK_MSG(decisions[0].role == "lambert-fx", "lambert-fx: the shader role");
  }
  size_t start = 0, size = 0;
  CHECK_MSG(FirstBlob(cmdl, start, size), "lambert-fx: the material blob is readable");
  CHECK_MSG(!BlobShield(cmdl, start, size).has, "lambert-fx: no PBR8 trailer");
  CHECK_MSG(BlobTag(cmdl, start, size) == "PBR2", "lambert-fx: the standard record (PBR2: no REFL, mode bit 256)");
  {  // Blended-flagged, over retail's blended surface: opaque blend words (dst 0, src 1).
    ModelMaterial blended = material;
    blended.unk1 = 0x1;
    std::vector<MaterialDecision> blendedDecisions;
    const std::vector<uint8_t> blendedCmdl = ConvertDecisions(blended, blendedDecisions);
    size_t bstart = 0, bsize = 0;
    const bool readable = !blendedCmdl.empty() && FirstBlob(blendedCmdl, bstart, bsize);
    CHECK_MSG(readable, "blended lambert-fx converts");
    if (readable) {
      uint16_t dst = 9, src = 9;
      BlobBlend(blendedCmdl, bstart, dst, src);
      CHECK_MSG(dst == 0 && src == 1, "blended lambert-fx: opaque blend words");
    }
    ModelMaterial other = Material(0, false);
    SetShader(other, 0x12345678);
    other.unk1 = 0x1;
    std::vector<MaterialDecision> otherDecisions;
    const std::vector<uint8_t> otherCmdl = ConvertDecisions(other, otherDecisions);
    size_t ostart = 0, osize = 0;
    CHECK_MSG(!otherCmdl.empty() && FirstBlob(otherCmdl, ostart, osize), "a blended unknown shader converts");
    if (!otherCmdl.empty() && osize != 0) {
      uint16_t dst = 9, src = 9;
      BlobBlend(otherCmdl, ostart, dst, src);
      CHECK_MSG(dst == 5 && src == 4, "a blended unknown shader: retail's blend words");
    }
  }
}

// The shader's cotangent frame (Schueler), as written in shader.cpp: T and B of a pixel from
// the screen derivatives of position and UV and the stored normal.
struct V3 {
  double x, y, z;
};
V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a) { return {-a.x, -a.y, -a.z}; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

void Frame(V3 dp1, V3 dp2, double uv1[2], double uv2[2], V3 ngs, V3& t, V3& b) {
  const V3 dp2perp = Cross(dp2, ngs), dp1perp = Cross(ngs, dp1);
  t = dp2perp * uv1[0] + dp1perp * uv2[0];
  b = dp2perp * uv1[1] + dp1perp * uv2[1];
}

// A front pixel with the surface's normal, and the same surface point seen from behind: the
// screen's handedness flips (one derivative negated), and a back copy stores the turned
// normal. The back must get the front's T and B, for any UV orientation. Derived by hand,
// and shown here with numbers: the shader's `ngs` is the stored normal, `ng` the restored.
void TestFrame() {
  const V3 pu = {1.0, 0.2, 0.0}, pv = {-0.1, 0.9, 0.3};
  for (const double mirror : {1.0, -1.0}) {  // mirrored UVs: V grows against the surface's winding
    const V3 pvm = pv * mirror;
    const V3 n = Cross(pu, pv) * (1.0 / std::sqrt(Dot(Cross(pu, pv), Cross(pu, pv))));
    // Screen derivatives as (du, dv) per pixel step, front viewer.
    const double a1[2] = {0.7, 0.2}, a2[2] = {-0.3, 0.9};
    const V3 dp1 = pu * a1[0] + pvm * a1[1], dp2 = pu * a2[0] + pvm * a2[1];
    V3 tf, bf;
    Frame(dp1, dp2, const_cast<double*>(a1), const_cast<double*>(a2), n, tf, bf);
    // From behind: the same surface, the screen's second axis runs the other way round.
    const double c2[2] = {-a2[0], -a2[1]};
    const V3 dq2 = pu * c2[0] + pvm * c2[1];
    V3 tb, bb, tr, br;
    Frame(dp1, dq2, const_cast<double*>(a1), const_cast<double*>(c2), -n, tb, bb);  // stored: turned round
    Frame(dp1, dq2, const_cast<double*>(a1), const_cast<double*>(c2), n, tr, br);   // the restored normal
    auto same = [](V3 a, V3 c) { return std::fabs(a.x - c.x) + std::fabs(a.y - c.y) + std::fabs(a.z - c.z) < 1e-9; };
    CHECK_MSG(same(tb, tf) && same(bb, bf), "a back copy's frame on the stored normal is the front's T and B");
    CHECK_MSG(same(tr, -tf) && same(br, -bf), "the restored normal in the frame would mirror T and B (why it is not used)");
    CHECK_MSG(Dot(tf, n) < 1e-9 && Dot(tb, n) < 1e-9, "T is in the surface's plane");
  }
}

} // namespace

// E2644678 (the Ore Processing pillar glass) is kind 29 with its own row 4 and 5: the phase scale CCH4.x, the
// shipped depth cap 0.02 and the flag that has the game bind the camera's probe. Without CCH4 it is no kind 29.
void TestHoloGlassE264() {
  ModelMaterial material = Material(0, false);
  SetShader(material, 0xE2644678);
  material.data.push_back(TexParam(FourCC('T', 'C', 'H', '0'), 0x51));
  material.data.push_back(TexParam(FourCC('T', 'C', 'H', '1'), 0x52));
  material.data.push_back(TexParam(FourCC('T', 'C', 'H', '2'), 0x53));
  for (int i = 0; i < 4; ++i) {
    const float b = float(i * 4);
    material.data.push_back(ColorParam(FourCC('C', 'C', 'H', char('0' + i)), b + 1.f, b + 2.f, b + 3.f, b + 4.f));
  }
  material.data.push_back(ColorParam(FourCC('D', 'I', 'F', 'C'), 0.5f, 0.25f, 0.125f, 0.75f));
  {
    std::vector<MaterialDecision> decisions;
    const auto cmdl = ConvertDecisions(material, decisions, true);
    CHECK_MSG(decisions.empty() || decisions[0].kind != 29, "E2644678 without CCH4 is not kind 29");
  }
  material.data.push_back(ColorParam(FourCC('C', 'C', 'H', '4'), 3.5f, 9.f, 9.f, 9.f));
  std::vector<MaterialDecision> decisions;
  const auto cmdl = ConvertDecisions(material, decisions, true);
  REQUIRE(!decisions.empty());
  CHECK_MSG(decisions[0].kind == 29 && decisions[0].path == "pbr", "E2644678: kind 29, PBR");
  size_t at = 0, size = 0;
  REQUIRE(FirstBlob(cmdl, at, size));
  const auto t = BlobShield(cmdl, at, size);
  CHECK_MSG(t.has && t.hasKind && t.kind == 29.f, "E2644678: a kind 29 trailer");
  for (int i = 0; i < 16; ++i) {
    CHECK_MSG(Near(t.row[i], float(i + 1)), "E2644678: rows 0-3 are CCH0..CCH3");
  }
  CHECK_MSG(t.row[16] == 0.f && t.row[17] == 0.f, "E2644678: phase and near are the game's");
  CHECK_MSG(t.row[18] == 0.019999999552965164f, "E2644678: the depth cap is binary32 0x3ca3d70a");
  CHECK_MSG(Near(t.row[19], 3.5f), "E2644678: row 4 w is CCH4.x");
  CHECK_MSG(t.row[20] == 0.f && t.row[21] == 1.f, "E2644678: row 5 marks it, without HoloGlassC's terms");
  CHECK_MSG(t.row[27] == 0.f, "E2644678: no cube of its own");
  CHECK_MSG(Near(t.row[28], 0.5f) && Near(t.row[31], 0.75f), "E2644678: row 7 is DIFC");
  // 3991da00 keeps rows 4 and 5 clear.
  SetShader(material, 0x3991DA00);
  std::vector<MaterialDecision> plainDecisions;
  const auto plain = ConvertDecisions(material, plainDecisions, true);
  REQUIRE(FirstBlob(plain, at, size));
  const auto p = BlobShield(plain, at, size);
  CHECK_MSG(p.hasKind && p.kind == 29.f && p.row[18] == 0.f && p.row[19] == 0.f && p.row[21] == 0.f,
            "3991da00: no E2644678 rows");
}

void TestRouteHologram() {
  ModelMaterial material = Material(0, false);
  SetShader(material, 0x5586D988);
  material.data.push_back(ColorParam(FourCC('C', 'C', 'H', '0'), .1f, 1.f, .05f, 0.f));
  material.data.push_back(ColorParam(FourCC('C', 'C', 'H', '1'), 1.f, 4.f, 1.f, 2.5f));
  material.data.push_back(ColorParam(FourCC('C', 'C', 'H', '2'), 0.f, .8f, 0.f, .028f));
  std::vector<MaterialDecision> incomplete;
  ConvertDecisions(material, incomplete, true);
  CHECK_MSG(incomplete.empty() || incomplete[0].kind != 45, "route hologram requires fade parameters");
  material.data.push_back(ColorParam(FourCC('C', 'C', 'H', '3'), 1.f, 1.f, 1.f, .2f));
  material.data.push_back(ColorParam(FourCC('D', 'I', 'F', 'C'), .5f, .25f, .125f, .75f));
  for (bool colored : {false, true}) {
    std::vector<MaterialDecision> decisions;
    const auto cmdl = ConvertDecisions(material, decisions, colored);
    REQUIRE(!decisions.empty());
    CHECK_MSG(decisions[0].kind == 45 && decisions[0].path == "pbr", "route hologram keeps its shader with or without a color stream");
    size_t at = 0, size = 0;
    REQUIRE(FirstBlob(cmdl, at, size));
    const auto t = BlobShield(cmdl, at, size);
    CHECK_MSG(t.has && t.hasKind && t.kind == 45.f, "route hologram writes the full PBR8 trailer");
    CHECK_MSG(Near(t.row[0], .1f) && Near(t.row[10], .8f) && Near(t.row[9], 0.f) && Near(t.row[11], .028f), "scanline/alpha survive and apex follows the import basis");
    CHECK_MSG(Near(t.row[12], 1.f) && Near(t.row[15], .2f) && Near(t.row[31], .75f), "fade and authored DIFC alpha survive import");
  }
}

int main() {
  TestRouteHologram();
  TestReader();
  TestHoloGlassE264();
  TestConverter();
  TestHoloKind();
  TestHoloReflKind();
  TestHologramKind();
  TestGunFxKind();
  TestLambertFx();
  TestFrame();
  if (port_test::Failures() == 0) {
    std::printf("port_remastered_lits_tests: ok\n");
  }
  return PORT_TEST_RESULT();
}
