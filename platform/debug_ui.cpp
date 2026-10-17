// In-game debug overlay. The port draws it with Aurora's ImGui backend, which
// is already initialized and rendered every presented frame, so this only has
// to build the windows between aurora_begin_frame and aurora_end_frame.
//
// The overlay is one translation unit split across files: this one holds the
// shared state, and each section (a page, the settings table, the toasts...)
// lives in platform/debug_ui/<section>.inc, #included below in its place. The
// parts are not compiled on their own; tools/check_settings_index.py reads
// them with this file.

#include "port_env.h"
#include "port_collision_view.h"
#include "port_debug.h"
#include "port_graphics_backend.h"
#include "port_gyro_motion.h"
#include "port_dynamic_res.h"
#include "port_freecam.h"
#include "port_photo.h"
#include "port_pointer.h"
#include "port_hd_font.h"
#include "port_room_env.h"
#include "port_room_geo.h"
#include "port_log.h"
#include "port_log_file.h"
#include "port_paths.h"
#include "port_config_reset.h"
#include "port_apclient.h"
#include "port_rando_gen.h"
#include "port_controls.h"
#include "port_input_devices.h"
#include "port_input_remap_ui.h"
#include "port_data_folder.h"
#include "port_gci.h"
#include "port_mods.h"
#include "port_importers.h"
#include "port_remastered_effect_import.h"
#include "port_remastered_import.h"
#include "port_remastered_text.h"
#include "port_discord.h"
#include "port_update_check.h"
#include "port_gallery.h"
#include "port_livesplit.h"
#include "port_map_pickups.h"
#include "port_prompts.h"
#include "port_tracker.h"
#include "port_savestate.h"
#include "port_skip_cutscenes.h"
#include "port_mouse.h"
#include "port_input_map.h"
#include "port_textures.h"
#include "port_build_info.h"
#include "port_gpu_driver.h"
#include "port_disc.h"
#include "port_pal_languages.h"
#if defined(__ANDROID__)
#include "touch_pad.h"
#endif

#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "Kyoto/Graphics/CCubeMaterial.hpp"
#include "Kyoto/Graphics/CGX.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Graphics/CCubeModel.hpp"
#include "Kyoto/Graphics/CModel.hpp"
#include "Kyoto/Graphics/CTexture.hpp"
#include "GuiSys/CGuiModel.hpp"
#include "MetroidPrime/CHealthInfo.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CMapWorld.hpp"
#include "MetroidPrime/CMemoryCard.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/CScriptLayerManager.hpp"
#include "MetroidPrime/Player/CGameOptions.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "Kyoto/CResFactory.hpp"
#include "Kyoto/Streams/CMemoryInStream.hpp"
#include "Kyoto/Streams/CMemoryStreamOut.hpp"
#include "Kyoto/Audio/CSfxManager.hpp"
#include "MetroidPrime/SFX/UI.h"

#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <aurora/imgui.h>
#include <dolphin/gx/GXExtra.h>
#include <dolphin/pad.h>
#include <dolphin/vi.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <musyx/port_voices.h>

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_sensor.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_video.h>

#if defined(__ANDROID__)
#include <jni.h>
#include <android/log.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_system.h>
#include <cctype>
#include <unistd.h>
#endif
#if !defined(_WIN32)
extern char** environ;
#endif

#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aurora {
void request_screenshot() noexcept;
}

// Implemented by the AI and MusyX audio backends.
extern "C" void AIPortSetOutputEnabled(int enabled);
extern "C" int AIPortOutputEnabled(void);
extern "C" void salSetMuted(int muted);

// ImGui's item hooks, forwarded by extern/aurora/extern/imgui_item_hook.cpp.
extern void (*gImGuiItemAddHook)(ImGuiContext* ctx, ImGuiID id, const ImRect& rect);
extern void (*gImGuiItemInfoHook)(ImGuiContext* ctx, ImGuiID id, const char* label,
                                  ImGuiItemStatusFlags flags);

namespace {

static_assert(PortInput::kScancodeLShift == SDL_SCANCODE_LSHIFT);
PortInput::Settings& sInput = PortInput::MutableSettings();
bool sInitialized = false;
bool sFastBoot = false;
bool sSkipCutscenes = false;
float sCutsceneSpeed = 8.f;
unsigned sSimRate = 60;
bool sSimAdaptive = false;
bool sFrameLimitEnabled = true;
int sFpsCap = 0; // with the 60 FPS cap off; 0 = unlimited
constexpr int kFpsCapMin = 30;
constexpr int kFpsCapMax = 500;
int ClampFpsCap(int fps) { return fps <= 0 ? 0 : std::clamp(fps, kFpsCapMin, kFpsCapMax); }
bool sTurbo = false;
unsigned sTurboTicks = 1;
bool sTraceTiming = false;
uint64_t sTimingNs = 0;
double sActualFps = 0.0, sActualTps = 0.0;
double sThroughputFps = 0.0;
// On by default on macOS: Dawn's Metal backend has no non-tearing mode besides
// vsync (Mailbox keeps displaySyncEnabled), and Immediate at the 60 fps cap on a
// 60 Hz panel shows a tear line rolling slowly up or down the screen.
#if defined(__APPLE__)
bool sVsyncEnabled = true;
#else
bool sVsyncEnabled = false;
#endif
// Android hides the status and navigation bars by default; a desktop starts windowed.
#if defined(__ANDROID__)
bool sFullscreen = true;
#else
bool sFullscreen = false;
#endif
bool sOverlayWindowed = false; // desktop: the old floating tabbed window
// F1 overlay look: 0 auto (Remastered with the Remastered import loaded, else Prime), 1 Prime,
// 2 Remastered, 3 Plain (stock Dear ImGui dark). Menu sounds are the game's own UI effects.
int sUiTheme = 0;
bool sUiSounds = true;
bool sMainLoopRan = false; // the game loop has run, so CSfxManager is being updated
float sRenderScale = 1.f;
// Dynamic resolution: draws the EFB between sDynamicResMin and sRenderScale to hold a frame rate.
bool sDynamicRes = false;
int sDynamicResTarget = 0;  // fps; 0 = the 60 fps cap, or the display's rate without it
float sDynamicResMin = 1.f; // the lowest scale it drops to: 0.5, 0.75 or 1
// Sharpening of a frame stretched up to the window: 0 off, 1 while dynamic resolution has
// lowered the scale, 2 whenever the EFB is smaller than the window. Strength 0-100.
int sSharpenMode = 1;
int sSharpenAmount = 50;
float sDynScale = 0.f;      // the scale in use; 0 = sRenderScale
PortDebug::EAspectMode sAspectMode = PortDebug::kAspect_Window;
bool sHudWide = true;
bool sCinemaBars = false;
bool sSharpScanWindow = false;
bool sAutoAim = true;
int sScanSpeed = 100;
bool sSecondaryCrosshairs = true;
// A remapped pad drives menus and the map with its standard buttons (issue #57).
bool sMenuStandardButtons = true;
// Index into kPromptGlyphs; 0 = follow the input in use. Atomic: PortPrompts reads it off the UI thread.
std::atomic< int > sPromptGlyphs{0};
constexpr const char* kPromptGlyphs[] = {"auto", "xbox", "playstation", "switch", "gamecube", "keyboard"};
bool sShowShaderCompilation = true;
bool sShowMenuHint = true;
int sHudScale = PortDebug::kHudScaleMax;
#if defined(__ANDROID__)
int sScanSpread = PortDebug::kScanSpreadMin; // touch controls cover the screen edges
#else
int sScanSpread = PortDebug::kScanSpreadMax;
#endif
int sCrosshairOpacity = PortDebug::kCrosshairOpacityMax;
bool sCrosshairCustomColor = false;
float sCrosshairColor[3] = {1.f, 1.f, 1.f};
bool sHideHelmet = false;
bool sHideVisorEffects = false;
bool sRevealMap = false;
int sMapPickups = PortDebug::kMapPickups_Off;
bool sMapLogicColors = true;
// Off by default: it says what kind of item a pickup holds, which spoils a
// first playthrough.
bool sMapPickupKinds = false;
bool sMapStationPickups = true;
// Off by default: the disc's own map legend and prompts, without the port's
// lines. Room marks themselves are unaffected.
bool sMapExtraUi = false;
// The Tracker tab's item, scan and room counts. Off so the tab doesn't spoil
// what is left to find until asked.
bool sTrackerProgress = false;
int sApSuitDamage = 1;
bool sCheats = false;
bool sSkippableCutscenes = false;
// The stored choice only. Randomizer and Archipelago games force it on at use
// (PortSkipCutscenes::Forced), which the getter does not fold in.
bool sRevision100Compatibility = false;
std::string sTextLanguage;
int sElevatorRide = PortDebug::kElevatorRide_Original;
int sBootLogos = PortDebug::kBootLogos_Skippable;
bool sSaveStateHotkeys = true;
bool sQuickResume = true;
bool sSaveStateRoomState = true;
std::atomic<bool> sTouchBeamShift{false}; // the touch twin layout's held Beam button
std::atomic<bool> sTouchTurboFire{false}; // the touch Turbo button, held
bool sInvulnerable = false;
// MP_GODMODE, for this run only: -1 unset, else 0 or 1. Never saved, and changing the
// setting ends it.
int sInvulnerableRun = -1;
// On by default: a game that closes at launch gives no chance to tick the box
// first, and a phone or a desktop launcher has no terminal to read.
bool sLogFile = true;
// The Randomizer page's options; the console's `rando gen` reads them too.
std::mutex sRandoMutex;
PortRandoGen::Settings sRandoSettings;
float sFirstPersonFov = PortDebug::kFovRetail;
float sGunOffset[3] = {};
int sMsaa = 1;
int sAnisotropy = 16;
bool sPerPixelLighting = false;
bool sSunShadows = true;
bool sProjectedShadows = true;
// Setting `opengles`; sOpenGlesAtStart is what this run was started with.
bool sOpenGles = false;
bool sOpenGlesAtStart = false;
// Setting `graphics_backend` (Windows, issue #135); sGraphicsBackendAtStart is what
// this run was started with, so the Video page can say when a change needs a restart
// and when Aurora fell back from it.
PortDebug::EGraphicsBackend sGraphicsBackend = PortDebug::kGraphicsBackend_Auto;
PortDebug::EGraphicsBackend sGraphicsBackendAtStart = PortDebug::kGraphicsBackend_Auto;
// Setting `gpu_driver`: an installed custom Vulkan driver's id (port_gpu_driver.h), "" = the system's.
std::string sGpuDriver;
std::string sGpuDriverAtStart;
bool sUnlockHardMode = false;
// Setting `storage_clamp`: -1 auto (aurora decides), 0 off, 1 on; read once when shaders are first made.
int sStorageClamp = -1;
int sStorageClampAtStart = -1;
// Set when ApplyStorageClamp wrote MP_STORAGE_CLAMP from the setting, so the log can tell it from a user's.
bool sStorageClampEnvFromSetting = false;
// Opt-in Dawn toggles for GPU drivers that draw wrong (issue #143): one checkbox each under
// Video > Compatibility > Driver workarounds, saved as `driver_workarounds`, applied when the GPU
// device is made (so after a restart). MP_DAWN_ENABLE / MP_DAWN_DISABLE still apply after them.
struct DriverWorkaround {
  const char* key; // its name in `driver_workarounds`
  const char* label;
  const char* tooltip;
  const char* enable;  // Dawn toggles it turns on, comma-separated, or nullptr
  const char* disable; // and the ones it turns off
};
constexpr DriverWorkaround kDriverWorkarounds[] = {
    {"keep_depth", "Keep depth buffers (Direct3D 11)",
     "Stops telling the driver it may throw away depth buffers the game is done with.\n"
     "Try it if geometry flickers on an older Intel GPU.",
     nullptr, "d3d11_use_discard_view"},
    {"gpu_constants", "Upload draw constants through the GPU (Direct3D 11)",
     "Sends each draw's constants with GPU buffer copies instead of the CPU-side path.\n"
     "Try it if models flicker, stretch or jump around.",
     "d3d11_disable_cpu_buffers", nullptr},
    {"unmonitored_fence", "Alternative GPU sync (Direct3D 11)",
     "Waits for the GPU with a different kind of fence before reusing upload buffers.\n"
     "Try it if the picture flickers from frame to frame.",
     "d3d11_use_unmonitored_fence", nullptr},
    {"validation", "Driver error checks (next start only, slower)",
     "Turns the graphics library's own checks back on for one start, so a command the driver\n"
     "rejects is named in the log. Costs performance, so it turns itself off after that start.",
     nullptr, "skip_validation,disable_robustness"},
};
constexpr size_t kDriverWorkaroundCount = std::size(kDriverWorkarounds);
// `validation` is for one start only: BeginDriverWorkarounds applies it and unticks it.
constexpr unsigned kDriverChecksBit = 1u << 3;
static_assert(std::string_view{kDriverWorkarounds[3].key} == "validation");
// Setting `driver_workarounds`: bit i is kDriverWorkarounds[i]; AtStart is what this run's device got.
unsigned sDriverWorkarounds = 0;
unsigned sDriverWorkaroundsAtStart = 0;
// What BeginDriverWorkarounds did at start, for the F1 section: error checks on this run,
// MP_DRIVER_WORKAROUNDS overriding the boxes, or the boxes cleared after a start that failed.
bool sDriverChecksThisRun = false;
const char* sDriverWorkaroundsEnv = nullptr;
bool sDriverWorkaroundsCleared = false;

// `driver_workarounds` / MP_DRIVER_WORKAROUNDS: comma-separated names to kDriverWorkarounds bits.
unsigned ParseDriverWorkarounds(std::string_view rest) {
  unsigned bits = 0;
  while (!rest.empty()) {
    const size_t comma = rest.find(',');
    const std::string_view name = rest.substr(0, comma);
    for (size_t i = 0; i < kDriverWorkaroundCount; ++i) {
      if (name == kDriverWorkarounds[i].key) {
        bits |= 1u << i;
      }
    }
    rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
  }
  return bits;
}
bool sUnlockFusionSuit = false;
bool sUnlockGalleries = false;
bool sSpeedrunTimer = false;
bool sLiveSplit = false;
std::string sLiveSplitAddress = "127.0.0.1:16834";
bool sLiveSplitSplitUpgrades = true;
bool sDiscord = true;
bool sUpdateCheck = true;
// Mods folder (port_mods.h): read at startup only.
bool sModsEnabled = true;
std::string sModsDisabled;
// Original experience (`original_experience=`, MP_ORIGINAL): the getters return retail
// values for the port's additions while it is on. The saved settings are left as they
// are, so turning it off brings them back.
bool sOriginalExperience = false;
std::atomic<uint32_t> sWheelMask{0};
// The touch wheels' icons: ARGB pixels per [wheel][item], filled by the game thread, copied out by
// the Android UI thread.
struct WheelIcon {
  int w = 0;
  int h = 0;
  std::vector<uint32_t> argb;
};
std::mutex sWheelIconMutex;
WheelIcon sWheelIcons[2][4];
// The game's CGameOptions as hex of its PutTo bits. Retail keeps them only in each
// save file and resets them on the title screen, so a change made without saving
// at a station was gone on the next launch (issue #45). The port keeps one global
// copy, like Remastered.
std::string sGameOptions;
PortMouse::AimState sMouseAimState;
PortMouse::ButtonGate sMouseButtonGate;
float sMouseFrameX = 0.f;
float sMouseFrameY = 0.f;
// The twin stick's part of this tick's sMouseFrame travel. Frames between ticks
// draw its turn after the tick (docs/FRAME_INTERPOLATION.md section 4).
float sStickFrameX = 0.f;
float sStickFrameY = 0.f;
// Frame interpolation (docs/FRAME_INTERPOLATION.md): uncapped frames show look
// input before the tick that applies it. One setting, smooth_frames, turns all
// four parts on or off; the console's interp command flips them one at a time
// for testing, which isn't saved.
bool sSmoothFrames = true;
bool sFrameInterpolation = true;
bool sActorInterpolation = true;
bool sPoseInterpolation = true;
// Faster on a phone (POCO F8 Ultra: 8-27% more fps in the heaviest rooms), even or mixed on a PC.
constexpr bool kRoomGeoResidentDefault = true;
bool sRoomGeoResident = kRoomGeoResidentDefault;
bool sParticleInterpolation = true;
float sPresentOverride = -1.f;
bool sAiAudioEnabled = true;
bool sMusyxAudioEnabled = true;
// F5 = 1 (save), F9 = 2 (load), from the event watch; handled on the game thread.
std::atomic< int > sSaveStateHotkey{0};
// F11 asks for a fullscreen toggle; DrawUI applies it on the main thread.
std::atomic< bool > sFullscreenHotkey{false};
// The window's own fullscreen state as SDL last reported it (-1 = no report
// yet), so leaving fullscreen through the window manager updates the setting.
std::atomic< int > sWindowFullscreen{-1};
// Mirrors sVisible for readers on other threads, so they never touch the lazy
// initialization or the ImGui state owned by the game thread.
std::atomic< bool > sOverlayVisible{false};
// Same idea for whether the Android touch overlay draws the GameCube pad's colours.
std::atomic< bool > sTouchColorsFlag{false};
std::atomic< bool > sTouchLabelsFlag{true};
std::atomic< bool > sTouchTurboFlag{false};
std::atomic< bool > sTouchFloatingStickFlag{true};
constexpr float kTouchMarginMaxDp = PortInput::kTouchMarginMaxDp;
constexpr float kTouchSideMarginDefault = PortInput::kTouchSideMarginDefault;
constexpr float kTouchStickInsetDefault = PortInput::kTouchStickInsetDefault;
constexpr float kTouchButtonInsetDefault = PortInput::kTouchButtonInsetDefault;
constexpr float kTouchOpacityMin = PortInput::kTouchOpacityMin;
constexpr float kTouchOpacityMax = PortInput::kTouchOpacityMax;
// Java's layout change is saved by the game thread's next frame: ImGui's settings
// path isn't safe from the UI thread.
std::atomic< bool > sTouchLayoutSavePending{false};
// The F1 "Edit layout" button; the touch view takes it and opens its editor.
std::atomic< bool > sTouchEditRequested{false};
// Set when a real pad, keyboard or mouse is used; the Android touch overlay takes
// it to get out of the way.
std::atomic< bool > sPhysicalInput{false};
// Set while the touch overlay is the active device (cleared by physical input).
std::atomic< bool > sTouchActive{false};
// Flick state: seconds left on the last flick, and whether the pitch has dropped
// back since, so a long flick counts once.
float sSpringFlickLatch = 0.f;
bool sSpringFlickArmed = true;
bool sGyroOverride = false;
float sGyroOverridePitch = 0.f;
float sGyroOverrideYaw = 0.f;
bool sVisible = false;
bool sSettingsDirty = false;
// Set once "Reset configuration" has moved the files away: nothing may write them
// back before the process exits (SaveSettings returns early, ImGui's ini is off).
bool sConfigReset = false;
CStateManager* sStateManager = nullptr;
std::string sDiscPath;
// The Remastered import's image and key file as last used: paths, or on Android
// the picked content:// addresses (the picker keeps their read grant).
std::string sRemasteredImagePath;
std::string sRemasteredKeysPath;
// The parts the panel leaves out of an import, comma separated (remastered_skip):
// hud, text, effects, movies, gallery, geometry.
std::string sRemasteredSkip;

std::string SettingsFilePath() {
  const std::string& dir = PortPaths::UserFolder();
  return (dir.empty() ? std::string("./") : dir) + "port_settings.ini";
}

bool ParseBool(const std::string& value) {
  return value == "1" || value == "true" || value == "on" || value == "yes";
}

std::string Trim(const std::string& text) {
  const size_t begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return std::string();
  }
  const size_t end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

void MarkDirty() { sSettingsDirty = true; }

PortRemastered::ImportParts RemasteredImportParts() {
  PortRemastered::ImportParts parts;
  for (size_t at = 0; at < sRemasteredSkip.size();) {
    const size_t comma = std::min(sRemasteredSkip.find(',', at), sRemasteredSkip.size());
    const std::string part = Trim(sRemasteredSkip.substr(at, comma - at));
    parts.hud &= part != "hud";
    parts.text &= part != "text";
    parts.effects &= part != "effects";
    parts.movies &= part != "movies";
    parts.gallery &= part != "gallery";
    parts.geometry &= part != "geometry";
    at = comma + 1;
  }
  return parts;
}

void SetRemasteredImportParts(const PortRemastered::ImportParts& parts) {
  std::string skip;
  const auto add = [&](bool kept, const char* name) {
    if (!kept) {
      skip += skip.empty() ? "" : ",";
      skip += name;
    }
  };
  add(parts.geometry, "geometry");
  add(parts.effects, "effects");
  add(parts.hud, "hud");
  add(parts.text, "text");
  add(parts.movies, "movies");
  add(parts.gallery, "gallery");
  if (skip != sRemasteredSkip) {
    sRemasteredSkip = skip;
    MarkDirty();
  }
}

void ApplyLiveSplit() {
  PortLiveSplit::Configure(sLiveSplit, sLiveSplitAddress, sLiveSplitSplitUpgrades);
}

void ApplyDiscord() {
  PortDiscord::Configure(sDiscord, PortDiscord::kDefaultAppId);
}

void ApplyUpdateCheck() {
  const std::string& dir = PortPaths::UserFolder();
  PortUpdateCheck::Configure(sUpdateCheck && port::EnvFlag("MP_UPDATE_CHECK", true), MP_BUILD_VERSION,
                             (dir.empty() ? std::string("./") : dir) + "update-check.txt");
}

} // namespace
#include "debug_ui/settings.inc"
namespace {
// Whether the player just used a real pad, keyboard or mouse. The touch overlay
// is itself a virtual pad, and touches also arrive as a mouse, so neither counts.
// A stick or trigger has to move well past rest, so drift does not count either.
bool IsPhysicalInput(const SDL_Event& event) {
  switch (event.type) {
  case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    return !SDL_IsJoystickVirtual(event.gbutton.which);
  case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    return std::abs(static_cast< int >(event.gaxis.value)) > 16000 &&
           !SDL_IsJoystickVirtual(event.gaxis.which);
  case SDL_EVENT_KEY_DOWN:
    // The soft keyboard types into the overlay's text fields, and Back and the
    // media keys come from the phone itself.
    if (event.key.repeat || sOverlayVisible.load(std::memory_order_acquire)) {
      return false;
    }
    switch (event.key.scancode) {
    case SDL_SCANCODE_AC_BACK:
    case SDL_SCANCODE_VOLUMEUP:
    case SDL_SCANCODE_VOLUMEDOWN:
    case SDL_SCANCODE_MUTE:
    case SDL_SCANCODE_MEDIA_PLAY:
    case SDL_SCANCODE_MEDIA_PAUSE:
    case SDL_SCANCODE_MEDIA_PLAY_PAUSE:
    case SDL_SCANCODE_MEDIA_NEXT_TRACK:
    case SDL_SCANCODE_MEDIA_PREVIOUS_TRACK:
    case SDL_SCANCODE_MEDIA_STOP:
    case SDL_SCANCODE_POWER:
      return false;
    default:
      return true;
    }
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
    return event.button.which != SDL_TOUCH_MOUSEID && event.button.which != SDL_PEN_MOUSEID;
  default:
    return false;
  }
}

// Photo mode on a touchscreen: the touch controls hide (nativeDebugOverlayVisible), a finger
// put down on the left half is an invisible stick centred where it landed, and one on the right
// half drags the view. Fingers that land on the photo windows are left to ImGui. Events arrive on
// Android's UI thread, so the state is shared under a lock and the game thread takes it each frame.
constexpr float kPhotoStickRadius = 0.12f; // of the screen height, for a full stick
constexpr float kPhotoLookDegrees = 100.f; // per screen height dragged
struct PhotoTouch {
  bool stickDown = false, lookDown = false; // Android finger ids start at 0
  SDL_FingerID stick = 0;
  SDL_FingerID look = 0;
  float stickX = 0.f, stickY = 0.f; // where the stick finger landed
  float moveX = 0.f, moveY = 0.f;
  float lookX = 0.f, lookY = 0.f; // the look finger's last place
  float yaw = 0.f, pitch = 0.f;   // not taken yet
  bool enabled = false;
  float aspect = 1.f; // window width over height
  std::vector<ImVec4> windows; // x0, y0, x1, y1 as fractions of the window
};
std::mutex sPhotoTouchMutex;
PhotoTouch sPhotoTouch;

void PhotoTouchEvent(const SDL_Event& event) {
  if (event.type != SDL_EVENT_FINGER_DOWN && event.type != SDL_EVENT_FINGER_MOTION &&
      event.type != SDL_EVENT_FINGER_UP && event.type != SDL_EVENT_FINGER_CANCELED) {
    return;
  }
  const SDL_TouchFingerEvent& f = event.tfinger;
  std::lock_guard lock(sPhotoTouchMutex);
  PhotoTouch& t = sPhotoTouch;
  if (event.type == SDL_EVENT_FINGER_DOWN) {
    if (!t.enabled) {
      return;
    }
    for (const ImVec4& r : t.windows) {
      if (f.x >= r.x && f.x <= r.z && f.y >= r.y && f.y <= r.w) {
        return;
      }
    }
    if (f.x < 0.5f && !t.stickDown) {
      t.stickDown = true;
      t.stick = f.fingerID;
      t.stickX = f.x;
      t.stickY = f.y;
      t.moveX = t.moveY = 0.f;
    } else if (f.x >= 0.5f && !t.lookDown) {
      t.lookDown = true;
      t.look = f.fingerID;
      t.lookX = f.x;
      t.lookY = f.y;
    }
  } else if (event.type == SDL_EVENT_FINGER_MOTION) {
    if (t.stickDown && f.fingerID == t.stick) {
      t.moveX = std::clamp((f.x - t.stickX) * t.aspect / kPhotoStickRadius, -1.f, 1.f);
      t.moveY = std::clamp((t.stickY - f.y) / kPhotoStickRadius, -1.f, 1.f);
    } else if (t.lookDown && f.fingerID == t.look) {
      t.yaw -= (f.x - t.lookX) * t.aspect * kPhotoLookDegrees;
      t.pitch -= (f.y - t.lookY) * kPhotoLookDegrees;
      t.lookX = f.x;
      t.lookY = f.y;
    }
  } else {
    if (t.stickDown && f.fingerID == t.stick) {
      t.stickDown = false;
      t.moveX = t.moveY = 0.f;
    }
    if (t.lookDown && f.fingerID == t.look) {
      t.lookDown = false;
    }
  }
}

bool SDLCALL debug_event_watch(void*, SDL_Event* event) {
  PhotoTouchEvent(*event);
  // The port menu, save state and screenshot keys are bindings
  // (PortInputDevices::PollPortActions).
  if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat &&
      event->key.scancode == SDL_SCANCODE_F11) {
    sFullscreenHotkey.store(true, std::memory_order_release);
  }
  if (event->type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ||
      event->type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) {
    sWindowFullscreen.store(event->type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ? 1 : 0,
                            std::memory_order_release);
  }
  if (IsPhysicalInput(*event)) {
    sPhysicalInput.store(true, std::memory_order_release);
    sTouchActive.store(false, std::memory_order_release);
  }
  return true;
}

void EnsureInitialized() {
  if (sInitialized) {
    return;
  }
  sInitialized = true;
  LoadSettings();

  // Environment variables are explicit per-run overrides and win over the file.
  if (port::EnvFlag("MP_TRACE_TIMING")) {
    sTraceTiming = true;
  }
  if (port::EnvFlag("MP_FAST_BOOT")) {
    sFastBoot = true;
  }
  if (const char* language = std::getenv("MP_LANGUAGE")) {
    sTextLanguage = language;
  }
  if (port::EnvFlag("MP_TURBO")) {
    sTurbo = true;
    const int ticks = port::EnvInt("MP_TURBO", 0);
    if (ticks >= 1 && ticks <= 16) {
      sTurboTicks = static_cast< unsigned >(ticks);
    }
  }
  if (const char* present = std::getenv("MP_PRESENT_T")) {
    if (std::strcmp(present, "cycle") == 0) {
      sPresentOverride = PortDebug::kPresentCycle;
    } else if (std::strcmp(present, "tick") == 0) {
      sPresentOverride = PortDebug::kPresentTick;
    } else {
      const float t = static_cast< float >(std::atof(present));
      sPresentOverride = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
    }
  }
  // Cutscene skipping is a test aid only: skipping on the first frame of each
  // cinematic left script state unbalanced (stuck visor filters, missing
  // music), so it is no longer a player setting.
  if (port::EnvFlag("MP_SKIP_CUTSCENES")) {
    sSkipCutscenes = true;
  }
  if (const char* god = std::getenv("MP_GODMODE")) {
    sInvulnerableRun = ParseBool(god) ? 1 : 0;
  }
  if (port::EnvFlag("MP_SHOW_DEBUG_UI")) {
    sVisible = true;
  }
  if (const char* aspect = std::getenv("MP_ASPECT")) {
    if (std::strcmp(aspect, "16:9") == 0) {
      sAspectMode = PortDebug::kAspect_16_9;
    } else if (std::strcmp(aspect, "window") == 0) {
      sAspectMode = PortDebug::kAspect_Window;
    } else if (std::strcmp(aspect, "4:3") == 0) {
      sAspectMode = PortDebug::kAspect_4_3;
    }
  } else if (port::EnvFlag("MP_WIDESCREEN")) {
    sAspectMode = PortDebug::kAspect_16_9;
  }
  sHudWide = port::EnvFlag("MP_HUD_WIDE", sHudWide);
  sCinemaBars = port::EnvFlag("MP_CINEMA_BARS", sCinemaBars);
  sSharpScanWindow = port::EnvFlag("MP_SHARP_SCAN_WINDOW", sSharpScanWindow);
  sAutoAim = port::EnvFlag("MP_AUTO_AIM", sAutoAim);
  sScanSpeed = std::clamp(port::EnvInt("MP_SCAN_SPEED", sScanSpeed), PortDebug::kScanSpeedMin,
                          PortDebug::kScanSpeedMax);
  sSecondaryCrosshairs = port::EnvFlag("MP_SECONDARY_CROSSHAIRS", sSecondaryCrosshairs);
  sMenuStandardButtons = port::EnvFlag("MP_MENU_STANDARD_BUTTONS", sMenuStandardButtons);
  PortPointer::SetEnabled(port::EnvFlag("MP_POINTER_MENUS", PortPointer::Enabled()));
  PortInput::ApplyEnvOverrides();
  if (port::EnvFlag("MP_DISABLE_AI_AUDIO")) {
    sAiAudioEnabled = false;
  }
  {
    const float value = port::EnvFloat("MP_CUTSCENE_SPEED", 0.f);
    if (std::isfinite(value) && value >= 1.f && value <= 32.f) {
      sCutsceneSpeed = value;
    }
    const int rate = port::EnvInt("MP_SIM_RATE", 0);
    if (rate >= 30 && rate <= 480) {
      sSimRate = static_cast< unsigned >(rate);
    }
  }
  if (port::EnvFlag("MP_SIM_ADAPTIVE")) {
    sSimAdaptive = true;
  }
  sOriginalExperience = port::EnvFlag("MP_ORIGINAL", sOriginalExperience);
  if (std::getenv("MP_FPS_CAP") != nullptr) {
    sFpsCap = ClampFpsCap(port::EnvInt("MP_FPS_CAP", 0));
  }

  std::atexit(SaveSettings);
  ApplyLiveSplit();
  ApplyDiscord();
  ApplyUpdateCheck();
  PortUpdateCheck::CheckNow(); // once per launch
}
} // namespace

namespace PortDebug {

#include "debug_ui/options.inc"
#include "debug_ui/input.inc"
#include "debug_ui/runtime.inc"
#include "debug_ui/ui_common.inc"
#include "debug_ui/performance.inc"
#include "debug_ui/file_dialogs.inc"
namespace {
} // namespace
#include "debug_ui/gallery.inc"
namespace {
} // namespace
#include "debug_ui/overlays.inc"
namespace {
} // namespace
#include "debug_ui/mods.inc"
#include "debug_ui/game_sections.inc"
#include "debug_ui/video.inc"
#include "debug_ui/controls.inc"
#include "debug_ui/audio.inc"
#include "debug_ui/archipelago.inc"
#include "debug_ui/cheats.inc"
#include "debug_ui/remastered.inc"
#include "debug_ui/debug.inc"
#include "debug_ui/tracker.inc"
#include "debug_ui/savestates.inc"
#include "debug_ui/tabs.inc"
#include "debug_ui/randomizer.inc"
#include "debug_ui/search.inc"
#include "debug_ui/window.inc"
void LoadDiscPath() {
  const std::string path = SettingsFilePath();
  std::ifstream file(path);
  if (!file.is_open()) {
    return;
  }
  std::string line;
  while (std::getline(file, line)) {
    const size_t separator = line.find('=');
    if (separator == std::string::npos || Trim(line.substr(0, separator)) != "disc_path") {
      continue;
    }
    sDiscPath = Trim(line.substr(separator + 1));
    std::fprintf(stderr, "metroid_prime_port: saved disc image %s\n", sDiscPath.c_str());
    return;
  }
}

const char* DiscPath() {
  return sDiscPath.empty() ? nullptr : sDiscPath.c_str();
}

void SetDiscPath(const char* path) {
  sDiscPath = path != nullptr ? path : "";
  sSettingsDirty = true;
}

} // namespace PortDebug

#include "debug_ui/android_jni.inc"
