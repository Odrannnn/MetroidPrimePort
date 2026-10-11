// The AUVI rule the converter applies to a material's map texcoords, checked both
// as the rule alone (all seven map coords, the flag and the parameter chosen) and
// through a real conversion: a synthetic model is run through
// PortRemastered::Converter and the texcoord of every map is read back out of the
// CMDL it wrote, which is where a remap that is computed and then ignored would
// show up as no change at all.

#include "port_remastered_convert.h"
#include "port_remastered_anuv_gun.h"
#include "port_pbr_record.h"
#include "port_remastered_uv.h"

#include <algorithm>
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

// The converter's own flag for a PBR material (kPbrFlag there), so the test can
// tell a PBR material from a TEV one without reaching into the converter.
constexpr uint32_t kPbrFlag = 0x4000;

// One material: a base map on `baseCoord`, an MR map on `mrCoord`, and whatever
// parameters `extra` adds.
struct Fixture {
  ModelMaterial material;
  explicit Fixture(uint32_t flags, uint32_t baseCoord, uint32_t mrCoord) {
    material.name = "uvtest";
    material.unk1 = flags;
    material.types.push_back(FourCC('R', 'L', 'T', 'G'));
    material.data.push_back(Texture(FourCC('D', 'I', 'F', 'T'), 0x11111111, baseCoord));
    material.data.push_back(Texture(FourCC('M', 'E', 'T', 'L'), 0x22222222, mrCoord));
  }
  ModelMaterialData Texture(uint32_t usage, uint32_t id, uint32_t coord) {
    ModelMaterialData d;
    d.usage = usage;
    d.kind = ModelMaterialData::Kind::Texture;
    d.texture.usage = usage;
    d.texture.hasUsage = true;
    d.texture.id[0] = uint8_t(id >> 24);
    d.texture.id[3] = uint8_t(id);
    d.texture.texCoord = coord;
    d.texture.wrapX = 1;
    d.texture.wrapY = 1;
    return d;
  }
  ModelMaterialData Auvi(int32_t a, int32_t b, int32_t c, int32_t d,
                         ModelMaterialData::Kind kind = ModelMaterialData::Kind::Int4) {
    ModelMaterialData p;
    p.usage = kAuviUsage;
    p.kind = kind;
    p.int4[0] = a;
    p.int4[1] = b;
    p.int4[2] = c;
    p.int4[3] = d;
    if (kind == ModelMaterialData::Kind::Int1) {
      p.int1 = a;
    }
    return p;
  }
};

uint32_t Be32(const std::vector<uint8_t>& d, size_t o) {
  return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
}

// Runs a conversion of the fixture's model, standalone (which is how a room's own
// material is converted), and returns the CMDL it wrote. False when it failed,
// with the message in `error`.
bool Convert(const Model& model, std::vector<uint8_t>& cmdl, std::string& error, bool lightmapUv = false) {
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
  opt.standalone = true;
  opt.lightmapUv = lightmapUv;
  opt.retail = 0xABCD1234;
  opt.skip.clear();  // the default drops a material whose name holds "simple"
  Converter converter(io);
  return converter.Convert(model, opt, error);
}

// A model with one triangle, one material and `uvSets` texcoord sets, so a coord
// past the third is one the vertex shader has no output channel for.
Model BuildModel(const ModelMaterial& material, size_t uvSets) {
  Model model;
  model.materials.push_back(material);
  ModelVertexBuffer vb;
  vb.vertexCount = 3;
  for (int v = 0; v < 3; ++v) {
    for (int c = 0; c < 3; ++c) {
      vb.positions.push_back(float(v + c));
    }
    vb.normals.push_back(0.f);
    vb.normals.push_back(1.f);
    vb.normals.push_back(0.f);
  }
  vb.uvs.resize(uvSets);
  for (size_t s = 0; s < uvSets; ++s) {
    for (int v = 0; v < 3; ++v) {
      vb.uvs[s].push_back(float(v));
      vb.uvs[s].push_back(float(s));
    }
  }
  model.vertexBuffers.push_back(vb);
  ModelMesh mesh;
  mesh.material = 0;
  mesh.vertexBuffer = 0;
  mesh.indices = {0, 1, 2};
  model.meshes.push_back(mesh);
  return model;
}

// Every map's texcoord as the converted CMDL binds it: the TEV fallback's stages
// name one per map, base first. A converter that writes no readable PBR material
// leaves `coords` empty. Every read is bounds-checked, so a converter that emitted
// a malformed CMDL fails the test rather than reading past the end.
bool ReadMapCoords(const std::vector<uint8_t>& d, std::vector<uint32_t>& coords) {
  coords.clear();
  const size_t n = d.size();
  size_t p = 0;
  auto need = [&](size_t at, size_t bytes) { return at <= n && n - at >= bytes; };
  auto word = [&](size_t at, uint32_t& out) {
    if (!need(at, 4)) {
      return false;
    }
    out = Be32(d, at);
    return true;
  };
  // Header: magic, version, flags, the bounding box, then the section count, the
  // material set count and one size per section.
  if (!need(0, 12 + 24 + 4 + 4)) {
    return false;
  }
  uint32_t nsec = 0, ntex = 0, nmats = 0, flags = 0, nmaps = 0, nchan = 0, nstages = 0;
  p = 12 + 24;
  if (!word(p, nsec)) {
    return false;
  }
  p += 8;  // past the section count and the material set count
  if (nsec == 0 || nsec > (n - p) / 4) {
    return false;
  }
  p += 4 * size_t(nsec);
  p = (p + 31) & ~size_t(31);  // the sections follow the header, 32-byte aligned
  // The first section is the material set: the texture ids, then the materials.
  if (!word(p, ntex) || ntex > (n - p - 4) / 4) {
    return false;
  }
  p += 4 + 4 * size_t(ntex);
  if (!word(p, nmats) || nmats == 0 || nmats > (n - p - 4) / 4) {
    return false;
  }
  p += 4 + 4 * size_t(nmats);
  // Material zero's blob, in the order PbrMaterial writes it.
  size_t m = p;
  if (!word(m, flags)) {
    return false;
  }
  m += 4;
  if ((flags & kPbrFlag) == 0) {  // not a PBR material: nothing to read
    return false;
  }
  if (!word(m, nmaps) || nmaps == 0 || nmaps > (n - m - 4) / 4) {
    return false;
  }
  m += 4 + 4 * size_t(nmaps);  // the maps' texture indices
  m += 4;                      // the vertex descriptor
  m += 4;                      // the material's cache id
  if ((flags & 0x8) != 0) {
    m += 8;  // a ColorUnlit material's constant colour
  }
  m += 4;  // the blend factors
  if (!word(m, nchan) || nchan > (n - m - 4) / 4) {
    return false;
  }
  m += 4 + 4 * size_t(nchan);  // the lit channels
  if (!word(m, nstages) || nstages > (n - m - 4) / 20) {
    return false;
  }
  m += 4;
  m += 20 * size_t(nstages);  // the stages, four words and four flags each
  // Then one entry per stage: padding, the map slot it samples and that map's
  // texcoord, base first.
  if (nstages > (n - m) / 4) {
    return false;
  }
  for (uint32_t i = 0; i < nstages; ++i) {
    if (!need(m, 4)) {
      return false;
    }
    m += 2;
    const uint32_t slot = d[m];
    m += 1;
    const uint32_t coord = d[m];
    m += 1;
    if (slot < nmaps) {
      coords.resize(slot + 1);
      coords[slot] = coord;
    }
  }
  return coords.size() == nmaps;
}

struct CmdlSection {
  size_t offset = 0;
  size_t size = 0;
};

struct LightmapOutput {
  int slot = -1;
  std::vector<std::pair<float, float>> coords;
};

uint16_t Be16(const std::vector<uint8_t>& d, size_t o) {
  return uint16_t((uint16_t(d[o]) << 8) | d[o + 1]);
}

bool ReadSections(const std::vector<uint8_t>& d, uint32_t& materialSets, std::vector<CmdlSection>& sections) {
  if (d.size() < 44 || Be32(d, 0) != 0xDEADBABE) {
    return false;
  }
  const uint32_t count = Be32(d, 36);
  materialSets = Be32(d, 40);
  if (count == 0 || count > (d.size() - 44) / 4) {
    return false;
  }
  size_t offset = (44 + size_t(count) * 4 + 31) & ~size_t(31);
  if (offset > d.size()) {
    return false;
  }
  sections.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    const size_t size = Be32(d, 44 + size_t(i) * 4);
    if (size > d.size() - offset) {
      return false;
    }
    sections[i] = {offset, size};
    offset += size;
  }
  return materialSets > 0 && count >= materialSets + 6;
}

// Reads the actual LMUV slot and follows the generated display-list indices into
// the CMDL texcoord section. The tiny fixtures use one 0x90 command per surface,
// no colour attribute and no vertex welding, so `vertexCount` is known exactly.
bool ReadLightmapOutputs(const std::vector<uint8_t>& d, size_t vertexCount, std::vector<LightmapOutput>& outputs) {
  uint32_t materialSets = 0;
  std::vector<CmdlSection> sections;
  if (vertexCount == 0 || !ReadSections(d, materialSets, sections)) {
    return false;
  }
  const CmdlSection& materialSection = sections[0];
  size_t p = materialSection.offset;
  if (materialSection.size < 8) {
    return false;
  }
  const uint32_t textureCount = Be32(d, p);
  p += 4;
  if (textureCount > (materialSection.offset + materialSection.size - p) / 4) {
    return false;
  }
  p += size_t(textureCount) * 4;
  if (p + 4 > materialSection.offset + materialSection.size) {
    return false;
  }
  const uint32_t materialCount = Be32(d, p);
  p += 4;
  if (materialCount == 0 || materialCount > (materialSection.offset + materialSection.size - p) / 4) {
    return false;
  }
  std::vector<uint32_t> ends(materialCount);
  for (uint32_t& end : ends) {
    end = Be32(d, p);
    p += 4;
  }
  const size_t materialData = p;
  outputs.assign(materialCount, LightmapOutput{});
  uint32_t previousEnd = 0;
  std::vector<uint32_t> texcoordCounts(materialCount, 0);
  for (uint32_t i = 0; i < materialCount; ++i) {
    if (ends[i] < previousEnd || ends[i] > materialSection.offset + materialSection.size - materialData) {
      return false;
    }
    const size_t start = materialData + previousEnd;
    const size_t size = ends[i] - previousEnd;
    if (size < 12) {
      return false;
    }
    const uint32_t flags = Be32(d, start);
    const uint32_t maps = Be32(d, start + 4);
    const size_t descriptorAt = start + 8 + size_t(maps) * 4;
    if ((flags & kPbrFlag) == 0 || descriptorAt + 4 > start + size) {
      return false;
    }
    const uint32_t descriptor = Be32(d, descriptorAt);
    for (int slot = 0; slot < 8; ++slot) {
      texcoordCounts[i] += ((descriptor >> (8 + 2 * slot)) & 3) != 0 ? 1u : 0u;
    }
    outputs[i].slot = PortPbrRecord::LightmapSlot(d.data() + start + size, size);
    previousEnd = ends[i];
  }

  const size_t surfaceTableIndex = size_t(materialSets) + 5;
  const CmdlSection& surfaceTable = sections[surfaceTableIndex];
  if (surfaceTable.size < 4) {
    return false;
  }
  const uint32_t surfaceCount = Be32(d, surfaceTable.offset);
  if (surfaceCount > sections.size() - (size_t(materialSets) + 6)) {
    return false;
  }
  const CmdlSection& uvSection = sections[size_t(materialSets) + 3];
  const size_t uvBytesPerVertex = 8;
  for (uint32_t surfaceIndex = 0; surfaceIndex < surfaceCount; ++surfaceIndex) {
    const CmdlSection& surface = sections[size_t(materialSets) + 6 + surfaceIndex];
    if (surface.size < 67) {
      return false;
    }
    const uint32_t material = Be32(d, surface.offset + 12);
    if (material >= materialCount) {
      return false;
    }
    const int lightmapSlot = outputs[material].slot;
    if (lightmapSlot < 0) {
      continue;
    }
    if (uint32_t(lightmapSlot) >= texcoordCounts[material]) {
      return false;
    }
    const uint32_t displayListSize = Be32(d, surface.offset + 16) & 0x7FFFFFFFu;
    const size_t displayList = surface.offset + 64;
    if (displayListSize < 3 || displayListSize > surface.size - 64 || d[displayList] != 0x90) {
      return false;
    }
    const uint16_t indexCount = Be16(d, displayList + 1);
    const size_t stride = 4 + size_t(texcoordCounts[material]) * 2;
    if (indexCount == 0 || 3 + size_t(indexCount) * stride > displayListSize) {
      return false;
    }
    for (uint16_t i = 0; i < indexCount; ++i) {
      const size_t item = displayList + 3 + size_t(i) * stride;
      const size_t attribute = item + 4 + size_t(lightmapSlot) * 2;
      const size_t emittedIndex = Be16(d, attribute);
      const size_t array = emittedIndex / vertexCount;
      const size_t vertex = emittedIndex % vertexCount;
      const size_t uvAt = uvSection.offset + (array * vertexCount + vertex) * uvBytesPerVertex;
      if (uvAt > uvSection.offset + uvSection.size || uvSection.offset + uvSection.size - uvAt < uvBytesPerVertex) {
        return false;
      }
      outputs[material].coords.emplace_back(PortPbrRecord::BeFloat(d.data() + uvAt),
                                             PortPbrRecord::BeFloat(d.data() + uvAt + 4));
    }
  }
  return true;
}

ModelMaterial LightmapMaterial(uint32_t shader, const char* name) {
  Fixture fixture(0, 0, 2);
  fixture.material.name = name;
  fixture.material.data[0].usage = FourCC('B', 'C', 'L', 'R');
  fixture.material.data[0].texture.usage = FourCC('B', 'C', 'L', 'R');
  for (int i = 0; i < 4; ++i) {
    fixture.material.shaderId[size_t(i)] = uint8_t(shader >> (24 - 8 * i));
  }
  return fixture.material;
}

ModelVertexBuffer LightmapBuffer(float offset, bool hasCoord3) {
  ModelVertexBuffer vb;
  vb.vertexCount = 3;
  vb.uvs.resize(2);
  vb.uvsZw.resize(hasCoord3 ? 2 : 1);
  for (int vertex = 0; vertex < 3; ++vertex) {
    vb.positions.insert(vb.positions.end(), {offset + float(vertex), float(vertex * vertex), float(vertex * 2)});
    vb.normals.insert(vb.normals.end(), {0.f, 1.f, 0.f});
    vb.uvs[0].insert(vb.uvs[0].end(), {0.25f, 0.5f});
    vb.uvsZw[0].insert(vb.uvsZw[0].end(), {11.f, 12.f});
    vb.uvs[1].insert(vb.uvs[1].end(), {21.f, 22.f});
    if (hasCoord3) {
      vb.uvsZw[1].insert(vb.uvsZw[1].end(), {31.f, 32.f});
    }
  }
  return vb;
}

Model LightmapModel(std::vector<ModelMaterial> materials, std::vector<ModelVertexBuffer> buffers,
                    const std::vector<std::pair<uint32_t, uint32_t>>& draws) {
  Model model;
  model.materials = std::move(materials);
  model.vertexBuffers = std::move(buffers);
  for (const auto& [material, buffer] : draws) {
    ModelMesh mesh;
    mesh.material = material;
    mesh.vertexBuffer = buffer;
    mesh.indices = {0, 1, 2};
    model.meshes.push_back(std::move(mesh));
  }
  return model;
}

bool AllLightmapCoords(const LightmapOutput& output, float u, float v) {
  return output.slot >= 0 && output.coords.size() >= 3 &&
         std::all_of(output.coords.begin(), output.coords.end(),
                     [=](const auto& coord) { return coord.first == u && coord.second == v; });
}

// The rule on its own, over the seven coords a material can have: base, MR,
// normal, emissive, then the second layer's base, MR and normal.
void TestRule() {
  {  // Nothing to remap with: every coord is left as it was.
    Fixture f(0x40, 0, 2);
    uint32_t coords[7] = {0, 1, 2, 3, 4, 5, 6};
    ApplyAuvi(f.material, coords, 7);
    Check(coords[0] == 0 && coords[1] == 1 && coords[2] == 2 && coords[3] == 3 && coords[4] == 4 && coords[5] == 5 &&
              coords[6] == 6,
          "a flagged material with no AUVI keeps every texcoord");
  }
  {  // The flag is what admits the parameter.
    Fixture f(0x40, 0, 0);
    f.material.data.push_back(f.Auvi(1, 0, 0, 0));
    f.material.unk1 &= ~kAuviFlag;
    uint32_t coords[7] = {0, 1, 2, 3, 4, 5, 6};
    ApplyAuvi(f.material, coords, 7);
    Check(coords[0] == 0 && coords[1] == 1 && coords[2] == 2 && coords[3] == 3 && coords[4] == 4 && coords[5] == 5 &&
              coords[6] == 6,
          "an AUVI without the flag is not read");
  }
  {  // Only the first three values are read; a coord past them has no channel.
    Fixture f(kAuviFlag, 0, 0);
    f.material.data.push_back(f.Auvi(2, 1, 0, 3));
    uint32_t coords[7] = {0, 1, 2, 3, 4, 5, 6};
    ApplyAuvi(f.material, coords, 7);
    Check(coords[0] == 2 && coords[1] == 1 && coords[2] == 0, "the three output channels are remapped");
    Check(coords[3] == 3 && coords[4] == 4 && coords[5] == 5 && coords[6] == 6,
          "a coord past the third output channel is left alone");
  }
  {  // Every coord is remapped, the second layer's three included: they are the
     // same array the converter gathers the primary four into.
    Fixture f(kAuviFlag, 0, 0);
    f.material.data.push_back(f.Auvi(1, 0, 0, 0));
    uint32_t coords[7] = {0, 2, 1, 2, 0, 2, 1};
    ApplyAuvi(f.material, coords, 7);
    Check(coords[0] == 1 && coords[1] == 0 && coords[2] == 0 && coords[3] == 0 && coords[4] == 1 && coords[5] == 0 &&
              coords[6] == 0,
          "the three output channels of all seven coords are remapped, the second layer's three included");
  }
  {  // Two AUVIs are a later saying again, not a chain to walk the coords through:
     // (1,0,0) then (0,2,0) leaves texcoord 0 on 0, while chaining walks it 0 -> 1 -> 2.
    Fixture f(kAuviFlag, 0, 0);
    f.material.data.push_back(f.Auvi(1, 0, 0, 0));
    f.material.data.push_back(f.Auvi(0, 2, 0, 0));
    uint32_t coords[7] = {0, 1, 2, 3, 4, 5, 6};
    ApplyAuvi(f.material, coords, 7);
    Check(coords[0] == 0 && coords[1] == 2 && coords[2] == 0,
          "only the last Int4 AUVI is applied, once (chaining would give coord 0 = 2)");
  }
  {  // An AUVI of another shape is not the parameter.
    Fixture f(kAuviFlag, 0, 0);
    f.material.data.push_back(f.Auvi(1, 0, 0, 0));
    f.material.data.push_back(f.Auvi(3, 3, 3, 3, ModelMaterialData::Kind::Scalar));
    uint32_t coords[7] = {0, 0, 0, 0, 0, 0, 0};
    ApplyAuvi(f.material, coords, 7);
    Check(coords[0] == 1, "a later AUVI that is not an Int4 is ignored");
    Fixture g(kAuviFlag, 0, 0);
    g.material.data.push_back(g.Auvi(3, 3, 3, 3, ModelMaterialData::Kind::Int1));
    uint32_t again[7] = {0, 0, 0, 0, 0, 0, 0};
    ApplyAuvi(g.material, again, 7);
    Check(again[0] == 0, "an Int1 AUVI is not read either");
  }
  {  // A negative selector is not one the port recognises, so the map keeps the
     // texcoord it had.
    Fixture f(kAuviFlag, 0, 0);
    f.material.data.push_back(f.Auvi(1, -1, -7, 0));
    uint32_t coords[7] = {0, 1, 2, 3, 4, 5, 6};
    ApplyAuvi(f.material, coords, 7);
    Check(coords[0] == 1 && coords[1] == 1 && coords[2] == 2 && coords[3] == 3 && coords[4] == 4 && coords[5] == 5 &&
              coords[6] == 6,
          "a negative mapped value preserves the texcoord");
  }
  Check(Auvi(Fixture(kAuviFlag, 0, 0).material) == nullptr, "Auvi is null without a parameter");
  Check(Auvi(Fixture(0, 0, 0).material) == nullptr, "Auvi is null without the flag");
}

// One conversion of the fixture's material, and the base and MR texcoords the
// written CMDL binds them with.
bool Converted(uint32_t flags, uint32_t baseCoord, uint32_t mrCoord, const std::vector<ModelMaterialData>& params,
               uint32_t& baseOut, uint32_t& mrOut, size_t uvSets = 8) {
  Fixture f(flags, baseCoord, mrCoord);
  for (const ModelMaterialData& p : params) {
    f.material.data.push_back(p);
  }
  std::vector<uint8_t> cmdl;
  std::string error;
  if (!Convert(BuildModel(f.material, uvSets), cmdl, error)) {
    std::fprintf(stderr, "FAIL: the conversion failed: %s\n", error.c_str());
    ++sFailures;
    return false;
  }
  std::vector<uint32_t> coords;
  if (!ReadMapCoords(cmdl, coords) || coords.size() < 2) {
    std::fprintf(stderr, "FAIL: no PBR material in the converted CMDL\n");
    ++sFailures;
    return false;
  }
  baseOut = coords[0];
  mrOut = coords[1];
  return true;
}

// The same rule through the converter, where a remap that never reaches the
// material it belongs to shows up as the base or the MR map still on its own
// texcoord.
void TestConverter() {
  Fixture probe(kAuviFlag, 0, 2);
  uint32_t base = 0xFFFFFFFFu, mr = 0xFFFFFFFFu;

  {  // AUVI (1,0,0): the base's channel 0 is fed from texcoord set 1 and the MR
     // map's channel 2 from set 0.
    const std::vector<ModelMaterialData> params{probe.Auvi(1, 0, 0, 0)};
    Check(Converted(kAuviFlag, 0, 2, params, base, mr), "converted with an AUVI");
    Check(base == 1 && mr == 0, "AUVI (1,0,0) sends base channel 0 to UV1 and MR channel 2 to UV0");
  }
  {  // The flag is what admits the parameter.
    const std::vector<ModelMaterialData> params{probe.Auvi(1, 0, 0, 0)};
    Check(Converted(0, 0, 2, params, base, mr), "converted without the flag");
    Check(base == 0 && mr == 2, "without the flag every map keeps its own texcoord");
  }
  {  // And with no parameter to read, the port's own fallback stands.
    Check(Converted(kAuviFlag, 0, 2, {}, base, mr), "converted with no AUVI");
    Check(base == 0 && mr == 2, "with no AUVI every map keeps its own texcoord");
  }
  {  // A coord past the vertex shader's third output channel has no channel to go
     // through, so it is left where it was (the model's texcoord set 4).
    const std::vector<ModelMaterialData> params{probe.Auvi(1, 0, 0, 0)};
    Check(Converted(kAuviFlag, 4, 2, params, base, mr), "converted with a coord past the third");
    Check(base == 4 && mr == 0, "a coord past the third output channel is unchanged");
  }
  {  // Two AUVIs: the last one counts, applied once. Chaining them would walk the
     // base map's texcoord 0 through 1 and out to 2.
    const std::vector<ModelMaterialData> params{probe.Auvi(1, 0, 0, 0), probe.Auvi(0, 2, 0, 0)};
    Check(Converted(kAuviFlag, 0, 2, params, base, mr), "converted with two AUVIs");
    Check(base == 0 && mr == 0, "the last Int4 AUVI is applied once (chaining would give base = 2)");
  }
  {  // A negative selector is not one the port recognises: the map keeps its own.
    const std::vector<ModelMaterialData> params{probe.Auvi(-1, -1, -1, 0)};
    Check(Converted(kAuviFlag, 0, 2, params, base, mr), "converted with a negative AUVI");
    Check(base == 0 && mr == 2, "a negative mapped value preserves the texcoord");
  }
  {  // An AUVI after the Int4 one that is not an Int4 is not read, so the Int4
     // still counts.
    const std::vector<ModelMaterialData> params{probe.Auvi(1, 0, 0, 0),
                                                probe.Auvi(3, 3, 3, 3, ModelMaterialData::Kind::Scalar)};
    Check(Converted(kAuviFlag, 0, 2, params, base, mr), "converted with a non-Int4 AUVI");
    Check(base == 1 && mr == 0, "a later AUVI of another shape is ignored");
  }
}

void TestLightmapUv() {
  constexpr uint32_t kProjectedBlend = 0xD6AA2A3A;
  constexpr uint32_t kOrdinaryPbr = 0xBFB300B6;
  std::vector<uint8_t> cmdl;
  std::vector<LightmapOutput> outputs;
  std::string error;

  {  // d6aa2a3a samples TEXCOORD_1.zw (converter coord 3), not UV0.zw.
    const Model model = LightmapModel({LightmapMaterial(kProjectedBlend, "projected")},
                                      {LightmapBuffer(0.f, true)}, {{0, 0}});
    Check(Convert(model, cmdl, error, true), "converted d6aa2a3a with lightmap UVs");
    Check(ReadLightmapOutputs(cmdl, 3, outputs), "read d6aa2a3a's emitted LMUV attribute");
    if (!outputs.empty()) {
      Check(AllLightmapCoords(outputs[0], 31.f, 32.f), "d6aa2a3a LMUV indices address TEXCOORD_1.zw data");
    }
  }
  {  // The ordinary PBR path keeps TEXCOORD_0.zw (converter coord 1).
    const Model model = LightmapModel({LightmapMaterial(kOrdinaryPbr, "ordinary")},
                                      {LightmapBuffer(0.f, false)}, {{0, 0}});
    cmdl.clear();
    outputs.clear();
    Check(Convert(model, cmdl, error, true), "converted ordinary PBR with lightmap UVs");
    Check(ReadLightmapOutputs(cmdl, 3, outputs), "read ordinary PBR's emitted LMUV attribute");
    if (!outputs.empty()) {
      Check(AllLightmapCoords(outputs[0], 11.f, 12.f), "ordinary PBR LMUV indices address TEXCOORD_0.zw data");
    }
  }
  {  // One output material spans two buffers; a missing coord 3 must disable LMUV
     // for the group rather than falling back to either buffer's UV0.
    const Model model = LightmapModel({LightmapMaterial(kProjectedBlend, "mixed")},
                                      {LightmapBuffer(0.f, true), LightmapBuffer(10.f, false)}, {{0, 0}, {0, 1}});
    cmdl.clear();
    outputs.clear();
    Check(Convert(model, cmdl, error, true), "converted mixed-buffer d6aa2a3a");
    Check(ReadLightmapOutputs(cmdl, 6, outputs), "read mixed-buffer d6aa2a3a material");
    if (!outputs.empty()) {
      Check(outputs[0].slot == -1 && outputs[0].coords.empty(),
            "mixed buffers omit LMUV when any primitive buffer lacks the selected channel");
    }
  }
  {  // Material groups choose independently: coord 3 for d6aa2a3a, coord 1 for
     // ordinary PBR, even though they share one model and the latter lacks coord 3.
    const Model model = LightmapModel({LightmapMaterial(kProjectedBlend, "projected"),
                                       LightmapMaterial(kOrdinaryPbr, "ordinary")},
                                      {LightmapBuffer(0.f, true), LightmapBuffer(10.f, false)}, {{0, 0}, {1, 1}});
    cmdl.clear();
    outputs.clear();
    Check(Convert(model, cmdl, error, true), "converted mixed-family lightmap model");
    Check(ReadLightmapOutputs(cmdl, 6, outputs), "read mixed-family LMUV attributes");
    if (outputs.size() >= 2) {
      Check(AllLightmapCoords(outputs[0], 31.f, 32.f), "d6aa2a3a group emits coord 3 independently");
      Check(AllLightmapCoords(outputs[1], 11.f, 12.f), "ordinary PBR group emits coord 1 independently");
    }
  }
}

// Two maps on one source set through different ANUV transforms: the gun model's
// entry moves transform 1 and leaves 0 still. With AUVI (0,0,0,0) both maps read
// set 0, and one texgen would carry the base along with the MR map's motion, so the
// MR map must be given a texcoord of its own.
void TestSlotSplit() {
  Fixture f(kAuviFlag, 0, 1);
  f.material.data.push_back(f.Auvi(0, 0, 0, 0));
  Model model = BuildModel(f.material, 2);
  model.anuv.assign(kAnuvGun, kAnuvGun + sizeof(kAnuvGun));
  std::vector<uint8_t> cmdl;
  std::string error;
  Check(Convert(model, cmdl, error), "converted with an ANUV");
  std::vector<uint32_t> coords;
  Check(ReadMapCoords(cmdl, coords) && coords.size() >= 2, "a PBR material with an ANUV");
  if (coords.size() >= 2) {
    Check(coords[0] != coords[1], "the moving map gets its own texcoord");
  }
  // With all eight texcoords already declared there is no slot to give, and the
  // maps share (the first transform then drives both).
  model.vertexBuffers[0].uvs.resize(8, model.vertexBuffers[0].uvs[0]);
  cmdl.clear();
  coords.clear();
  Check(Convert(model, cmdl, error) && ReadMapCoords(cmdl, coords) && coords.size() >= 2 && coords[0] == coords[1],
        "with no texcoord left the maps share");
  // Without the ANUV the same material shares set 0.
  model.anuv.clear();
  cmdl.clear();
  coords.clear();
  Check(Convert(model, cmdl, error) && ReadMapCoords(cmdl, coords) && coords.size() >= 2 && coords[0] == coords[1],
        "without an ANUV the maps share their set");
}

} // namespace

int main() {
  TestRule();
  TestConverter();
  TestLightmapUv();
  TestSlotSplit();
  if (sFailures == 0) {
    std::printf("port_remastered_uv_tests: ok\n");
  }
  return sFailures == 0 ? 0 : 1;
}
