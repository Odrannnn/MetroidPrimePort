// Part of platform/debug_ui.cpp: The Video page: display, GPU driver, quality, compatibility and
// textures. Included by debug_ui.cpp at this point in the file, not compiled on its own: it shares
// that file's state and namespaces.

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
    MarkDirty();
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
  ImGui::SetItemTooltip("On: the scan visor's magnifier draws the world again through a zoomed\n"
                        "camera, so it shows full detail (and HD textures) at any resolution.\n"
                        "Costs a second world render while the scan visor is up.\n"
                        "Off: the original magnified copy of the screen.");

  ImGui::SeparatorText("HUD and view");
  bool hudWide = sHudWide;
  if (ImGui::Checkbox("Widescreen HUD (spread to edges)", &hudWide)) {
    SetHudWide(hudWide);
    MarkDirty();
  }
  ImGui::SetItemTooltip("Keeps each HUD element's shape but spreads its position so edge elements\n"
                        "reach the wide corners. Only affects the in-game HUD, not menus.");

  int hudScale = sHudScale;
  if (ImGui::SliderInt("HUD scale", &hudScale, kHudScaleMin, kHudScaleMax, "%d%%")) {
    SetHudScale(hudScale);
  }
  int scanSpread = sScanSpread;
  if (ImGui::SliderInt("Scan image spread", &scanSpread, kScanSpreadMin, kScanSpreadMax, "%d%%")) {
    SetScanSpread(scanSpread);
  }
  ImGui::SetItemTooltip("How far the widescreen HUD moves the scan visor's side panels and images\n"
                        "toward the screen edges. 0%% keeps them where they are at 4:3, clear of\n"
                        "touch controls; 100%% puts them at the edges with the rest of the HUD.");
  if (ImGui::SliderInt("Crosshair opacity", &sCrosshairOpacity, kCrosshairOpacityMin,
                       kCrosshairOpacityMax, "%d%%")) {
    MarkDirty();
  }
  ImGui::SetItemTooltip("The crosshair in the middle of the screen (free aim, mouse aim, twin "
                        "stick). 100%% is full strength; the game's own crosshair is much fainter (19%%).");
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
  ImGui::SetItemTooltip("The reticle on the target Lock-On would pick (combat visor), and the ring or\n"
                        "crosshair on it in the X-Ray and Thermal visors. Off hides them; the lock-on\n"
                        "reticle and the scan visor's target boxes stay.");
  bool hideHelmet = sHideHelmet;
  if (ImGui::Checkbox("Hide helmet", &hideHelmet)) {
    SetHideHelmet(hideHelmet);
  }
  ImGui::SameLine();
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
  ImGui::SetItemTooltip("Loads a custom Vulkan driver instead of the phone's, such as Mesa Turnip for\n"
                        "Adreno GPUs. Install one from a driver zip (the kind Android emulators use).\n"
                        "Takes effect after a restart; if it crashes starting, the next start goes\n"
                        "back to the system driver.");
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
  ImGui::SetItemTooltip("What the GPU reports right now. A loaded custom driver says so here\n"
                        "(Turnip: \"Mesa Turnip ...\"); otherwise the phone's driver is in use.");
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
    ImGui::SetItemTooltip("Sums the game's lights per pixel instead of per vertex, like Dolphin's option:\n"
                          "smoother lighting on low-poly models. Not the original look. Remastered models\n"
                          "shade per pixel already.");
    bool sunShadows = sSunShadows;
    if (ImGui::Checkbox("Sun shadows", &sunShadows)) {
      SetSunShadows(sunShadows);
    }
    ImGui::SetItemTooltip("Sun shadows: the port's shadow from the sun (outdoor rooms). Remastered does not\n"
                          "draw sun shadows; turn off for the Remastered look.");
    bool projShadows = sProjectedShadows;
    if (ImGui::Checkbox("Projected actor shadows", &projShadows)) {
      SetProjectedShadows(projShadows);
    }
    ImGui::SetItemTooltip("Remastered's actor shadow: each actor casts a depth map straight down onto the\n"
                          "floor, multiplied over it. Needs the Remastered world geometry; otherwise the\n"
                          "original blob shadow is drawn.");
  }

  bool autoScale = sRenderScale <= 0.f;
  if (ImGui::Checkbox("Auto render scale (native)", &autoScale)) {
    SetRenderScale(autoScale ? 0.f : 1.f);
    MarkDirty();
  }
  if (!autoScale) {
    // Applied when the slider is let go: each new scale reallocates the EFB targets, which
    // a drag would otherwise do every frame.
    static float sPendingScale = 0.f;
    float scale = sPendingScale > 0.f ? sPendingScale : sRenderScale;
    if (ImGui::SliderFloat("EFB scale", &scale, 1.f, 4.f, "%.2fx")) {
      sPendingScale = scale;
    }
    if (sPendingScale > 0.f && !ImGui::IsItemActive()) {
      SetRenderScale(sPendingScale);
      MarkDirty();
      sPendingScale = 0.f;
    }
    ImGui::SetItemTooltip("Scales the internal EFB; higher values use more GPU memory.\n"
                          "Above 2x it supersamples, and each step costs much more GPU time.");
    if (ImGui::Checkbox("Dynamic resolution", &sDynamicRes)) {
      ResetDynamicRes();
      MarkDirty();
    }
    ImGui::SetItemTooltip("Lowers the EFB scale, down to the lowest scale below, while the GPU cannot\n"
                          "hold the target frame rate, and raises it back toward the scale above when\n"
                          "there is room. It times the GPU to tell whether pixels are what is slow (a\n"
                          "CPU-heavy scene is left alone) and goes straight to the scale that fits.\n"
                          "Each change stutters for a frame, so it moves only when that is worth it.");
    if (sDynamicRes) {
      static const int kTargets[] = {0, 30, 60, 90, 120};
      int target = 0;
      for (int i = 0; i < 5; ++i) {
        if (kTargets[i] == sDynamicResTarget) {
          target = i;
        }
      }
      ImGui::SameLine();
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.f);
      if (ImGui::Combo("Target", &target, "Display\0" "30 fps\0" "60 fps\0" "90 fps\0" "120 fps\0")) {
        sDynamicResTarget = kTargets[target];
        ResetDynamicRes();
        MarkDirty();
      }
      ImGui::SetItemTooltip("Display: the screen's refresh rate, or 60 with the 60 FPS cap on.");
      static const float kLowest[] = {0.5f, 0.75f, 1.f};
      int lowest = 2;
      for (int i = 0; i < 3; ++i) {
        if (kLowest[i] == sDynamicResMin) {
          lowest = i;
        }
      }
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.f);
      if (ImGui::Combo("Lowest scale", &lowest, "0.5x\0" "0.75x\0" "1x\0")) {
        sDynamicResMin = kLowest[lowest];
        ResetDynamicRes();
        MarkDirty();
      }
      ImGui::SetItemTooltip("Below 1x it draws fewer pixels than the GameCube did and looks softer,\n"
                            "which lets a phone reach 90 or 120 fps.");
      ImGui::Text("Drawing at %.2fx (target %.0f fps)", sDynScale > 0.f ? sDynScale : sRenderScale,
                  DynamicResTargetFps());
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
    int sharpen = sSharpenMode;
    if (ImGui::Combo("Sharpen when upscaled", &sharpen, "Off\0" "With dynamic resolution\0" "Always\0")) {
      sSharpenMode = sharpen;
      ApplyPresentSharpness();
      MarkDirty();
    }
    ImGui::SetItemTooltip("Contrast-adaptive sharpening for a frame drawn smaller than the window and\n"
                          "stretched to it, so a lowered scale looks less soft. With dynamic\n"
                          "resolution: only while it has lowered the scale. Always: also a fixed low\n"
                          "scale on a large window. A frame at or above the window's size is not touched.");
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
  ImGui::TextDisabled("For GPU driver problems, such as a black world or a crash at start. Most of these "
                      "take effect after a restart.");
  ImGui::PopTextWrapPos();
#if defined(_WIN32)
  // Which API to ask Aurora for (issue #135). Read at window creation, so it takes a
  // restart: Aurora tries the choice first and falls back to its own order if it
  // won't start, and the setting is left alone.
  {
    static const char* const kBackendNames[] = {"Auto", "Direct3D 12", "Vulkan", "Direct3D 11"};
    int choice = static_cast< int >(sGraphicsBackend);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11.f);
    if (ImGui::Combo("Renderer", &choice, kBackendNames, PortDebug::kGraphicsBackendCount)) {
      SetGraphicsBackend(static_cast< PortDebug::EGraphicsBackend >(choice));
    }
    ImGui::SetItemTooltip("Which graphics API to start on. Auto tries Direct3D 12, then Vulkan, then\n"
                          "Direct3D 11. Takes effect after a restart. If the driver can't start the chosen\n"
                          "API, this run falls back to Auto and the setting is kept, so it can be changed\n"
                          "here. MP_GRAPHICS_BACKEND=auto|d3d12|vulkan|d3d11 overrides it for one run.");
    // What this run asked for is the resolution of the STARTUP selection, not the
    // setting: MP_GRAPHICS_BACKEND outranks the file, and changing the combo must not
    // pretend the API this run started on was the one asked for.
    const char* const env = std::getenv("MP_GRAPHICS_BACKEND");
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
    ImGui::SetItemTooltip("Reads GPU buffers without the bounds-check branch some Adreno Vulkan drivers\n"
                          "miscompile (the world draws black, issue #7). Auto turns it on for Adreno 7xx GPUs;\n"
                          "try On if the world is black on another Adreno. Takes effect after a restart.");
    if (sStorageClamp != sStorageClampAtStart) {
      ImGui::SameLine();
      ImGui::TextColored(ThemeWarnColor(), "Restart to apply");
    }
  }
  {
    bool software = sSoftwareRendering;
    if (ImGui::Checkbox("Software rendering (next start only, very slow)", &software)) {
      sSoftwareRendering = software;
      MarkDirty();
    }
    ImGui::SetItemTooltip("Draws the next start on the CPU (WARP on Windows) instead of the GPU, at a few frames a\n"
                          "second. If a problem is gone there, it is the GPU driver's; if it is still there, it is\n"
                          "the port's. Run the GPU self-test while on it. The start after that uses the GPU again.");
    if (aurora_software_adapter()) {
      ImGui::SameLine();
      ImGui::TextColored(ThemeWarnColor(), "On this run");
    } else if (sSoftwareRenderingThisRun) {
      ImGui::SameLine();
      ImGui::TextColored(ThemeWarnColor(), "No software renderer started; using the GPU");
    } else if (sSoftwareRendering) {
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
      ImGui::TextDisabled("For a GPU that draws wrong, such as flickering or missing geometry. Tick one, "
                          "restart, then run the GPU self-test below; leave them off otherwise. If a start "
                          "with them fails, the next start turns them off.");
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
    ImGui::SetItemTooltip("Renders known patterns offscreen through the game's own GX path, reads them back\n"
                          "and logs one PASS or FAIL line per feature (indexed vertices, TEV, textures, blend,\n"
                          "depth, EFB copy), plus checks of the readback and EFB copy alone that say which\n"
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
