// F1 overlay: The Video page: display, GPU driver, quality, compatibility and textures. State and
// helpers shared with the other parts are in debug_ui_internal.h.

#include "debug_ui_internal.h"

#include "port_env.h"
#include "port_debug.h"
#include "port_third_person.h"
#include "port_graphics_backend.h"
#include "port_gyro_motion.h"
#include "port_dynamic_res.h"
#include "port_pointer.h"
#include "port_hd_font.h"
#include "port_paths.h"
#include "port_config_reset.h"
#include "port_textures.h"
#include "port_gpu_driver.h"
#include "port_mods.h"
#include "port_savestate.h"
#include "port_suit_colours.h"

#include "MetroidPrime/Cameras/CCameraManager.hpp"

#include <aurora/imgui.h>
#include <imgui_internal.h>
#include <SDL3/SDL_dialog.h>

#if defined(__ANDROID__)
#include "touch_pad.h"
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace PortDebug {

void DrawVideoDisplay() {
  ImGui::SeparatorText("Display");
  bool fullscreen = sFullscreen;
#if defined(__ANDROID__)
  if (ImGui::Checkbox("Fullscreen (hide the status and navigation bars)", &fullscreen)) {
#else
  if (ImGui::Checkbox("Fullscreen (F11)", &fullscreen)) {
#endif
    SetFullscreen(fullscreen);
  }
  bool vsync = sVsyncEnabled;
  if (ImGui::Checkbox("Vsync", &vsync)) {
    SetVsyncEnabled(vsync);
  }

  const bool locked = BeginOriginalLocked();
  int aspect = static_cast< int >(sAspectMode);
  if (ImGui::Combo("Aspect ratio", &aspect, "4:3\0" "16:9\0" "Follow window\0")) {
    SetAspectMode(static_cast< EAspectMode >(aspect));
    MarkDirty();
  }
  if (ImGui::Checkbox("Cutscene black bars", &sCinemaBars)) {
    MarkDirty();
  }
  ImGui::SetItemTooltip("Off: cutscenes narrower than 16:9 fill the screen with the full shot.\n"
                        "On: the original 16:9 letterbox.");

  if (ImGui::Checkbox("Sharp scan window", &sSharpScanWindow)) {
    MarkDirty();
  }
  ImGui::SetItemTooltip(
      "On: the scan visor's magnifier re-renders the world through a zoomed camera,\n"
      "so it shows full detail (and HD textures) at any resolution. Costs a second\n"
      "world render while the scan visor is up.\nOff: the original magnified copy of the screen.");

  bool ballLooks = sGeneratedBallLooks;
  if (ImGui::Checkbox("Suit-coloured morph balls", &ballLooks)) {
    SetGeneratedBallLooks(ballLooks);
  }
  ImGui::SetItemTooltip(
      "On: every suit's morph ball takes its suit's colours, recoloured from your disc at\n"
      "boot, including a Power Suit spider ball.\nOff: the original, where suits borrow each "
      "other's balls.");

  ImGui::SeparatorText("Suit colours");
  {
    static int sSuitColoursSuit = 0;
    const PortSuitColours::Suit& suit = PortSuitColours::SuitInfo(sSuitColoursSuit);
    if (ImGui::BeginCombo("Suit", suit.name)) {
      for (int i = 0; i < PortSuitColours::kSuitCount; ++i) {
        const bool custom = !PortSuitColours::PicksKey(i).empty();
        const std::string label = std::string(PortSuitColours::SuitInfo(i).name) + (custom ? " *" : "");
        if (ImGui::Selectable(label.c_str(), i == sSuitColoursSuit)) {
          sSuitColoursSuit = i;
        }
      }
      ImGui::EndCombo();
    }
    ImGui::SetItemTooltip(
        "Each suit's colours, saved per suit (* = changed). Recoloured from your disc, so\n"
        "the change shows everywhere: cutscenes, pause screen, arm cannon, morph ball.\nA mod's "
        "own suit textures (the Remastered import) are left alone.");
    for (int slot = 0; slot < suit.slots; ++slot) {
      ImGui::PushID(slot);
      const std::optional< uint32_t > picked = PortSuitColours::Colour(sSuitColoursSuit, slot);
      const uint32_t shown = picked ? *picked : PortSuitColours::DefaultColour(sSuitColoursSuit, slot);
      float rgb[3] = {float((shown >> 16) & 255) / 255.f, float((shown >> 8) & 255) / 255.f,
                      float(shown & 255) / 255.f};
      if (ImGui::ColorEdit3(suit.slotNames[slot], rgb, ImGuiColorEditFlags_NoInputs)) {
        uint32_t value = 0;
        for (const float c : rgb) {
          value = (value << 8) | uint32_t(std::clamp(std::lround(c * 255.f), 0l, 255l));
        }
        PortSuitColours::SetColour(sSuitColoursSuit, slot, value);
        MarkDirty();
      }
      if (sSuitColoursSuit == 3) {
        ImGui::SetItemTooltip("The Phazon Suit's body is nearly black: only its glow and accent change.");
      }
      ImGui::SameLine();
      ImGui::BeginDisabled(!picked);
      if (ImGui::SmallButton("Reset")) {
        PortSuitColours::SetColour(sSuitColoursSuit, slot, std::nullopt);
        MarkDirty();
      }
      ImGui::EndDisabled();
      ImGui::PopID();
    }
    if (ImGui::Button("Random")) {
      PortSuitColours::Randomise(sSuitColoursSuit, std::random_device{}());
      MarkDirty();
    }
    ImGui::SetItemTooltip("Random colours for every slot of this suit; Apply to see them.");
    ImGui::SameLine();
    const bool pending = PortSuitColours::Pending(!sOriginalExperience);
    ImGui::BeginDisabled(!pending || PortMods::Suspended());
    if (ImGui::Button("Apply suit colours")) {
      PortSaveState::RequestModReload();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(
        "Rebuilds the recoloured textures and reloads the mods (a moment; the game\n"
        "carries on). Original experience uses the original colours.");
  }

  ImGui::SeparatorText("HUD and view");
  bool hudWide = sHudWide;
  if (ImGui::Checkbox("Widescreen HUD (spread to edges)", &hudWide)) {
    SetHudWide(hudWide);
    MarkDirty();
  }
  ImGui::SetItemTooltip("Spreads each HUD element's position so edge elements reach the wide "
                        "corners.\nIn-game HUD only, not menus.");

  int hudScale = sHudScale;
  if (ImGui::SliderInt("HUD scale", &hudScale, kHudScaleMin, kHudScaleMax, "%d%%")) {
    SetHudScale(hudScale);
  }
  int scanSpread = sScanSpread;
  if (ImGui::SliderInt("Scan image spread", &scanSpread, kScanSpreadMin, kScanSpreadMax, "%d%%")) {
    SetScanSpread(scanSpread);
  }
  ImGui::SetItemTooltip(
      "How far the widescreen HUD moves the scan visor's side panels toward the screen\n"
      "edges. 0%% keeps them at the 4:3 position, clear of touch controls; 100%% puts\n"
      "them at the edges.");
  // Issue #144: the game's Visor Opacity is one value for every visor and never reaches the
  // energy readout.
  static const char* const kHudOpacityLabels[kHudOpacityPartCount] = {
      "X-Ray HUD opacity", "Thermal HUD opacity", "Scan HUD opacity", "Energy readout opacity"};
  for (int i = 0; i < kHudOpacityPartCount; ++i) {
    const HudOpacityPart part = static_cast< HudOpacityPart >(i);
    ImGui::PushID(i);
    bool followVisor = sHudOpacity[i] == kHudOpacityVisor;
    if (ImGui::Checkbox("##follow", &followVisor)) {
      SetHudOpacity(part, followVisor ? kHudOpacityVisor : kHudOpacityMax);
    }
    ImGui::SetItemTooltip("On: use the game's Visor Opacity (Options > Visor).");
    ImGui::SameLine();
    ImGui::BeginDisabled(followVisor);
    int percent = followVisor ? kHudOpacityMax : sHudOpacity[i];
    if (ImGui::SliderInt(kHudOpacityLabels[i], &percent, kHudOpacityMin, kHudOpacityMax,
                         followVisor ? "Visor Opacity" : "%d%%")) {
      SetHudOpacity(part, percent);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(
        part == kHudOpacityEnergy
            ? "The energy bar, tanks, digits and low-energy warning. The game draws\nthem at full "
              "strength whatever Visor Opacity is; tick the box to follow it."
            : "This visor's HUD, the visor and beam menus and the minimap.\nTick the box to use "
              "the game's Visor Opacity.");
    ImGui::PopID();
  }
  if (ImGui::SliderInt("Crosshair opacity", &sCrosshairOpacity, kCrosshairOpacityMin,
                       kCrosshairOpacityMax, "%d%%")) {
    MarkDirty();
  }
  ImGui::SetItemTooltip("The crosshair in the middle of the screen (free aim, mouse aim, twin "
                        "stick). 100%% is full strength; the game's own crosshair is much fainter (19%%).");
  {
    static const char* const kStyles[] = {"Classic", "Remastered"};
    int style = static_cast< int >(sCrosshairStyle);
    if (ImGui::Combo("Crosshair style", &style, kStyles, 2)) {
      sCrosshairStyle = static_cast< PortDebug::ECrosshairStyle >(style);
      MarkDirty();
    }
    ImGui::SetItemTooltip(
        "Classic: the game's own crosshair model. Remastered: rings and three spikes in\n"
        "the Remastered design. Original experience uses Classic.");
  }
  if (ImGui::Checkbox("Custom crosshair colour", &sCrosshairCustomColor)) {
    MarkDirty();
  }
  ImGui::SetItemTooltip("Off: the game's own colour.");
  ImGui::SameLine();
  ImGui::BeginDisabled(!sCrosshairCustomColor);
  if (ImGui::ColorEdit3("##crosshair_color", sCrosshairColor,
                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
    MarkDirty();
  }
  ImGui::EndDisabled();
  if (ImGui::Checkbox("Secondary crosshairs", &sSecondaryCrosshairs)) {
    MarkDirty();
  }
  ImGui::SetItemTooltip(
      "The reticle on Lock-On's target (combat visor), and the ring or crosshair on it\n"
      "in X-Ray and Thermal. Off hides them; the lock-on reticle and scan target\n"
      "boxes stay.");
  bool hideHelmet = sHideHelmet;
  if (ImGui::Checkbox("Hide helmet", &hideHelmet)) {
    SetHideHelmet(hideHelmet);
  }
  bool thirdPerson = PortThirdPerson::Enabled();
  if (ImGui::Checkbox("Third-person camera", &thirdPerson)) {
    PortThirdPerson::SetEnabled(thirdPerson);
    MarkDirty();
  }
  ImGui::SetItemTooltip(
      "Plays from behind Samus's shoulder (not the morph ball or cutscenes), showing her\n"
      "body and hiding the arm cannon and helmet. Distance, shoulder and height:\n"
      "console `thirdperson`, or third_person_* in the settings file.");
  bool hideVisorFx = sHideVisorEffects;
  if (ImGui::Checkbox("Hide visor effects", &hideVisorFx)) {
    SetHideVisorEffects(hideVisorFx);
  }
  ImGui::SetItemTooltip("Steam, Samus's reflection, and rain, water and goo on the visor.");

  float fov = sFirstPersonFov;
  if (ImGui::SliderFloat("Field of view", &fov, kFovMin, kFovMax, "%.0f deg")) {
    SetFirstPersonFov(std::round(fov));
  }
  ImGui::SameLine();
  if (ImGui::Button("Retail##fov")) {
    SetFirstPersonFov(kFovRetail);
  }
  {
    // The horizontal FOV this gives at the current aspect, which is the number
    // most PC games show.
    const float aspect = CCameraManager::GetDefaultAspectRatio();
    const float hfov = 2.f * std::atan(std::tan(sFirstPersonFov * 0.5f * 0.017453292f) * aspect) /
                       0.017453292f;
    ImGui::SetItemTooltip("Back to the retail 55 deg.");
    ImGui::TextDisabled("About %.0f deg horizontal at this aspect.", hfov);
    ImGui::SetItemTooltip("First-person vertical FOV (retail 55). The arm cannon stays at the retail\n"
                          "FOV; morph ball and cutscene cameras are unchanged.");
  }
  {
    // Issue #72: PrimeHack-style arm cannon placement.
    float offset[3] = {sGunOffset[0], sGunOffset[1], sGunOffset[2]};
    bool changed = ImGui::SliderFloat("Arm cannon right", &offset[0], -kGunOffsetMax, kGunOffsetMax, "%.2f m");
    changed |= ImGui::SliderFloat("Arm cannon forward", &offset[1], -kGunOffsetMax, kGunOffsetMax, "%.2f m");
    changed |= ImGui::SliderFloat("Arm cannon up", &offset[2], -kGunOffsetMax, kGunOffsetMax, "%.2f m");
    if (changed) {
      SetGunOffset(offset);
    }
    ImGui::SameLine();
    if (ImGui::Button("Retail##gunoffset")) {
      const float zero[3] = {};
      SetGunOffset(zero);
    }
    ImGui::SetItemTooltip("Back to the retail place.");
    ImGui::TextDisabled("Moves the arm cannon; shots leave from where it is.");
  }
  EndOriginalLocked(locked);

}

// Custom Vulkan drivers (port_gpu_driver.h, Android only). The file dialog answers
// on another thread, so its pick waits in sGpuDriverPick for ProcessGpuDriverPick.
namespace {
std::mutex sGpuDriverPickMutex;
std::optional<std::string> sGpuDriverPick;
std::atomic<bool> sGpuDriverDialogOpen{false};
std::string sGpuDriverStatus;
std::vector<PortGpuDriver::Driver> sGpuDrivers;
bool sGpuDriversListed = false;

void OpenGpuDriverDialog() {
  int windowCount = 0;
  SDL_Window** windows = SDL_GetWindows(&windowCount);
  SDL_Window* window = windows != nullptr && windowCount > 0 ? windows[0] : nullptr;
  SDL_free(windows);
  const SDL_DialogFileCallback done = [](void*, const char* const* files, int) {
    std::lock_guard lock(sGpuDriverPickMutex);
    // Empty = cancelled; a leading \x01 marks a failed dialog's error.
    sGpuDriverPick = files != nullptr ? std::string(files[0] != nullptr ? files[0] : "")
                                      : std::string("\x01") + SDL_GetError();
    sGpuDriverDialogOpen = false;
  };
  sGpuDriverDialogOpen = true;
  SDL_ShowOpenFileDialog(done, nullptr, window, nullptr, 0, nullptr, false);
}

} // namespace

void ProcessGpuDriverPick() {
  std::string path;
  {
    std::lock_guard lock(sGpuDriverPickMutex);
    if (!sGpuDriverPick) {
      return;
    }
    path = std::move(*sGpuDriverPick);
    sGpuDriverPick.reset();
  }
  if (path.empty() || path[0] == '\x01') {
    if (path.size() > 1) {
      sGpuDriverStatus = "The file dialog failed: " + path.substr(1);
    }
    return;
  }
  std::string error;
  const std::string id = PortGpuDriver::Install(path, error);
  sGpuDriversListed = false;
  if (id.empty()) {
    sGpuDriverStatus = "Couldn't install it: " + error + ".";
  } else {
    SetGpuDriver(id);
    sGpuDriverStatus = "Installed " + id + ".";
  }
}

namespace {
void DrawGpuDriver() {
  if (!PortGpuDriver::Supported() || aurora_get_backend() != BACKEND_VULKAN) {
    return;
  }
  if (!sGpuDriversListed) {
    sGpuDrivers = PortGpuDriver::List();
    sGpuDriversListed = true;
  }
  const auto label = [](const PortGpuDriver::Driver& d) {
    return d.version.empty() ? d.name : d.name + " " + d.version;
  };
  std::string preview = "System";
  for (const auto& d : sGpuDrivers) {
    if (d.id == sGpuDriver) {
      preview = label(d);
    }
  }
  if (ImGui::BeginCombo("Vulkan driver", preview.c_str())) {
    if (ImGui::Selectable("System", sGpuDriver.empty())) {
      SetGpuDriver("");
    }
    for (const auto& d : sGpuDrivers) {
      ImGui::PushID(d.id.c_str());
      if (ImGui::Selectable(label(d).c_str(), d.id == sGpuDriver)) {
        SetGpuDriver(d.id);
      }
      if (!d.description.empty()) {
        ImGui::SetItemTooltip("%s", d.description.c_str());
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip(
      "Loads a custom Vulkan driver instead of the phone's, such as Mesa Turnip for\n"
      "Adreno GPUs, from a driver zip. Restart to apply; if it crashes at start, the\n"
      "next start goes back to the system driver.");
  // main() already switched a driver that crashed back to System: nothing to restart for.
  const bool crashedBack = sGpuDriver.empty() && !PortGpuDriver::LoadError().empty();
  if (sGpuDriver != sGpuDriverAtStart && !crashedBack) {
    ImGui::SameLine();
    ImGui::TextColored(ThemeWarnColor(), "Restart to apply");
  }
  ImGui::BeginDisabled(sGpuDriverDialogOpen);
  if (ImGui::Button("Install driver (.zip)...")) {
    sGpuDriverStatus.clear();
    OpenGpuDriverDialog();
  }
  ImGui::EndDisabled();
  const bool removable = !sGpuDriver.empty() && sGpuDriver != PortGpuDriver::Active();
  if (removable) {
    ImGui::SameLine();
    if (ImGui::Button("Remove")) {
      const std::string id = sGpuDriver;
      sGpuDriverStatus = PortGpuDriver::Remove(id) ? "Removed " + id + "." : "Couldn't remove " + id + ".";
      SetGpuDriver("");
      sGpuDriversListed = false;
    }
  }
  if (!PortGpuDriver::LoadError().empty() && !sGpuDriverAtStart.empty()) {
    ImGui::TextColored(ThemeWarnColor(), "%s didn't load: %s", sGpuDriverAtStart.c_str(),
                       PortGpuDriver::LoadError().c_str());
  }
  ImGui::TextDisabled("Running: %s", aurora_get_gpu_driver());
  ImGui::SetItemTooltip("What the GPU reports now. A loaded custom driver says so here\n"
                        "(Turnip: \"Mesa Turnip ...\").");
  if (!sGpuDriverStatus.empty()) {
    ImGui::TextWrapped("%s", sGpuDriverStatus.c_str());
  }
}
} // namespace

void DrawVideoQuality() {
  ImGui::SeparatorText("Quality");
  bool locked = BeginOriginalLocked();
  int msaa = sMsaa >= 4 ? 1 : 0;
  if (ImGui::Combo("Anti-aliasing", &msaa, "Off\0" "4x MSAA\0")) {
    SetMsaa(msaa == 1 ? 4 : 1);
  }
  ImGui::SetItemTooltip("Smooths polygon edges, at about 4x the framebuffer memory.");
  EndOriginalLocked(locked);
  if (ImGui::Checkbox("Show shader compilation", &sShowShaderCompilation)) {
    MarkDirty();
  }
  ImGui::SetItemTooltip("A small progress bar while the shader cache compiles at startup (longest on the first\n"
                        "start after an install or update). Draws whose shader isn't ready yet are skipped.");
  if (ImGui::Checkbox("Show F1 hint at launch", &sShowMenuHint)) {
    MarkDirty();
  }
  ImGui::SetItemTooltip("A note in the top left corner for the first seconds of each launch saying how to\n"
                        "open this panel.");
  locked = BeginOriginalLocked();
  {
    int aniso = 0;
    while ((2 << aniso) <= sAnisotropy && aniso < 4) {
      ++aniso;
    }
    if (ImGui::Combo("Anisotropic filtering", &aniso, "1x\0" "2x\0" "4x\0" "8x\0" "16x\0")) {
      SetAnisotropy(1 << aniso);
    }
    ImGui::SetItemTooltip("Keeps textures sharp at grazing angles. Recommended: 16x.");
    bool perPixel = sPerPixelLighting;
    if (ImGui::Checkbox("Per-pixel lighting", &perPixel)) {
      SetPerPixelLighting(perPixel);
    }
    ImGui::SetItemTooltip(
        "Sums the game's lights per pixel instead of per vertex, like Dolphin: smoother\n"
        "on low-poly models, not the original look. Remastered models already shade per\n"
        "pixel.");
    bool sunShadows = sSunShadows;
    if (ImGui::Checkbox("Sun shadows", &sunShadows)) {
      SetSunShadows(sunShadows);
    }
    ImGui::SetItemTooltip(
        "The port's sun shadow in outdoor rooms. Remastered has none: turn off for its look.");
    bool projShadows = sProjectedShadows;
    if (ImGui::Checkbox("Projected actor shadows", &projShadows)) {
      SetProjectedShadows(projShadows);
    }
    ImGui::SetItemTooltip(
        "Remastered's actor shadow: each actor casts a shadow straight down onto the\n"
        "floor. Needs the Remastered world geometry; otherwise the original blob shadow\n"
        "is drawn.");
  }

  // Dynamic resolution's mode is the two saved settings together: Auto draws up to the
  // window's own scale (auto render scale), User up to the resolution scale set.
  int dynMode = !sDynamicRes ? 0 : sRenderScale <= 0.f ? 1 : 2;
  if (ImGui::Combo("Dynamic resolution", &dynMode, "Off\0" "Auto\0" "User\0")) {
    sDynamicRes = dynMode != 0;
    if (dynMode == 1) {
      SetRenderScale(0.f);
    } else if (dynMode == 2 && sRenderScale <= 0.f) {
      SetRenderScale(std::clamp(std::round(BaseRenderScale() * 4.f) / 4.f, 1.f, 4.f));
    }
    ResetDynamicRes();
    MarkDirty();
  }
  ImGui::SetItemTooltip(
      "Lowers the resolution scale while the GPU can't hold the target frame rate, and\n"
      "raises it back when there is room. Each change stutters for a frame, so it moves\n"
      "only when that is worth it.\n"
      "Auto: up to the window's own resolution. User: between a minimum and a maximum.");
  // Applied when a slider is let go: each new scale reallocates the EFB targets, which
  // a drag would otherwise do every frame.
  static float sPendingScale = 0.f;
  static float sPendingMin = 0.f;
  static int sPendingTarget = -1;
  if (dynMode == 0) {
    bool autoScale = sRenderScale <= 0.f;
    if (ImGui::Checkbox("Auto render scale (native)", &autoScale)) {
      SetRenderScale(autoScale ? 0.f : 1.f);
      MarkDirty();
    }
    ImGui::SetItemTooltip("Draws at the window's own resolution.");
  }
  if (dynMode != 1 && sRenderScale > 0.f) {
    float scale = sPendingScale > 0.f ? sPendingScale : sRenderScale;
    char label[32];
    std::snprintf(label, sizeof(label), "%.2fx (%dp)", scale, DrawnLines(scale));
    if (ImGui::SliderFloat(dynMode == 2 ? "Maximum resolution scale" : "Resolution scale", &scale, 1.f, 4.f, label)) {
      sPendingScale = scale;
    }
    if (sPendingScale > 0.f && !ImGui::IsItemActive()) {
      SetRenderScale(sPendingScale);
      MarkDirty();
      sPendingScale = 0.f;
    }
    ImGui::SetItemTooltip("The resolution the game draws at, as a multiple of the GameCube's 480 lines;\n"
                          "higher values use more GPU memory. Above 2x it supersamples, and each step\n"
                          "costs much more GPU time.");
  } else if (sRenderScale <= 0.f) {
    ImGui::TextDisabled("Window resolution: %.2fx (%dp)", BaseRenderScale(), DrawnLines(0.f));
  }
  if (dynMode != 0) {
    float lowest = sPendingMin > 0.f ? sPendingMin : sDynamicResMin;
    const float top = std::max(DynamicResTop(), PortDebug::kDynamicResMinLowest);
    char label[32];
    std::snprintf(label, sizeof(label), "%.2fx (%dp)", lowest, DrawnLines(lowest));
    if (ImGui::SliderFloat("Minimum resolution scale", &lowest, PortDebug::kDynamicResMinLowest, top, label,
                           ImGuiSliderFlags_AlwaysClamp)) {
      sPendingMin = lowest;
    }
    if (sPendingMin > 0.f && !ImGui::IsItemActive()) {
      sDynamicResMin = sPendingMin;
      sPendingMin = 0.f;
      ResetDynamicRes();
      MarkDirty();
    }
    ImGui::SetItemTooltip("The lowest it drops to. Below 1x it draws fewer pixels than the GameCube did\n"
                          "and looks softer, which lets a phone reach 90 or 120 fps; the lowest values are\n"
                          "for very high resolution displays.");
    // The left end is "Display" (saved as 0).
    constexpr int kTargetMin = 30;
    constexpr int kTargetMax = kFpsCapMax;
    const int saved = sPendingTarget >= 0 ? sPendingTarget : sDynamicResTarget;
    int target = saved > 0 ? std::clamp(saved, kTargetMin, kTargetMax) : kTargetMin - 1;
    if (ImGui::SliderInt("Frame rate target", &target, kTargetMin - 1, kTargetMax,
                         target < kTargetMin ? "Display" : "%d fps", ImGuiSliderFlags_AlwaysClamp)) {
      sPendingTarget = target < kTargetMin ? 0 : target;
    }
    // Applied when let go, like the scales: a change starts the controller over.
    if (sPendingTarget >= 0 && !ImGui::IsItemActive()) {
      sDynamicResTarget = sPendingTarget;
      sPendingTarget = -1;
      ResetDynamicRes();
      MarkDirty();
    }
    ImGui::SetItemTooltip("Display (all the way left): the screen's refresh rate. Never above the frame\n"
                          "rate cap: with the 60 FPS cap on it aims for 60. Ctrl+click to type a value.");
    if (DynamicResTop() <= sDynamicResMin) {
      ImGui::PushTextWrapPos(0.f);
      ImGui::TextColored(ThemeWarnColor(), "Not lowering: the minimum is not below the top scale (%.2fx). "
                         "Lower the minimum%s.", DynamicResTop(), dynMode == 1 ? " or enlarge the window" : "");
      ImGui::PopTextWrapPos();
    }
    const float drawing = sDynScale > 0.f ? sDynScale : DynamicResTop();
    ImGui::Text("Drawing at %.2fx (%dp), target %.0f fps", drawing,
                DrawnLines(sDynScale > 0.f ? sDynScale : sRenderScale), DynamicResTargetFps());
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    const char* reason = PortDynamicRes::ReasonText(sDynController.LastReason());
    if (sDynController.GpuTimesDropped()) {
      ImGui::Text("GPU times not used (none arrived, or lowering on them gained nothing): %s", reason);
    } else if (sDynGpuMs > 0.0) {
      ImGui::Text("GPU %.1f of %.1f ms a frame: %s", sDynGpuMs, 1000.0 / DynamicResTargetFps(), reason);
    } else if (sDynGpuMs < 0.0) {
      ImGui::Text("No GPU times (unsupported, or MP_DYNRES_GPU_TIMES=0); following the frame rate: %s", reason);
    } else {
      ImGui::Text("%s", reason);
    }
    ImGui::PopStyleColor();
  }
  if (dynMode != 0 || sRenderScale > 0.f) {
    int sharpen = sSharpenMode;
    if (ImGui::Combo("Sharpen when upscaled", &sharpen, "Off\0" "With dynamic resolution\0" "Always\0")) {
      sSharpenMode = sharpen;
      ApplyPresentSharpness();
      MarkDirty();
    }
    ImGui::SetItemTooltip(
        "Sharpens a frame drawn smaller than the window, so a lowered scale looks less\n"
        "soft. With dynamic resolution: only while it has lowered the scale. Always:\n"
        "also a fixed low scale on a large window.");
    if (sSharpenMode != 0) {
      if (ImGui::SliderInt("Sharpening", &sSharpenAmount, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) {
        ApplyPresentSharpness();
        MarkDirty();
      }
    }
  }
  EndOriginalLocked(locked);
  ImGui::BeginDisabled(sOriginalExperience);
  bool font = PortHdFont::Enabled();
  if (ImGui::Checkbox("HD font", &font)) {
    PortHdFont::SetEnabled(font);
  }
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("Draws the game's text with a sharp, high-resolution font. Recommended: on.\n"
                        "Not saved: it is on at each start. Off under Original experience.");
}

// Rendering API, driver and workarounds for GPU driver bugs, and the self-test that finds them.
void DrawVideoCompatibility() {
  ImGui::SeparatorText("Compatibility");
  ImGui::PushTextWrapPos(0.f);
  ImGui::TextDisabled("For GPU driver problems, such as a black world or a crash at start. Most of "
                      "these take effect after a restart.");
  ImGui::PopTextWrapPos();
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
  // Which API to ask Aurora for (issue #135). Read at window creation, so it takes a
  // restart: Aurora tries the choice first and falls back to its own order if it
  // won't start, and the setting is left alone. Only this build's choices are listed,
  // so an API it can't start is never offered.
  {
    const std::span<const PortDebug::EGraphicsBackend> choices = PortGraphicsBackend::Choices();
    // The labels in Choices() order, NUL-separated as ImGui wants them, built from the
    // choices so the list cannot drift from the span. Auto asks for nothing, so it is
    // named as such rather than by whatever Aurora picks.
    static const std::string kBackendLabels = [] {
      std::string labels;
      for (const PortDebug::EGraphicsBackend choice : PortGraphicsBackend::Choices()) {
        labels += choice == PortDebug::kGraphicsBackend_Auto
                      ? "Auto"
                      : PortGraphicsBackend::DisplayName(PortGraphicsBackend::ToAurora(choice));
        labels.push_back('\0');
      }
      labels.push_back('\0');
      return labels;
    }();
    // The combo's index is a position in Choices(), never an enum value. A saved choice
    // this build doesn't offer (a file from another platform) shows as Auto; it applies
    // again, untouched, where it is offered.
    int choice = 0;
    for (size_t i = 0; i < choices.size(); ++i) {
      if (choices[i] == sGraphicsBackend) {
        choice = static_cast< int >(i);
        break;
      }
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11.f);
    if (ImGui::Combo("Renderer", &choice, kBackendLabels.c_str(), static_cast< int >(choices.size()))) {
      SetGraphicsBackend(choices[static_cast< size_t >(choice)]);
    }
#if defined(_WIN32)
    ImGui::SetItemTooltip(
        "Which graphics API to start on. Auto tries Direct3D 12, then Vulkan, then\n"
        "Direct3D 11. Restart to apply. If the chosen API can't start, this run falls\n"
        "back to Auto and the setting is kept.");
#else
    ImGui::SetItemTooltip(
        "Which graphics API to start on. Auto uses Vulkan. OpenGL (4.4 or newer) is for\n"
        "GPUs whose Vulkan driver is missing or draws wrong. Restart to apply. If the\n"
        "chosen API can't start, this run falls back to Auto and the setting is kept.");
#endif
    // What this run asked for is the resolution of the STARTUP selection, not the
    // setting: MP_GRAPHICS_BACKEND outranks the file, and changing the combo must not
    // pretend the API this run started on was the one asked for.
    const char* const env = PortGraphicsBackend::EnvRequest();
    const PortGraphicsBackend::Request request = PortGraphicsBackend::Resolve(sGraphicsBackendAtStart, env);
    const AuroraBackend live = aurora_get_backend();
    const PortGraphicsBackend::Status status =
        PortGraphicsBackend::Describe(sGraphicsBackend, sGraphicsBackendAtStart, request, live);
    const char* const requested = PortGraphicsBackend::DisplayName(PortGraphicsBackend::ToAurora(status.requested));
    ImGui::SameLine();
    // The restart notice is about the saved selection, so an override (which wins the
    // next start too, while it is set) reports it in its own note below instead.
    const char* const restartNote =
        status.restartPending && !status.overridden ? " (restart to apply the new selection)" : "";
    if (status.fellBack) {
      ImGui::TextColored(ThemeWarnColor(), "%s could not start; using %s%s", requested,
                         PortGraphicsBackend::DisplayName(live), restartNote);
    } else if (restartNote[0] != '\0') {
      ImGui::TextColored(ThemeWarnColor(), "Restart to apply");
    } else {
      ImGui::TextDisabled("Using %s", PortGraphicsBackend::DisplayName(live));
    }
    if (status.overridden) {
      // The override is what this run asked for, so the setting does nothing to it:
      // say so rather than let a changed combo look like it did. `asks for` also stays
      // true when the override is the one that fell back.
      ImGui::PushTextWrapPos(0.f);
      ImGui::TextDisabled("MP_GRAPHICS_BACKEND=%s asks for %s this run, over the saved selection; a change here "
                          "starts applying once it is unset%s.",
                          env, requested, status.restartPending ? ", and the next start uses it then" : "");
      ImGui::PopTextWrapPos();
    }
  }
#endif
  size_t backendCount = 0;
  const AuroraBackend* backends = aurora_get_available_backends(&backendCount);
  if (std::find(backends, backends + backendCount, BACKEND_OPENGLES) != backends + backendCount) {
    bool gles = sOpenGles;
    if (ImGui::Checkbox("Use OpenGL ES", &gles)) {
      SetOpenGles(gles);
    }
    ImGui::SetItemTooltip("Renders through OpenGL ES instead of Vulkan. Try it if the world draws black\n"
                          "or untextured (some Adreno drivers). Takes effect after a restart; if the\n"
                          "driver crashes starting it, the next start goes back to Vulkan.");
    const AuroraBackend live = aurora_get_backend();
    if (sOpenGles != sOpenGlesAtStart) {
      ImGui::SameLine();
      ImGui::TextColored(ThemeWarnColor(), "Restart to apply");
    } else if (sOpenGles && live != BACKEND_OPENGLES) {
      ImGui::SameLine();
      ImGui::TextColored(ThemeWarnColor(), "OpenGL ES failed to start; using %s",
                         PortGraphicsBackend::DisplayName(live));
    }
  }
  DrawGpuDriver();
  {
    static const char* const kClampNames[] = {"Auto", "Off", "On"};
    int clamp = sStorageClamp + 1;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.f);
    if (ImGui::Combo("Adreno shader fix", &clamp, kClampNames, 3)) {
      sStorageClamp = clamp - 1;
      MarkDirty();
    }
    ImGui::SetItemTooltip(
        "Reads GPU buffers without the bounds check some Adreno Vulkan drivers\n"
        "miscompile (black world, issue #7). Auto turns it on for Adreno 7xx; try On\n"
        "on another Adreno with a black world. Restart to apply.");
    if (sStorageClamp != sStorageClampAtStart) {
      ImGui::SameLine();
      ImGui::TextColored(ThemeWarnColor(), "Restart to apply");
    }
  }
  {
    const int ticked = std::popcount(sDriverWorkarounds);
    char header[64];
    std::snprintf(header, sizeof(header), ticked > 0 ? "Driver workarounds (%d on)###driver-workarounds"
                                                     : "Driver workarounds###driver-workarounds",
                  ticked);
    const bool open = ImGui::TreeNode(header);
    if (sDriverWorkarounds != sDriverWorkaroundsAtStart) {
      ImGui::SameLine();
      ImGui::TextColored(ThemeWarnColor(), "Restart to apply");
    }
    if (open) {
      ImGui::PushTextWrapPos(0.f);
      ImGui::TextDisabled("For a GPU that draws wrong (flickering or missing geometry). Tick one, "
                          "restart, then run the GPU self-test below; leave them off otherwise. If "
                          "a start with them fails, the next start turns them off.");
      if (sDriverWorkaroundsCleared) {
        ImGui::TextColored(ThemeWarnColor(), "Turned off: the last start with them did not reach the game.");
      }
      if (sDriverWorkaroundsEnv != nullptr) {
        ImGui::TextDisabled("MP_DRIVER_WORKAROUNDS=%s replaces these boxes this run.", sDriverWorkaroundsEnv);
      }
      if (sDriverChecksThisRun) {
        ImGui::TextColored(ThemeWarnColor(), "Driver error checks are on this run (slower); the next start "
                                             "turns them off.");
      }
      ImGui::PopTextWrapPos();
      for (size_t i = 0; i < kDriverWorkaroundCount; ++i) {
        bool on = (sDriverWorkarounds & (1u << i)) != 0;
        if (ImGui::Checkbox(kDriverWorkarounds[i].label, &on)) {
          sDriverWorkarounds = on ? sDriverWorkarounds | (1u << i) : sDriverWorkarounds & ~(1u << i);
          MarkDirty();
        }
        ImGui::SetItemTooltip("%s", kDriverWorkarounds[i].tooltip);
      }
      ImGui::TreePop();
    }
  }
  {
    const bool pending = aurora_gpu_selftest_pending();
    ImGui::BeginDisabled(pending);
    if (ImGui::Button("GPU self-test")) {
      RequestGpuSelfTest();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(
        "Renders test patterns offscreen and logs one PASS or FAIL line per feature\n"
        "(vertices, TEV, textures, blend, depth, EFB copy), with checks that say which\n"
        "stage fails. For GPU driver bugs such as a black world; attach the log to a report.");
    char summary[160];
    if (pending) {
      ImGui::SameLine();
      ImGui::TextDisabled("running...");
    } else if (aurora_gpu_selftest_summary(summary, sizeof(summary)) != 0) {
      ImGui::SameLine();
      ImGui::TextUnformatted(summary);
    }
  }
}

void DrawTexturePack();

// The HD texture set and the user's texture pack: the Mods page.
void DrawTexturesSection() {
  ImGui::SeparatorText("Textures");
  ImGui::Text("HD texture set: %s", PortTextures::DeviceName());
  ImGui::SetItemTooltip("Follows the input last used (xbox, playstation, switch,\n"
                        "gamecube, standard, keyboard); set MP_TEXTURE_DEVICE to override.");
  DrawTexturePack();
}

// The user's texture pack, layered over the built-in set (see port_textures.h).
// On Android the folder is picked with the system folder picker and copied into
// the data folder; on the desktop the player fills the folder themselves.
void DrawTexturePack() {
  const char* root = PortTextures::UserRoot();
  if (root[0] == '\0') {
    ImGui::TextUnformatted("No user texture folder.");
    return;
  }
  const size_t count = PortTextures::UserPackCount();
  if (count > 0) {
    ImGui::Text("%zu replacements loaded, over the built-in set.", count);
  } else {
    ImGui::TextUnformatted("No texture pack loaded.");
  }
#if defined(__ANDROID__)
  const std::string status = TexturePackStatus();
  if (!status.empty()) {
    ImGui::TextWrapped("%s", status.c_str());
  }
  if (ImGui::Button("Choose texture pack folder...")) {
    PickTexturePack();
  }
  ImGui::SameLine();
  if (ImGui::Button("Remove texture pack")) {
    PortTextures::RequestUserPackRemoval();
    SetTexturePackStatus("Texture pack removed.");
  }
  ImGui::TextWrapped(
      "The folder is copied into the data folder, so it keeps working if the original is "
      "moved. Pick it again after changing it.");
#else
  ImGui::TextWrapped("Folder: %s", root);
  if (ImGui::Button("Reload texture pack")) {
    PortTextures::RequestUserPackReload();
  }
#endif
}

} // namespace PortDebug
