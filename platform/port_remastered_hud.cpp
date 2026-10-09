// Remastered's HUD frames as frames the original loads (port_remastered_hud.h).

#include "port_remastered_hud.h"
#include "port_strings.h"
#include "port_bytes.h"

#include "port_hud_bars.h"
#include "port_remastered_pak.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>

namespace PortRemastered {
namespace {

constexpr uint32_t Tag(char a, char b, char c, char d) {
  return uint32_t(uint8_t(a)) << 24 | uint32_t(uint8_t(b)) << 16 | uint32_t(uint8_t(c)) << 8 | uint32_t(uint8_t(d));
}

// GUIF widget types.
enum : uint32_t {
  kBaseWidget = 0,
  kCamera = 1,
  kEnergyBar = 2,
  kHeadWidget = 4,
  kMeter = 8,
  kModel = 9,
  kTextPane = 12,
  kLastType = 12,
};

constexpr uint32_t kNoModel = 0xFFFFFFFF;
// Disc models whose fourth material is the one a HUD picture wants: the
// texture times the widget's colour, alpha from the texture. PAL's helmet
// E64E5DBA has only three materials; E942F7EA has the same one on every disc.
constexpr uint32_t kTemplateModels[] = {0xE64E5DBA, 0xE942F7EA};
constexpr size_t kTemplateMaterial = 3;
// The ids the output is written under start here and step past the disc's own.
constexpr uint32_t kModelIds = 0x52480000;
constexpr uint32_t kTextureIds = 0x52470000;
constexpr int kNativeSize = 2048;  // largest edge of a picture
constexpr int kStubSize = 64;      // and of the TXTR standing in for it on the game's heap
// What the game looks a frame's head up by; Remastered names its own after the frame.
constexpr const char* kHeadName = "kGSYS_HeadWidgetID";

// Remastered widgets with no use here: the face reflection, the second set of
// missile digits and a spare camera.
const char* const kSkip[] = {"model_samusface", "textpane_missiledigits1", "camera_default"};
// The game moves the visor and beam menus' items about a base widget; in
// Remastered the place they are laid out around is another widget.
const char* const kAnchor[][2] = {
    {"BaseWidget_VisorMenu", "basewidget_visormenuicons"},
    {"BaseWidget_BeamMenu", "basewidget_beammenuicons"},
};
const char* const kAlias[][2] = {
    {"group_energytank", "group_energytank0"},
    // The map screen's "key / legend" prompt and the pane the game slides it on.
    {"textpane_yicon", "textpane_togglelegend"},
    {"basewidget_ybuttonpane", "basewidget_togglelegendpane"},
};
// Words the disc draws as models (the pause screen's "next", "back" and
// "exit" under their buttons), where Remastered has a text pane its own code
// fills. The model stands in the middle of that pane's box. The pairs go by
// the button each word is under: Remastered's "nexttext" is under the pane
// the disc calls textpane_back, and the other way round.
const char* const kStandIn[][2] = {
    {"model_next1", "textpane_backtext"},
    {"model_back1", "textpane_nexttext"},
    {"model_return1", "textpane_exittext"},
};

// The map screen. Remastered words its prompts as one line and keeps the
// world map's off screen; the disc's code fills these three itself, so they
// stay the disc's.
constexpr uint32_t kMapScreen = 0x97FF54DE;
constexpr double kPi = 3.14159265358979323846;
const char* const kMapPrompt = "textpane_togglelegend";
const char* const kMapDiscOnly[] = {"textpane_instructions", "textpane_right", "textpane_right1"};
// The box around those prompts. Remastered has none, having no such row.
const char* const kMapPromptBox = "model_framemap";

using port::EndsWith;
using port::Hex8;
using port::Lower;

int NextPow2(int v) {
  int p = 1;
  while (p < v) {
    p <<= 1;
  }
  return p;
}

// --- Transforms ------------------------------------------------------------------

struct Mat {
  double m[4][4];
};

Mat Identity() {
  Mat r{};
  for (int i = 0; i < 4; ++i) {
    r.m[i][i] = 1.0;
  }
  return r;
}

Mat Mul(const Mat& a, const Mat& b) {
  Mat r{};
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      for (int k = 0; k < 4; ++k) {
        r.m[i][j] += a.m[i][k] * b.m[k][j];
      }
    }
  }
  return r;
}

// The disc's frame is z-up, Remastered's y-up: disc = (x, -z, y) of Remastered.
void ToDisc(const float* in, float* out) {
  out[0] = in[0];
  out[1] = -in[2];
  out[2] = in[1];
}

// A Remastered transform between the disc's axes.
Mat ToDisc(const Mat& w) {
  Mat a{};
  a.m[0][0] = 1.0;
  a.m[1][2] = -1.0;
  a.m[2][1] = 1.0;
  a.m[3][3] = 1.0;
  Mat t{};
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      t.m[i][j] = a.m[j][i];
    }
  }
  return Mul(Mul(a, w), t);
}

// The inverse of a rotation, scale and translation; false for a flattened one.
bool Inverse(const Mat& a, Mat& out) {
  const double (*m)[4] = a.m;
  const double c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
  const double c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
  const double c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
  const double det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
  if (det == 0.0 || !std::isfinite(det)) {
    return false;
  }
  out = Identity();
  out.m[0][0] = c00 / det;
  out.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
  out.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
  out.m[1][0] = c01 / det;
  out.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
  out.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
  out.m[2][0] = c02 / det;
  out.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
  out.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
  for (int i = 0; i < 3; ++i) {
    out.m[i][3] = -(out.m[i][0] * m[0][3] + out.m[i][1] * m[1][3] + out.m[i][2] * m[2][3]);
  }
  return true;
}

double AxisLength(const Mat& a, int k) {
  return std::sqrt(a.m[0][k] * a.m[0][k] + a.m[1][k] * a.m[1][k] + a.m[2][k] * a.m[2][k]);
}

// --- Reading and writing ---------------------------------------------------------

// Bounds-checked: a read past the end yields zeros and clears `ok`.
struct Reader {
  const uint8_t* data;
  size_t size;
  size_t at;
  bool big;
  bool ok = true;

  bool Has(size_t count) {
    if (at > size || count > size - at) {
      ok = false;
    }
    return ok;
  }
  uint8_t U8() { return Has(1) ? data[at++] : 0; }
  uint16_t U16() {
    if (!Has(2)) {
      return 0;
    }
    const uint16_t v = big ? uint16_t(data[at] << 8 | data[at + 1]) : uint16_t(data[at + 1] << 8 | data[at]);
    at += 2;
    return v;
  }
  uint32_t U32() {
    if (!Has(4)) {
      return 0;
    }
    const uint8_t* p = data + at;
    at += 4;
    return big ? uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3])
               : uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | uint32_t(p[0]);
  }
  float Float() {
    const uint32_t bits = U32();
    float v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
  }
  void Skip(size_t count) {
    if (Has(count)) {
      at += count;
    }
  }
  std::string CString() {
    std::string s;
    while (Has(1) && data[at] != 0) {
      s.push_back(char(data[at++]));
    }
    Skip(1);
    return s;
  }
};

using Blob = std::vector<uint8_t>;

using port::AppendBE16;
using port::AppendBE32;
using port::AppendBEFloat;
using port::GetBE32;
using port::GetBEFloat;
using port::SetBE32;
using port::SetBEFloat;

void Pad32(Blob& out) { out.resize((out.size() + 31) & ~size_t(31)); }

// --- The disc's frame ------------------------------------------------------------

// Offsets into a text pane's type data.
constexpr size_t kTextPaneSize = 74;
constexpr size_t kPalTextPaneExtra = 12;
constexpr size_t kTextPaneExtent = 66;
constexpr size_t kTextPaneJustify = 26;
constexpr size_t kModelSize = 12;

struct Widget {
  uint32_t type = 0;
  std::string name;
  std::string parent;
  uint8_t flags[4] = {};  // animated, visible, active, cull faces
  float color[4] = {};
  uint32_t draw = 0;
  Blob typeData;
  bool hasWorker = false;
  uint16_t worker = 0;
  Mat local = Identity();
  Mat world = Identity();
  bool placed = false;      // `world` is known
  uint8_t tail[18] = {};
  int guif = -1;            // the Remastered widget of the same name
  bool added = false;       // Remastered's own, not on the disc
  bool kept = false;        // the disc's own art, drawn in this layout as it is
  bool row = false;         // on the map screen's row of prompts, which is the disc's
};

bool ParseFrame(const Blob& data, uint32_t header[4], std::vector<Widget>& out, std::string& error) {
  Reader in{data.data(), data.size(), 0, true};
  for (int i = 0; i < 4; ++i) {
    header[i] = in.U32();
  }
  // PAL frames (version 1) follow each text pane with a Japanese font and
  // extents. They're dropped, and the frame is written as version 0 (1.00's).
  const uint32_t version = header[0];
  header[0] = 0;
  const uint32_t count = in.U32();
  for (uint32_t k = 0; k < count && in.ok; ++k) {
    Widget w;
    w.type = in.U32();
    w.name = in.CString();
    w.parent = in.CString();
    for (uint8_t& flag : w.flags) {
      flag = in.U8();
    }
    for (float& c : w.color) {
      c = in.Float();
    }
    w.draw = in.U32();
    const size_t start = in.at;
    switch (w.type) {
    case Tag('C', 'A', 'M', 'R'):
      in.Skip(in.U32() == 0 ? 16 : 24);
      break;
    case Tag('L', 'I', 'T', 'E'): {
      const uint32_t light = in.U32();
      in.Skip(light == 0 ? 32 : 28);
      break;
    }
    case Tag('M', 'O', 'D', 'L'):
      in.Skip(kModelSize);
      break;
    case Tag('M', 'E', 'T', 'R'):
      in.Skip(10);
      break;
    case Tag('G', 'R', 'U', 'P'):
      in.Skip(3);
      break;
    case Tag('T', 'B', 'G', 'P'):
      in.Skip(35);
      break;
    case Tag('S', 'L', 'G', 'P'):
      in.Skip(16);
      break;
    case Tag('P', 'A', 'N', 'E'):
      in.Skip(20);
      break;
    case Tag('T', 'X', 'P', 'N'):
      in.Skip(kTextPaneSize);
      break;
    case Tag('I', 'M', 'G', 'P'): {
      in.Skip(12);
      const uint32_t coords = in.U32();
      in.Skip(size_t(coords) * 12);
      const uint32_t uvs = in.U32();
      in.Skip(size_t(uvs) * 8);
      break;
    }
    case Tag('E', 'N', 'R', 'G'):
      in.Skip(4);
      break;
    case Tag('H', 'W', 'I', 'G'):
    case Tag('B', 'W', 'I', 'G'):
      break;
    default:
      error = "the disc's frame has a widget of an unknown type (" + w.name + ")";
      return false;
    }
    if (!in.ok) {
      break;
    }
    w.typeData.assign(data.begin() + long(start), data.begin() + long(in.at));
    if (w.type == Tag('T', 'X', 'P', 'N') && version >= 1) {
      in.Skip(kPalTextPaneExtra);
    }
    w.hasWorker = in.U8() != 0;
    if (w.hasWorker) {
      w.worker = in.U16();
    }
    for (int i = 0; i < 3; ++i) {
      w.local.m[i][3] = in.Float();
    }
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        w.local.m[i][j] = in.Float();
      }
    }
    if (in.Has(sizeof(w.tail))) {
      std::memcpy(w.tail, in.data + in.at, sizeof(w.tail));
      in.at += sizeof(w.tail);
    }
    out.push_back(std::move(w));
  }
  if (!in.ok || out.empty()) {
    error = "the disc's frame is cut short";
    return false;
  }
  return true;
}

// --- Remastered's frame ----------------------------------------------------------

struct RemWidget {
  uint32_t type = 0;
  uint32_t index = 0;
  uint32_t parent = 0;
  std::string name;
  uint8_t flags[3] = {};
  float color[4] = {};
  uint32_t draw = 0;
  uint32_t variant = 0;  // above 1: another language's copy of a widget
  Mat local = Identity();
  Mat world = Identity();
  bool placed = false;
  uint32_t projection = 0;
  float camera[4] = {};  // fov, aspect; or right, left, top, bottom
  std::vector<uint32_t> meshes;
  uint32_t textJustify[2] = {9, 9};  // a text pane's; 9 when unread
};

constexpr size_t kFrameHeader = 32;

bool IsFrame(const uint8_t* data, size_t size) {
  return size >= kFrameHeader + 20 && std::memcmp(data, "RFRM", 4) == 0 && std::memcmp(data + 0x14, "GUIF", 4) == 0;
}

// Whether the widget numbered `index` starts at `at`. Several widget types end
// in data of no fixed size, so the next one is found by what a header must
// look like: the next index, a parent before it, a short plain name.
bool LooksLikeWidget(const uint8_t* data, size_t size, size_t at, uint32_t index) {
  if (at > size || size - at < 20) {
    return false;
  }
  Reader in{data, size, at, false};
  const uint32_t type = in.U32();
  const uint32_t own = in.U32();
  const uint32_t parent = in.U32();
  const int32_t worker = int32_t(in.U32());
  const uint32_t length = in.U32();
  if (type > kLastType || own != index || parent >= index || worker < -1 || worker >= 256 || length == 0 ||
      length >= 64 || !in.Has(length)) {
    return false;
  }
  for (uint32_t i = 0; i < length; ++i) {
    if (data[in.at + i] >= 0x80) {
      return false;
    }
  }
  return true;
}

bool ParseRemFrame(const uint8_t* data, size_t size, std::vector<RemWidget>& out, std::string& error) {
  if (!IsFrame(data, size)) {
    error = "not a frame";
    return false;
  }
  Reader in{data, size, kFrameHeader + 16, false};
  const uint32_t count = in.U32();
  for (uint32_t k = 0; k < count; ++k) {
    RemWidget w;
    w.type = in.U32();
    w.index = in.U32();
    w.parent = in.U32();
    in.U32();  // worker id; the disc's frame supplies those
    const uint32_t length = in.U32();
    if (!in.Has(length)) {
      break;
    }
    w.name.assign(reinterpret_cast<const char*>(data + in.at), length);
    in.at += length;
    for (uint8_t& flag : w.flags) {
      flag = in.U8();
    }
    for (float& c : w.color) {
      c = in.Float();
    }
    w.draw = in.U32();
    w.variant = in.U32();
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 4; ++j) {
        w.local.m[i][j] = in.Float();
      }
    }
    bool fixed = true;
    switch (w.type) {
    case kBaseWidget:
    case kHeadWidget:
      break;
    case kCamera:
      w.projection = in.U32();
      for (int i = 0; i < (w.projection == 0 ? 2 : 4); ++i) {
        w.camera[i] = in.Float();
      }
      break;
    case kEnergyBar:
      w.meshes.push_back(in.U32());
      break;
    case kMeter:
      in.Skip(8);
      break;
    case kModel:
    case kTextPane: {
      const uint32_t meshes = in.U32();
      if (!in.Has(size_t(meshes) * 4)) {
        break;
      }
      for (uint32_t i = 0; i < meshes; ++i) {
        w.meshes.push_back(in.U32());
      }
      if (w.type == kModel) {
        in.U32();
      } else {
        // Box (2 floats), offset (3), font name, wrap and horizontal flags,
        // then the horizontal and vertical justification.
        in.Skip(20);
        in.Skip(size_t(in.U32()) + 2);
        const uint32_t justify[2] = {in.U32(), in.U32()};
        if (in.ok) {
          std::memcpy(w.textJustify, justify, sizeof(justify));
        }
        in.ok = true;
        fixed = false;
      }
      break;
    }
    default:
      fixed = false;
      break;
    }
    if (!in.ok) {
      break;
    }
    if (k + 1 < count) {
      if (!fixed) {
        while (in.at < size && !LooksLikeWidget(data, size, in.at, w.index + 1)) {
          ++in.at;
        }
      }
      if (!LooksLikeWidget(data, size, in.at, w.index + 1)) {
        error = "lost after the widget " + w.name;
        return false;
      }
    }
    out.push_back(std::move(w));
  }
  if (!in.ok || out.empty()) {
    error = "the frame is cut short";
    return false;
  }
  return true;
}

// --- Meshes ----------------------------------------------------------------------

// One mesh of the frame's model, on its own vertices, in the disc's axes.
struct Part {
  std::vector<std::array<float, 3>> positions;
  std::vector<std::array<float, 2>> uvs;
  std::vector<uint32_t> indices;
  const ModelMaterial* material = nullptr;
  uint32_t slot = 0;  // which of the model's textures it draws with
  bool glow = false;  // takes the glow sum T + T*c (HudGlow)
  uint32_t sample = 0;  // kStateFlag_PortHud* bits (SampleFlags)
};

bool MeshPart(const Model& model, uint32_t mesh, Part& out) {
  if (mesh >= model.meshes.size()) {
    return false;
  }
  const ModelMesh& m = model.meshes[mesh];
  if (m.vertexBuffer >= model.vertexBuffers.size() || m.material >= model.materials.size() || m.indices.empty() ||
      m.indices.size() % 3 != 0) {
    return false;
  }
  const ModelVertexBuffer& vb = model.vertexBuffers[m.vertexBuffer];
  if (vb.positions.size() != size_t(vb.vertexCount) * 3 || vb.uvs.empty() ||
      vb.uvs[0].size() != size_t(vb.vertexCount) * 2) {
    return false;
  }
  std::vector<uint32_t> used(m.indices);
  std::sort(used.begin(), used.end());
  used.erase(std::unique(used.begin(), used.end()), used.end());
  if (used.back() >= vb.vertexCount) {
    return false;
  }
  out = {};
  out.material = &model.materials[m.material];
  for (uint32_t v : used) {
    std::array<float, 3> p;
    ToDisc(&vb.positions[size_t(v) * 3], p.data());
    out.positions.push_back(p);
    out.uvs.push_back({vb.uvs[0][size_t(v) * 2], vb.uvs[0][size_t(v) * 2 + 1]});
  }
  for (uint32_t v : m.indices) {
    out.indices.push_back(uint32_t(std::lower_bound(used.begin(), used.end(), v) - used.begin()));
  }
  return true;
}

// The picture a HUD material shows: its base colour map, or failing that its
// diffuse one. In a pak's byte order.
bool MaterialTexture(const ModelMaterial& material, ModelUuid& out) {
  for (uint32_t usage : {Tag('B', 'C', 'L', 'R'), Tag('D', 'I', 'F', 'T')}) {
    for (const ModelMaterialData& d : material.data) {
      if (d.kind != ModelMaterialData::Kind::Texture || d.usage != usage || d.texture.id == ModelUuid{}) {
        continue;
      }
      out = d.texture.id;
      std::swap(out[0], out[3]);
      std::swap(out[1], out[2]);
      std::swap(out[4], out[5]);
      std::swap(out[6], out[7]);
      return true;
    }
  }
  return false;
}

uint32_t ShaderOf(const ModelMaterial& material) {
  uint8_t sid[4];
  std::memcpy(sid, &material.shaderId, 4);
  return uint32_t(sid[0]) << 24 | uint32_t(sid[1]) << 16 | uint32_t(sid[2]) << 8 | sid[3];
}

// Whether a material's shader writes its base map's sampled alpha squared: ca1106b0 (the
// unlit alpha-blended image, a = BCLR.a^2 * DIFC.a, DIFC 1 on all 57), 3853595c (filterlight /
// lightglow: a = DIFT.a^2 * v1.a * DIFC.a, and it adds ICAN * ICNC to its colour, unscaled by
// the widget colour v1) and 2d606234 (the GUI frames' unlit vertex-colour texture: the same
// alpha in every perm). The picture keeps its raw alpha; the draw squares the filtered sample
// (kStateFlag_PortHudSquare, as the shader does).
bool SquaresAlpha(const ModelMaterial& material) {
  const uint32_t shader = ShaderOf(material);
  return (shader == 0xCA1106B0u && (material.unk1 & 1) != 0) || shader == 0x3853595Cu || shader == 0x2D606234u;
}

// ad2c208c (UI_Interference): the picture, with the static of Remastered's HUD fade-in while
// DYIN is set (kStateFlag_PortHudInterference). `rows` is CCH5.x, the picture's height in
// rows; its index in kHudInterferenceRows goes into the material word. False when the
// material is another shader, or has a CCH5 the draw doesn't know.
bool InterferenceRows(const ModelMaterial& material, uint32_t& index) {
  if (ShaderOf(material) != 0xAD2C208Cu) {
    return false;
  }
  for (const ModelMaterialData& d : material.data) {
    if (d.usage == Tag('C', 'C', 'H', '5') && d.kind == ModelMaterialData::Kind::Color) {
      for (uint32_t i = 0; i < std::size(kHudInterferenceRows); ++i) {
        if (std::fabs(d.color[0] - kHudInterferenceRows[i]) < 0.01f) {
          index = i;
          return true;
        }
      }
    }
  }
  return false;
}

// The state flags of a part's material word for its sampling (kStateFlag_PortHud*).
uint32_t SampleFlags(const ModelMaterial& material) {
  uint32_t index = 0;
  if (SquaresAlpha(material)) {
    return 1u << 16;
  }
  if (InterferenceRows(material, index)) {
    return 1u << 17 | index << 18;
  }
  return 0;
}

// Whether a mesh takes the glow sum (kStateFlag_PortHudGlow: TEV T + T*c, see the draw code):
// 3853595c with ICNC white, as on all of FRME_Helmet's. Another ICNC isn't supported (the TEV
// has no constant for it), so such a mesh keeps the plain product.
bool HudGlow(const ModelMaterial& material) {
  if (ShaderOf(material) != 0x3853595Cu) {
    return false;
  }
  const ModelMaterialData* icnc = nullptr;
  for (const ModelMaterialData& d : material.data) {
    if (d.usage == Tag('I', 'C', 'N', 'C') && d.kind == ModelMaterialData::Kind::Color) {
      icnc = &d;
    }
  }
  return icnc != nullptr && icnc->color[0] == 1.f && icnc->color[1] == 1.f && icnc->color[2] == 1.f;
}

// A bar's mesh as the strip the game fills (port_hud_bars.h). The mesh is a
// ribbon: each of its two edges holds one value of one texture coordinate, and
// the other coordinate runs along it from the empty end to the full one.
bool Stations(const Part& part, PortHudBars::Bar& bar) {
  const size_t count = part.positions.size();
  if (count < 4) {
    return false;
  }
  std::set<long long> values[2];
  double lo[3], hi[3], mean[2] = {0.0, 0.0};
  for (int c = 0; c < 3; ++c) {
    lo[c] = hi[c] = part.positions[0][c];
  }
  for (size_t i = 0; i < count; ++i) {
    for (int c = 0; c < 2; ++c) {
      values[c].insert(std::llrint(double(part.uvs[i][c]) * 1000.0));
      mean[c] += double(part.uvs[i][c]) / double(count);
    }
    for (int c = 0; c < 3; ++c) {
      lo[c] = std::min(lo[c], double(part.positions[i][c]));
      hi[c] = std::max(hi[c], double(part.positions[i][c]));
    }
  }
  const int across = values[0].size() <= values[1].size() ? 0 : 1;
  if (values[across].size() != 2) {
    return false;
  }
  int along = 0;
  for (int c = 1; c < 3; ++c) {
    if (hi[c] - lo[c] > hi[along] - lo[along]) {
      along = c;
    }
  }
  using Row = std::array<double, 5>;  // position, then texture coordinate
  std::vector<Row> edges[2];
  for (int side = 0; side < 2; ++side) {
    std::set<Row> rows;
    for (size_t i = 0; i < count; ++i) {
      if ((double(part.uvs[i][across]) > mean[across]) != (side == 1)) {
        continue;
      }
      Row row;
      for (int c = 0; c < 3; ++c) {
        row[c] = std::nearbyint(double(part.positions[i][c]) * 1e5) / 1e5;
      }
      for (int c = 0; c < 2; ++c) {
        row[3 + c] = std::nearbyint(double(part.uvs[i][c]) * 1e5) / 1e5;
      }
      rows.insert(row);
    }
    edges[side].assign(rows.begin(), rows.end());
    std::stable_sort(edges[side].begin(), edges[side].end(), [&](const Row& a, const Row& b) {
      return a[along] != b[along] ? a[along] < b[along] : a[4 - across] < b[4 - across];
    });
  }
  if (edges[0].size() != edges[1].size() || edges[0].size() < 2) {
    return false;
  }
  bar.stations.clear();
  for (size_t i = 0; i < edges[0].size(); ++i) {
    PortHudBars::Station s;
    for (int c = 0; c < 3; ++c) {
      s.a[c] = float(edges[0][i][c]);
      s.b[c] = float(edges[1][i][c]);
    }
    for (int c = 0; c < 2; ++c) {
      s.uvA[c] = float(edges[0][i][3 + c]);
      s.uvB[c] = float(edges[1][i][3 + c]);
    }
    bar.stations.push_back(s);
  }
  return true;
}

// A disc model of `parts`, one surface and one material each: `material` with
// its texture slot set, unlit.
bool BuildModel(const std::vector<Part>& parts, const std::vector<uint32_t>& textures, const Blob& material,
                Blob& out) {
  size_t vertices = 0;
  for (const Part& part : parts) {
    vertices += part.positions.size();
    if (part.indices.size() >= 0xFFFF) {
      return false;
    }
  }
  if (vertices >= 0xFFFF) {
    return false;  // the display list's indices are 16 bit
  }
  std::vector<Blob> sections;
  Blob set;
  AppendBE32(set, uint32_t(textures.size()));
  for (uint32_t id : textures) {
    AppendBE32(set, id);
  }
  AppendBE32(set, uint32_t(parts.size()));
  for (size_t i = 0; i < parts.size(); ++i) {
    AppendBE32(set, uint32_t((i + 1) * material.size()));
  }
  for (const Part& part : parts) {
    const size_t at = set.size();
    set.insert(set.end(), material.begin(), material.end());
    SetBE32(set, at + 8, part.slot);
    SetBE32(set, at + 0x1c, 0x3000);  // both colour channels unlit
    if (part.glow) {
      SetBE32(set, at, GetBE32(set, at) | 0x8000);  // kStateFlag_PortHudGlow
    }
    if (part.sample != 0) {
      SetBE32(set, at, GetBE32(set, at) | part.sample);  // kStateFlag_PortHudSquare / Interference
    }
  }
  sections.push_back(std::move(set));

  Blob positions, uvs;
  float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
  bool first = true;
  for (const Part& part : parts) {
    for (size_t i = 0; i < part.positions.size(); ++i) {
      for (int c = 0; c < 3; ++c) {
        const float v = part.positions[i][c];
        AppendBEFloat(positions, v);
        lo[c] = first ? v : std::min(lo[c], v);
        hi[c] = first ? v : std::max(hi[c], v);
      }
      first = false;
      AppendBEFloat(uvs, part.uvs[i][0]);
      AppendBEFloat(uvs, part.uvs[i][1]);
    }
  }
  sections.push_back(std::move(positions));
  Blob normals;  // one, facing the camera; nothing is lit by it
  AppendBE16(normals, 0);
  AppendBE16(normals, uint16_t(-0x4000));
  AppendBE16(normals, 0);
  sections.push_back(std::move(normals));
  sections.push_back(Blob(32, 0));  // colours
  sections.push_back(std::move(uvs));
  sections.push_back(Blob());  // lightmap coordinates

  std::vector<Blob> surfaces;
  uint32_t base = 0;
  for (size_t k = 0; k < parts.size(); ++k) {
    const Part& part = parts[k];
    Blob list;
    list.push_back(0x91);  // triangles, 16-bit position / normal / texture coordinate indices
    AppendBE16(list, uint16_t(part.indices.size()));
    for (uint32_t index : part.indices) {
      AppendBE16(list, uint16_t(base + index));
      AppendBE16(list, 0);
      AppendBE16(list, uint16_t(base + index));
    }
    Pad32(list);
    base += uint32_t(part.positions.size());
    double centre[3] = {0.0, 0.0, 0.0};
    for (const auto& p : part.positions) {
      for (int c = 0; c < 3; ++c) {
        centre[c] += double(p[c]) / double(part.positions.size());
      }
    }
    Blob surface;
    for (double c : centre) {
      AppendBEFloat(surface, float(c));
    }
    AppendBE32(surface, uint32_t(k));
    AppendBE32(surface, uint32_t(list.size()) | 0x80000000u);
    AppendBE32(surface, 0);
    AppendBE32(surface, 0);
    AppendBE32(surface, 0);
    AppendBEFloat(surface, 0.0f);
    AppendBEFloat(surface, -1.0f);
    AppendBEFloat(surface, 0.0f);
    Pad32(surface);
    surface.insert(surface.end(), list.begin(), list.end());
    surfaces.push_back(std::move(surface));
  }
  Blob ends;
  AppendBE32(ends, uint32_t(surfaces.size()));
  uint32_t end = 0;
  for (const Blob& surface : surfaces) {
    end += uint32_t(surface.size());
    AppendBE32(ends, end);
  }
  sections.push_back(std::move(ends));
  for (Blob& surface : surfaces) {
    sections.push_back(std::move(surface));
  }

  out.clear();
  AppendBE32(out, 0xDEADBABE);
  AppendBE32(out, 2);
  AppendBE32(out, 6);  // 16-bit normals, 16-bit texture coordinates off
  for (float v : lo) {
    AppendBEFloat(out, v);
  }
  for (float v : hi) {
    AppendBEFloat(out, v);
  }
  AppendBE32(out, uint32_t(sections.size()));
  AppendBE32(out, 1);
  for (Blob& section : sections) {
    Pad32(section);
    AppendBE32(out, uint32_t(section.size()));
  }
  Pad32(out);
  for (const Blob& section : sections) {
    out.insert(out.end(), section.begin(), section.end());
  }
  return true;
}

// The fourth material of the template model.
bool TemplateMaterial(const Blob& model, Blob& out) {
  Reader in{model.data(), model.size(), 0x24, true};
  const uint32_t sections = in.U32();
  in.at = (0x2c + size_t(sections) * 4 + 31) & ~size_t(31);
  const uint32_t textures = in.U32();
  in.Skip(size_t(textures) * 4);
  const uint32_t materials = in.U32();
  if (!in.ok || materials <= kTemplateMaterial || !in.Has(size_t(materials) * 4)) {
    return false;
  }
  std::vector<uint32_t> ends;
  for (uint32_t i = 0; i < materials; ++i) {
    ends.push_back(in.U32());
  }
  const uint32_t from = ends[kTemplateMaterial - 1], to = ends[kTemplateMaterial];
  if (to < from + 0x20 || !in.Has(to)) {
    return false;
  }
  out.assign(model.begin() + long(in.at + from), model.begin() + long(in.at + to));
  return true;
}

}  // namespace

const std::vector<HudFrame>& HudFrames() {
  static const std::vector<HudFrame> frames = {
      {"FRME_CombatHud", 0xB10E1DCD}, {"FRME_ScanHud", 0xE47CD0DC}, {"FRME_ThermalHud", 0x143ACA19},
      {"FRME_XRayHudNew", 0x6493BB4F}, {"FRME_BallHud", 0xBF687554}, {"FRME_BaseHud", 0x2F972D0C},
      {"FRME_MapScreen", 0x97FF54DE}, {"FRME_PauseScreen", 0x6352720C},
      {"FRME_PauseScreenInstructions", 0xB101FA1D}, {"FRME_GenericMenu", 0x7F85F25F},
      {"FRME_MsgScreen", 0xAF0D63E8}, {"FRME_QuitScreen", 0x920276D2}, {"FRME_ScanHudFlat", 0xF176A9D5},
      {"FRME_Helmet", 0xB7A308BD},
  };
  return frames;
}

bool HudFrameModel(const uint8_t* guif, size_t size, ModelUuid& model) {
  if (!IsFrame(guif, size)) {
    return false;
  }
  // Stored with its first three groups little endian; a pak has them the other way round.
  static const uint8_t kOrder[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
  for (size_t i = 0; i < 16; ++i) {
    model[i] = guif[kFrameHeader + kOrder[i]];
  }
  return true;
}

// TweakGuiColorsMP1 is an RFRM/LDTA file: a list of (hash, size, body)
// properties, a colour being a list of (component hash, 4, float). A component
// it leaves out stays at the type's default of 1. Remastered's beam menu clamps
// each beam icon's colour and multiplies it by that beam's colour here
// (CHudVisorBeamMenuMP1::Update, Color Assist off); the visor menu, lozenges
// and ghost are left alone.
bool HudBeamIconTints(const uint8_t* ldta, size_t size, std::map<std::string, std::array<float, 4>>& out,
                      std::string& error) {
  auto u16 = [&](size_t at) { return uint32_t(ldta[at]) | uint32_t(ldta[at + 1]) << 8; };
  auto u32 = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };
  constexpr size_t kRoot = 0x38;
  if (size < kRoot + 2 || std::memcmp(ldta, "RFRM", 4) != 0 || std::memcmp(ldta + 0x14, "LDTA", 4) != 0 ||
      std::memcmp(ldta + 0x20, "LDCH", 4) != 0) {
    error = "not a tweak file";
    return false;
  }
  // Widget i of the menu is beam 3 - i (CHudVisorBeamMenu's "3210").
  static const std::pair<uint32_t, const char*> kBeams[] = {
      {0x2584A7DF, "model_beamicon3"},  // Power
      {0xE8DF071A, "model_beamicon2"},  // Ice
      {0x735A17B9, "model_beamicon1"},  // Wave
      {0xB7B9CFBC, "model_beamicon0"},  // Plasma
  };
  static const uint32_t kComponents[4] = {0x110889D1, 0x8A7AFF22, 0x2A5349E9, 0xE364C93A};  // R G B A
  const uint32_t count = u16(kRoot);
  size_t at = kRoot + 2;
  for (uint32_t i = 0; i < count; ++i) {
    if (at + 6 > size) {
      break;
    }
    const uint32_t hash = u32(at);
    const size_t length = u16(at + 4);
    at += 6;
    if (at + length > size) {
      break;
    }
    const auto beam = std::find_if(std::begin(kBeams), std::end(kBeams), [&](const auto& b) { return b.first == hash; });
    if (beam != std::end(kBeams) && length >= 2) {
      std::array<float, 4> tint{1.f, 1.f, 1.f, 1.f};
      const size_t end = at + length;
      size_t c = at + 2;
      for (uint32_t k = u16(at); k > 0 && c + 6 <= end; --k) {
        const uint32_t component = u32(c);
        const size_t clen = u16(c + 4);
        c += 6;
        if (clen == 4 && c + 4 <= end) {
          const auto slot = std::find(std::begin(kComponents), std::end(kComponents), component);
          if (slot != std::end(kComponents)) {
            float v;
            const uint32_t bits = u32(c);
            std::memcpy(&v, &bits, 4);
            tint[size_t(slot - std::begin(kComponents))] = std::isfinite(v) ? std::clamp(v, 0.f, 1.f) : 1.f;
          }
        }
        c += clen;
      }
      out[beam->second] = tint;
    }
    at += length;
  }
  if (out.empty()) {
    error = "no beam colours";
    return false;
  }
  return true;
}

HudConverter::HudConverter(ConvertIO io)
: m_io(std::move(io)), m_nextModel(kModelIds), m_nextTexture(kTextureIds) {}

uint32_t HudConverter::NewId(uint32_t& next) {
  while (m_io.retailId && m_io.retailId(next)) {
    ++next;
  }
  return next++;
}

bool HudConverter::LoadMaterial(std::string& error) {
  for (const uint32_t id : kTemplateModels) {
    Blob templateModel;
    if (m_material.empty() && m_io.retail && m_io.retail(Tag('C', 'M', 'D', 'L'), id, templateModel)) {
      TemplateMaterial(templateModel, m_material);
    }
  }
  if (m_material.empty()) {
    error = "none of the disc's template models " + Hex8(kTemplateModels[0]) + ", " + Hex8(kTemplateModels[1]) +
            " is usable";
    return false;
  }
  return true;
}

std::optional<uint32_t> HudConverter::Texture(const ModelUuid& id, HudCounts& counts, const std::string& owner,
                                              const Tint& tint, bool squareAlpha) {
  const std::tuple<ModelUuid, Tint, bool> key{id, tint, squareAlpha};
  const auto known = m_textures.find(key);
  if (known != m_textures.end()) {
    return known->second == 0 ? std::nullopt : std::optional<uint32_t>(known->second);
  }
  m_textures[key] = 0;
  Image image;
  std::string textureError;
  if (!m_io.texture || !m_io.texture(id, image, textureError) || image.width <= 0 || image.height <= 0) {
    if (m_io.log) {
      m_io.log(owner + ": texture " + IdToString(id) + ": " + textureError);
    }
    return std::nullopt;
  }
  if (tint != Tint{1.f, 1.f, 1.f, 1.f}) {
    // The draw squares the sampled alpha (squareAlpha), so the tint's alpha goes in as its
    // square root: (a * sqrt(t))^2 = a^2 * t, as the shader's a^2 * v1.a.
    for (size_t i = 0; i < image.rgba.size(); ++i) {
      const float factor = std::clamp(tint[i % 4], 0.f, 1.f);
      image.rgba[i] = uint8_t(std::lround(image.rgba[i] * (squareAlpha && i % 4 == 3 ? std::sqrt(factor) : factor)));
    }
  }
  const int w = std::clamp(NextPow2(image.width), 8, kNativeSize);
  const int h = std::clamp(NextPow2(image.height), 8, kNativeSize);
  if (w != image.width || h != image.height) {
    image = Resize(image, w, h, MapKind::Colour);
  }
  const int edge = std::max(w, h);
  const int sw = edge <= kStubSize ? w : std::max(8, w * kStubSize / edge);
  const int sh = edge <= kStubSize ? h : std::max(8, h * kStubSize / edge);
  const uint32_t tid = NewId(m_nextTexture);
  if ((edge > kStubSize && !m_io.write(Hex8(tid) + ".dds", EncodeDds(image, ColourDdsFormat(), false, MapKind::Colour))) ||
      !m_io.write(Hex8(tid) + ".TXTR", EncodeTxtrRgba8(edge > kStubSize ? Resize(image, sw, sh, MapKind::Colour) : image, 8, MapKind::Colour))) {
    if (m_io.log) {
      m_io.log(owner + ": texture " + IdToString(id) + ": cannot write");
    }
    return std::nullopt;
  }
  m_textures[key] = tid;
  ++counts.textures;
  return tid;
}

bool HudConverter::ConvertModel(const Model& model, uint32_t id, HudCounts& counts, std::string& error) {
  if (!m_io.write || !LoadMaterial(error)) {
    if (error.empty()) {
      error = "nowhere to write";
    }
    return false;
  }
  // Each level of detail is a set of meshes of its own; keep the finest's.
  std::vector<bool> finest(model.meshes.size(), false);
  bool anyFinest = false;
  for (size_t r = 0; r < 5 && r < model.lods.size(); ++r) {
    const ModelLod& range = model.lods[r];
    for (uint64_t i = range.indexOffset; i < uint64_t(range.indexOffset) + range.indexCount; ++i) {
      if (i < model.lodMeshes.size() && model.lodMeshes[i] < finest.size()) {
        finest[model.lodMeshes[i]] = true;
        anyFinest = true;
      }
    }
  }
  std::vector<Part> parts;
  std::vector<uint32_t> textures;
  for (uint32_t mesh = 0; mesh < model.meshes.size(); ++mesh) {
    if (anyFinest && !finest[mesh]) {
      continue;
    }
    Part part;
    ModelUuid picture;
    if (!MeshPart(model, mesh, part) || !MaterialTexture(*part.material, picture)) {
      continue;
    }
    const std::optional<uint32_t> tid = Texture(picture, counts, Hex8(id), {1.f, 1.f, 1.f, 1.f}, SquaresAlpha(*part.material));
    if (!tid) {
      continue;
    }
    const auto slot = std::find(textures.begin(), textures.end(), *tid);
    part.slot = uint32_t(slot - textures.begin());
    part.glow = HudGlow(*part.material);
    part.sample = SampleFlags(*part.material);
    if (slot == textures.end()) {
      textures.push_back(*tid);
    }
    parts.push_back(std::move(part));
  }
  Blob file;
  if (parts.empty()) {
    error = "no mesh with a picture";
    return false;
  }
  if (!BuildModel(parts, textures, m_material, file)) {
    error = "too many vertices";
    return false;
  }
  if (!m_io.write(Hex8(id) + ".CMDL", file)) {
    error = "cannot write a model";
    return false;
  }
  ++counts.models;
  return true;
}

bool HudConverter::Convert(uint32_t retailFrame, const uint8_t* guif, size_t size, const Model& model,
                           HudCounts& counts, std::string& error) {
  auto log = [&](const std::string& line) {
    if (m_io.log) {
      m_io.log(Hex8(retailFrame) + ": " + line);
    }
  };
  Blob retail;
  if (!m_io.retail || !m_io.write || !m_io.retail(Tag('F', 'R', 'M', 'E'), retailFrame, retail)) {
    error = "the disc has no frame " + Hex8(retailFrame);
    return false;
  }
  uint32_t header[4];
  std::vector<Widget> disc;
  std::vector<RemWidget> rem;
  if (!ParseFrame(retail, header, disc, error) || !ParseRemFrame(guif, size, rem, error)) {
    return false;
  }
  if (!LoadMaterial(error)) {
    return false;
  }

  // Where each widget is in its frame, with all its parents applied. A name
  // used twice means its last widget, as it does to the game's lookup.
  std::unordered_map<uint32_t, size_t> remByIndex;
  std::unordered_map<std::string, size_t> remByName;
  for (size_t i = 0; i < rem.size(); ++i) {
    remByIndex[rem[i].index] = i;
    remByName[Lower(rem[i].name)] = i;
  }
  for (RemWidget& w : rem) {  // parents come first: a header's parent is a lower index
    const auto parent = remByIndex.find(w.parent);
    const bool root = w.parent == 0 || parent == remByIndex.end() || !rem[parent->second].placed;
    w.world = root ? w.local : Mul(rem[parent->second].world, w.local);
    w.placed = true;
  }
  auto remWorld = [&](size_t i) { return ToDisc(rem[i].world); };
  std::unordered_map<std::string, size_t> discByName;
  for (size_t i = 0; i < disc.size(); ++i) {
    discByName[disc[i].name] = i;
  }
  for (size_t pass = 0; pass < disc.size(); ++pass) {  // a parent may come after its child
    bool waiting = false;
    for (Widget& w : disc) {
      if (w.placed) {
        continue;
      }
      const auto parent = discByName.find(w.parent);
      if (parent == discByName.end() || &disc[parent->second] == &w) {
        w.world = w.local;
        w.placed = true;
      } else if (disc[parent->second].placed) {
        w.world = Mul(disc[parent->second].world, w.local);
        w.placed = true;
      } else {
        waiting = true;
      }
    }
    if (!waiting) {
      break;
    }
  }
  const std::vector<Widget> original = disc;

  // The disc's widgets, moved to where Remastered has them.
  std::unordered_map<std::string, std::string> newName;  // Remastered's name, lower case -> the output's
  int camera = -1;
  for (size_t i = 0; i < rem.size() && camera < 0; ++i) {
    if (rem[i].type == kCamera) {
      camera = int(i);
    }
  }
  // The map screen's panes slide off screen by distances in the game's code,
  // measured on the plane the disc's frame is drawn on. Remastered sets some
  // widgets far behind that plane, so they are brought onto it, along the
  // line of sight: the picture is the same and a slide moves it as far as
  // its neighbours. The disc's own widgets keep their place on screen under
  // the new camera.
  const bool map = retailFrame == kMapScreen && camera >= 0;
  double keep = 1.0;
  if (map) {
    const Mat eye = remWorld(size_t(camera));
    const RemWidget& g = rem[size_t(camera)];
    for (const Widget& w : disc) {
      if (w.type == Tag('C', 'A', 'M', 'R') && w.typeData.size() >= 8 && GetBE32(w.typeData, 0) == 0 &&
          g.projection == 0) {
        const double was = -w.world.m[1][3] * std::tan(double(GetBEFloat(w.typeData, 4)) * kPi / 360.0);
        const double now = -eye.m[1][3] * std::tan(double(g.camera[0]) * kPi / 360.0);
        if (was > 1e-6 && now > 1e-6) {
          keep = now / was;
        }
      }
    }
  }
  // The height of the view, in world units, at a point.
  auto viewHeight = [](const Mat& eye, double fov, const double* at) {
    const double len = AxisLength(eye, 1);
    double depth = 0.0;
    for (int c = 0; c < 3; ++c) {
      depth += (at[c] - eye.m[c][3]) * eye.m[c][1] / len;
    }
    return depth > 1e-9 ? 2.0 * depth * std::tan(fov * kPi / 360.0) : 0.0;
  };
  int discCamera = -1;
  for (size_t i = 0; i < original.size() && discCamera < 0; ++i) {
    if (original[i].type == Tag('C', 'A', 'M', 'R') && original[i].typeData.size() >= 20) {
      discCamera = int(i);
    }
  }
  auto discView = [&](const double* at) {
    if (discCamera < 0) {
      return 0.0;
    }
    const Widget& c = original[size_t(discCamera)];
    if (GetBE32(c.typeData, 0) == 0) {
      return viewHeight(c.world, GetBEFloat(c.typeData, 4), at);
    }
    return std::abs(double(GetBEFloat(c.typeData, 12)) - GetBEFloat(c.typeData, 16));
  };
  auto remView = [&](const Mat& m) {
    if (camera < 0) {
      return 0.0;
    }
    const RemWidget& c = rem[size_t(camera)];
    if (c.projection != 0) {
      return std::abs(double(c.camera[2]) - c.camera[3]);
    }
    const double at[3] = {m.m[0][3], m.m[1][3], m.m[2][3]};
    return viewHeight(remWorld(size_t(camera)), c.camera[0], at);
  };
  auto meshBox = [&](size_t i, double* lo, double* hi) {
    Part part;
    if (rem[i].meshes.empty() || !MeshPart(model, rem[i].meshes[0], part) || part.positions.empty()) {
      return false;
    }
    for (int c = 0; c < 3; ++c) {
      lo[c] = hi[c] = part.positions[0][size_t(c)];
      for (const auto& p : part.positions) {
        lo[c] = std::min(lo[c], double(p[size_t(c)]));
        hi[c] = std::max(hi[c], double(p[size_t(c)]));
      }
    }
    return true;
  };
  auto place = [&](size_t i) {
    Mat w = remWorld(i);
    if (!map) {
      return w;
    }
    const Mat eye = remWorld(size_t(camera));
    // A model is where its mesh is, which may be far from its origin.
    double at[3] = {0.0, 0.0, 0.0};
    double lo[3], hi[3];
    if (rem[i].type == kModel && meshBox(i, lo, hi)) {
      for (int c = 0; c < 3; ++c) {
        at[c] = 0.5 * (lo[c] + hi[c]);
      }
    }
    const double depth = w.m[1][0] * at[0] + w.m[1][1] * at[1] + w.m[1][2] * at[2] + w.m[1][3] - eye.m[1][3];
    // A panel goes a little behind the text it is a ground for: a frame draws
    // back to front.
    const double plane = -eye.m[1][3] + (rem[i].type == kModel ? 1.0 : 0.0);
    if (depth <= 1e-6 || plane <= 1e-6) {
      return w;
    }
    const double t = plane / depth;
    Mat s = Identity();
    for (int c = 0; c < 3; ++c) {
      s.m[c][c] = t;
      s.m[c][3] = eye.m[c][3] * (1.0 - t);
    }
    return Mul(s, w);
  };
  std::vector<Widget> out;
  std::optional<double> promptLeft;
  for (Widget w : disc) {
    std::string name = Lower(w.name);
    for (const auto& alias : kAlias) {
      if (name == alias[0]) {
        name = alias[1];
      }
    }
    auto found = remByName.find(name);
    if (map && found != remByName.end() &&
        std::any_of(std::begin(kMapDiscOnly), std::end(kMapDiscOnly), [&](const char* n) { return name == n; })) {
      newName[Lower(rem[found->second].name)] = w.name;
      found = remByName.end();
    }
    if (map && found != remByName.end() && name == kMapPrompt && w.typeData.size() >= kTextPaneSize) {
      // The legend's prompt: the disc's line of text, which is wider than
      // the box Remastered has for its own, set from that box's left edge.
      Mat s = Identity();
      s.m[0][0] = s.m[2][2] = keep;
      w.world = Mul(s, w.world);
      const Mat there = place(found->second);
      double lo[3], hi[3];
      if (meshBox(found->second, lo, hi)) {
        const double half = 0.5 * double(GetBEFloat(w.typeData, 0)) * AxisLength(w.world, 0);
        const double halfThere = 0.5 * (hi[0] - lo[0]) * AxisLength(there, 0);
        double from[3], to[3];
        for (int c = 0; c < 3; ++c) {
          from[c] = w.world.m[c][3];
          to[c] = there.m[c][3];
          for (int k = 0; k < 3; ++k) {
            from[c] += w.world.m[c][k] * double(GetBEFloat(w.typeData, 8 + size_t(k) * 4));
            to[c] += there.m[c][k] * 0.5 * (lo[k] + hi[k]);
          }
        }
        double left = to[0] - halfThere;
        // No further right than the legend above it: the disc's line is
        // longer than Remastered's and would run under the Exit prompt.
        const auto legend = remByName.find("textpane_maplegend");
        double llo[3], lhi[3];
        if (legend != remByName.end() && meshBox(legend->second, llo, lhi)) {
          const Mat lm = place(legend->second);
          double centre = lm.m[0][3];
          for (int k = 0; k < 3; ++k) {
            centre += lm.m[0][k] * 0.5 * (llo[k] + lhi[k]);
          }
          left = std::min(left, centre - 0.5 * (lhi[0] - llo[0]) * AxisLength(lm, 0));
        }
        w.world.m[0][3] += left - (from[0] - half);
        promptLeft = left;
        // From the left, where the disc centres the line on its box and lets
        // it run over both edges; the box grows to the right to hold it.
        SetBE32(w.typeData, kTextPaneJustify, 0);
        const float wide = GetBEFloat(w.typeData, 0);
        SetBEFloat(w.typeData, 0, wide * 1.5f);
        SetBEFloat(w.typeData, 8, GetBEFloat(w.typeData, 8) + wide * 0.25f);
        SetBEFloat(w.typeData, kTextPaneExtent, std::nearbyint(GetBEFloat(w.typeData, kTextPaneExtent) * 1.5f));
        w.world.m[1][3] += to[1] - from[1];
        w.world.m[2][3] += to[2] - from[2];
      }
      newName[Lower(rem[found->second].name)] = w.name;
      found = remByName.end();
    } else if (found == remByName.end() && map &&
               (w.type == Tag('T', 'X', 'P', 'N') || (w.type == Tag('M', 'O', 'D', 'L') && name == kMapPromptBox))) {
      Mat s = Identity();
      s.m[0][0] = s.m[2][2] = keep;
      w.world = Mul(s, w.world);
      w.kept = w.type == Tag('M', 'O', 'D', 'L');
      w.row = true;
    }
    if (found != remByName.end()) {
      w.guif = int(found->second);
      newName[Lower(rem[found->second].name)] = w.name;
      switch (w.type) {
      case Tag('B', 'W', 'I', 'G'):
      case Tag('E', 'N', 'R', 'G'):
      case Tag('G', 'R', 'U', 'P'):
      case Tag('M', 'E', 'T', 'R'):
      case Tag('M', 'O', 'D', 'L'):
      case Tag('T', 'X', 'P', 'N'):
        w.world = place(found->second);
        break;
      default:
        break;
      }
    }
    for (const auto& stand : kStandIn) {
      const auto pane = remByName.find(stand[1]);
      Blob cmdl;
      double lo[3], hi[3];
      if (found != remByName.end() || name != stand[0] || pane == remByName.end() ||
          w.type != Tag('M', 'O', 'D', 'L') || w.typeData.size() < 4 ||
          !m_io.retail(Tag('C', 'M', 'D', 'L'), GetBE32(w.typeData, 0), cmdl) || cmdl.size() < 36 ||
          !meshBox(pane->second, lo, hi)) {
        continue;
      }
      // The model's middle (its bounding box follows magic, version and
      // flags) goes to the middle of the pane's box, and keeps the share of
      // the view's height it had on the disc.
      const Mat there = remWorld(pane->second);
      double mid[3], from[3], to[3];
      for (int c = 0; c < 3; ++c) {
        mid[c] = 0.5 * (double(GetBEFloat(cmdl, 12 + size_t(c) * 4)) + GetBEFloat(cmdl, 24 + size_t(c) * 4));
      }
      for (int c = 0; c < 3; ++c) {
        from[c] = w.world.m[c][3];
        to[c] = there.m[c][3];
        for (int k = 0; k < 3; ++k) {
          from[c] += w.world.m[c][k] * mid[k];
          to[c] += there.m[c][k] * 0.5 * (lo[k] + hi[k]);
        }
      }
      Mat at = Identity();
      for (int c = 0; c < 3; ++c) {
        at.m[c][3] = to[c];
      }
      const double was = discView(from);
      const double now = remView(at);
      const double scale = was > 1e-9 && now > 1e-9 ? now / was : 1.0;
      for (int c = 0; c < 3; ++c) {
        for (int k = 0; k < 3; ++k) {
          w.world.m[c][k] *= scale;
        }
        w.world.m[c][3] = to[c];
        for (int k = 0; k < 3; ++k) {
          w.world.m[c][3] -= w.world.m[c][k] * mid[k];
        }
      }
      w.kept = true;
      newName[Lower(rem[pane->second].name)] = w.name;
    }
    if (w.type == Tag('C', 'A', 'M', 'R') && camera >= 0 && w.typeData.size() >= 20) {
      // The layout is made for Remastered's camera, a 16:9 one. The clip planes stay the disc's.
      const RemWidget& g = rem[size_t(camera)];
      const float zNear = GetBEFloat(w.typeData, w.typeData.size() - 8);
      const float zFar = GetBEFloat(w.typeData, w.typeData.size() - 4);
      w.world = remWorld(size_t(camera));
      newName[Lower(g.name)] = w.name;
      w.typeData.clear();
      AppendBE32(w.typeData, g.projection == 0 ? 0 : 1);
      if (g.projection == 0) {
        AppendBEFloat(w.typeData, g.camera[0]);
        AppendBEFloat(w.typeData, g.camera[1]);
      } else {
        AppendBEFloat(w.typeData, std::min(g.camera[0], g.camera[1]));
        AppendBEFloat(w.typeData, std::max(g.camera[0], g.camera[1]));
        AppendBEFloat(w.typeData, std::max(g.camera[2], g.camera[3]));
        AppendBEFloat(w.typeData, std::min(g.camera[2], g.camera[3]));
      }
      AppendBEFloat(w.typeData, zNear);
      AppendBEFloat(w.typeData, zFar);
    }
    for (const auto& anchor : kAnchor) {
      const auto at = remByName.find(anchor[1]);
      if (w.name == anchor[0] && at != remByName.end()) {
        w.world = remWorld(at->second);
      }
    }
    out.push_back(std::move(w));
  }

  // The disc centres its row of prompts and sets the legend's above it.
  // Remastered's legend prompt is on the row's own line, at the left, so the
  // row moves right until its box is as far from the right edge as that
  // prompt is from the left one.
  if (map && promptLeft) {
    for (const Widget& w : out) {
      Blob cmdl;
      if (!w.kept || w.typeData.size() < 4 || !m_io.retail(Tag('C', 'M', 'D', 'L'), GetBE32(w.typeData, 0), cmdl) ||
          cmdl.size() < 36) {
        continue;
      }
      // A model's bounding box follows its magic, version and flags.
      double right = -1e30;
      for (int corner = 0; corner < 8; ++corner) {
        double x = w.world.m[0][3];
        for (int k = 0; k < 3; ++k) {
          x += w.world.m[0][k] * double(GetBEFloat(cmdl, 12 + size_t(k) * 4 + ((corner >> k) & 1 ? 12 : 0)));
        }
        right = std::max(right, x);
      }
      const double shift = 2.0 * remWorld(size_t(camera)).m[0][3] - *promptLeft - right;
      if (shift > 0.0) {
        for (Widget& other : out) {
          if (other.row) {
            other.world.m[0][3] += shift;
          }
        }
      }
      break;
    }
  }

  // Remastered's own widgets, each after its parent and its parent's children.
  for (const RemWidget& g : rem) {
    if (g.type == kHeadWidget && g.parent == 0) {
      newName[Lower(g.name)] = kHeadName;
    }
  }
  const Widget& pattern = disc[std::min<size_t>(6, disc.size() - 1)];
  for (size_t i = 0; i < rem.size(); ++i) {
    const RemWidget& g = rem[i];
    const std::string name = Lower(g.name);
    const auto parent = remByIndex.find(g.parent);
    if (g.parent == 0 || parent == remByIndex.end()) {
      continue;
    }
    // What a text pane holds is shown with its string, and nothing here
    // drives it. The map legend's panel is the exception: it is drawn before
    // the legend, under the pane that slides both.
    size_t over = parent->second;
    if (rem[over].type == kTextPane) {
      const auto pane = remByIndex.find(rem[over].parent);
      if (!map || g.type != kModel || pane == remByIndex.end()) {
        continue;
      }
      over = pane->second;
    }
    if (newName.count(name) != 0 || g.variant > 1 || EndsWith(name, "_jp") || EndsWith(name, "_ck") ||
        std::any_of(std::begin(kSkip), std::end(kSkip), [&](const char* skip) { return name == skip; })) {
      continue;
    }
    if (g.type != kBaseWidget && g.type != kModel) {
      log("left out " + g.name);
      continue;
    }
    if (g.type == kModel && g.color[3] == 0.0f) {
      continue;  // a damage flash: the game's code for it is not the disc's
    }
    const auto under = newName.find(Lower(rem[over].name));
    if (under == newName.end()) {
      log("no parent for " + g.name);
      continue;
    }
    newName[name] = g.name;
    Widget w;
    w.type = g.type == kBaseWidget ? Tag('B', 'W', 'I', 'G') : Tag('M', 'O', 'D', 'L');
    w.name = g.name;
    w.parent = under->second;
    w.flags[1] = g.flags[0];
    w.flags[2] = g.flags[1];
    std::memcpy(w.color, g.color, sizeof(w.color));
    w.draw = g.draw;
    w.world = place(i);
    std::memcpy(w.tail, pattern.tail, sizeof(w.tail));
    w.guif = int(i);
    w.added = true;
    if (g.type == kModel) {
      w.typeData.assign(kModelSize, 0);
    }
    size_t at = out.size();
    for (size_t k = out.size(); k-- > 0;) {
      if (out[k].name == w.parent || out[k].parent == w.parent) {
        at = k + 1;
        break;
      }
    }
    if (map && g.type == kModel && rem[over].type != kHeadWidget) {
      // A pane's panel goes behind the text of the pane.
      for (size_t k = 0; k < out.size(); ++k) {
        if (out[k].name == w.parent) {
          at = k + 1;
          break;
        }
      }
    }
    out.insert(out.begin() + long(at), std::move(w));
  }

  // Models, pictures, text boxes and bars.
  auto texture = [&](const Part& part, const std::string& widget) -> std::optional<uint32_t> {
    ModelUuid id;
    if (!MaterialTexture(*part.material, id)) {
      return std::nullopt;
    }
    const auto tint = m_tints.find(Lower(widget));
    const bool square = SquaresAlpha(*part.material);
    return tint == m_tints.end() ? Texture(id, counts, Hex8(retailFrame), {1.f, 1.f, 1.f, 1.f}, square)
                                 : Texture(id, counts, Hex8(retailFrame), tint->second, square);
  };
  PortHudBars::Bars bars;
  for (Widget& w : out) {
    if (w.type == Tag('M', 'O', 'D', 'L') && w.typeData.size() >= kModelSize) {
      if (w.kept) {
        continue;
      }
      if (w.guif < 0) {
        SetBE32(w.typeData, 0, kNoModel);  // the disc's art has no place in this layout
        continue;
      }
      const RemWidget& g = rem[size_t(w.guif)];
      std::vector<Part> parts;
      std::vector<uint32_t> textures;
      for (uint32_t mesh : g.meshes) {
        Part part;
        if (!MeshPart(model, mesh, part)) {
          log(g.name + ": mesh " + std::to_string(mesh) + " is not usable");
          continue;
        }
        const std::optional<uint32_t> tid = texture(part, w.name);
        if (!tid) {
          log(g.name + ": no picture for " + part.material->name);
          continue;
        }
        const auto slot = std::find(textures.begin(), textures.end(), *tid);
        part.slot = uint32_t(slot - textures.begin());
        const auto tint = m_tints.find(Lower(w.name));
        part.glow = HudGlow(*part.material) && (tint == m_tints.end() || tint->second == Tint{1.f, 1.f, 1.f, 1.f});
        part.sample = SampleFlags(*part.material);
        if (slot == textures.end()) {
          textures.push_back(*tid);
        }
        parts.push_back(std::move(part));
      }
      uint32_t id = kNoModel;
      Blob file;
      if (!parts.empty() && !BuildModel(parts, textures, m_material, file)) {
        log(g.name + ": too many vertices");
      } else if (!parts.empty()) {
        id = NewId(m_nextModel);
        if (!m_io.write(Hex8(id) + ".CMDL", file)) {
          error = "cannot write a model";
          return false;
        }
        ++counts.models;
      }
      SetBE32(w.typeData, 0, id);
      SetBE32(w.typeData, 4, 0);
      std::memcpy(w.color, g.color, sizeof(w.color));
      w.draw = g.draw;
    } else if (w.type == Tag('T', 'X', 'P', 'N') && w.guif >= 0 && w.typeData.size() >= kTextPaneSize) {
      // The box is Remastered's mesh for the text. The disc's text is laid out
      // in whole units of its font: the box's extent in those units gives the
      // glyphs the proportions they had, filling the box's height or, where
      // that would draw them bigger than the disc did, at the disc's size.
      const RemWidget& g = rem[size_t(w.guif)];
      Part part;
      if (g.meshes.empty() || !MeshPart(model, g.meshes[0], part)) {
        continue;
      }
      double lo[3], hi[3];
      for (int c = 0; c < 3; ++c) {
        lo[c] = hi[c] = part.positions[0][c];
      }
      for (const auto& p : part.positions) {
        for (int c = 0; c < 3; ++c) {
          lo[c] = std::min(lo[c], double(p[c]));
          hi[c] = std::max(hi[c], double(p[c]));
        }
      }
      const double dim[2] = {hi[0] - lo[0], hi[2] - lo[2]};
      const Mat& old = original[discByName[w.name]].world;
      const double extent[2] = {GetBEFloat(w.typeData, kTextPaneExtent), GetBEFloat(w.typeData, kTextPaneExtent + 4)};
      const double px = double(GetBEFloat(w.typeData, 0)) * AxisLength(old, 0) / extent[0];
      const double pz = double(GetBEFloat(w.typeData, 4)) * AxisLength(old, 2) / extent[1];
      const double pz2 = dim[1] * AxisLength(w.world, 2) / extent[1];
      const double unit = pz2 * px / pz;
      if (std::min({std::abs(pz2), std::abs(px), std::abs(pz), dim[0], dim[1]}) < 1e-9 || !std::isfinite(unit)) {
        log(w.name + ": no box to lay the text out in, the disc's kept");
        continue;
      }
      // Remastered's text scale is relative to its own font, so the glyphs
      // keep the size they had on the disc's screen when Remastered's box would
      // make them bigger, and then take Remastered's justification in the box.
      Mat centre = Identity();
      for (int c = 0; c < 3; ++c) {
        centre.m[c][3] = (lo[c] + hi[c]) / 2.0;
      }
      double from[3];
      for (int c = 0; c < 3; ++c) {
        from[c] = old.m[c][3];
        for (int k = 0; k < 3; ++k) {
          from[c] += old.m[c][k] * double(GetBEFloat(w.typeData, 8 + size_t(k) * 4));
        }
      }
      const double was = discView(from);
      const Mat placed = Mul(w.world, centre);
      const double now = remView(placed);
      double scale = 1.0;
      if (was > 1e-9 && now > 1e-9 && pz / was < pz2 / now) {
        scale = (pz / was) / (pz2 / now);
        log(w.name + ": text at the disc's size, " + std::to_string(int(std::lround(100.0 / scale))) + "% of the box");
        if (g.textJustify[0] <= 2 && g.textJustify[1] <= 2) {
          SetBE32(w.typeData, kTextPaneJustify, g.textJustify[0]);
          SetBE32(w.typeData, kTextPaneJustify + 4, g.textJustify[1]);
        }
      }
      w.world = placed;
      SetBEFloat(w.typeData, 0, float(dim[0]));
      SetBEFloat(w.typeData, 4, float(dim[1]));
      for (size_t at = 8; at < 20; at += 4) {
        SetBEFloat(w.typeData, at, 0.0f);
      }
      SetBEFloat(w.typeData, kTextPaneExtent,
               float(std::max(1.0, std::nearbyint(dim[0] * AxisLength(w.world, 0) / (unit * scale)))));
      SetBEFloat(w.typeData, kTextPaneExtent + 4, float(std::max(1.0, std::nearbyint(extent[1] / scale))));
    } else if (w.type == Tag('E', 'N', 'R', 'G') && w.guif >= 0 && w.typeData.size() >= 4) {
      const RemWidget& g = rem[size_t(w.guif)];
      Part part;
      if (g.meshes.empty() || !MeshPart(model, g.meshes[0], part)) {
        log(w.name + ": no mesh for the bar");
        continue;
      }
      const std::optional<uint32_t> tid = texture(part, w.name);
      PortHudBars::Bar bar;
      bar.name = w.name;
      if (!tid || !Stations(part, bar)) {
        log(w.name + ": the bar's mesh is not a strip");
        continue;
      }
      SetBE32(w.typeData, 0, *tid);
      w.draw = g.draw;
      bars.push_back(std::move(bar));
    }
  }

  // The frame: every widget relative to its parent again.
  std::unordered_map<std::string, const Widget*> byName;
  for (const Widget& w : out) {
    byName[w.name] = &w;
  }
  Blob file;
  for (uint32_t v : header) {
    AppendBE32(file, v);
  }
  AppendBE32(file, uint32_t(out.size()));
  for (const Widget& w : out) {
    Mat local = w.world;
    const auto parent = byName.find(w.parent);
    Mat inverse;
    if (parent != byName.end() && Inverse(parent->second->world, inverse)) {
      local = Mul(inverse, w.world);
    }
    AppendBE32(file, w.type);
    file.insert(file.end(), w.name.begin(), w.name.end());
    file.push_back(0);
    file.insert(file.end(), w.parent.begin(), w.parent.end());
    file.push_back(0);
    file.insert(file.end(), w.flags, w.flags + 4);
    for (float c : w.color) {
      AppendBEFloat(file, c);
    }
    AppendBE32(file, w.draw);
    file.insert(file.end(), w.typeData.begin(), w.typeData.end());
    file.push_back(w.hasWorker ? 1 : 0);
    if (w.hasWorker) {
      AppendBE16(file, w.worker);
    }
    for (int i = 0; i < 3; ++i) {
      AppendBEFloat(file, float(local.m[i][3]));
    }
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        AppendBEFloat(file, float(local.m[i][j]));
      }
    }
    file.insert(file.end(), w.tail, w.tail + sizeof(w.tail));
  }
  if (!bars.empty()) {
    Blob barFile;
    PortHudBars::WriteFile(bars, barFile);
    if (!m_io.write(Hex8(retailFrame) + ".hudbars", barFile)) {
      error = "cannot write the bars";
      return false;
    }
  }
  if (!m_io.write(Hex8(retailFrame) + ".FRME", file)) {
    error = "cannot write the frame";
    return false;
  }
  counts.widgets += int(out.size());
  counts.bars += int(bars.size());
  return true;
}

}  // namespace PortRemastered
