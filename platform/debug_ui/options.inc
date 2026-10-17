// Part of platform/debug_ui.cpp: Accessors for the settings and runtime switches the game and other
// port code read, and the settings self-test. Included by debug_ui.cpp at this point in the file,
// not compiled on its own: it shares that file's state and namespaces.

// State only this part uses.
namespace {
float sTickPeriod = 1.f / 60.f;
// Wall-clock for the same span as sTimingNs, so presented frames can be divided
// by elapsed time rather than by the frames' own cost. Zero on the first call,
// which is why the delta is skipped until a previous reading exists.
uint64_t sTimingWallNs = 0;
uint64_t sTimingWallLastNs = 0;
unsigned sTimingFrames = 0, sTimingTicks = 0;
// Dynamic resolution (port_dynamic_res.h); sDynScale, the scale it draws at, is shared.
PortDynamicRes::Controller sDynController;
bool sDynActive = false;
double sDynGpuMs = -1.0; // the last GPU time it was given, for the Video page
// GPU pass times (Aurora's gpu_prof) are wanted by dynamic resolution and by the
// Remastered debug panel. Switching them on restarts their average, so only a change
// in whether anyone wants them reaches Aurora.
unsigned sGpuTimesUsers = 0;
float sTwinStickRightY = 0.f;
} // namespace

int SettingsSelfTest() {
  int failures = 0;
  int sampled = 0;
  // Each sampled key, through the file reader: both values must come back as written.
  for (const SettingKey& entry : kSettingKeys) {
    if (entry.key == nullptr || entry.write == nullptr || entry.sampleA == nullptr) {
      continue;
    }
    ++sampled;
    for (const char* sample : {entry.sampleA, entry.sampleB}) {
      std::istringstream line(std::string(entry.key) + "=" + sample + "\n");
      ApplySettingsText(line);
      const auto written = SettingsValues(SettingsText());
      const auto found = written.find(entry.key);
      const std::string got = found == written.end() ? std::string() : found->second;
      if (got != sample) {
        std::fprintf(stderr, "settings self-test: %s=%s reads back as %s=%s\n", entry.key, sample, entry.key,
                     got.c_str());
        ++failures;
      }
    }
  }
  // Every line written is a key something reads, and reading the whole file
  // back writes it unchanged.
  const std::string first = SettingsText();
  for (const auto& [key, value] : SettingsValues(first)) {
    if (!ApplySetting(key, value)) {
      std::fprintf(stderr, "settings self-test: %s is written but nothing reads it\n", key.c_str());
      ++failures;
    }
  }
  std::istringstream again(first);
  ApplySettingsText(again);
  const std::string second = SettingsText();
  if (second != first) {
    std::fprintf(stderr, "settings self-test: the file changes when read back\n%s\n---\n%s\n", first.c_str(),
                 second.c_str());
    ++failures;
  }
  std::fprintf(stderr, "settings self-test: %d keys sampled, %zu lines round-tripped, %d failure(s)\n", sampled,
               SettingsValues(first).size(), failures);
  return failures == 0 ? 0 : 1;
}

bool FastBoot() {
  EnsureInitialized();
  return sFastBoot;
}

bool BootWorld(uint32_t& worldId, uint32_t& areaAssetId) {
  static uint32_t sWorld = 0, sArea = 0;
  static const bool sSet = [] {
    const char* value = std::getenv("MP_BOOT_WORLD");
    if (value == nullptr) {
      return false;
    }
    char* end = nullptr;
    sWorld = static_cast< uint32_t >(std::strtoul(value, &end, 16));
    if (end != nullptr && *end == ':') {
      sArea = static_cast< uint32_t >(std::strtoul(end + 1, nullptr, 16));
    }
    return sWorld != 0;
  }();
  worldId = sWorld;
  areaAssetId = sArea;
  return sSet;
}

bool SkipCutscenes() {
  EnsureInitialized();
  return sSkipCutscenes;
}

float CutsceneSpeed() {
  EnsureInitialized();
  return sCutsceneSpeed;
}

unsigned SimRate() {
  EnsureInitialized();
  return sOriginalExperience ? 60u : sSimRate;
}

void SetSimRate(unsigned hz) {
  EnsureInitialized();
  if (hz < 30u || hz > 480u) {
    return;
  }
  sSimRate = hz;
  MarkDirty();
}

float SimPeriod() { return 1.f / static_cast< float >(SimRate()); }

bool SimAdaptive() {
  EnsureInitialized();
  return sSimAdaptive && !sOriginalExperience;
}

bool Turbo() {
  EnsureInitialized();
  return sTurbo;
}

unsigned TurboTicks() {
  EnsureInitialized();
  return sTurbo ? sTurboTicks : 1;
}

void SetSimAdaptive(bool enabled) {
  EnsureInitialized();
  sSimAdaptive = enabled;
  MarkDirty();
}

float TickPeriod() {
  EnsureInitialized();
  return sTickPeriod;
}

void SetTickPeriod(float dt) {
  if (std::isfinite(dt) && dt > 0.f) {
    sTickPeriod = dt;
  }
}

float TickFrames() { return sTickPeriod * 60.f; }

// What the frame code uses: the settings, or retail's under Original experience.
static bool EffectiveFrameLimit() { return sFrameLimitEnabled || sOriginalExperience; }
// Photo mode's supersampled capture holds the EFB at this scale for a few frames; never saved.
static float sCaptureScale = 0.f;
static float EffectiveRenderScale() {
  return sCaptureScale > 0.f ? sCaptureScale : sOriginalExperience ? 1.f : sRenderScale;
}
static int EffectiveMsaa() { return sOriginalExperience ? 1 : sMsaa; }
static int EffectiveAnisotropy() { return sOriginalExperience ? 1 : sAnisotropy; }

// Sharpening for a frame stretched up to the window (Aurora's present resample); only
// passed on when it changes.
static void ApplyPresentSharpness() {
  float amount = 0.f;
  if (!sOriginalExperience && sSharpenAmount > 0) {
    const bool lowered = sDynScale > 0.f && sDynScale < EffectiveRenderScale();
    if (sSharpenMode == 2 || (sSharpenMode == 1 && lowered)) {
      amount = static_cast< float >(sSharpenAmount) / 100.f;
    }
  }
  static float sApplied = -1.f;
  if (amount != sApplied) {
    sApplied = amount;
    aurora_set_present_sharpness(amount);
  }
}

static void ApplyGraphicsQuality() {
  aurora_set_graphics_quality(static_cast< uint32_t >(EffectiveMsaa()),
                              static_cast< uint16_t >(EffectiveAnisotropy()));
  aurora_set_per_pixel_lighting(sPerPixelLighting && !sOriginalExperience);
  ApplyPresentSharpness();
}

bool FrameLimitEnabled() {
  EnsureInitialized();
  return EffectiveFrameLimit();
}

int FrameRateCap() {
  EnsureInitialized();
  return EffectiveFrameLimit() ? 60 : sFpsCap;
}


static double DynamicResTargetFps() {
  double target = sDynamicResTarget;
  if (target <= 0.0) {
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    target = mode != nullptr && mode->refresh_rate > 0.f ? mode->refresh_rate : 60.0;
  }
  const int cap = EffectiveFrameLimit() ? 60 : sFpsCap;
  return cap > 0 ? std::min(target, static_cast< double >(cap)) : target;
}

enum : unsigned { kGpuTimesDynamicRes = 1, kGpuTimesPanel = 2 };
void WantGpuTimes(unsigned user, bool on) {
  const unsigned before = sGpuTimesUsers;
  sGpuTimesUsers = on ? (before | user) : (before & ~user);
  if ((before != 0) != (sGpuTimesUsers != 0)) {
    GXPortSetGpuTimes(sGpuTimesUsers != 0 ? GX_TRUE : GX_FALSE);
  }
}

static void ResetDynamicRes() {
  sDynController.Reset(EffectiveRenderScale());
  sDynGpuMs = -1.0;
  if (sDynScale > 0.f) {
    sDynScale = 0.f;
    VISetFrameBufferScale(EffectiveRenderScale());
  }
  ApplyPresentSharpness();
}

// Once a second, from the presented rate and, where the GPU can time itself, the GPU's
// time per frame (gpu_prof's span). The controller (port_dynamic_res.cpp) decides; each
// change reallocates the EFB and stutters for a frame.
static void UpdateDynamicRes(double fps, unsigned frames) {
  if (sCaptureScale > 0.f) {
    return;
  }
  const float top = EffectiveRenderScale();
  const float bottom = std::min(sDynamicResMin, top);
  const bool active = sDynamicRes && !sOriginalExperience && top > bottom && !sTurbo;
  // MP_DYNRES_GPU_TIMES=0 steers on the frame rate alone, as on a GPU without timestamps.
  static const bool gpuTimesAllowed = port::EnvFlag("MP_DYNRES_GPU_TIMES", true);
  const bool gpuTimes = gpuTimesAllowed && GXPortGpuTimesSupported() == GX_TRUE;
  WantGpuTimes(kGpuTimesDynamicRes, active && gpuTimes);
  if (!active) {
    if (sDynActive) {
      sDynActive = false;
      ResetDynamicRes();
    }
    return;
  }
  if (!sDynActive) {
    sDynActive = true;
    ResetDynamicRes();
  }
  PortDynamicRes::Sample sample;
  sample.fps = fps;
  sample.frames = frames;
  sample.targetFps = DynamicResTargetFps();
  sample.top = top;
  sample.bottom = bottom;
  if (gpuTimes) {
    float total = 0.f;
    float span = 0.f;
    GXPortGetGpuTimes(nullptr, 0, &total, &span);
    sample.gpuMs = span;
    sample.gpuPassMs = span > 0.f ? total : -1.0;
  }
  sDynGpuMs = sample.gpuMs;
  const PortDynamicRes::Decision decision = sDynController.Update(sample);
  if (decision.changed) {
    sDynScale = decision.scale;
    VISetFrameBufferScale(decision.scale);
    ApplyPresentSharpness();
    PortLog::Write("port: dynamic resolution %.2fx (%.0f fps, target %.0f, GPU %.1f ms): %s\n", decision.scale,
                   fps, sample.targetFps, sample.gpuMs, PortDynamicRes::ReasonText(decision.reason));
  }
}

void RecordFrame(uint64_t durationNs, unsigned ticks, bool presented) {
  sMainLoopRan = true;
  sTimingNs += durationNs;
  sTimingTicks += ticks;
  if (presented) ++sTimingFrames;
  // Wall-clock time for the same span, kept separately from the frame's own
  // duration. Dividing presented frames by the frame's CPU time reports
  // throughput, which is what the process can *produce*; dividing by wall time
  // reports what reaches the screen. The two agree while the 60 Hz cap is
  // waiting more than the frame costs, and part company exactly when a frame
  // overruns its budget - which is the case someone opens the Video > Frame rate page
  // to diagnose. One of the two numbers without the other is misleading there.
  {
    const uint64_t nowNs = SDL_GetTicksNS();
    if (sTimingWallLastNs != 0) {
      sTimingWallNs += nowNs - sTimingWallLastNs;
    }
    sTimingWallLastNs = nowNs;
  }
  // Also on wall time: a light frame under the cap took several seconds to
  // gather one second of work, so the readout lagged that far behind.
  if (sTimingNs >= 1000000000ull || sTimingWallNs >= 1000000000ull) {
    const double seconds = static_cast<double>(sTimingNs) / 1000000000.0;
    const double wallSeconds = static_cast<double>(sTimingWallNs) / 1000000000.0;
    sActualFps = sTimingFrames / (wallSeconds > 0.0 ? wallSeconds : seconds);
    sThroughputFps = sTimingFrames / seconds;
    // Ticks per wall second: the simulation's rate is what the player sees, not
    // what it could reach. Over the frame's own time it read above the target
    // whenever the cap or vsync left the loop idle.
    sActualTps = sTimingTicks / (wallSeconds > 0.0 ? wallSeconds : seconds);
    if (sTraceTiming) {
      std::fprintf(stderr,
                   "[timing] presented=%.1f FPS throughput=%.1f FPS simulation=%.1f ticks/s cap=%d\n",
                   sActualFps, sThroughputFps, sActualTps, EffectiveFrameLimit() ? 60 : sFpsCap);
    }
    UpdateDynamicRes(sActualFps, sTimingFrames);
    sTimingNs = 0;
    sTimingWallNs = 0;
    sTimingFrames = sTimingTicks = 0;
  }
}

void SetFrameLimitEnabled(bool enabled) {
  EnsureInitialized();
  // Original experience holds the cap; F10 must not change what it restores.
  if (sOriginalExperience) {
    return;
  }
  if (sFrameLimitEnabled != enabled) {
    sFrameLimitEnabled = enabled;
    MarkDirty();
  }
}

bool VsyncEnabled() {
  EnsureInitialized();
  return sVsyncEnabled;
}

void SetVsyncEnabled(bool enabled) {
  EnsureInitialized();
  // Always re-apply: the stored value can match while the surface still has the
  // previous present mode (e.g. the persisted value applied before the first
  // frame), which made the first toggle a no-op.
  sVsyncEnabled = enabled;
  aurora_enable_vsync(enabled);
}

bool Fullscreen() {
  EnsureInitialized();
  return sFullscreen;
}

void SetFullscreen(bool enabled) {
  EnsureInitialized();
  if (sFullscreen != enabled) {
    sFullscreen = enabled;
    MarkDirty();
  }
  PortLog::Write("metroid_prime_port: fullscreen %s\n", enabled ? "on" : "off");
  VISetWindowFullscreen(enabled);
}

float RenderScale() {
  EnsureInitialized();
  return EffectiveRenderScale();
}

void SetRenderScale(float scale) {
  EnsureInitialized();
  if (sRenderScale == scale) {
    return;
  }
  sRenderScale = scale;
  sDynScale = 0.f;
  ResetDynamicRes();
  VISetFrameBufferScale(EffectiveRenderScale());
}

SDL_Window* MainWindow();

float BaseRenderScale() {
  EnsureInitialized();
  if (sOriginalExperience) {
    return 1.f;
  }
  if (sRenderScale > 0.f) {
    return sRenderScale;
  }
  // Auto draws at the window's pixel size; as a multiple of the 480-line base.
  int width = 0, height = 0;
  SDL_Window* window = MainWindow();
  if (window != nullptr && SDL_GetWindowSizeInPixels(window, &width, &height) && height > 0) {
    return static_cast< float >(height) / 480.f;
  }
  return 1.f;
}

void SetCaptureRenderScale(float scale) {
  EnsureInitialized();
  const float next = scale > 0.f ? scale : 0.f;
  if (sCaptureScale == next) {
    return;
  }
  sCaptureScale = next;
  sDynScale = 0.f;
  VISetFrameBufferScale(EffectiveRenderScale());
}

bool DynamicRes() {
  EnsureInitialized();
  return sDynamicRes && !sOriginalExperience;
}

int DynamicResTarget() {
  EnsureInitialized();
  return sDynamicResTarget;
}

float DynamicResMin() {
  EnsureInitialized();
  return sDynamicResMin;
}

EAspectMode AspectMode() {
  EnsureInitialized();
  return sOriginalExperience ? kAspect_4_3 : sAspectMode;
}

void SetAspectMode(EAspectMode mode) {
  EnsureInitialized();
  sAspectMode = mode;
  MarkDirty();
}

bool HudWide() {
  EnsureInitialized();
  return sHudWide && !sOriginalExperience;
}

void SetHudWide(bool enabled) {
  EnsureInitialized();
  sHudWide = enabled;
  MarkDirty();
}

bool CinemaBars() {
  EnsureInitialized();
  return sCinemaBars || sOriginalExperience;
}

bool SharpScanWindow() {
  EnsureInitialized();
  return sSharpScanWindow && !sOriginalExperience;
}

const char* PromptGlyphs() {
  const int glyphs = sPromptGlyphs.load(std::memory_order_relaxed);
  return glyphs > 0 ? kPromptGlyphs[glyphs] : nullptr;
}

int HudScale() {
  EnsureInitialized();
  return sOriginalExperience ? kHudScaleMax : sHudScale;
}

void SetHudScale(int percent) {
  EnsureInitialized();
  sHudScale = std::clamp(percent, kHudScaleMin, kHudScaleMax);
  MarkDirty();
}

int ScanSpread() {
  EnsureInitialized();
  return sOriginalExperience ? kScanSpreadMax : sScanSpread;
}

void SetScanSpread(int percent) {
  EnsureInitialized();
  sScanSpread = std::clamp(percent, kScanSpreadMin, kScanSpreadMax);
  MarkDirty();
}

bool AutoAim() {
  EnsureInitialized();
  return sAutoAim || sOriginalExperience;
}

float ScanSpeed() {
  EnsureInitialized();
  return sOriginalExperience ? 1.f : static_cast< float >(sScanSpeed) / 100.f;
}

bool MenuStandardButtons() {
  EnsureInitialized();
  return sMenuStandardButtons;
}

void SetMenuStandardButtons(bool enabled) {
  EnsureInitialized();
  sMenuStandardButtons = enabled;
  MarkDirty();
}

void SetPointerMenus(bool enabled) {
  EnsureInitialized();
  PortPointer::SetEnabled(enabled);
  MarkDirty();
}

bool SecondaryCrosshairs() {
  EnsureInitialized();
  return sSecondaryCrosshairs || sOriginalExperience;
}

int CrosshairOpacity() {
  EnsureInitialized();
  return sOriginalExperience ? kCrosshairOpacityMax : sCrosshairOpacity;
}

bool CrosshairColor(float rgb[3]) {
  EnsureInitialized();
  if (!sCrosshairCustomColor || sOriginalExperience) {
    return false;
  }
  std::copy_n(sCrosshairColor, 3, rgb);
  return true;
}

bool HideHelmet() {
  EnsureInitialized();
  return (sHideHelmet || PortPhoto::Active()) && !sOriginalExperience;
}

void SetHideHelmet(bool enabled) {
  EnsureInitialized();
  sHideHelmet = enabled;
  MarkDirty();
}

bool HideVisorEffects() {
  EnsureInitialized();
  return (sHideVisorEffects || PortPhoto::Active()) && !sOriginalExperience;
}

void SetHideVisorEffects(bool enabled) {
  EnsureInitialized();
  sHideVisorEffects = enabled;
  MarkDirty();
}

bool RevealMap() {
  EnsureInitialized();
  return sRevealMap && !sOriginalExperience;
}

void SetRevealMap(bool enabled) {
  EnsureInitialized();
  sRevealMap = enabled;
  MarkDirty();
}

EMapPickups MapPickups() {
  EnsureInitialized();
  return static_cast< EMapPickups >(sMapPickups);
}

void SetMapPickups(EMapPickups mode) {
  EnsureInitialized();
  sMapPickups = mode >= kMapPickups_Off && mode <= kMapPickups_Rooms ? mode : kMapPickups_Off;
  MarkDirty();
}

bool MapLogicColors() {
  EnsureInitialized();
  return sMapLogicColors;
}

void SetMapLogicColors(bool enabled) {
  EnsureInitialized();
  sMapLogicColors = enabled;
  MarkDirty();
}

bool MapPickupKinds() {
  EnsureInitialized();
  return sMapPickupKinds;
}

void SetMapPickupKinds(bool enabled) {
  EnsureInitialized();
  sMapPickupKinds = enabled;
  MarkDirty();
}

bool MapStationPickups() {
  EnsureInitialized();
  return sMapStationPickups;
}

void SetMapStationPickups(bool enabled) {
  EnsureInitialized();
  sMapStationPickups = enabled;
  MarkDirty();
}

bool MapExtraUi() {
  EnsureInitialized();
  return sMapExtraUi;
}

void SetMapExtraUi(bool enabled) {
  EnsureInitialized();
  sMapExtraUi = enabled;
  MarkDirty();
}

int ApSuitDamage() {
  EnsureInitialized();
  return sApSuitDamage;
}

void SetApSuitDamage(int mode) {
  EnsureInitialized();
  sApSuitDamage = mode >= 0 && mode <= 2 ? mode : 1;
  MarkDirty();
}

bool SkippableCutscenes() {
  EnsureInitialized();
  return sSkippableCutscenes && !sOriginalExperience;
}

void SetSkippableCutscenes(bool enabled) {
  EnsureInitialized();
  sSkippableCutscenes = enabled;
  MarkDirty();
}

bool Revision100Compatibility() {
  EnsureInitialized();
  return sRevision100Compatibility;
}

void SetRevision100Compatibility(bool enabled) {
  EnsureInitialized();
  sRevision100Compatibility = enabled;
  MarkDirty();
}

const char* TextLanguage() {
  EnsureInitialized();
  for (size_t i = 0; i < PortRemastered::kTextLanguageCount; ++i) {
    if (sTextLanguage == PortRemastered::kTextLanguages[i].code) {
      return PortRemastered::kTextLanguages[i].code;
    }
  }
  return "";
}

void SetTextLanguage(const char* code) {
  EnsureInitialized();
  sTextLanguage = code;
  MarkDirty();
}

EElevatorRide ElevatorRide() {
  EnsureInitialized();
  return sOriginalExperience ? kElevatorRide_Original : static_cast< EElevatorRide >(sElevatorRide);
}

void SetElevatorRide(EElevatorRide mode) {
  EnsureInitialized();
  sElevatorRide = mode >= kElevatorRide_Original && mode <= kElevatorRide_Skip ? mode
                                                                               : kElevatorRide_Original;
  MarkDirty();
}

EBootLogos BootLogos() {
  EnsureInitialized();
  return sOriginalExperience ? kBootLogos_Show : static_cast< EBootLogos >(sBootLogos);
}

void SetBootLogos(EBootLogos mode) {
  EnsureInitialized();
  sBootLogos = mode >= kBootLogos_Show && mode <= kBootLogos_Off ? mode : kBootLogos_Skippable;
  MarkDirty();
}

float FirstPersonFov() {
  EnsureInitialized();
  return sOriginalExperience ? kFovRetail : sFirstPersonFov;
}

void SetFirstPersonFov(float degrees) {
  EnsureInitialized();
  if (std::isfinite(degrees)) {
    sFirstPersonFov = std::clamp(degrees, kFovMin, kFovMax);
    MarkDirty();
  }
}

void GunOffset(float out[3]) {
  EnsureInitialized();
  for (int i = 0; i < 3; ++i) {
    out[i] = sOriginalExperience ? 0.f : sGunOffset[i];
  }
}

void SetGunOffset(const float offset[3]) {
  EnsureInitialized();
  for (int i = 0; i < 3; ++i) {
    if (std::isfinite(offset[i])) {
      sGunOffset[i] = std::clamp(offset[i], -kGunOffsetMax, kGunOffsetMax);
    }
  }
  MarkDirty();
}

int Msaa() {
  EnsureInitialized();
  return EffectiveMsaa();
}

void SetMsaa(int samples) {
  EnsureInitialized();
  samples = samples >= 4 ? 4 : 1;
  if (sMsaa != samples) {
    sMsaa = samples;
    ApplyGraphicsQuality();
    MarkDirty();
  }
}

bool OpenGles() {
  EnsureInitialized();
  return sOpenGles;
}

void SetOpenGles(bool enabled) {
  EnsureInitialized();
  if (sOpenGles != enabled) {
    sOpenGles = enabled;
    MarkDirty();
  }
}

PortDebug::EGraphicsBackend GraphicsBackend() {
  EnsureInitialized();
  return sGraphicsBackend;
}

void SetGraphicsBackend(PortDebug::EGraphicsBackend backend) {
  EnsureInitialized();
  if (sGraphicsBackend != backend) {
    sGraphicsBackend = backend;
    MarkDirty();
  }
}

const std::string& GpuDriver() {
  EnsureInitialized();
  return sGpuDriver;
}

void SetGpuDriver(const std::string& id) {
  EnsureInitialized();
  if (sGpuDriver != id) {
    sGpuDriver = id;
    MarkDirty();
  }
}

int Anisotropy() {
  EnsureInitialized();
  return EffectiveAnisotropy();
}
void ApplyStorageClamp() {
  EnsureInitialized();
  if (sStorageClamp < 0 || std::getenv("MP_STORAGE_CLAMP") != nullptr) {
    return;
  }
#ifdef _WIN32
  _putenv_s("MP_STORAGE_CLAMP", sStorageClamp > 0 ? "1" : "0");
#else
  setenv("MP_STORAGE_CLAMP", sStorageClamp > 0 ? "1" : "0", 1);
#endif
  sStorageClampEnvFromSetting = true;
}


void SetAnisotropy(int level) {
  EnsureInitialized();
  level = std::clamp(level, 1, 16);
  if (sAnisotropy != level) {
    sAnisotropy = level;
    ApplyGraphicsQuality();
    MarkDirty();
  }
}

bool PerPixelLighting() {
  EnsureInitialized();
  return sPerPixelLighting && !sOriginalExperience;
}
void SetPerPixelLighting(bool enabled) {
  EnsureInitialized();
  if (sPerPixelLighting != enabled) {
    sPerPixelLighting = enabled;
    ApplyGraphicsQuality();
    MarkDirty();
  }
}

bool SunShadows() {
  EnsureInitialized();
  return sSunShadows;
}
void SetSunShadows(bool enabled) {
  EnsureInitialized();
  if (sSunShadows != enabled) {
    sSunShadows = enabled;
    MarkDirty();
  }
}

bool ProjectedShadows() {
  EnsureInitialized();
  return sProjectedShadows;
}
void SetProjectedShadows(bool enabled) {
  EnsureInitialized();
  if (sProjectedShadows != enabled) {
    sProjectedShadows = enabled;
    MarkDirty();
  }
}

bool UnlockHardMode() {
  EnsureInitialized();
  return sUnlockHardMode && !sOriginalExperience;
}

void SetUnlockHardMode(bool enabled) {
  EnsureInitialized();
  sUnlockHardMode = enabled;
  MarkDirty();
}

bool UnlockFusionSuit() {
  EnsureInitialized();
  return sUnlockFusionSuit && !sOriginalExperience;
}

void SetUnlockFusionSuit(bool enabled) {
  EnsureInitialized();
  sUnlockFusionSuit = enabled;
  MarkDirty();
}

bool UnlockGalleries() {
  EnsureInitialized();
  return sUnlockGalleries && !sOriginalExperience;
}

void SetUnlockGalleries(bool enabled) {
  EnsureInitialized();
  sUnlockGalleries = enabled;
  MarkDirty();
}

bool MouseAim() {
  EnsureInitialized();
  return sInput.mouseAim;
}

void SetMouseAim(bool enabled) {
  EnsureInitialized();
  sInput.mouseAim = enabled;
  ResetMouseAim();
}

bool TouchDirectAim() {
#if defined(__ANDROID__)
  return !sInput.touchClassic && sTouchActive.load(std::memory_order_acquire) && !Visible();
#else
  return false;
#endif
}

bool TouchActive() {
#if defined(__ANDROID__)
  return sTouchActive.load(std::memory_order_acquire) && !Visible();
#else
  return false;
#endif
}

bool DirectAim() { return MouseAim() || TwinStick() || TouchDirectAim(); }

// The pad-preset getters below read as the GameCube preset (off / unbound) while touch is in use: the
// touch overlay always does what its buttons say. The F1 menu (Visible) sees the stored values.
bool TwinStick() {
  EnsureInitialized();
#if defined(__ANDROID__)
  // Touch has its own twin stick (the right stick aims), apart from the pad preset.
  if (TouchActive()) {
    return sInput.touchTwinStick;
  }
#endif
  return sInput.twinStick && !TouchActive();
}

bool PadTwinStick() {
  EnsureInitialized();
  return sInput.twinStick;
}

void SetTwinStick(bool enabled) {
  EnsureInitialized();
  sInput.twinStick = enabled;
  MarkDirty();
}

float TwinStickRightY() { return sTwinStickRightY; }

void SetTwinStickRightY(float y) { sTwinStickRightY = y; }

bool TouchBeamShift() { return sTouchBeamShift.load(std::memory_order_acquire); }

bool TouchTurboFire() {
  return sTouchTurboFire.load(std::memory_order_acquire) && TouchActive() && !sOriginalExperience;
}

bool SpringBall() {
  EnsureInitialized();
  return sInput.springBall && !sOriginalExperience;
}

void SetSpringBall(bool enabled) {
  EnsureInitialized();
  sInput.springBall = enabled;
  MarkDirty();
}

bool SwapScanXray() {
  EnsureInitialized();
  return sInput.swapScanXray && !TouchActive() && !sOriginalExperience;
}

void SetSwapScanXray(bool enabled) {
  EnsureInitialized();
  sInput.swapScanXray = enabled;
  MarkDirty();
}

int TurboBinding(int slot) {
  EnsureInitialized();
  if (slot == 2 && TouchActive()) {
    return -1;
  }
  return slot >= 0 && slot < 3 ? sInput.turboBindings[slot] : -1;
}

void SetTurboBinding(int slot, int code) {
  EnsureInitialized();
  if (slot >= 0 && slot < 3) {
    sInput.turboBindings[slot] = code;
    MarkDirty();
  }
}

int ShiftBinding(int slot) {
  EnsureInitialized();
  if (slot == 2 && TouchActive()) {
    return -1;
  }
  return slot >= 0 && slot < 3 ? sInput.shiftBindings[slot] : -1;
}

void SetShiftBinding(int slot, int code) {
  EnsureInitialized();
  if (slot >= 0 && slot < 3) {
    sInput.shiftBindings[slot] = code;
    MarkDirty();
  }
}

int PadAltButton(int bit) {
  EnsureInitialized();
  return bit >= 0 && bit < kPadAltCount && !TouchActive() ? sInput.padAltButtons[bit] : -1;
}

void SetPadAltButton(int bit, int code) {
  EnsureInitialized();
  if (bit >= 0 && bit < kPadAltCount && sInput.padAltButtons[bit] != code) {
    sInput.padAltButtons[bit] = code;
    MarkDirty();
  }
}

int MouseAction(int button) {
  EnsureInitialized();
  return button >= 0 && button < PortInputMap::kMouseButtonCount ? sInput.mouseActions[button]
                                                                  : PortInputMap::kMA_None;
}

void SetMouseAction(int button, int action) {
  EnsureInitialized();
  if (button >= 0 && button < PortInputMap::kMouseButtonCount && action >= 0 &&
      action < PortInputMap::kMA_Count) {
    sInput.mouseActions[button] = action;
    // A button held as it changes must not start the new action mid-press.
    sMouseButtonGate.Reset();
    MarkDirty();
  }
}

bool SpeedrunTimer() {
  EnsureInitialized();
  return sSpeedrunTimer;
}

void SetSpeedrunTimer(bool enabled) {
  EnsureInitialized();
  sSpeedrunTimer = enabled;
  MarkDirty();
}

bool LiveSplit() {
  EnsureInitialized();
  return sLiveSplit;
}

void SetLiveSplit(bool enabled) {
  EnsureInitialized();
  sLiveSplit = enabled;
  ApplyLiveSplit();
  MarkDirty();
}

bool DiscordPresence() {
  EnsureInitialized();
  return sDiscord;
}

void SetDiscordPresence(bool enabled) {
  EnsureInitialized();
  sDiscord = enabled;
  ApplyDiscord();
  MarkDirty();
}

std::string LiveSplitAddress() {
  EnsureInitialized();
  return sLiveSplitAddress;
}

void SetLiveSplitAddress(const std::string& address) {
  EnsureInitialized();
  if (address.empty()) {
    return;
  }
  sLiveSplitAddress = address;
  ApplyLiveSplit();
  MarkDirty();
}

bool ModsEnabled() {
  EnsureInitialized();
  return sModsEnabled && !sOriginalExperience;
}

void SetModsEnabled(bool enabled) {
  EnsureInitialized();
  sModsEnabled = enabled;
  MarkDirty();
}

bool OriginalExperience() {
  EnsureInitialized();
  return sOriginalExperience;
}

void SetOriginalExperience(bool enabled) {
  EnsureInitialized();
  if (sOriginalExperience == enabled) {
    return;
  }
  sOriginalExperience = enabled;
  MarkDirty();
  PortLog::Write("port: original experience %s\n", enabled ? "on" : "off");
  // The rest reads the getters each frame; these were applied once.
  sDynScale = 0.f;
  ResetDynamicRes();
  VISetFrameBufferScale(EffectiveRenderScale());
  ApplyGraphicsQuality();
  if (PortMods::CurrentStatus().active != ModsEnabled() && !PortMods::Suspended()) {
    PortSaveState::RequestModReload();
  }
}

std::string ModsDisabled() {
  EnsureInitialized();
  return sModsDisabled;
}

void SetModsDisabled(const std::string& list) {
  EnsureInitialized();
  sModsDisabled = list;
  MarkDirty();
}

bool Invulnerable() {
  EnsureInitialized();
  return sInvulnerableRun >= 0 ? sInvulnerableRun != 0 : sInvulnerable;
}

void SetInvulnerable(bool enabled) {
  EnsureInitialized();
  sInvulnerable = enabled;
  sInvulnerableRun = -1;
  MarkDirty();
}

bool LogFile() {
  EnsureInitialized();
  return sLogFile;
}

void SetLogFile(bool enabled) {
  EnsureInitialized();
  sLogFile = enabled;
  MarkDirty();
}

bool FastMorph() {
  EnsureInitialized();
  return sInput.fastMorph && !sOriginalExperience;
}

void SetFastMorph(bool enabled) {
  EnsureInitialized();
  sInput.fastMorph = enabled;
  MarkDirty();
}

bool LockOnToggle() {
  EnsureInitialized();
  return sInput.lockOnToggle && !sOriginalExperience;
}

void SetLockOnToggle(bool enabled) {
  EnsureInitialized();
  sInput.lockOnToggle = enabled;
  MarkDirty();
}

bool StickyCharge() {
  EnsureInitialized();
  return sInput.stickyCharge && !sOriginalExperience;
}

void SetStickyCharge(bool enabled) {
  EnsureInitialized();
  sInput.stickyCharge = enabled;
  MarkDirty();
}

PortRandoGen::Settings RandoSettings() {
  EnsureInitialized();
  std::lock_guard< std::mutex > lock(sRandoMutex);
  return sRandoSettings;
}

bool RapidCharge() {
  EnsureInitialized();
  return sInput.rapidCharge && !sOriginalExperience;
}

void SetRapidCharge(bool enabled) {
  EnsureInitialized();
  sInput.rapidCharge = enabled;
  MarkDirty();
}

bool RemasteredMovement() {
  EnsureInitialized();
  return sInput.remasteredMovement && !sOriginalExperience;
}

void SetRemasteredMovement(bool enabled) {
  EnsureInitialized();
  sInput.remasteredMovement = enabled;
  MarkDirty();
}

bool SpringBallFlick() {
  EnsureInitialized();
  return sInput.springFlick && !sOriginalExperience;
}

void SetSpringBallFlick(bool enabled) {
  EnsureInitialized();
  sInput.springFlick = enabled;
  MarkDirty();
}

float SpringBallFlickRate() {
  EnsureInitialized();
  return sInput.springFlickRate;
}

void SetSpringBallFlickRate(float radiansPerSecond) {
  EnsureInitialized();
  if (std::isfinite(radiansPerSecond) && radiansPerSecond >= 2.f && radiansPerSecond <= 20.f) {
    sInput.springFlickRate = radiansPerSecond;
    MarkDirty();
  }
}

bool SpringBallFlickPending() { return sSpringFlickLatch > 0.f; }

void ClearSpringBallFlick() { sSpringFlickLatch = 0.f; }

void SetGyroOverride(bool active, float pitch, float yaw) {
  sGyroOverride = active;
  sGyroOverridePitch = pitch;
  sGyroOverrideYaw = yaw;
}

float StickAimRate() {
  EnsureInitialized();
  return sInput.stickAimRate;
}

void SetStickAimRate(float pixelsPerSecond) {
  EnsureInitialized();
  if (std::isfinite(pixelsPerSecond) && pixelsPerSecond >= 50.f && pixelsPerSecond <= 10000.f) {
    sInput.stickAimRate = pixelsPerSecond;
    MarkDirty();
  }
}

void AddStickAim(float x, float y, float dt) {
  EnsureInitialized();
  if (!TwinStick() || Visible() || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(dt) ||
      dt <= 0.f) {
    return;
  }
  // Stick aim stands in for free look, so it follows the game's Reverse Y Axis
  // option, as free look does.
  if (gpGameState != nullptr && gpGameState->GameOptions().GetInvertYAxis()) {
    y = -y;
  }
  // x right / y up; the aim state expects SDL-style right/down positive.
  const float travelX = x * sInput.stickAimRate * dt;
  const float travelY = -y * (sInput.stickAimSplit ? sInput.stickAimRateY : sInput.stickAimRate) * dt;
  sMouseFrameX += travelX;
  sMouseFrameY += travelY;
  sStickFrameX += travelX;
  sStickFrameY += travelY;
}
