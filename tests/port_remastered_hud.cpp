#include "port_remastered_hud.h"
#include "port_test.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

using namespace PortRemastered;

namespace {

bool Close(float a, float b) { return std::fabs(a - b) < 1e-5f; }

using Blob = std::vector<uint8_t>;

void Big32(Blob& out, uint32_t v) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(uint8_t(v >> shift));
  }
}
void Little32(Blob& out, uint32_t v) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(uint8_t(v >> shift));
  }
}
uint32_t Bits(float v) {
  uint32_t bits;
  std::memcpy(&bits, &v, sizeof(bits));
  return bits;
}
uint32_t Tag(const char* text) {
  return uint32_t(uint8_t(text[0])) << 24 | uint32_t(uint8_t(text[1])) << 16 | uint32_t(uint8_t(text[2])) << 8 |
         uint32_t(uint8_t(text[3]));
}

// One widget of a disc frame, at `x` along its parent's x axis, unrotated.
void DiscWidget(Blob& out, const char* type, const char* name, const char* parent, const Blob& typeData, float x) {
  Big32(out, Tag(type));
  out.insert(out.end(), name, name + std::strlen(name) + 1);
  out.insert(out.end(), parent, parent + std::strlen(parent) + 1);
  out.insert(out.end(), {0, 1, 1, 0});
  for (int i = 0; i < 4; ++i) {
    Big32(out, Bits(1.0f));
  }
  Big32(out, 1);
  out.insert(out.end(), typeData.begin(), typeData.end());
  out.push_back(0);
  Big32(out, Bits(x));
  Big32(out, 0);
  Big32(out, 0);
  for (int i = 0; i < 9; ++i) {
    Big32(out, Bits(i % 4 == 0 ? 1.0f : 0.0f));
  }
  out.insert(out.end(), 18, 0);
}

Blob DiscFrame() {
  Blob out;
  for (uint32_t v : {0u, 0u, 0u, 0u, 4u}) {
    Big32(out, v);
  }
  DiscWidget(out, "HWIG", "kGSYS_HeadWidgetID", "kGSYS_DummyWidgetID", {}, 0.0f);
  Blob camera;
  for (uint32_t v : {0u, Bits(55.0f), Bits(1.33f), Bits(0.25f), Bits(500.0f)}) {
    Big32(camera, v);
  }
  DiscWidget(out, "CAMR", "camera", "kGSYS_HeadWidgetID", camera, 0.0f);
  Blob model;
  for (uint32_t v : {0x11111111u, 0u, 7u}) {
    Big32(model, v);
  }
  DiscWidget(out, "MODL", "model_frame", "kGSYS_HeadWidgetID", model, 1.0f);
  DiscWidget(out, "MODL", "model_gone", "kGSYS_HeadWidgetID", model, 2.0f);
  return out;
}

// A disc model with nothing but the material set the converter takes its material from.
Blob TemplateModel() {
  Blob out(0x24, 0);
  Big32(out, 1);
  out.resize(0x40);
  Big32(out, 0);  // textures
  Big32(out, 4);
  for (uint32_t end : {0x20u, 0x40u, 0x60u, 0x80u}) {
    Big32(out, end);
  }
  out.resize(out.size() + 0x80, 0);
  return out;
}

void RemWidget(Blob& out, uint32_t type, uint32_t index, uint32_t parent, const char* name, float y, const Blob& data) {
  for (uint32_t v : {type, index, parent, 0xFFFFFFFFu, uint32_t(std::strlen(name))}) {
    Little32(out, v);
  }
  out.insert(out.end(), name, name + std::strlen(name));
  out.insert(out.end(), {1, 1, 0});
  for (float c : {1.0f, 0.5f, 0.25f, 1.0f}) {
    Little32(out, Bits(c));
  }
  Little32(out, 2);
  Little32(out, 0);
  const float matrix[12] = {1, 0, 0, 0, 0, 1, 0, y, 0, 0, 1, 0};
  for (float v : matrix) {
    Little32(out, Bits(v));
  }
  out.insert(out.end(), data.begin(), data.end());
}

Blob RemFrame() {
  Blob out(48, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GUIF", 4);
  for (int i = 0; i < 16; ++i) {
    out[32 + size_t(i)] = uint8_t(i);
  }
  Little32(out, 5);
  RemWidget(out, 4, 0, 0, "root", 0.0f, {});
  RemWidget(out, 4, 1, 0, "FRME_Test", 0.0f, {});
  Blob camera;
  for (uint32_t v : {0u, Bits(40.0f), Bits(1.75f)}) {
    Little32(camera, v);
  }
  RemWidget(out, 1, 2, 1, "Camera", 0.0f, camera);
  Blob mesh;
  for (uint32_t v : {1u, 0u, 0u}) {
    Little32(mesh, v);
  }
  RemWidget(out, 9, 3, 1, "Model_Frame", 3.0f, mesh);
  RemWidget(out, 9, 4, 3, "model_trim", 1.0f, mesh);
  return out;
}

// A unit square in Remastered's x/y plane.
Model Square() {
  Model model;
  ModelVertexBuffer buffer;
  buffer.vertexCount = 4;
  buffer.positions = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
  buffer.uvs.push_back({0, 0, 1, 0, 1, 1, 0, 1});
  model.vertexBuffers.push_back(buffer);
  ModelMaterial material;
  ModelMaterialData data;
  data.usage = Tag("BCLR");
  data.kind = ModelMaterialData::Kind::Texture;
  data.texture.id[0] = 1;
  material.data.push_back(data);
  model.materials.push_back(material);
  ModelMesh mesh;
  mesh.indices = {0, 1, 2, 0, 2, 3};
  model.meshes.push_back(mesh);
  return model;
}

struct Read {
  const Blob& data;
  size_t at = 0;
  uint32_t U32() {
    const uint32_t v = at + 4 <= data.size() ? uint32_t(data[at]) << 24 | uint32_t(data[at + 1]) << 16 |
                                                   uint32_t(data[at + 2]) << 8 | uint32_t(data[at + 3])
                                             : 0;
    at += 4;
    return v;
  }
  float Float() {
    const uint32_t bits = U32();
    float v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
  }
  std::string String() {
    std::string s;
    while (at < data.size() && data[at] != 0) {
      s.push_back(char(data[at++]));
    }
    ++at;
    return s;
  }
};

void TestFrame() {
  std::map<std::string, Blob> files;
  const Blob disc = DiscFrame();
  const Blob templateModel = TemplateModel();
  ConvertIO io;
  io.retail = [&](uint32_t type, uint32_t id, Blob& out) {
    if (type == Tag("FRME") && id == 0x12345678) {
      out = disc;
    } else if (type == Tag("CMDL") && id == 0xE64E5DBA) {
      out = templateModel;
    } else {
      return false;
    }
    return true;
  };
  io.retailId = [](uint32_t id) { return id == 0x52480000; };
  ModelUuid asked{};
  io.texture = [&](const ModelUuid& id, Image& out, std::string&) {
    asked = id;
    out.width = out.height = 16;
    out.rgba.assign(16 * 16 * 4, 0xFF);
    return true;
  };
  io.write = [&](const std::string& name, const Blob& data) {
    files[name] = data;
    return true;
  };
  const Blob frame = RemFrame();
  ModelUuid modelId{};
  CHECK_MSG(HudFrameModel(frame.data(), frame.size(), modelId) && modelId[0] == 3 && modelId[3] == 0 && modelId[4] == 5 &&
                modelId[8] == 8,
            "the frame's model id, in a pak's order");
  CHECK_MSG(!HudFrameModel(disc.data(), disc.size(), modelId), "a disc frame is not a Remastered one");

  HudConverter converter(io);
  HudCounts counts;
  std::string error;
  CHECK_MSG(converter.Convert(0x12345678, frame.data(), frame.size(), Square(), counts, error), "the frame converts");
  CHECK_MSG(counts.widgets == 5 && counts.models == 2 && counts.textures == 1 && counts.bars == 0, "what was made");
  CHECK_MSG(asked[3] == 1 && asked[0] == 0, "the texture is asked for in a pak's order");
  // The first model id is the disc's, so the models start one past it; a small picture needs no native copy.
  CHECK_MSG(files.count("52480001.CMDL") == 1 && files.count("52480002.CMDL") == 1 && files.count("52470000.TXTR") == 1 &&
            files.count("52470000.dds") == 0 && files.size() == 4,
        "the files");
  CHECK_MSG(!converter.Convert(0x99999999, frame.data(), frame.size(), Square(), counts, error), "no such disc frame");
  CHECK_MSG(!converter.Convert(0x12345678, frame.data(), frame.size() - 30, Square(), counts, error), "a cut frame");

  const auto found = files.find("12345678.FRME");
  CHECK_MSG(found != files.end(), "the frame is written under the disc's id");
  if (found == files.end()) {
    return;
  }
  Read in{found->second, 16};
  CHECK_MSG(in.U32() == 5, "the disc's four widgets and Remastered's trim");
  const char* names[5] = {"kGSYS_HeadWidgetID", "camera", "model_frame", "model_trim", "model_gone"};
  const char* parents[5] = {"kGSYS_DummyWidgetID", "kGSYS_HeadWidgetID", "kGSYS_HeadWidgetID", "model_frame",
                            "kGSYS_HeadWidgetID"};
  for (int i = 0; i < 5; ++i) {
    const uint32_t type = in.U32();
    CHECK_MSG(in.String() == names[i], names[i]);
    CHECK_MSG(in.String() == parents[i], "its parent");
    in.at += 4;
    const float red = in.Float(), green = in.Float();
    in.at += 8;
    const uint32_t draw = in.U32();
    uint32_t model = 0;
    if (type == Tag("CAMR")) {
      CHECK_MSG(in.U32() == 0 && Close(in.Float(), 40.0f) && Close(in.Float(), 1.75f) && Close(in.Float(), 0.25f) &&
                    Close(in.Float(), 500.0f),
                "Remastered's projection between the disc's clip planes");
    } else if (type == Tag("MODL")) {
      model = in.U32();
      in.at += 4;
      CHECK_MSG(in.U32() == (i == 3 ? 0u : 7u), "the model's light mask");
    }
    CHECK_MSG(found->second[in.at] == 0, "no worker");
    in.at += 1;
    const float x = in.Float(), y = in.Float(), z = in.Float();
    in.at += 36 + 18;
    if (i == 2) {
      // Remastered's y is up, the disc's z.
      CHECK_MSG(model == 0x52480001 && draw == 2 && Close(green, 0.5f), "the widget is drawn as Remastered's");
      CHECK_MSG(Close(x, 0.0f) && Close(y, 0.0f) && Close(z, 3.0f), "and stands where Remastered has it");
    } else if (i == 3) {
      CHECK_MSG(model == 0x52480002 && Close(z, 1.0f) && Close(red, 1.0f), "a widget only Remastered has, under its parent");
    } else if (i == 4) {
      CHECK_MSG(model == 0xFFFFFFFF && Close(x, 2.0f), "a model only the disc has is emptied, and stays put");
    }
  }
  CHECK_MSG(in.at == found->second.size(), "nothing after the last widget");

  // The model: the square stood up, drawn with the disc's material.
  const Blob& cmdl = files["52480001.CMDL"];
  Read model{cmdl, 0};
  CHECK_MSG(model.U32() == 0xDEADBABE && cmdl.size() % 32 == 0, "a disc model");
  model.at = 12;
  float bounds[6];
  for (float& v : bounds) {
    v = model.Float();
  }
  CHECK_MSG(Close(bounds[0], 0.0f) && Close(bounds[3], 1.0f) && Close(bounds[1], 0.0f) && Close(bounds[4], 0.0f) &&
                Close(bounds[2], 0.0f) && Close(bounds[5], 1.0f),
            "its bounds, in the disc's axes");
  CHECK_MSG(model.U32() == 8, "seven data sections and one surface");
}

void Little16(Blob& out, uint32_t v) {
  out.push_back(uint8_t(v));
  out.push_back(uint8_t(v >> 8));
}

// A colour property of a tweak file: only the components given, as Remastered writes them.
void TweakColor(Blob& out, uint32_t hash, const std::vector<std::pair<uint32_t, float>>& components) {
  Little32(out, hash);
  Little16(out, uint32_t(2 + components.size() * 10));
  Little16(out, uint32_t(components.size()));
  for (const auto& [component, value] : components) {
    Little32(out, component);
    Little16(out, 4);
    Little32(out, Bits(value));
  }
}

void TestBeamTints() {
  constexpr uint32_t kR = 0x110889D1, kG = 0x8A7AFF22, kB = 0x2A5349E9;
  Blob ldta(0x38, 0);
  std::memcpy(ldta.data(), "RFRM", 4);
  std::memcpy(ldta.data() + 0x14, "LDTA", 4);
  std::memcpy(ldta.data() + 0x20, "LDCH", 4);
  Little16(ldta, 5);
  TweakColor(ldta, 0x12345678, {{kR, 0.f}});            // not a beam's
  TweakColor(ldta, 0x2584A7DF, {{kB, 0.f}});            // Power
  TweakColor(ldta, 0xE8DF071A, {});                     // Ice
  TweakColor(ldta, 0x735A17B9, {{kR, .5f}, {kG, .2f}});  // Wave
  TweakColor(ldta, 0xB7B9CFBC, {{kR, .8f}, {kG, .1f}, {kB, 2.f}});  // Plasma, clamped
  std::map<std::string, std::array<float, 4>> tints;
  std::string error;
  CHECK_MSG(HudBeamIconTints(ldta.data(), ldta.size(), tints, error) && tints.size() == 4, "four beam colours");
  CHECK_MSG((tints["model_beamicon3"] == std::array<float, 4>{1.f, 1.f, 0.f, 1.f}), "Power is yellow");
  CHECK_MSG((tints["model_beamicon2"] == std::array<float, 4>{1.f, 1.f, 1.f, 1.f}), "Ice keeps the defaults");
  CHECK_MSG(Close(tints["model_beamicon1"][0], .5f) && Close(tints["model_beamicon1"][1], .2f) &&
                Close(tints["model_beamicon1"][2], 1.f),
            "Wave is purple");
  CHECK_MSG(Close(tints["model_beamicon0"][1], .1f) && Close(tints["model_beamicon0"][2], 1.f), "Plasma, clamped");
  tints.clear();
  CHECK_MSG(!HudBeamIconTints(ldta.data(), 0x30, tints, error), "a cut file");
  ldta[0] = 'X';
  CHECK_MSG(!HudBeamIconTints(ldta.data(), ldta.size(), tints, error), "not a tweak file");
}

void TestIcyMaterial() {
  Blob ldta(0x38, 0);
  std::memcpy(ldta.data(), "RFRM", 4);
  std::memcpy(ldta.data() + 0x14, "LDTA", 4);
  std::memcpy(ldta.data() + 0x20, "LDCH", 4);
  Little16(ldta, 2);
  TweakColor(ldta, 0x12345678, {{0x110889D1, 0.f}});  // something else first
  // The materials: two GUIDs, the icy one second, as RenderLooseDataMP1 has them.
  Little32(ldta, 0x5F2E374B);
  Little16(ldta, 2 + 2 * 22);
  Little16(ldta, 2);
  for (const uint32_t member : {0x8FC08AABu, 0xC2347D95u}) {
    Little32(ldta, member);
    Little16(ldta, 16);
    for (int b = 0; b < 16; ++b) {
      ldta.push_back(uint8_t(member + b));
    }
  }
  ModelUuid icy{};
  std::string error;
  CHECK_MSG(RenderIcyMaterial(ldta.data(), ldta.size(), icy, error), error);
  CHECK_MSG(icy[0] == uint8_t(0xC2347D95u) && icy[15] == uint8_t(0xC2347D95u + 15), "the second GUID");
  CHECK_MSG(!RenderIcyMaterial(ldta.data(), ldta.size() - 1, icy, error), "a cut file");
  ldta[0] = 'X';
  CHECK_MSG(!RenderIcyMaterial(ldta.data(), ldta.size(), icy, error), "not a tweak file");
}
}  // namespace

int main() {
  TestFrame();
  TestBeamTints();
  TestIcyMaterial();
  if (port_test::Failures() == 0) {
    std::printf("port_remastered_hud: ok\n");
  }
  return PORT_TEST_RESULT();
}
