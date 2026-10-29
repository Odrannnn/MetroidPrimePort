#pragma once

// Remastered's HUD frames, as frames the original game loads.
//
// A Remastered frame (GUIF) lists the same widgets as the disc's FRME, by the
// same names, laid out for a 16:9 screen and drawn with meshes of one model the
// frame names. The game's code finds its widgets by name and by worker id, so
// the disc's frame stays the skeleton: every widget of it is kept, and the ones
// Remastered also has take its transform, colour, draw mode and mesh. Widgets
// only Remastered has (trim, housings) are added under their parents; models
// only the disc has are emptied, their art having no place in the new layout.
//
// One frame becomes:
//   <FRME id>.FRME      the frame, in the disc's format, under the disc's id
//   <id>.CMDL           one model per mesh widget, unlit, one texture a surface
//   <id>.dds + .TXTR    each picture once: the native texture and its stub
//   <FRME id>.hudbars   the bars' strips (port_hud_bars.h)
// A tinted widget (SetTints) gets its own copy of each picture, multiplied by
// the tint: the game sets the widget's colour every frame, so the picture is
// the only place a constant factor can go.
//
// As with the model converter, bytes come and go through ConvertIO, and the
// output is the player's own, made from their copies of both games.

#include <array>
#include <iterator>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "port_remastered_cmdl.h"
#include "port_remastered_convert.h"

namespace PortRemastered {

// UI_Interference (ad2c208c)'s CCH5.x, the picture's height in rows, one per material the HUD
// has; a converted material carries the index (kStateFlag_PortHudInterference) and the shader
// (aurora's GX_AURORA_SET_HUD_SAMPLE mode 2 + index) has the same table.
inline constexpr float kHudInterferenceRows[5] = {414.476f, 195.048f, 100.f, 64.f, 128.f};

// A Remastered frame by its asset name, and the disc frame it stands in for.
struct HudFrame {
  const char* name;
  uint32_t retail;
};
const std::vector<HudFrame>& HudFrames();

// The model a frame draws with, in a pak's byte order. False when `guif` is
// not a frame.
bool HudFrameModel(const uint8_t* guif, size_t size, ModelUuid& model);

// Remastered colours each beam's icon in the beam menu: the game's colour for
// the icon times the beam's colour from its GUI colour tweak (the LDTA asset
// TweakGuiColorsMP1), with the default colour assist. The beam icon widgets'
// lower-case names -> RGBA; false when `ldta` is not that tweak.
bool HudBeamIconTints(const uint8_t* ldta, size_t size, std::map<std::string, std::array<float, 4>>& out,
                      std::string& error);

// The icy material Remastered draws a frozen enemy with (CPatternedMP1::RenderIcy): the
// MATI GUID, in the file's byte order, that the render loose data (the LDTA asset
// RenderLooseDataMP1) holds in property 0xC2347D95 of its root property 0x5F2E374B.
// False when `ldta` is not a tweak file or holds no such GUID.
bool RenderIcyMaterial(const uint8_t* ldta, size_t size, ModelUuid& out, std::string& error);

struct HudCounts {
  int widgets = 0;
  int models = 0;
  int textures = 0;
  int bars = 0;
};

// Converts frames one after another, sharing the pictures they have in common.
class HudConverter {
public:
  explicit HudConverter(ConvertIO io);

  // `retailFrame` is the disc frame's id; it is read through ConvertIO::retail.
  // `counts` is added to.
  bool Convert(uint32_t retailFrame, const uint8_t* guif, size_t size, const Model& model, HudCounts& counts,
               std::string& error);

  // A whole model (its finest level of detail), drawn the way the frames' are,
  // under the given id: for models the game places itself, like the map's compass.
  bool ConvertModel(const Model& model, uint32_t id, HudCounts& counts, std::string& error);

  // Model widgets, by lower-case name, whose pictures are multiplied by a colour
  // (RGBA, 0..1) in the frames converted after this.
  using Tint = std::array<float, 4>;
  void SetTints(std::map<std::string, Tint> tints) { m_tints = std::move(tints); }

  // The height of the ink of a capital H in pixels of Remastered's font (the
  // glyph's box less its padding), which text panes are sized by. Without it
  // they keep the disc's text.
  void SetGlyphInk(float ink) { m_glyphInk = ink; }

private:
  uint32_t NewId(uint32_t& next);
  bool LoadMaterial(std::string& error);
  // The port id of a Remastered picture (pak byte order) multiplied by `tint`,
  // written once; nothing for one that would not open.
  std::optional<uint32_t> Texture(const ModelUuid& id, HudCounts& counts, const std::string& owner,
                                  const Tint& tint = {1.f, 1.f, 1.f, 1.f}, bool squareAlpha = false);

  ConvertIO m_io;
  std::map<std::string, Tint> m_tints;
  float m_glyphInk = 0.f;
  // By pak-order id, tint and whether the alpha is squared; 0 for one that would not open.
  std::map<std::tuple<ModelUuid, Tint, bool>, uint32_t> m_textures;
  std::vector<uint8_t> m_material;           // the disc material the models are drawn with
  uint32_t m_nextModel;
  uint32_t m_nextTexture;
};

}  // namespace PortRemastered
