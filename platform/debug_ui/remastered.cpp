// F1 overlay: The Mods page's built-in sections, PAL languages and Remastered debugging. State and
// helpers shared with the other parts are in debug_ui_internal.h.

#include "debug_ui_internal.h"

#include "port_env.h"
#include "port_debug.h"
#include "port_gyro_motion.h"
#include "port_pointer.h"
#include "port_room_env.h"
#include "port_room_geo.h"
#include "port_paths.h"
#include "port_mods.h"
#include "port_dread_samus_mod.h"
#include "port_remastered_import.h"
#include "port_remastered_text.h"
#include "port_savestate.h"
#include "port_disc.h"
#include "port_log.h"
#include "port_pal_languages.h"

#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "Kyoto/Graphics/CCubeMaterial.hpp"
#include "MetroidPrime/Player/CGameOptions.hpp"

#include <aurora/imgui.h>
#include <dolphin/gx/GXExtra.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace PortDebug {

void DrawRemasteredWarning() {
  {
    const ImVec4 bad = ThemeBadColor();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(bad.x * 0.25f, bad.y * 0.25f, bad.z * 0.25f, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_Border, bad);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 2.f);
    ImGui::BeginChild("RemasteredWarning", ImVec2(0.f, 0.f),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextColored(bad, "EXPERIMENTAL - EXPECT BREAKAGE");
    ImGui::SetWindowFontScale(1.f);
    ImGui::TextWrapped("Remastered support is unfinished: expect wrong materials, missing models, "
                       "glitches and crashes. Re-import after updates.");
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
  }

}

// The PAL disc's languages for a USA one (port_pal_languages.h).
void DrawPalLanguages() {
  if (PortDisc::IsTrilogy(PortDisc::Current())) {
    ImGui::TextDisabled("Trilogy disc: support is experimental.");
    ItemHelp("The port runs the USA 1.00 code on the Trilogy disc's Metroid Prime data, which "
             "already has its languages.");
    return;
  }
  if (PortDisc::Current() == PortDisc::Version::Pal) {
    ImGui::TextDisabled("PAL disc: support is experimental.");
    ItemHelp("The port runs the USA 1.00 code on the PAL disc's data. Checked on every room, but "
             "less played than USA: please report differences.");
    return;
  }
  std::string picked;
  {
    std::lock_guard lock(sRemasteredPickMutex);
#if defined(__ANDROID__)
    // A pick the previous process never saw (the game was killed behind the picker).
    static bool sTookFile = false;
    if (!sTookFile) {
      sTookFile = true;
      std::ifstream in(RemasteredPickFile(kPalLanguagesPick));
      std::string uri;
      if (std::getline(in, uri) && !uri.empty()) {
        sPalLanguagesPick = uri;
      }
    }
#endif
    picked.swap(sPalLanguagesPick);
  }
  static std::string sStartError;
  if (!picked.empty()) {
#if defined(__ANDROID__)
    std::remove(RemasteredPickFile(kPalLanguagesPick).c_str());
#endif
    sStartError = PortPalLanguages::Start(picked) ? "" : "An import is already running.";
  }
  const PortPalLanguages::State state = PortPalLanguages::Status();
  const int installed = PortPalLanguages::Installed();
  ImGui::BeginDisabled(state.running);
  if (ImGui::Button(installed > 0 ? "Import the PAL languages again..." : "Add languages from a PAL disc...")) {
    OpenRemasteredDialog(kPalLanguagesPick);
  }
  ImGui::EndDisabled();
  ItemHelp("For a USA disc: pick your PAL Metroid Prime image (GM8P01) to add its French, German, "
           "Spanish and Italian text and fonts. The full Flaahgra boss theme is added too. Restart "
           "afterwards.");
  if (installed > 0 && !state.running) {
    ImGui::SameLine();
    if (ImGui::Button("Remove")) {
      PortPalLanguages::Remove();
    }
    ImGui::SetItemTooltip("Deletes the imported PAL languages. Text already loaded stays until a restart.");
  }
  if (state.running) {
    ImGui::TextDisabled("%s...", state.message.c_str());
  } else if (state.finished) {
    if (state.ok) {
      ImGui::TextWrapped("%s", state.message.c_str());
    } else {
      ImGui::TextColored(ThemeWarnColor(), "%s", state.message.c_str());
    }
  } else if (!sStartError.empty()) {
    ImGui::TextColored(ThemeWarnColor(), "%s", sStartError.c_str());
  } else if (installed > 0) {
    ImGui::TextDisabled("PAL languages added (%d text tables).", installed);
  }
}

void DrawLanguageSection() {
  ImGui::SeparatorText("Language");
  {
    int language = 0;
    const char* current = TextLanguage();
    for (size_t i = 0; i < PortRemastered::kTextLanguageCount; ++i) {
      if (std::strcmp(current, PortRemastered::kTextLanguages[i].code) == 0) {
        language = static_cast< int >(i) + 1;
      }
    }
    const auto name = [](void*, int index) {
      return index == 0 ? "English" : PortRemastered::kTextLanguages[index - 1].name;
    };
    if (ImGui::Combo("Language", &language, name, nullptr,
                     static_cast< int >(PortRemastered::kTextLanguageCount) + 1)) {
      SetTextLanguage(language == 0 ? "" : PortRemastered::kTextLanguages[language - 1].code);
    }
    ItemHelp("The language of the game's text. A USA disc has English only; a PAL disc, the "
             "options below and the Remastered import add more. Missing text stays English. Open "
             "menus change the next time they open; the font changes after a restart.");
  }
  DrawPalLanguages();

}

// The saved settings for Remastered's rooms, at the top of Debug's room models header.
void DrawRemasteredRoomModels(bool geoReady) {
  ImGui::BeginDisabled(!geoReady);
  static const char* const kModes[] = {"off", "in place of the room", "on top of the room"};
  int mode = int(PortRoomGeo::GetMode());
  if (ImGui::Combo("Room geometry", &mode, kModes, 3)) {
    PortRoomGeo::SetMode(PortRoomGeo::Mode(mode));
  }
  ImGui::SetItemTooltip("Recommended: in place of the room.\nDraws a mod's Remastered room models "
                        "instead of the original rooms,\n"
                        "or over them to compare. Needs a room geometry mod.");
#ifdef __ANDROID__
  // The defaults (MSAA off, 1x) are fine; this catches a phone set up for the original rooms.
  if (PortRoomGeo::GetMode() != PortRoomGeo::Mode::Off && (EffectiveMsaa() > 1 || EffectiveRenderScale() <= 0.f || EffectiveRenderScale() > 2.f)) {
    ImGui::TextColored(ThemeWarnColor(), "Slow on phones with MSAA or native scale.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Use 2x, MSAA off")) {
      SetMsaa(1);
      SetRenderScale(2.f);
      MarkDirty();
    }
    ImGui::SetItemTooltip(
        "Turns anti-aliasing off and sets the resolution scale to 2x (Quality settings).\nRemastered "
        "rooms draw many more models, which is heavy for a phone's GPU.");
  }
#endif
  bool areaLights = PortRoomGeo::AreaLights();
  if (ImGui::Checkbox("Take the area's lights", &areaLights)) {
    PortRoomGeo::SetAreaLights(areaLights);
  }
  ImGui::SetItemTooltip("Recommended: off.\nAlso lights Remastered rooms with the game's lights "
                        "where the room has baked\n"
                        "light. Off matches Remastered.");
  float minPixels = PortRoomGeo::MinPixels();
  if (ImGui::SliderFloat("Skip small models", &minPixels, 0.f, 8.f, minPixels > 0.f ? "under %.1f px" : "off")) {
    PortRoomGeo::SetMinPixels(minPixels);
    MarkDirty();
  }
  ImGui::SetItemTooltip("Skips room models that look smaller than this on screen.\nRaise it if "
                        "frames are slow; 0 draws everything.");
  float lodDistance = PortRoomGeo::LodDistance();
  if (ImGui::SliderFloat("Detail distance", &lodDistance, 0.f, 4.f, lodDistance > 0.f ? "x%.2f" : "full detail")) {
    PortRoomGeo::SetLodDistance(lodDistance);
    MarkDirty();
  }
  ImGui::SetItemTooltip(
      "Distance where room models switch to simpler versions: 1 is Remastered's own,\n"
      "higher keeps full models farther out, 0 never switches. Lower it if frames are\n"
      "slow. Needs an import made by this version.");
  ImGui::EndDisabled();
  bool resident = sRoomGeoResident;
  if (ImGui::Checkbox("Keep on the GPU (next start)", &resident)) {
    PortDebug::SetRoomGeoResident(resident);
  }
  ImGui::SetItemTooltip("Recommended: on.\nUploads a room geometry mod's models once on load "
                        "instead of every frame.\n"
                        "Takes effect at the next start.");
  ImGui::Separator();
}

namespace {
// Whether a built-in mod's import is older than this build. port_mods marks an import made by the
// Remastered importer on its ModInfo; the Dread import keeps its own stamp instead (port_dread_samus_mod.h),
// which the loader reports among the status messages.
bool DreadImportStale(const PortMods::Status& status) {
  return std::any_of(status.messages.begin(), status.messages.end(), [](const std::string& message) {
    return message.find(PortDreadSamus::kModName) != std::string::npos &&
           message.find("imported by an older version") != std::string::npos;
  });
}

// A built-in mod's switch, shared by the Remastered and Dread sections: the mods_disabled entry the
// Mods page writes for every mod. Nothing to switch while the mod is not installed. Below it: the
// switch's value (what the next reload loads) and, apart from it, whether this launch loaded it.
void DrawBuiltinModToggle(const char* modName, const char* label, bool stale) {
  const PortMods::Status& status = PortMods::CurrentStatus();
  const auto mod = std::find_if(status.mods.begin(), status.mods.end(), [modName](const PortMods::ModInfo& m) {
    return m.name == modName;
  });
  if (mod == status.mods.end()) {
    ImGui::TextDisabled("Not imported yet.");
    return;
  }
  if (stale) {
    ImGui::TextColored(ThemeWarnColor(), "Imported by an older version: re-import.");
  } else {
    ImGui::TextDisabled("Imported.");
  }
  std::vector<std::string> disabled = PortMods::SplitDisabled(sModsDisabled);
  bool on = std::find(disabled.begin(), disabled.end(), mod->name) == disabled.end();
  // ModsEnabled, not the Load mods setting: Original experience turns every mod off whatever
  // mods_disabled says, and the switch would be a lie. The setting itself is kept as it is.
  const bool modsOn = ModsEnabled();
  ImGui::BeginDisabled(!modsOn);
  if (ImGui::Checkbox(label, &on)) {
    if (on) {
      disabled.erase(std::remove(disabled.begin(), disabled.end(), mod->name), disabled.end());
    } else {
      disabled.push_back(mod->name);
    }
    SetModsDisabled(PortMods::JoinDisabled(disabled));
  }
  ImGui::EndDisabled();
  ItemHelp("Off unloads this mod alone; the other mods still load. Takes effect when the mods are reloaded.");
  if (!modsOn) {
    SameLineAfterHelp();
    ImGui::TextDisabled(sModsEnabled ? "(Original experience is on: no mods load)" : "(mods are off)");
  }
  // What this launch loaded, which the switch above only changes at the next reload.
  if (mod->enabled) {
    ImGui::TextDisabled(on ? "Loaded." : "Loaded until the mods are reloaded.");
  } else if (!on) {
    ImGui::TextDisabled("Off, not loaded.");
  } else if (PortMods::Suspended()) {
    ImGui::TextDisabled("Not loaded while an import runs.");
  } else if (modsOn && mod->pendingRestart) {
    ImGui::TextColored(ThemeWarnColor(), "Not loaded: Restart the application to apply.");
  } else if (modsOn) {
    ImGui::TextColored(ThemeWarnColor(), "Not loaded: reload the mods to apply.");
  }
}

// Which parts of the installed import the mods load (remastered_hide): the disc's own stands in
// for each part left out, without importing again. Applies at the next reload.
void DrawRemasteredParts() {
  const PortMods::Status& status = PortMods::CurrentStatus();
  const auto mod = std::find_if(status.mods.begin(), status.mods.end(), [](const PortMods::ModInfo& m) {
    return m.name == PortRemastered::kImportModName;
  });
  if (mod == status.mods.end() || !ImGui::TreeNode("Parts to use")) {
    return;
  }
  struct Part {
    const char* key;
    const char* label;
    const char* help;
  };
  static constexpr Part kParts[] = {
      {"geometry", "Room geometry and lighting",
       "Remastered's rooms with their lighting, fog and colour grade. Off: the disc's rooms (no restart "
       "needed to load them)."},
      {"effects", "Particle effects", "Remastered's particles, beams and room effects."},
      {"models", "Models", "Remastered's characters, creatures, items and objects."},
      {"hud", "HUD", "Remastered's visor and HUD."},
      {"font", "Font", "Remastered's sharp font."},
      {"text", "Text", "Remastered's text tables."},
      {"map", "Map", "Remastered's map."},
      {"movies", "Movies", "Remastered's movies."},
  };
  static_assert(std::size(kParts) == std::size(PortMods::kImportParts));
  const std::string hidden = Loaded(sRemasteredHide);
  std::string next;
  bool edited = false;
  for (const Part& part : kParts) {
    bool on = !PortMods::ImportPartHidden(hidden, part.key);
    edited |= ImGui::Checkbox(part.label, &on);
    ItemHelp(part.help);
    if (!on) {
      next += next.empty() ? "" : ",";
      next += part.key;
    }
  }
  if (edited) {
    SetRemasteredHidden(next);
  }
  if (std::getenv("MP_REMASTERED_HIDE") != nullptr) {
    ImGui::TextDisabled("(MP_REMASTERED_HIDE is set: it decides for this run)");
  } else if (mod->enabled && !PortMods::SameImportParts(mod->hidden, next)) {
    ImGui::TextColored(ThemeWarnColor(), "Reload the mods to apply.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload mods")) {
      PortSaveState::RequestModReload();
    }
  }
  ImGui::TreePop();
}
} // namespace

// The port's own mods, each with its import: Prime Remastered's models and Dread's Samus.
void DrawBuiltinMods() {
  ImGui::SeparatorText("Built-in mods");
  const PortMods::Status& status = PortMods::CurrentStatus();
  // One prod.keys serves both imports, so the page has one row for it, above both sections and
  // inside neither: it stays visible whichever of them is open, and neither section owns it.
  DrawImportKeys();
  // MP_DEBUG_TAB=Mods/<section>, and the old Remastered and Dread page names, open one of these.
  if (StartSection("Mods", "Prime Remastered")) {
    ImGui::SetNextItemOpen(true);
  }
  // Its header is the only wrapper Prime's content has: the import and the gallery are drawn
  // straight into this indented body, which ends before the next header.
  if (ImGui::CollapsingHeader("Prime Remastered")) {
    ImGui::Indent();
    DrawBuiltinModToggle(PortRemastered::kImportModName, "Use the Remastered models",
                         PortMods::StaleRemasteredImport() != nullptr);
    DrawRemasteredParts();
    DrawRemasteredWarning();
    ImGui::SeparatorText("Import");
    DrawRemasteredImport();
    ImGui::SeparatorText("Gallery");
    const std::vector<std::string> pictures = PortMods::GalleryPaths();
    if (pictures.empty()) {
      ImGui::TextDisabled("Remastered's concept art. It comes with the Remastered import.");
    } else if (ImGui::Button(("Open gallery (" + std::to_string(pictures.size()) + " pictures)").c_str())) {
      sGalleryPaths = pictures;
      sGalleryIndex = std::min(sGalleryIndex, int(pictures.size()) - 1);
      ReleaseGalleryTexture();
      sGalleryOpen = true;
    }
    ImGui::Unindent();
  }
  if (StartSection("Mods", "Metroid Dread")) {
    ImGui::SetNextItemOpen(true);
  }
  // A sibling of the Prime section, at the same level: neither import sits under the other.
  if (ImGui::CollapsingHeader("Metroid Dread")) {
    ImGui::Indent();
    DrawBuiltinModToggle(PortDreadSamus::kModName, "Use the Dread Suit models", DreadImportStale(status));
    bool cannon = PortDreadSamus::FirstPersonCannonShown();
    if (ImGui::Checkbox("Dread arm cannon in first person", &cannon)) {
      PortDreadSamus::SetFirstPersonCannonShown(cannon);
      MarkDirty();
    }
    ItemHelp("Off keeps Prime's own arm cannon in first person; the Dread Suit bodies stay. Applies at once.");
    ImGui::SeparatorText("Import");
    DrawDreadSamusImport();
    ImGui::Unindent();
  }
}

// The Remastered pass tests and inspectors, for the Debug page.
void DrawRemasteredDebug() {
  ImGui::SeparatorText("Remastered");
  // What the middle of the screen looks at.
  float origin[3];
  float forward[3];
  const bool inWorld = PortDebug::ViewRay(origin, forward);

  if (ImGui::CollapsingHeader("Remastered room models")) {
    // The frame's buffers are only sized for room geometry when the game started with some.
    const bool geoReady = PortRoomGeo::BuffersReady();
    if (!geoReady && PortMods::RoomGeometryLoaded()) {
      ImGui::TextColored(ThemeWarnColor(), "Restart the game to see the Remastered rooms.");
      ImGui::SetItemTooltip("No room geometry was installed at start, so no room was set aside for "
                            "it.\nStart with some to change mods without a restart.");
    }
    DrawRemasteredRoomModels(geoReady);
    ImGui::BeginDisabled(!geoReady);
    bool merged = PortRoomGeo::MergedDraws();
    if (ImGui::Checkbox("Draw merged copies", &merged)) {
      PortRoomGeo::SetMergedDraws(merged);
    }
    ImGui::SetItemTooltip("Recommended: on. Not saved.\nDraws repeated props (rocks, grass) near "
                          "each other as one model. Off draws\n"
                          "each on its own: slower, for comparing.");
    bool frontToBack = PortRoomGeo::FrontToBack();
    if (ImGui::Checkbox("Draw nearest first", &frontToBack)) {
      PortRoomGeo::SetFrontToBack(frontToBack);
    }
    ImGui::SetItemTooltip("Recommended: on. Not saved.\nDraws solid room models nearest first so "
                          "the GPU skips shading hidden ones.\n"
                          "Looks the same; off is for comparing frame rates.");
    bool prepass = PortRoomGeo::DepthPrepass();
    if (ImGui::Checkbox("Depth pre-pass for cut-outs", &prepass)) {
      PortRoomGeo::SetDepthPrepass(prepass);
    }
    ImGui::SetItemTooltip("Recommended: off. Not saved. Needs \"Draw nearest first\".\nDraws grass "
                          "and leaves for depth first, then shades only what stays visible.\n"
                          "Looks the same; slower on the phones tried so far.");
    bool opaquePrepass = PortRoomGeo::OpaquePrepass();
    if (ImGui::Checkbox("Depth pre-pass for all room models", &opaquePrepass)) {
      PortRoomGeo::SetOpaquePrepass(opaquePrepass);
    }
    ImGui::SetItemTooltip("Recommended: on, off on phones (the default). Not saved.\nDraws every room "
                          "model for depth first, then shades only what stays visible.\n"
                          "Looks the same; faster on desktop GPUs short of shading power.");
    static constexpr const char* kCostTests[] = {"Off", "Flat", "No lights", "No ambient volume",
                                                 "No reflections", "No normal maps", "No ORM/emissive maps",
                                                 "Maps only", "Maps without anisotropy",
                                                 "Maps without anisotropy or mip blend",
                                                 "No post-processing", "No screen copies", "No bloom",
                                                 "Post without the frame copy",
                                                 "Post without the depth reload",
                                                 "Post always reloading the depth"};
    static bool gpuTimes = false;
    if (ImGui::Checkbox("GPU pass times", &gpuTimes)) {
      WantGpuTimes(kGpuTimesPanel, gpuTimes);
    }
    ImGui::SetItemTooltip(
        "Not saved. Times each render pass on the GPU, averaged over 60 frames.\nTotal is the sum "
        "of the passes; span is first start to last end.");
    if (gpuTimes) {
      if (!GXPortGpuTimesSupported()) {
        ImGui::TextUnformatted("timestamps unsupported");
      } else {
        GXPortGpuTime times[32];
        float total = 0.f;
        float span = 0.f;
        const u32 count = GXPortGetGpuTimes(times, 32, &total, &span);
        if (count == 0) {
          ImGui::TextUnformatted("collecting...");
        } else if (ImGui::BeginTable("gputimes", 3, ImGuiTableFlags_SizingFixedFit)) {
          ImGui::TableSetupColumn("Pass");
          ImGui::TableSetupColumn("ms");
          ImGui::TableSetupColumn("x");
          ImGui::TableHeadersRow();
          for (u32 i = 0; i < count; ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(times[i].name);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", times[i].msPerFrame);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", times[i].passesPerFrame);
          }
          ImGui::EndTable();
          ImGui::Text("Total %.3f ms, span %.3f ms", total, span);
        }
      }
    }
    int costTest = int(GXGetPBRCostTest());
    if (ImGui::Combo("Shading cost test", &costTest, kCostTests, IM_ARRAYSIZE(kCostTests))) {
      GXSetPBRCostTest(u32(costTest));
    }
    ImGui::SetItemTooltip(
        "Not saved. A speed test: leaves out one part of the Remastered shading so the\n"
        "frame rate shows what it costs. Flat leaves out all but the colour map. Maps\n"
        "only reads every map but skips the maths. The anisotropy options shade in\n"
        "full but filter maps more cheaply, looking a little softer. The rest time the\n"
        "frame around the shading (bloom, colour grade, screen copies, depth reload for\n"
        "the HUD). Most look wrong on purpose.");
    ImGui::EndDisabled();

    static std::string picked;
    static uint32_t pickedModel = 0;
    static std::string materials;
    ImGui::BeginDisabled(!inWorld);
    if (ImGui::Button("Pick the model ahead")) {
      picked.clear();
      pickedModel = PortRoomGeo::Pick(CVector3f(origin[0], origin[1], origin[2]),
                                      CVector3f(forward[0], forward[1], forward[2]), picked);
      materials = pickedModel != 0 ? PortRoomGeo::Materials(pickedModel) : std::string();
      if (picked.empty()) {
        picked = "No room geometry ahead.";
      }
    }
    ImGui::SetItemTooltip("Names the room model in the middle of the screen and lists its materials.");
    ImGui::EndDisabled();
    if (pickedModel != 0) {
      ImGui::SameLine();
      if (ImGui::Button("Hide it")) {
        PortRoomGeo::SetHidden(pickedModel, true);
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Show all")) {
      PortRoomGeo::SetHidden(0, false);
    }
    if (!picked.empty()) {
      ImGui::TextUnformatted(picked.c_str());
    }
    if (!materials.empty() && ImGui::TreeNode("Materials of the first")) {
      ImGui::TextUnformatted(materials.c_str());
      ImGui::TreePop();
    }
  }

  if (ImGui::CollapsingHeader("Remastered materials and lighting")) {
    int view = PortDebug::PbrView();
    if (ImGui::BeginCombo("PBR surfaces show", view == 0 ? "the shaded result" : PortDebug::PbrViewName(view))) {
      for (int i = 0; i < PortDebug::PbrViewCount(); ++i) {
        if (ImGui::Selectable(i == 0 ? "the shaded result" : PortDebug::PbrViewName(i), i == view)) {
          PortDebug::SetPbrView(i);
        }
      }
      ImGui::EndCombo();
    }
    ImGui::SetItemTooltip(
        "Recommended: the shaded result.\nThe others show one input of a mod's PBR materials "
        "alone: base colour,\n"
        "normals, roughness, metalness, occlusion, ambient light, reflection, glow,\n"
        "exposure or material kind.");
    static const char* const kProbes[] = {"off", "on", "mirror", "window"};
    int probe = std::clamp(CCubeMaterial::sPortPBRProbeMode, 0, 3);
    if (ImGui::Combo("Reflection probe", &probe, kProbes, 4)) {
      CCubeMaterial::sPortPBRProbeMode = probe;
    }
    ImGui::SetItemTooltip(
        "Recommended: on.\nWhere shiny surfaces get reflections: the room's own cube if the mod "
        "has one,\n"
        "else a live capture around the camera. Mirror and window show it as a perfect\n"
        "mirror or as windows onto it, to check what it holds.");

    bool env = PortRoomEnv::Enabled();
    if (ImGui::Checkbox("Room environments", &env)) {
      PortRoomEnv::SetEnabled(env);
    }
    ImGui::SetItemTooltip("Recommended: on.\nUses a mod's own lighting for each room: reflection "
                          "cubes, baked ambient light,\n"
                          "exposure and bloom.");
    ImGui::BeginDisabled(!env);
    ImGui::SameLine();
    bool volumes = PortRoomEnv::VolumesEnabled();
    if (ImGui::Checkbox("Baked light per pixel", &volumes)) {
      PortRoomEnv::SetVolumesEnabled(volumes);
    }
    ImGui::SetItemTooltip("Recommended: on.\nLights Remastered rooms from the room's baked light "
                          "grid, so light changes\n"
                          "across a surface. Off gives each model one ambient colour.");
    float ambient = PortRoomEnv::AmbientScale();
    if (ImGui::SliderFloat("Baked ambient scale", &ambient, 0.f, 4.f, "%.2f")) {
      PortRoomEnv::SetAmbientScale(ambient);
    }
    ImGui::SetItemTooltip("Recommended: 1.\n"
                          "Multiplies the baked ambient light. 0 uses the game's own ambient instead.");
    static const char* const kVolumeViews[] = {"the shaded surface", "volume coordinates", "the baked light"};
    int volumeView = std::clamp(PortRoomEnv::VolumeView(), 0, 2);
    if (ImGui::Combo("Baked surfaces show", &volumeView, kVolumeViews, 3)) {
      PortRoomEnv::SetVolumeView(volumeView);
    }
    ImGui::SetItemTooltip("Recommended: the shaded surface.\n"
                          "The others show where each pixel samples the baked light grid, or the baked\n"
                          "light alone, to check it lines up.");
    ImGui::EndDisabled();
    if (inWorld && ImGui::TreeNode("Room environment here")) {
      ImGui::TextUnformatted(PortRoomEnv::Info(origin).c_str());
      ImGui::TreePop();
    }
  }
}

void ToggleRemastered() {
  const PortMods::Status& status = PortMods::CurrentStatus();
  const auto mod = std::find_if(status.mods.begin(), status.mods.end(), [](const PortMods::ModInfo& m) {
    return m.name == PortRemastered::kImportModName;
  });
  if (mod == status.mods.end()) {
    ShowNote("Remastered: not imported");
    return;
  }
  if (!ModsEnabled()) {
    ShowNote(sOriginalExperience ? "Remastered: off (Original experience is on)" : "Remastered: off (mods are off)");
    return;
  }
  std::vector<std::string> disabled = PortMods::SplitDisabled(sModsDisabled);
  const auto found = std::find(disabled.begin(), disabled.end(), mod->name);
  const bool on = found != disabled.end();
  if (on) {
    disabled.erase(found);
  } else {
    disabled.push_back(mod->name);
  }
  // Saved now: outside the overlay, changes are otherwise written at exit, which a phone's
  // app rarely reaches before Android kills it.
  SetModsDisabled(PortMods::JoinDisabled(disabled));
  SaveSettingsNow();
  PortLog::Write("port: remastered %s\n", on ? "on" : "off");
  if (on && mod->pendingRestart) {
    ShowNote("Remastered: on after a restart");
    return;
  }
  if (!PortMods::Suspended()) {
    ReloadUntilDone();
  }
  ShowNote(on ? "Remastered: on" : "Remastered: off");
}

} // namespace PortDebug
