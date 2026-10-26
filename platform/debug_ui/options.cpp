// F1 overlay: Accessors for the settings and runtime switches the game and other port code read.
// State and helpers shared with the other parts are in debug_ui_internal.h.

#include "debug_ui_internal.h"

#include "port_env.h"
#include "port_debug.h"
#include "port_graphics_backend.h"
#include "port_gyro_motion.h"
#include "port_dynamic_res.h"
#include "port_photo.h"
#include "port_third_person.h"
#include "port_pointer.h"
#include "port_hd_font.h"
#include "port_log.h"
#include "port_paths.h"
#include "port_mods.h"
#include "port_ball_looks.h"
#include "port_suit_colours.h"
#include "port_remastered_text.h"
#include "port_discord.h"
#include "port_savestate.h"
#include "port_mouse.h"

#include "MetroidPrime/Player/CGameOptions.hpp"
#include "MetroidPrime/Player/CGameState.hpp"

#include <aurora/gfx.h>
#include <dolphin/gx/GXExtra.h>
#include <dolphin/vi.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_timer.h>

#if defined(PORT_TOUCH_CONTROLS)
#include "touch_pad.h"
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

namespace aurora {
bool frame_buffer_size_for_scale(float scale, uint32_t* width, uint32_t* height) noexcept;
} // namespace aurora

namespace PortDebug {

// State only this part uses.
namespace {
float sTickPeriod = 1.f / 60.f;
// Wall-clock for the same span as sTimingNs, so presented frames can be divided
// by elapsed time rather than by the frames' own cost. Zero on the first call,
// which is why the delta is skipped until a previous reading exists.
uint64_t sTimingWallNs = 0;
uint64_t sTimingWallLastNs = 0;
unsigned sTimingFrames = 0, sTimingTicks = 0;
bool sDynActive = false;
// GPU pass times (Aurora's gpu_prof) are wanted by dynamic resolution and by the
// Remastered debug panel. Switching them on restarts their average, so only a change
// in whether anyone wants them reaches Aurora.
unsigned sGpuTimesUsers = 0;
float sTwinStickRightY = 0.f;
} // namespace

// Dynamic resolution (port_dynamic_res.h); sDynScale, the scale it draws at, is shared.
PortDynamicRes::Controller sDynController;
double sDynGpuMs = -1.0; // the last GPU time it was given, for the Video page

bool FastBoot() { return Loaded(sFastBoot); }

bool BootWorld(uint32_t& worldId, uint32_t& areaAssetId) {
  static uint32_t sWorld = 0, sArea = 0;
  static const bool sSet = [] {
    const char* value = port::EnvString("MP_BOOT_WORLD");
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

bool SkipCutscenes() { return Loaded(sSkipCutscenes); }

float CutsceneSpeed() { return Loaded(sCutsceneSpeed); }

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

bool Turbo() { return Loaded(sTurbo); }

unsigned TurboTicks() {
  EnsureInitialized();
  return sTurbo ? sTurboTicks : 1;
}

void SetSimAdaptive(bool enabled) { StoreSetting(sSimAdaptive, enabled); }

float TickPeriod() { return Loaded(sTickPeriod); }

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
float EffectiveRenderScale() {
  return sCaptureScale > 0.f ? sCaptureScale : sOriginalExperience ? 1.f : sRenderScale;
}
// The most dynamic resolution draws at: the scale set, or with auto render scale the
// window's own (BaseRenderScale), which follows the window as it is resized.
float DynamicResTop() {
  const float scale = EffectiveRenderScale();
  return scale > 0.f ? scale : BaseRenderScale();
}
int EffectiveMsaa() { return sOriginalExperience ? 1 : sMsaa; }
static int EffectiveAnisotropy() { return sOriginalExperience ? 1 : sAnisotropy; }

// Sharpening for a frame stretched up to the window (Aurora's present resample); only
// passed on when it changes.
void ApplyPresentSharpness() {
  float amount = 0.f;
  if (!sOriginalExperience && sSharpenAmount > 0) {
    const bool lowered = sDynScale > 0.f;
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


double DynamicResTargetFps() {
  double target = sDynamicResTarget;
  if (target <= 0.0) {
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    target = mode != nullptr && mode->refresh_rate > 0.f ? mode->refresh_rate : 60.0;
  }
  const int cap = EffectiveFrameLimit() ? 60 : sFpsCap;
  return cap > 0 ? std::min(target, static_cast< double >(cap)) : target;
}

void WantGpuTimes(unsigned user, bool on) {
  const unsigned before = sGpuTimesUsers;
  sGpuTimesUsers = on ? (before | user) : (before & ~user);
  if ((before != 0) != (sGpuTimesUsers != 0)) {
    GXPortSetGpuTimes(sGpuTimesUsers != 0 ? GX_TRUE : GX_FALSE);
  }
}

void ResetDynamicRes() {
  sDynController.Reset(DynamicResTop());
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
  const float top = DynamicResTop();
  const float bottom = std::min(sDynamicResMin, top);
  const bool active = sDynamicRes && !sOriginalExperience && top > bottom && !sTurbo;
  // Auto's top moves with the window: start over from the new one.
  static float sDynTop = 0.f;
  const bool topMoved = sDynTop != top;
  sDynTop = top;
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
  if (!sDynActive || topMoved) {
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
    // Back at the top draws as the setting does (auto: the window's exact size).
    sDynScale = decision.scale < top ? decision.scale : 0.f;
    VISetFrameBufferScale(sDynScale > 0.f ? sDynScale : EffectiveRenderScale());
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
      PortLog::Write("[timing] presented=%.1f FPS throughput=%.1f FPS simulation=%.1f ticks/s cap=%d\n",
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

bool VsyncEnabled() { return Loaded(sVsyncEnabled); }

void SetVsyncEnabled(bool enabled) {
  EnsureInitialized();
  // Always re-apply: the stored value can match while the surface still has the
  // previous present mode (e.g. the persisted value applied before the first
  // frame), which made the first toggle a no-op.
  if (sVsyncEnabled != enabled) {
    sVsyncEnabled = enabled;
    MarkDirty();
  }
  // Turbo keeps vsync off until it ends, as the first-frame apply does.
  aurora_enable_vsync(enabled && !sTurbo);
}

bool Fullscreen() { return Loaded(sFullscreen); }

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
  // Auto draws at the window's pixel size, fitted to the game's aspect when that is
  // narrower than the window's (16:9 in a 4:3 window); as a multiple of what 1x draws,
  // the render mode's EFB height (480 lines NTSC, 528 PAL).
  uint32_t width = 0, native = 0, base = 0;
  if (aurora::frame_buffer_size_for_scale(0.f, &width, &native) &&
      aurora::frame_buffer_size_for_scale(1.f, &width, &base) && native > 0 && base > 0) {
    return static_cast< float >(native) / static_cast< float >(base);
  }
  int w = 0, h = 0;
  SDL_Window* window = MainWindow();
  if (window != nullptr && SDL_GetWindowSizeInPixels(window, &w, &h) && h > 0) {
    return static_cast< float >(h) / 480.f;
  }
  return 1.f;
}

int DrawnLines(float scale) {
  uint32_t width = 0, height = 0;
  if (aurora::frame_buffer_size_for_scale(scale, &width, &height)) {
    return static_cast< int >(height);
  }
  return static_cast< int >(std::lround(scale * 480.f));
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

int DynamicResTarget() { return Loaded(sDynamicResTarget); }

float DynamicResMin() { return Loaded(sDynamicResMin); }

EAspectMode AspectMode() {
  EnsureInitialized();
  return sOriginalExperience ? kAspect_4_3 : sAspectMode;
}

void SetAspectMode(EAspectMode mode) { StoreSetting(sAspectMode, mode); }

bool HudWide() {
  EnsureInitialized();
  return sHudWide && !sOriginalExperience;
}

void SetHudWide(bool enabled) { StoreSetting(sHudWide, enabled); }

bool CinemaBars() {
  EnsureInitialized();
  return sCinemaBars || sOriginalExperience;
}

bool SharpScanWindow() {
  EnsureInitialized();
  return sSharpScanWindow && !sOriginalExperience;
}

bool GeneratedBallLooks() {
  EnsureInitialized();
  return sGeneratedBallLooks && !sOriginalExperience;
}

void SetGeneratedBallLooks(bool enabled) {
  EnsureInitialized();
  if (sGeneratedBallLooks == enabled) {
    return;
  }
  sGeneratedBallLooks = enabled;
  MarkDirty();
  if (PortBallLooks::Requested() != GeneratedBallLooks() && !PortMods::Suspended()) {
    PortSaveState::RequestModReload();
  }
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

float HintDelaySeconds() {
  EnsureInitialized();
  return sOriginalExperience ? 0.f : static_cast< float >(sHintDelay) * 60.f;
}

static bool sShowHintRequested = false;

void RequestShowHint() {
  sShowHintRequested = true;
}

bool ConsumeShowHintRequest() {
  const bool requested = sShowHintRequested;
  sShowHintRequested = false;
  return requested;
}

float ScanSpeed() {
  EnsureInitialized();
  return sOriginalExperience ? 1.f : static_cast< float >(sScanSpeed) / 100.f;
}

bool MenuStandardButtons() { return Loaded(sMenuStandardButtons); }
void SetMenuStandardButtons(bool enabled) { StoreSetting(sMenuStandardButtons, enabled); }

void SetPointerMenus(bool enabled) {
  EnsureInitialized();
  PortPointer::SetEnabled(enabled);
  MarkDirty();
}

void SetMapSwapFingers(bool swap) {
  EnsureInitialized();
  PortPointer::SetMapSwapFingers(swap);
  MarkDirty();
}

bool SecondaryCrosshairs() {
  EnsureInitialized();
  return sSecondaryCrosshairs || sOriginalExperience;
}

int HudOpacity(HudOpacityPart part) {
  EnsureInitialized();
  if (sOriginalExperience) {
    return part == kHudOpacityEnergy ? kHudOpacityMax : kHudOpacityVisor;
  }
  return sHudOpacity[part];
}

void SetHudOpacity(HudOpacityPart part, int percent) {
  EnsureInitialized();
  sHudOpacity[part] = percent == kHudOpacityVisor ? kHudOpacityVisor
                                                  : std::clamp(percent, kHudOpacityMin, kHudOpacityMax);
  MarkDirty();
}

int CrosshairOpacity() {
  EnsureInitialized();
  return sOriginalExperience ? kCrosshairOpacityMax : sCrosshairOpacity;
}

ECrosshairStyle CrosshairStyle() {
  EnsureInitialized();
  return sOriginalExperience ? kCrosshairStyle_Classic : sCrosshairStyle;
}

void SetCrosshairStyle(ECrosshairStyle style) {
  EnsureInitialized();
  sCrosshairStyle = style;
  MarkDirty();
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
  return (sHideHelmet || PortPhoto::Active() || PortThirdPerson::Active()) && !sOriginalExperience;
}

void SetHideHelmet(bool enabled) { StoreSetting(sHideHelmet, enabled); }

bool HideVisorEffects() {
  EnsureInitialized();
  return (sHideVisorEffects || PortPhoto::Active()) && !sOriginalExperience;
}

void SetHideVisorEffects(bool enabled) { StoreSetting(sHideVisorEffects, enabled); }

bool RevealMap() {
  EnsureInitialized();
  return sRevealMap && !sOriginalExperience;
}

void SetRevealMap(bool enabled) { StoreSetting(sRevealMap, enabled); }

EMapPickups MapPickups() {
  EnsureInitialized();
  return static_cast< EMapPickups >(sMapPickups);
}

void SetMapPickups(EMapPickups mode) {
  EnsureInitialized();
  sMapPickups = mode >= kMapPickups_Off && mode <= kMapPickups_Rooms ? mode : kMapPickups_Off;
  MarkDirty();
}

bool MapLogicColors() { return Loaded(sMapLogicColors); }
void SetMapLogicColors(bool enabled) { StoreSetting(sMapLogicColors, enabled); }

bool MapPickupKinds() { return Loaded(sMapPickupKinds); }
void SetMapPickupKinds(bool enabled) { StoreSetting(sMapPickupKinds, enabled); }

bool MapStationPickups() { return Loaded(sMapStationPickups); }
void SetMapStationPickups(bool enabled) { StoreSetting(sMapStationPickups, enabled); }

bool MapExtraUi() { return Loaded(sMapExtraUi); }
void SetMapExtraUi(bool enabled) { StoreSetting(sMapExtraUi, enabled); }

int ApSuitDamage() { return Loaded(sApSuitDamage); }

void SetApSuitDamage(int mode) {
  EnsureInitialized();
  sApSuitDamage = mode >= 0 && mode <= 2 ? mode : 1;
  MarkDirty();
}

bool SkippableCutscenes() {
  EnsureInitialized();
  return sSkippableCutscenes && !sOriginalExperience;
}

void SetSkippableCutscenes(bool enabled) { StoreSetting(sSkippableCutscenes, enabled); }

bool BeamDoorsStayUnlocked() {
  EnsureInitialized();
  return sBeamDoorsStayUnlocked && !sOriginalExperience;
}

void SetBeamDoorsStayUnlocked(bool enabled) { StoreSetting(sBeamDoorsStayUnlocked, enabled); }

bool StackBeams() {
  EnsureInitialized();
  return sStackBeams && !sOriginalExperience;
}

void SetStackBeams(bool enabled) { StoreSetting(sStackBeams, enabled); }

int WalkSpeed() {
  EnsureInitialized();
  return sOriginalExperience ? kWalkSpeedDefault : sWalkSpeed;
}

void SetWalkSpeed(int percent) {
  EnsureInitialized();
  sWalkSpeed = std::clamp(percent, kWalkSpeedMin, kWalkSpeedMax);
  MarkDirty();
}

bool Revision100Compatibility() { return Loaded(sRevision100Compatibility); }
void SetRevision100Compatibility(bool enabled) { StoreSetting(sRevision100Compatibility, enabled); }

const char* TextLanguage() {
  EnsureInitialized();
  for (size_t i = 0; i < PortRemastered::kTextLanguageCount; ++i) {
    if (sTextLanguage == PortRemastered::kTextLanguages[i].code) {
      return PortRemastered::kTextLanguages[i].code;
    }
  }
  return "";
}

void SetTextLanguage(const char* code) { StoreSetting(sTextLanguage, code); }

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

bool OpenGles() { return Loaded(sOpenGles); }

void SetOpenGles(bool enabled) {
  EnsureInitialized();
  if (sOpenGles != enabled) {
    sOpenGles = enabled;
    MarkDirty();
  }
}

PortDebug::EGraphicsBackend GraphicsBackend() { return Loaded(sGraphicsBackend); }

void SetGraphicsBackend(PortDebug::EGraphicsBackend backend) {
  EnsureInitialized();
  if (sGraphicsBackend != backend) {
    sGraphicsBackend = backend;
    MarkDirty();
  }
}

const std::string& GpuDriver() { return Loaded(sGpuDriver); }

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
  if (sStorageClamp < 0 || port::EnvString("MP_STORAGE_CLAMP") != nullptr) {
    return;
  }
#ifdef _WIN32
  _putenv_s("MP_STORAGE_CLAMP", sStorageClamp > 0 ? "1" : "0");
#else
  setenv("MP_STORAGE_CLAMP", sStorageClamp > 0 ? "1" : "0", 1);
#endif
  sStorageClampEnvFromSetting = true;
}
// A file in the user folder while a start with driver workarounds is under way: still there at the
// next start means that one never reached its first second of frames, the same kind of marker
// main.cpp keeps for OpenGL ES and custom GPU drivers.
std::filesystem::path sDriverWorkaroundsMarker;

void BeginDriverWorkarounds(const std::string& userFolder, std::string& enable, std::string& disable) {
  EnsureInitialized();
  enable.clear();
  disable.clear();
  sDriverWorkaroundsMarker =
      std::filesystem::path(userFolder.empty() ? "." : userFolder) / "driver_workarounds_starting";
  std::error_code ec;
  unsigned bits = sDriverWorkarounds;
  bool keepMarker = false;
  sDriverWorkaroundsEnv = port::EnvString("MP_DRIVER_WORKAROUNDS");
  if (sDriverWorkaroundsEnv != nullptr) {
    // "0" (or anything that names none) starts without them, over the ticked boxes.
    bits = ParseDriverWorkarounds(sDriverWorkaroundsEnv);
    PortLog::Write("port: MP_DRIVER_WORKAROUNDS=%s replaces the ticked driver workarounds this run\n",
                   sDriverWorkaroundsEnv);
  } else if (bits != 0 && std::filesystem::exists(sDriverWorkaroundsMarker, ec)) {
    PortLog::Write("port: the last start with driver workarounds did not reach the game; turning them off\n");
    bits = 0;
    sDriverWorkarounds = 0;
    sDriverWorkaroundsCleared = true;
    MarkDirty();
    // Until a start reaches the game and saves the cleared setting, a crash before that (for any
    // reason) must not bring the boxes back: the marker stays until DriverWorkaroundsStarted.
    keepMarker = true;
  }
  if (sDriverWorkaroundsEnv == nullptr && (bits & kDriverChecksBit) != 0) {
    // One start only: untick it now, so the next start is back to full speed whatever happens.
    sDriverWorkarounds &= ~kDriverChecksBit;
    MarkDirty();
    PortLog::Write("port: driver error checks are on for this run (slower); the next start turns them off\n");
  }
  sDriverChecksThisRun = (bits & kDriverChecksBit) != 0;
  sDriverWorkaroundsAtStart = sDriverWorkarounds;
  const auto append = [](std::string& list, const char* toggles) {
    if (toggles != nullptr) {
      list += list.empty() ? "" : ",";
      list += toggles;
    }
  };
  for (size_t i = 0; i < kDriverWorkaroundCount; ++i) {
    if (bits & (1u << i)) {
      append(enable, kDriverWorkarounds[i].enable);
      append(disable, kDriverWorkarounds[i].disable);
    }
  }
  // A run on MP_DRIVER_WORKAROUNDS says nothing about the ticked boxes: it leaves the marker alone.
  if (sDriverWorkaroundsEnv != nullptr) {
    sDriverWorkaroundsMarker.clear();
  } else if (bits != 0) {
    std::ofstream(sDriverWorkaroundsMarker) << "1\n";
  } else if (!keepMarker) {
    std::filesystem::remove(sDriverWorkaroundsMarker, ec);
  }
}

void DriverWorkaroundsStarted() {
  if (!sDriverWorkaroundsMarker.empty()) {
    std::error_code ec;
    std::filesystem::remove(sDriverWorkaroundsMarker, ec);
  }
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

bool SunShadows() { return Loaded(sSunShadows); }
void SetSunShadows(bool enabled) {
  EnsureInitialized();
  if (sSunShadows != enabled) {
    sSunShadows = enabled;
    MarkDirty();
  }
}

bool ProjectedShadows() { return Loaded(sProjectedShadows); }
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

void SetUnlockHardMode(bool enabled) { StoreSetting(sUnlockHardMode, enabled); }

bool UnlockFusionSuit() {
  EnsureInitialized();
  return sUnlockFusionSuit && !sOriginalExperience;
}

void SetUnlockFusionSuit(bool enabled) { StoreSetting(sUnlockFusionSuit, enabled); }

bool UnlockGalleries() {
  EnsureInitialized();
  return sUnlockGalleries && !sOriginalExperience;
}

void SetUnlockGalleries(bool enabled) { StoreSetting(sUnlockGalleries, enabled); }

bool MouseAim() { return Loaded(sInput.mouseAim); }

void SetMouseAim(bool enabled) {
  EnsureInitialized();
  sInput.mouseAim = enabled;
  ResetMouseAim();
}

bool TouchDirectAim() {
#if defined(PORT_TOUCH_CONTROLS)
  return !sInput.touchClassic && sTouchActive.load(std::memory_order_acquire) && !Visible();
#else
  return false;
#endif
}

bool TouchActive() {
#if defined(PORT_TOUCH_CONTROLS)
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
#if defined(PORT_TOUCH_CONTROLS)
  // Touch has its own twin stick (the right stick aims), apart from the pad preset.
  if (TouchActive()) {
    return sInput.touchTwinStick;
  }
#endif
  return sInput.twinStick && !TouchActive();
}

bool PadTwinStick() { return Loaded(sInput.twinStick); }

void SetTwinStick(bool enabled) { StoreSetting(sInput.twinStick, enabled); }

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

void SetSpringBall(bool enabled) { StoreSetting(sInput.springBall, enabled); }

bool SwapScanXray() {
  EnsureInitialized();
  return sInput.swapScanXray && !TouchActive() && !sOriginalExperience;
}

void SetSwapScanXray(bool enabled) { StoreSetting(sInput.swapScanXray, enabled); }

int TurboBinding(int slot) {
  EnsureInitialized();
  if (slot == 2 && TouchActive()) {
    return -1;
  }
  return slot >= 0 && slot < 3 ? sInput.turboBindings[slot] : -1;
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

bool SpeedrunTimer() { return Loaded(sSpeedrunTimer); }
void SetSpeedrunTimer(bool enabled) { StoreSetting(sSpeedrunTimer, enabled); }

bool LiveSplit() { return Loaded(sLiveSplit); }

void SetLiveSplit(bool enabled) {
  EnsureInitialized();
  sLiveSplit = enabled;
  ApplyLiveSplit();
  MarkDirty();
}

bool DiscordPresence() { return Loaded(sDiscord); }

void SetDiscordPresence(bool enabled) {
  EnsureInitialized();
  sDiscord = enabled;
  ApplyDiscord();
  MarkDirty();
}

std::string LiveSplitAddress() { return Loaded(sLiveSplitAddress); }

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

void SetModsEnabled(bool enabled) { StoreSetting(sModsEnabled, enabled); }

bool OriginalExperience() { return Loaded(sOriginalExperience); }

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
  if ((PortMods::CurrentStatus().active != ModsEnabled() ||
       PortBallLooks::Requested() != GeneratedBallLooks() || PortSuitColours::Pending(!sOriginalExperience)) &&
      !PortMods::Suspended()) {
    PortSaveState::RequestModReload();
  }
}

std::string ModsDisabled() { return Loaded(sModsDisabled); }
void SetModsDisabled(const std::string& list) { StoreSetting(sModsDisabled, list); }

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

bool PhazonBeamCheat() { return Loaded(sPhazonBeamCheat); }
void SetPhazonBeamCheat(bool enabled) { StoreSetting(sPhazonBeamCheat, enabled); }

bool LogFile() { return Loaded(sLogFile); }
void SetLogFile(bool enabled) { StoreSetting(sLogFile, enabled); }

bool FastMorph() {
  EnsureInitialized();
  return sInput.fastMorph && !sOriginalExperience;
}

void SetFastMorph(bool enabled) { StoreSetting(sInput.fastMorph, enabled); }

bool LockOnToggle() {
  EnsureInitialized();
  return sInput.lockOnToggle && !sOriginalExperience;
}

void SetLockOnToggle(bool enabled) { StoreSetting(sInput.lockOnToggle, enabled); }

bool StickyCharge() {
  EnsureInitialized();
  return sInput.stickyCharge && !sOriginalExperience;
}

void SetStickyCharge(bool enabled) { StoreSetting(sInput.stickyCharge, enabled); }

PortRandoGen::Settings RandoSettings() {
  EnsureInitialized();
  std::lock_guard< std::mutex > lock(sRandoMutex);
  return sRandoSettings;
}

bool RapidCharge() {
  EnsureInitialized();
  return sInput.rapidCharge && !sOriginalExperience;
}

void SetRapidCharge(bool enabled) { StoreSetting(sInput.rapidCharge, enabled); }

bool RemasteredMovement() {
  EnsureInitialized();
  return sInput.remasteredMovement && !sOriginalExperience;
}

void SetRemasteredMovement(bool enabled) { StoreSetting(sInput.remasteredMovement, enabled); }

void SetSpringBallFlick(bool enabled) { StoreSetting(sInput.springFlick, enabled); }

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

float StickAimRate() { return Loaded(sInput.stickAimRate); }

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
  // x right / y up; the aim state expects SDL-style right/down positive. The travel
  // turns (and moves the pointer) by the mouse sensitivity, so divide that out: a stick
  // pixel turns kAimPixelRadians whatever the mouse is set to (issue #209).
  const float pixel = PortMouse::kAimPixelRadians / MouseSensitivity();
  const float travelX = x * sInput.stickAimRate * dt * pixel;
  const float travelY =
      -y * (sInput.stickAimSplit ? sInput.stickAimRateY : sInput.stickAimRate) * dt * pixel;
  sMouseFrameX += travelX;
  sMouseFrameY += travelY;
  sStickFrameX += travelX;
  sStickFrameY += travelY;
}

} // namespace PortDebug
