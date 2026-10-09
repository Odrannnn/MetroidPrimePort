// In-game debug overlay. The port draws it with Aurora's ImGui backend, which
// is already initialized and rendered every presented frame, so this only has
// to build the windows between aurora_begin_frame and aurora_end_frame.

#include "port_env.h"
#include "port_collision_view.h"
#include "port_debug.h"
#include "port_freecam.h"
#include "port_hd_font.h"
#include "port_room_env.h"
#include "port_room_geo.h"
#include "port_log.h"
#include "port_log_file.h"
#include "port_paths.h"
#include "port_apclient.h"
#include "port_rando_gen.h"
#include "port_controls.h"
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
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "Kyoto/CResFactory.hpp"
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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aurora {
void request_screenshot() noexcept;
}

// Implemented by the AI and MusyX audio backends.
extern "C" void AIPortSetOutputEnabled(int enabled);
extern "C" int AIPortOutputEnabled(void);
extern "C" void salSetMuted(int muted);

namespace {
bool sInitialized = false;
bool sFastBoot = false;
bool sSkipCutscenes = false;
float sCutsceneSpeed = 8.f;
unsigned sSimRate = 60;
bool sSimAdaptive = false;
float sTickPeriod = 1.f / 60.f;
bool sFrameLimitEnabled = true;
bool sTurbo = false;
unsigned sTurboTicks = 1;
bool sTraceTiming = false;
uint64_t sTimingNs = 0;
// Wall-clock for the same span as sTimingNs, so presented frames can be divided
// by elapsed time rather than by the frames' own cost. Zero on the first call,
// which is why the delta is skipped until a previous reading exists.
uint64_t sTimingWallNs = 0;
uint64_t sTimingWallLastNs = 0;
unsigned sTimingFrames = 0, sTimingTicks = 0;
double sActualFps = 0.0, sActualTps = 0.0;
double sThroughputFps = 0.0;
bool sVsyncEnabled = false;
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
float sDynScale = 0.f;      // the scale in use; 0 = sRenderScale
int sDynSlow = 0, sDynSteady = 0, sDynSettle = 0;
int sDynRaiseAfter = 4;  // steady seconds before trying a step up; doubles when one fails
int sDynSinceRaise = -1; // seconds since the last step up, while it is on probation
double sDynFpsBefore = 0.0; // the rate before the last step down
int sDynHoldLow = 0;        // seconds left without stepping down (a step down gained nothing)
int sDynHoldFor = 30;       // the next such hold; doubles each time
PortDebug::EAspectMode sAspectMode = PortDebug::kAspect_Window;
bool sHudWide = true;
bool sCinemaBars = false;
bool sShowShaderCompilation = true;
int sHudScale = PortDebug::kHudScaleMax;
bool sHideHelmet = false;
bool sHideVisorEffects = false;
bool sRevealMap = false;
bool sMapPickups = false;
bool sMapLogicColors = true;
// The Tracker tab's item, scan and room counts. Off so the tab doesn't spoil
// what is left to find until asked.
bool sTrackerProgress = false;
int sApSuitDamage = 1;
bool sCheats = false;
bool sSkippableCutscenes = false;
std::string sTextLanguage;
int sElevatorRide = PortDebug::kElevatorRide_Original;
bool sSaveStateHotkeys = true;
bool sMouseAim = false;
bool sTwinStick = false;
float sTwinStickRightY = 0.f;
bool sBeamShiftHeld = false;
std::atomic<bool> sTouchBeamShift{false}; // the touch twin layout's held Beam button
std::atomic<bool> sTouchTurboFire{false}; // the touch Turbo button, held
bool sSpringBall = false;
bool sSwapScanXray = false;
bool sTouchColors = false; // Android touch overlay: the GameCube pad's colours
bool sTouchLabels = true;  // and each button's function under its letter
bool sTouchTurbo = false;  // a Turbo button beside Fire
bool sTouchFloatingStick = true;  // the left stick appears where the left half is touched
bool sFastMorph = false;
bool sInvulnerable = false;
// MP_GODMODE, for this run only: -1 unset, else 0 or 1. Never saved, and changing the
// setting ends it.
int sInvulnerableRun = -1;
// On by default: a game that closes at launch gives no chance to tick the box
// first, and a phone or a desktop launcher has no terminal to read.
bool sLogFile = true;
bool sLockOnToggle = false;
bool sStickyCharge = false;
bool sRapidCharge = false;
// The Randomizer page's options; the console's `rando gen` reads them too.
std::mutex sRandoMutex;
PortRandoGen::Settings sRandoSettings;
bool sSpringFlick = false;
float sSpringFlickRate = 6.f;
float sStickAimRate = 900.f;
float sFirstPersonFov = PortDebug::kFovRetail;
int sMsaa = 1;
int sAnisotropy = 16;
// Setting `opengles`; sOpenGlesAtStart is what this run was started with.
bool sOpenGles = false;
bool sOpenGlesAtStart = false;
// Setting `gpu_driver`: an installed custom Vulkan driver's id (port_gpu_driver.h), "" = the system's.
std::string sGpuDriver;
std::string sGpuDriverAtStart;
bool sUnlockHardMode = false;
// Setting `storage_clamp`: -1 auto (aurora decides), 0 off, 1 on; read once when shaders are first made.
int sStorageClamp = -1;
int sStorageClampAtStart = -1;
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
// Gyro aiming: off / hold / always, auto / controller / phone, and how fast a
// rotation turns into aim travel.
int sGyroMode = 0;
int sGyroSource = 0;
float sGyroRate = 600.f;
// Touch aim (Android): dragging on the free screen area turns the view by the
// finger's travel. Speed is aim pixels per dp; 2.25 turns ~180 degrees over a
// 400 dp drag at the default mouse sensitivity (pi / 0.0035 / 400).
bool sTouchAim = true;
float sTouchAimSpeed = 2.25f;
// Tap the minimap to open the map (Android): the HUD publishes the minimap's
// screen rect, the touch overlay hit-tests it and injects a Z press per tap.
bool sTouchMapTap = true;
// Hold-and-slide beam and visor wheels (Android overlay): the player's state is
// published each frame, the overlay's pick comes back as a request that
// ControlMapper reads for a few polls as if the command's button were pressed.
bool sTouchWheels = true;
// Classic GameCube layout (Android overlay): C-stick, D-pad and the per-option
// toggles. Off, the overlay has no C-stick and dragging aims like a mouse.
bool sTouchClassic = false;
bool sTouchTwinStick = false;  // exclusive with sTouchClassic; classic wins on load
bool sTouchVisorTapScan = false;
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
std::atomic<uint64_t> sWheelStampNs{0};
// Whether the pause menu (inventory, logbook) is up; the touch overlay shows R there.
std::atomic<bool> sPauseScreenOpen{false};
std::atomic<uint64_t> sPauseScreenStampNs{0};
std::atomic<int> sVisorRequest{-1};
std::atomic<uint64_t> sVisorRequestUntilNs{0};
std::atomic<int> sBeamRequest{-1};
std::atomic<uint64_t> sBeamRequestUntilNs{0};
constexpr uint64_t kWheelRequestNs = 120'000'000;
std::mutex sMinimapMutex;
bool sMinimapValid = false;
bool sMinimapDrawn = false;
float sMinimapRect[4] = {};
std::chrono::steady_clock::time_point sMinimapStamp;
std::atomic< int > sMapTapPending{0};
bool sMapTapHeld = false;
// Drag to pan the map screen (Android): the overlay sends dp deltas and the view
// height, CAutoMapper drains them; it publishes whether panning applies.
std::mutex sMapPanMutex;
bool sMapScreenOpen = false;
std::chrono::steady_clock::time_point sMapScreenStamp;
float sMapPanX = 0.f;
float sMapPanY = 0.f;
float sMapPanViewDp = 400.f;
float sMapZoomPending = 1.f;
float sMapRotatePending = 0.f;
std::chrono::steady_clock::time_point sMapPanHeldUntil;
std::mutex sTouchAimMutex;
float sTouchAimPendingX = 0.f;
float sTouchAimPendingY = 0.f;
// GameCube scheme: this tick's touch travel, and whether a touch-aim finger is down
// (JNI, or the console's hold timer).
float sTouchLookFrameX = 0.f;
float sTouchLookFrameY = 0.f;
std::atomic<bool> sTouchAimDown{false};
std::atomic<uint64_t> sTouchAimHoldUntilNs{0};
bool sMouseCaptured = false;
bool sMouseGameplayActive = false;
bool sMouseInvertX = false;
bool sMouseInvertY = false;
bool sMouseButtons = true;
// What each mouse button does under mouse aim (PortInputMap::EMouseAction).
int sMouseActions[PortInputMap::kMouseButtonCount] = {
    PortInputMap::DefaultMouseAction(0), PortInputMap::DefaultMouseAction(1),
    PortInputMap::DefaultMouseAction(2), PortInputMap::DefaultMouseAction(3),
    PortInputMap::DefaultMouseAction(4)};
// The beam shift: two keys or mouse buttons (scancode or PAD_KEY_MOUSE_*) and
// a controller button (an SDL gamepad button or PAD_NATIVE_BUTTON_TRIGGER_*),
// -1 for none.
int sShiftBindings[3] = {SDL_SCANCODE_LSHIFT, -1, -1};
int sTurboBindings[3] = {-1, -1, -1}; // turbo fire: two keys, then a controller button
// A second controller button per PAD button, indexed by the PAD bit's position;
// the same codes as the shift's pad slot, -1 for none.
int sPadAltButtons[PortDebug::kPadAltCount] = {-1, -1, -1, -1, -1, -1, -1, -1,
                                               -1, -1, -1, -1, -1, -1, -1, -1};
bool sMouseCrosshair = true;
int sCrosshairSize = PortDebug::kCrosshairSizeDefault;
PortMouse::AimState sMouseAimState;
PortMouse::ButtonGate sMouseButtonGate;
PortMouse::ButtonGate sMouseMenuGate;
PortMouse::HeldButtons sMouseHeldButtons;
float sMouseSensitivity = 0.0035f;
float sMousePendingX = 0.f;
float sMousePendingY = 0.f;
float sMouseFrameX = 0.f;
float sMouseFrameY = 0.f;
// Gyro travel since the last tick. Kept apart from the mouse's pending delta,
// which is dropped whenever the mouse is not captured (twin stick, phones).
float sGyroPendingX = 0.f;
float sGyroPendingY = 0.f;
// The twin stick's aim speed at the last tick, in pixels per second, so frames
// between ticks can show the travel the next tick will add.
float sStickAimVelX = 0.f;
float sStickAimVelY = 0.f;
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
// The Remastered import's choices. Off on a phone: the rooms have never run on
// one, and need storage and memory many phones lack (a 256 MB game arena and
// 12x frame buffers).
#if defined(__ANDROID__)
bool sImportGeometry = false;
#else
bool sImportGeometry = true;
#endif
bool sImportEffects = false;
float sPresentOverride = -1.f;
unsigned sPresentCycleFrame = 0;
bool sTickHold = false;
unsigned sHeldTicks = 0;
// The last tick applied look input. A paused game or a cinematic skips the
// player update, and would then drop what the frames between ticks showed.
bool sAimAppliedLastTick = false;
bool sAiAudioEnabled = true;
bool sMusyxAudioEnabled = true;
bool sResetRequested = false;
std::atomic< bool > sToggleRequested{false};
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
// The Android touch overlay's gap to the side edges for every control, and the
// left stick's extra gap on top of it, in dp. Read from the UI thread.
constexpr float kTouchMarginMaxDp = 300.f;
constexpr float kTouchSideMarginDefault = 16.f;
constexpr float kTouchStickInsetDefault = 32.f;
std::atomic< float > sTouchSideMargin{kTouchSideMarginDefault};
std::atomic< float > sTouchStickInset{kTouchStickInsetDefault};
// The face buttons' and C-stick's extra gap to the right edge, in dp.
constexpr float kTouchButtonInsetDefault = 0.f;
std::atomic< float > sTouchButtonInset{kTouchButtonInsetDefault};
// Each touch control's own offset and size, as an opaque string the touch view
// parses (`<id>:<dx>,<dy>,<scale>;...`). Java reads and writes it on the UI thread
// and the settings file on the game thread, hence the mutex.
std::mutex sTouchLayoutMutex;
std::string sTouchLayout;
constexpr size_t kTouchLayoutMaxLen = 4096;
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
// Gyro state: the phone's sensor is looked up once, so the sensor list is not
// walked on every tick.
bool sPhoneGyroSearched = false;
SDL_Sensor* sPhoneGyro = nullptr;
const char* sGyroStatus = "off";
// Flick state: seconds left on the last flick, and whether the pitch has dropped
// back since, so a long flick counts once.
float sSpringFlickLatch = 0.f;
bool sSpringFlickArmed = true;
bool sGyroOverride = false;
float sGyroOverridePitch = 0.f;
float sGyroOverrideYaw = 0.f;
bool sVisible = false;
bool sSettingsDirty = false;
bool sAudioSettingsApplied = false;
bool sPresentationSettingsApplied = false;
CStateManager* sStateManager = nullptr;
int sPendingTeleport = -1;
bool sHasWorldTeleport = false;
std::string sDiscPath;
// The Remastered import's image and key file as last used: paths, or on Android
// the picked content:// addresses (the picker keeps their read grant).
std::string sRemasteredImagePath;
std::string sRemasteredKeysPath;
uint32_t sWorldTeleportWorld = 0;
uint32_t sWorldTeleportArea = 0;
bool sWorldSweepRequested = false;
struct WorldSweep {
  std::vector< uint32_t > worlds;
  std::vector< uint32_t > areas;
  size_t world = 0;
  size_t area = 0;
  unsigned settledTicks = 0;
  unsigned stalledTicks = 0;
  unsigned completedAreas = 0;
  bool active = false;
  bool waiting = false;
  // An area can hold several layers, and only the active ones are built, so a
  // pickup behind a layer the save has not unlocked is not in the dump at all.
  // The tour revisits each area once per layer with a different one active,
  // which is what makes the dump cover the whole area rather than the state the
  // save happens to be in.
  int layer = 0;
  int layerCount = 1;
  unsigned layerPasses = 0;
  // Set between the hop away from an area and the hop back to it: the area has
  // to be gone before it is rebuilt with the next layer.
  bool revisiting = false;
} sWorldSweep;

std::string SettingsFilePath() {
  const std::string& dir = PortPaths::UserFolder();
  return (dir.empty() ? std::string("./") : dir) + "port_settings.ini";
}

bool ParseBool(const std::string& value) {
  return value == "1" || value == "true" || value == "on" || value == "yes";
}

// The mouse button a settings key such as "mouse_left" names, or -1.
int MouseButtonSetting(const std::string& key) {
  for (int i = 0; i < PortInputMap::kMouseButtonCount; ++i) {
    if (key == PortInputMap::MouseButtonKey(i)) {
      return i;
    }
  }
  return -1;
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

void ApplySetting(const std::string& key, const std::string& value) {
  if (key == "frame_limit") {
    sFrameLimitEnabled = ParseBool(value);
  } else if (key == "vsync") {
    sVsyncEnabled = ParseBool(value);
  } else if (key == "fullscreen") {
    sFullscreen = ParseBool(value);
  } else if (key == "overlay_windowed") {
    sOverlayWindowed = ParseBool(value);
  } else if (key == "ui_theme") {
    sUiTheme = SDL_strcasecmp(value.c_str(), "prime") == 0        ? 1
               : SDL_strcasecmp(value.c_str(), "remastered") == 0 ? 2
               : SDL_strcasecmp(value.c_str(), "plain") == 0      ? 3
                                                                  : 0;
  } else if (key == "ui_sounds") {
    sUiSounds = ParseBool(value);
  } else if (key == "disc_path") {
    sDiscPath = value;
  } else if (key == "remastered_nsp") {
    sRemasteredImagePath = value;
  } else if (key == "remastered_keys") {
    sRemasteredKeysPath = value;
  } else if (key == "render_scale") {
    const float f = static_cast< float >(std::atof(value.c_str()));
    if (std::isfinite(f) && f >= 0.f && f <= 4.f) {
      sRenderScale = f;
    }
  } else if (key == "dynamic_res") {
    sDynamicRes = ParseBool(value);
  } else if (key == "dynamic_res_target") {
    sDynamicResTarget = std::clamp(std::atoi(value.c_str()), 0, 240);
  } else if (key == "dynamic_res_min") {
    const float f = static_cast< float >(std::atof(value.c_str()));
    if (std::isfinite(f)) {
      sDynamicResMin = std::clamp(f, 0.5f, 1.f);
    }
  } else if (key == "aspect") {
    if (value == "16:9") {
      sAspectMode = PortDebug::kAspect_16_9;
    } else if (value == "window") {
      sAspectMode = PortDebug::kAspect_Window;
    } else if (value == "4:3") {
      sAspectMode = PortDebug::kAspect_4_3;
    }
  } else if (key == "cinema_bars") {
    sCinemaBars = ParseBool(value);
  } else if (key == "show_shader_compilation") {
    sShowShaderCompilation = ParseBool(value);
  } else if (key == "hud_wide") {
    sHudWide = ParseBool(value);
  } else if (key == "hud_scale") {
    const int s = std::atoi(value.c_str());
    if (s >= PortDebug::kHudScaleMin && s <= PortDebug::kHudScaleMax) {
      sHudScale = s;
    }
  } else if (key == "hide_helmet") {
    sHideHelmet = ParseBool(value);
  } else if (key == "hide_visor_effects") {
    sHideVisorEffects = ParseBool(value);
  } else if (key == "reveal_map") {
    sRevealMap = ParseBool(value);
  } else if (key == "map_pickups") {
    sMapPickups = ParseBool(value);
  } else if (key == "tracker_progress") {
    sTrackerProgress = ParseBool(value);
  } else if (key == "map_logic_colors") {
    sMapLogicColors = ParseBool(value);
  } else if (key == "ap_suit_damage") {
    const int mode = std::atoi(value.c_str());
    sApSuitDamage = mode >= 0 && mode <= 2 ? mode : 1;
  } else if (key == "cheats") {
    sCheats = ParseBool(value);
  } else if (key == "text_language") {
    sTextLanguage = value;
  } else if (key == "skippable_cutscenes") {
    sSkippableCutscenes = ParseBool(value);
  } else if (key == "elevator_ride") {
    const int mode = std::atoi(value.c_str());
    sElevatorRide = mode >= PortDebug::kElevatorRide_Original && mode <= PortDebug::kElevatorRide_Skip
                        ? mode
                        : PortDebug::kElevatorRide_Original;
  } else if (key == "savestate_hotkeys") {
    sSaveStateHotkeys = ParseBool(value);
  } else if (key == "unlock_hard_mode") {
    sUnlockHardMode = ParseBool(value);
  } else if (key == "unlock_fusion_suit") {
    sUnlockFusionSuit = ParseBool(value);
  } else if (key == "unlock_galleries") {
    sUnlockGalleries = ParseBool(value);
  } else if (key == "msaa") {
    sMsaa = std::atoi(value.c_str()) >= 4 ? 4 : 1;
  } else if (key == "opengles") {
    sOpenGles = ParseBool(value);
    sOpenGlesAtStart = sOpenGles;
  } else if (key == "gpu_driver") {
    sGpuDriver = value;
    sGpuDriverAtStart = value;
  } else if (key == "storage_clamp") {
    const int v = std::atoi(value.c_str());
    sStorageClamp = v < 0 ? -1 : (v > 0 ? 1 : 0);
    sStorageClampAtStart = sStorageClamp;
  } else if (key == "anisotropy") {
    const int a = std::atoi(value.c_str());
    if (a >= 1 && a <= 16) {
      sAnisotropy = a;
    }
  } else if (key == "fov") {
    const float f = static_cast< float >(std::atof(value.c_str()));
    if (std::isfinite(f) && f >= PortDebug::kFovMin && f <= PortDebug::kFovMax) {
      sFirstPersonFov = f;
    }
  } else if (key == "mouse_aim") {
    sMouseAim = ParseBool(value);
  } else if (key == "twin_stick") {
    sTwinStick = ParseBool(value);
  } else if (key == "touch_colors") {
    sTouchColors = ParseBool(value);
  } else if (key == "touch_labels") {
    sTouchLabels = ParseBool(value);
  } else if (key == "touch_turbo") {
    sTouchTurbo = ParseBool(value);
  } else if (key == "touch_floating_stick") {
    sTouchFloatingStick = ParseBool(value);
  } else if (key == "stick_aim_rate") {
    const float f = static_cast< float >(std::atof(value.c_str()));
    if (std::isfinite(f) && f >= 50.f && f <= 4000.f) {
      sStickAimRate = f;
    }
  } else if (key == "gyro_mode") {
    const long v = std::strtol(value.c_str(), nullptr, 10);
    if (v >= 0 && v <= 2) {
      sGyroMode = static_cast< int >(v);
    }
  } else if (key == "gyro_source") {
    const long v = std::strtol(value.c_str(), nullptr, 10);
    if (v >= 0 && v <= 2) {
      sGyroSource = static_cast< int >(v);
    }
  } else if (key == "gyro_rate") {
    const float f = static_cast< float >(std::atof(value.c_str()));
    if (std::isfinite(f) && f >= 20.f && f <= 5000.f) {
      sGyroRate = f;
    }
  } else if (key == "touch_aim") {
    sTouchAim = ParseBool(value);
  } else if (key == "touch_map_tap") {
    sTouchMapTap = ParseBool(value);
  } else if (key == "touch_classic_gc") {
    sTouchClassic = ParseBool(value);
  } else if (key == "touch_twin_stick") {
    sTouchTwinStick = ParseBool(value);
  } else if (key == "touch_wheels") {
    sTouchWheels = ParseBool(value);
  } else if (key == "touch_visor_tap_scan") {
    sTouchVisorTapScan = ParseBool(value);
  } else if (key == "touch_aim_speed") {
    const float f = static_cast< float >(std::atof(value.c_str()));
    if (std::isfinite(f) && f >= 0.25f && f <= 10.f) {
      sTouchAimSpeed = f;
    }
  } else if (key == "touch_side_margin" || key == "touch_stick_inset" ||
             key == "touch_button_inset") {
    const float f = static_cast< float >(std::atof(value.c_str()));
    if (std::isfinite(f) && f >= 0.f && f <= kTouchMarginMaxDp) {
      (key == "touch_side_margin"   ? sTouchSideMargin
       : key == "touch_stick_inset" ? sTouchStickInset
                                    : sTouchButtonInset)
          .store(f);
    }
  } else if (key == "touch_layout") {
    // Printable ASCII only: JNI's NewStringUTF aborts on invalid UTF-8.
    const bool ascii = std::all_of(value.begin(), value.end(),
                                   [](unsigned char c) { return c >= 0x20 && c < 0x7F; });
    if (ascii && value.size() <= kTouchLayoutMaxLen) {
      std::lock_guard< std::mutex > lock(sTouchLayoutMutex);
      sTouchLayout = value;
    }
  } else if (key == "mouse_invert_x") {
    sMouseInvertX = ParseBool(value);
  } else if (key == "mouse_invert_y") {
    sMouseInvertY = ParseBool(value);
  } else if (key == "mouse_buttons") {
    sMouseButtons = ParseBool(value);
  } else if (key == "mouse_crosshair") {
    sMouseCrosshair = ParseBool(value);
  } else if (key == "crosshair_size") {
    const int s = std::atoi(value.c_str());
    if (s >= PortDebug::kCrosshairSizeMin && s <= PortDebug::kCrosshairSizeMax) {
      sCrosshairSize = s;
    }
  } else if (key == "mouse_sensitivity") {
    const float f = static_cast< float >(std::atof(value.c_str()));
    if (std::isfinite(f) && f > 0.f && f <= 1.f) {
      sMouseSensitivity = f;
    }
  } else if (key == "spring_ball") {
    sSpringBall = ParseBool(value);
  } else if (key == "swap_scan_xray") {
    sSwapScanXray = ParseBool(value);
  } else if (key == "shift_key" || key == "shift_key_alt" || key == "shift_pad") {
    const int slot = key == "shift_key" ? 0 : key == "shift_key_alt" ? 1 : 2;
    // A number, or the value is ignored (atoi would read junk as scancode 0).
    char* end = nullptr;
    const long code = std::strtol(value.c_str(), &end, 10);
    if (end != value.c_str() && *end == '\0') {
      sShiftBindings[slot] = static_cast< int >(code);
    }
  } else if (key == "turbo_key" || key == "turbo_key_alt" || key == "turbo_pad") {
    const int slot = key == "turbo_key" ? 0 : key == "turbo_key_alt" ? 1 : 2;
    char* end = nullptr;
    const long code = std::strtol(value.c_str(), &end, 10);
    if (end != value.c_str() && *end == '\0') {
      sTurboBindings[slot] = static_cast< int >(code);
    }
  } else if (key == "pad_alt") {
    // kPadAltCount comma-separated codes; a short or malformed list keeps the
    // rest as they are.
    const char* cursor = value.c_str();
    for (int i = 0; i < PortDebug::kPadAltCount && *cursor != '\0'; ++i) {
      char* end = nullptr;
      const long code = std::strtol(cursor, &end, 10);
      if (end == cursor) {
        break;
      }
      sPadAltButtons[i] = static_cast< int >(code);
      cursor = *end == ',' ? end + 1 : end;
    }
  } else if (MouseButtonSetting(key) >= 0) {
    const int action = PortInputMap::MouseActionFromName(value.c_str());
    if (action >= 0) {
      sMouseActions[MouseButtonSetting(key)] = action;
    }
  } else if (key == "speedrun_timer") {
    sSpeedrunTimer = ParseBool(value);
  } else if (key == "livesplit") {
    sLiveSplit = ParseBool(value);
  } else if (key == "livesplit_address") {
    if (!value.empty()) {
      sLiveSplitAddress = value;
    }
  } else if (key == "livesplit_split_upgrades") {
    sLiveSplitSplitUpgrades = ParseBool(value);
  } else if (key == "discord_presence") {
    // Not "discord": 0.16.0 saved discord=0 for everyone while it was opt-in.
    sDiscord = ParseBool(value);
  } else if (key == "update_check") {
    sUpdateCheck = ParseBool(value);
  } else if (key == "mods") {
    sModsEnabled = ParseBool(value);
  } else if (key == "original_experience") {
    sOriginalExperience = ParseBool(value);
  } else if (key == "mods_disabled") {
    sModsDisabled = value;
  } else if (key == "fast_morph") {
    sFastMorph = ParseBool(value);
  } else if (key == "invulnerable") {
    sInvulnerable = ParseBool(value);
  } else if (key == "logging") {
    // Not "log_file": builds from before the log was on by default wrote
    // log_file=0 into every settings file, which kept it off after an update.
    sLogFile = ParseBool(value);
  } else if (key == "lock_on_toggle") {
    sLockOnToggle = ParseBool(value);
  } else if (key == "sticky_charge") {
    sStickyCharge = ParseBool(value);
  } else if (key == "rapid_charge") {
    sRapidCharge = ParseBool(value);
  } else if (key == "rando_settings") {
    PortRandoGen::Settings parsed;
    if (!PortRandoGen::ParseSettings(value, parsed)) {
      parsed = PortRandoGen::Settings();
    }
    std::lock_guard< std::mutex > lock(sRandoMutex);
    sRandoSettings = parsed;
  } else if (key == "spring_ball_flick") {
    sSpringFlick = ParseBool(value);
  } else if (key == "spring_ball_flick_rate") {
    const float f = static_cast< float >(std::atof(value.c_str()));
    if (std::isfinite(f) && f >= 2.f && f <= 20.f) {
      sSpringFlickRate = f;
    }
  } else if (key == "sim_rate") {
    const long rate = std::strtol(value.c_str(), nullptr, 10);
    if (rate >= 30 && rate <= 480) {
      sSimRate = static_cast< unsigned >(rate);
    }
  } else if (key == "sim_adaptive") {
    sSimAdaptive = ParseBool(value);
  } else if (key == "smooth_frames") {
    sSmoothFrames = ParseBool(value);
    sFrameInterpolation = sActorInterpolation = sPoseInterpolation = sParticleInterpolation = sSmoothFrames;
  } else if (key == "room_geo_gpu") {
    sRoomGeoResident = ParseBool(value);
  } else if (key == "room_geo_resident") {
    // The old key, saved as 0 by everyone while it was off by default: only an explicit 1 counts.
    sRoomGeoResident = sRoomGeoResident || ParseBool(value);
  } else if (key == "room_geo_min_px") {
    PortRoomGeo::SetMinPixels(std::strtof(value.c_str(), nullptr));
  } else if (key == "room_geo_lod") {
    PortRoomGeo::SetLodDistance(std::strtof(value.c_str(), nullptr));
  } else if (key == "remastered_import_geometry") {
    sImportGeometry = ParseBool(value);
  } else if (key == "remastered_import_effects") {
    sImportEffects = ParseBool(value);
  } else if (key == "ai_audio") {
    sAiAudioEnabled = ParseBool(value);
  } else if (key == "musyx_audio") {
    sMusyxAudioEnabled = ParseBool(value);
  } else if (key == "voices_muted") {
    MusyxPortClearSampleMutes();
    const char* cursor = value.c_str();
    while (*cursor != '\0') {
      char* end = nullptr;
      const unsigned long id = std::strtoul(cursor, &end, 10);
      if (end == cursor) {
        break;
      }
      MusyxPortSetSampleMuted(static_cast< unsigned >(id), 1);
      cursor = end;
      while (*cursor == ',' || *cursor == ' ') {
        ++cursor;
      }
    }
  }
}

void LoadSettings() {
  const std::string path = SettingsFilePath();
  std::ifstream file(path);
  if (!file.is_open()) {
    return;
  }
  std::fprintf(stderr, "metroid_prime_port: loaded settings from %s\n", path.c_str());
  std::string line;
  while (std::getline(file, line)) {
    const size_t comment = line.find('#');
    if (comment != std::string::npos) {
      line.erase(comment);
    }
    const size_t separator = line.find('=');
    if (separator == std::string::npos) {
      continue;
    }
    const std::string key = Trim(line.substr(0, separator));
    const std::string value = Trim(line.substr(separator + 1));
    if (!key.empty()) {
      ApplySetting(key, value);
    }
  }
  if (sTouchClassic) {
    sTouchTwinStick = false;
  }
}

void SaveSettings() {
  if (!sInitialized || !sSettingsDirty) {
    return;
  }
  const std::string path = SettingsFilePath();
  std::ofstream file(path, std::ios::trunc);
  if (!file.is_open()) {
    std::fprintf(stderr, "metroid_prime_port: could not write settings to %s\n", path.c_str());
    return;
  }
  const char* aspect = sAspectMode == PortDebug::kAspect_16_9  ? "16:9"
                       : sAspectMode == PortDebug::kAspect_Window ? "window"
                                                                  : "4:3";
  file << "# Metroid Prime native port settings. Written by the F1 debug overlay.\n";
  file << "# Environment variables (MP_*) override these for a single run.\n";
  file << "aspect=" << aspect << '\n';
  file << "hud_wide=" << (sHudWide ? 1 : 0) << '\n';
  file << "cinema_bars=" << (sCinemaBars ? 1 : 0) << '\n';
  file << "show_shader_compilation=" << (sShowShaderCompilation ? 1 : 0) << '\n';
  file << "hud_scale=" << sHudScale << '\n';
  file << "hide_helmet=" << (sHideHelmet ? 1 : 0) << '\n';
  file << "hide_visor_effects=" << (sHideVisorEffects ? 1 : 0) << '\n';
  file << "reveal_map=" << (sRevealMap ? 1 : 0) << '\n';
  file << "map_pickups=" << (sMapPickups ? 1 : 0) << '\n';
  file << "tracker_progress=" << (sTrackerProgress ? 1 : 0) << '\n';
  file << "map_logic_colors=" << (sMapLogicColors ? 1 : 0) << '\n';
  file << "ap_suit_damage=" << sApSuitDamage << '\n';
  file << "cheats=" << (sCheats ? 1 : 0) << '\n';
  file << "skippable_cutscenes=" << (sSkippableCutscenes ? 1 : 0) << '\n';
  file << "text_language=" << sTextLanguage << '\n';
  file << "elevator_ride=" << sElevatorRide << '\n';
  file << "savestate_hotkeys=" << (sSaveStateHotkeys ? 1 : 0) << '\n';
  file << "fov=" << sFirstPersonFov << '\n';
  file << "msaa=" << sMsaa << '\n';
  file << "opengles=" << (sOpenGles ? 1 : 0) << '\n';
  file << "gpu_driver=" << sGpuDriver << '\n';
  file << "anisotropy=" << sAnisotropy << '\n';
  file << "unlock_hard_mode=" << (sUnlockHardMode ? 1 : 0) << '\n';
  file << "unlock_fusion_suit=" << (sUnlockFusionSuit ? 1 : 0) << '\n';
  file << "unlock_galleries=" << (sUnlockGalleries ? 1 : 0) << '\n';
  file << "storage_clamp=" << sStorageClamp << '\n';
  file << "speedrun_timer=" << (sSpeedrunTimer ? 1 : 0) << '\n';
  file << "livesplit=" << (sLiveSplit ? 1 : 0) << '\n';
  file << "livesplit_address=" << sLiveSplitAddress << '\n';
  file << "livesplit_split_upgrades=" << (sLiveSplitSplitUpgrades ? 1 : 0) << '\n';
  file << "discord_presence=" << (sDiscord ? 1 : 0) << '\n';
  file << "update_check=" << (sUpdateCheck ? 1 : 0) << '\n';
  file << "mods=" << (sModsEnabled ? 1 : 0) << '\n';
  file << "original_experience=" << (sOriginalExperience ? 1 : 0) << '\n';
  file << "mods_disabled=" << sModsDisabled << '\n';
  file << "vsync=" << (sVsyncEnabled ? 1 : 0) << '\n';
  file << "fullscreen=" << (sFullscreen ? 1 : 0) << '\n';
  file << "overlay_windowed=" << (sOverlayWindowed ? 1 : 0) << '\n';
  file << "ui_theme=" << (sUiTheme == 1 ? "prime" : sUiTheme == 2 ? "remastered" : sUiTheme == 3 ? "plain" : "auto")
       << '\n';
  file << "ui_sounds=" << (sUiSounds ? 1 : 0) << '\n';
  file << "render_scale=" << sRenderScale << '\n';
  file << "dynamic_res=" << (sDynamicRes ? 1 : 0) << '\n';
  file << "dynamic_res_target=" << sDynamicResTarget << '\n';
  file << "dynamic_res_min=" << sDynamicResMin << '\n';
  file << "frame_limit=" << (sFrameLimitEnabled ? 1 : 0) << '\n';
  file << "sim_rate=" << sSimRate << '\n';
  file << "sim_adaptive=" << (sSimAdaptive ? 1 : 0) << '\n';
  file << "smooth_frames=" << (sSmoothFrames ? 1 : 0) << '\n';
  file << "room_geo_gpu=" << (sRoomGeoResident ? 1 : 0) << '\n';
  file << "room_geo_min_px=" << PortRoomGeo::MinPixels() << '\n';
  file << "room_geo_lod=" << PortRoomGeo::LodDistance() << '\n';
  file << "remastered_import_geometry=" << (sImportGeometry ? 1 : 0) << '\n';
  file << "remastered_import_effects=" << (sImportEffects ? 1 : 0) << '\n';
  file << "mouse_aim=" << (sMouseAim ? 1 : 0) << '\n';
  file << "twin_stick=" << (sTwinStick ? 1 : 0) << '\n';
  file << "touch_colors=" << (sTouchColors ? 1 : 0) << '\n';
  file << "touch_labels=" << (sTouchLabels ? 1 : 0) << '\n';
  file << "touch_turbo=" << (sTouchTurbo ? 1 : 0) << '\n';
  file << "touch_floating_stick=" << (sTouchFloatingStick ? 1 : 0) << '\n';
  file << "spring_ball=" << (sSpringBall ? 1 : 0) << '\n';
  file << "swap_scan_xray=" << (sSwapScanXray ? 1 : 0) << '\n';
  file << "shift_key=" << sShiftBindings[0] << '\n';
  file << "shift_key_alt=" << sShiftBindings[1] << '\n';
  file << "shift_pad=" << sShiftBindings[2] << '\n';
  file << "turbo_key=" << sTurboBindings[0] << '\n';
  file << "turbo_key_alt=" << sTurboBindings[1] << '\n';
  file << "turbo_pad=" << sTurboBindings[2] << '\n';
  file << "pad_alt=";
  for (int i = 0; i < PortDebug::kPadAltCount; ++i) {
    file << (i != 0 ? "," : "") << sPadAltButtons[i];
  }
  file << '\n';
  file << "fast_morph=" << (sFastMorph ? 1 : 0) << '\n';
  file << "invulnerable=" << (sInvulnerable ? 1 : 0) << '\n';
  file << "logging=" << (sLogFile ? 1 : 0) << '\n';
  file << "lock_on_toggle=" << (sLockOnToggle ? 1 : 0) << '\n';
  file << "sticky_charge=" << (sStickyCharge ? 1 : 0) << '\n';
  file << "rapid_charge=" << (sRapidCharge ? 1 : 0) << '\n';
  {
    std::lock_guard< std::mutex > lock(sRandoMutex);
    file << "rando_settings=" << PortRandoGen::SettingsText(sRandoSettings) << '\n';
  }
  file << "spring_ball_flick=" << (sSpringFlick ? 1 : 0) << '\n';
  file << "spring_ball_flick_rate=" << sSpringFlickRate << '\n';
  file << "stick_aim_rate=" << sStickAimRate << '\n';
  file << "gyro_mode=" << sGyroMode << '\n';
  file << "gyro_source=" << sGyroSource << '\n';
  file << "gyro_rate=" << sGyroRate << '\n';
  file << "touch_aim=" << (sTouchAim ? 1 : 0) << '\n';
  file << "touch_aim_speed=" << sTouchAimSpeed << '\n';
  file << "touch_side_margin=" << sTouchSideMargin.load() << '\n';
  file << "touch_stick_inset=" << sTouchStickInset.load() << '\n';
  file << "touch_button_inset=" << sTouchButtonInset.load() << '\n';
  {
    std::lock_guard< std::mutex > lock(sTouchLayoutMutex);
    file << "touch_layout=" << sTouchLayout << '\n';
  }
  file << "touch_map_tap=" << (sTouchMapTap ? 1 : 0) << '\n';
  file << "touch_classic_gc=" << (sTouchClassic ? 1 : 0) << '\n';
  file << "touch_twin_stick=" << (sTouchTwinStick ? 1 : 0) << '\n';
  file << "touch_wheels=" << (sTouchWheels ? 1 : 0) << '\n';
  file << "touch_visor_tap_scan=" << (sTouchVisorTapScan ? 1 : 0) << '\n';
  file << "mouse_invert_x=" << (sMouseInvertX ? 1 : 0) << '\n';
  file << "mouse_invert_y=" << (sMouseInvertY ? 1 : 0) << '\n';
  file << "mouse_buttons=" << (sMouseButtons ? 1 : 0) << '\n';
  for (int i = 0; i < PortInputMap::kMouseButtonCount; ++i) {
    file << PortInputMap::MouseButtonKey(i) << '=' << PortInputMap::MouseActionInfo(sMouseActions[i]).name
         << '\n';
  }
  file << "mouse_crosshair=" << (sMouseCrosshair ? 1 : 0) << '\n';
  file << "crosshair_size=" << sCrosshairSize << '\n';
  file << "mouse_sensitivity=" << sMouseSensitivity << '\n';
  if (!sDiscPath.empty()) {
    file << "disc_path=" << sDiscPath << '\n';
  }
  if (!sRemasteredImagePath.empty()) {
    file << "remastered_nsp=" << sRemasteredImagePath << '\n';
  }
  if (!sRemasteredKeysPath.empty()) {
    file << "remastered_keys=" << sRemasteredKeysPath << '\n';
  }
  file << "ai_audio=" << (sAiAudioEnabled ? 1 : 0) << '\n';
  file << "musyx_audio=" << (sMusyxAudioEnabled ? 1 : 0) << '\n';
  unsigned muted[64];
  const int mutedCount = MusyxPortGetMutedSamples(muted, 64);
  if (mutedCount > 0) {
    file << "voices_muted=";
    for (int i = 0; i < mutedCount; ++i) {
      file << (i == 0 ? "" : ",") << muted[i];
    }
    file << '\n';
  }
  file.flush();
  std::fprintf(stderr, "metroid_prime_port: saved settings to %s\n", path.c_str());
  sSettingsDirty = false;
}

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

bool SDLCALL debug_event_watch(void*, SDL_Event* event) {
  // F1 toggles the overlay. Watch the event rather than polling the key state:
  // a short tap can begin and end between two frames, so polling misses it.
  if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat &&
      event->key.scancode == SDL_SCANCODE_F1) {
    PortDebug::RequestToggle();
  }
  if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat &&
      (event->key.scancode == SDL_SCANCODE_F5 || event->key.scancode == SDL_SCANCODE_F9)) {
    sSaveStateHotkey.store(event->key.scancode == SDL_SCANCODE_F5 ? 1 : 2,
                           std::memory_order_release);
  }
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
  sRapidCharge = port::EnvFlag("MP_RAPID_CHARGE", sRapidCharge);
  if (port::EnvFlag("MP_MOUSE_AIM")) {
    sMouseAim = true;
  }
  if (port::EnvFlag("MP_TWIN_STICK")) {
    sTwinStick = true;
  }
  if (port::EnvFlag("MP_MOUSE_INVERT_X")) {
    sMouseInvertX = true;
  }
  if (port::EnvFlag("MP_MOUSE_INVERT_Y")) {
    sMouseInvertY = true;
  }
  if (port::EnvFlag("MP_DISABLE_MOUSE_BUTTONS")) {
    sMouseButtons = false;
  }
  if (port::EnvFlag("MP_DISABLE_MOUSE_CROSSHAIR")) {
    sMouseCrosshair = false;
  }
  {
    const float value = port::EnvFloat("MP_MOUSE_SENS", 0.f);
    if (std::isfinite(value) && value > 0.f) {
      sMouseSensitivity = value;
    }
  }
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

  std::atexit(SaveSettings);
  ApplyLiveSplit();
  ApplyDiscord();
  ApplyUpdateCheck();
  PortUpdateCheck::CheckNow(); // once per launch
}
} // namespace

namespace PortDebug {

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
static float EffectiveRenderScale() { return sOriginalExperience ? 1.f : sRenderScale; }
static int EffectiveMsaa() { return sOriginalExperience ? 1 : sMsaa; }
static int EffectiveAnisotropy() { return sOriginalExperience ? 1 : sAnisotropy; }

static void ApplyGraphicsQuality() {
  aurora_set_graphics_quality(static_cast< uint32_t >(EffectiveMsaa()),
                              static_cast< uint16_t >(EffectiveAnisotropy()));
}

bool FrameLimitEnabled() {
  EnsureInitialized();
  return EffectiveFrameLimit();
}


static double DynamicResTargetFps() {
  double target = sDynamicResTarget;
  if (target <= 0.0) {
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    target = mode != nullptr && mode->refresh_rate > 0.f ? mode->refresh_rate : 60.0;
  }
  return EffectiveFrameLimit() ? std::min(target, 60.0) : target;
}

static void ResetDynamicRes() {
  sDynSlow = sDynSteady = sDynSettle = sDynHoldLow = 0;
  sDynRaiseAfter = 4;
  sDynHoldFor = 30;
  sDynSinceRaise = -1;
  sDynFpsBefore = 0.0;
  if (sDynScale > 0.f) {
    sDynScale = 0.f;
    VISetFrameBufferScale(EffectiveRenderScale());
  }
}

// Once a second, from the presented rate. Each step reallocates the EFB and waits for the
// GPU, so it steps rarely: down after two slow seconds, up after a steady stretch that
// doubles each time a step up turns out too slow.
static void UpdateDynamicRes(double fps, unsigned frames) {
  const float top = EffectiveRenderScale();
  const float bottom = std::min(sDynamicResMin, top);
  if (!sDynamicRes || sOriginalExperience || top <= bottom || sTurbo) {
    ResetDynamicRes();
    return;
  }
  // Paused or loading: nothing to go on.
  if (frames < 10) {
    return;
  }
  // The second after a step holds its reallocation stall.
  if (sDynSettle > 0) {
    --sDynSettle;
    return;
  }
  const float scale = sDynScale > 0.f ? sDynScale : top;
  // Half steps above 2x, where a quarter step changes the pixel count by little.
  const float stepUp = scale >= 2.f ? 0.5f : 0.25f;
  const float stepDown = scale > 2.f ? 0.5f : 0.25f;
  const double target = DynamicResTargetFps();
  if (sDynHoldLow > 0) {
    --sDynHoldLow;
  }
  float next = scale;
  if (sDynFpsBefore > 0.0) {
    // The first reading after a step down: no gain means the GPU's pixels were not what
    // held the frame back, so go back up and stay there for a while.
    if (fps < sDynFpsBefore * 1.03) {
      next = scale + stepUp;
      sDynHoldLow = sDynHoldFor;
      sDynHoldFor = std::min(sDynHoldFor * 2, 240);
    }
    sDynFpsBefore = 0.0;
  } else if (fps < target * 0.93) {
    sDynSteady = 0;
    if (sDynSinceRaise >= 0) {
      next = scale - stepDown;
      sDynRaiseAfter = std::min(sDynRaiseAfter * 2, 64);
      sDynSinceRaise = -1;
    } else if (++sDynSlow >= 2 && sDynHoldLow == 0 && scale > bottom) {
      next = scale - stepDown;
      sDynFpsBefore = fps;
    }
  } else {
    sDynSlow = 0;
    if (sDynSinceRaise >= 0 && ++sDynSinceRaise >= 3) {
      sDynSinceRaise = -1;
    }
    if (sDynSinceRaise < 0 && fps >= target * 0.97 && scale < top && ++sDynSteady >= sDynRaiseAfter) {
      next = scale + stepUp;
      sDynSteady = 0;
      sDynSinceRaise = 0;
    }
  }
  next = std::clamp(next, bottom, top);
  if (next != scale) {
    sDynSlow = 0;
    sDynScale = next;
    sDynSettle = 1;
    VISetFrameBufferScale(next);
    PortLog::Write("port: dynamic resolution %.2fx (%.0f fps, target %.0f)\n", next, fps, target);
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
                   "[timing] presented=%.1f FPS throughput=%.1f FPS simulation=%.1f ticks/s cap=%s\n",
                   sActualFps, sThroughputFps, sActualTps, EffectiveFrameLimit() ? "60" : "off");
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

int HudScale() {
  EnsureInitialized();
  return sOriginalExperience ? kHudScaleMax : sHudScale;
}

void SetHudScale(int percent) {
  EnsureInitialized();
  sHudScale = std::clamp(percent, kHudScaleMin, kHudScaleMax);
  MarkDirty();
}

bool HideHelmet() {
  EnsureInitialized();
  return sHideHelmet && !sOriginalExperience;
}

void SetHideHelmet(bool enabled) {
  EnsureInitialized();
  sHideHelmet = enabled;
  MarkDirty();
}

bool HideVisorEffects() {
  EnsureInitialized();
  return sHideVisorEffects && !sOriginalExperience;
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

bool MapPickups() {
  EnsureInitialized();
  return sMapPickups;
}

void SetMapPickups(bool enabled) {
  EnsureInitialized();
  sMapPickups = enabled;
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
  return sMouseAim;
}

void SetMouseAim(bool enabled) {
  EnsureInitialized();
  sMouseAim = enabled;
  ResetMouseAim();
}

bool TouchDirectAim() {
#if defined(__ANDROID__)
  return !sTouchClassic && sTouchActive.load(std::memory_order_acquire) && !Visible();
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
    return sTouchTwinStick;
  }
#endif
  return sTwinStick && !TouchActive();
}

bool PadTwinStick() {
  EnsureInitialized();
  return sTwinStick;
}

void SetTwinStick(bool enabled) {
  EnsureInitialized();
  sTwinStick = enabled;
  MarkDirty();
}

float TwinStickRightY() { return sTwinStickRightY; }

void SetTwinStickRightY(float y) { sTwinStickRightY = y; }

bool BeamShiftHeld() { return sBeamShiftHeld; }

bool TouchBeamShift() { return sTouchBeamShift.load(std::memory_order_acquire) && TouchActive(); }

bool TouchTurboFire() {
  return sTouchTurboFire.load(std::memory_order_acquire) && TouchActive() && !sOriginalExperience;
}

void SetBeamShiftHeld(bool held) { sBeamShiftHeld = held; }

bool SpringBall() {
  EnsureInitialized();
  return sSpringBall && !sOriginalExperience;
}

void SetSpringBall(bool enabled) {
  EnsureInitialized();
  sSpringBall = enabled;
  MarkDirty();
}

bool SwapScanXray() {
  EnsureInitialized();
  return sSwapScanXray && !TouchActive() && !sOriginalExperience;
}

void SetSwapScanXray(bool enabled) {
  EnsureInitialized();
  sSwapScanXray = enabled;
  MarkDirty();
}

int TurboBinding(int slot) {
  EnsureInitialized();
  if (slot == 2 && TouchActive()) {
    return -1;
  }
  return slot >= 0 && slot < 3 ? sTurboBindings[slot] : -1;
}

void SetTurboBinding(int slot, int code) {
  EnsureInitialized();
  if (slot >= 0 && slot < 3) {
    sTurboBindings[slot] = code;
    MarkDirty();
  }
}

int ShiftBinding(int slot) {
  EnsureInitialized();
  if (slot == 2 && TouchActive()) {
    return -1;
  }
  return slot >= 0 && slot < 3 ? sShiftBindings[slot] : -1;
}

void SetShiftBinding(int slot, int code) {
  EnsureInitialized();
  if (slot >= 0 && slot < 3) {
    sShiftBindings[slot] = code;
    MarkDirty();
  }
}

int PadAltButton(int bit) {
  EnsureInitialized();
  return bit >= 0 && bit < kPadAltCount && !TouchActive() ? sPadAltButtons[bit] : -1;
}

void SetPadAltButton(int bit, int code) {
  EnsureInitialized();
  if (bit >= 0 && bit < kPadAltCount && sPadAltButtons[bit] != code) {
    sPadAltButtons[bit] = code;
    MarkDirty();
  }
}

int MouseAction(int button) {
  EnsureInitialized();
  return button >= 0 && button < PortInputMap::kMouseButtonCount ? sMouseActions[button]
                                                                  : PortInputMap::kMA_None;
}

void SetMouseAction(int button, int action) {
  EnsureInitialized();
  if (button >= 0 && button < PortInputMap::kMouseButtonCount && action >= 0 &&
      action < PortInputMap::kMA_Count) {
    sMouseActions[button] = action;
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
  return sFastMorph && !sOriginalExperience;
}

void SetFastMorph(bool enabled) {
  EnsureInitialized();
  sFastMorph = enabled;
  MarkDirty();
}

bool LockOnToggle() {
  EnsureInitialized();
  return sLockOnToggle && !sOriginalExperience;
}

void SetLockOnToggle(bool enabled) {
  EnsureInitialized();
  sLockOnToggle = enabled;
  MarkDirty();
}

bool StickyCharge() {
  EnsureInitialized();
  return sStickyCharge && !sOriginalExperience;
}

void SetStickyCharge(bool enabled) {
  EnsureInitialized();
  sStickyCharge = enabled;
  MarkDirty();
}

PortRandoGen::Settings RandoSettings() {
  EnsureInitialized();
  std::lock_guard< std::mutex > lock(sRandoMutex);
  return sRandoSettings;
}

bool RapidCharge() {
  EnsureInitialized();
  return sRapidCharge && !sOriginalExperience;
}

void SetRapidCharge(bool enabled) {
  EnsureInitialized();
  sRapidCharge = enabled;
  MarkDirty();
}

bool SpringBallFlick() {
  EnsureInitialized();
  return sSpringFlick && !sOriginalExperience;
}

void SetSpringBallFlick(bool enabled) {
  EnsureInitialized();
  sSpringFlick = enabled;
  MarkDirty();
}

float SpringBallFlickRate() {
  EnsureInitialized();
  return sSpringFlickRate;
}

void SetSpringBallFlickRate(float radiansPerSecond) {
  EnsureInitialized();
  if (std::isfinite(radiansPerSecond) && radiansPerSecond >= 2.f && radiansPerSecond <= 20.f) {
    sSpringFlickRate = radiansPerSecond;
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
  return sStickAimRate;
}

void SetStickAimRate(float pixelsPerSecond) {
  EnsureInitialized();
  if (std::isfinite(pixelsPerSecond) && pixelsPerSecond >= 50.f && pixelsPerSecond <= 4000.f) {
    sStickAimRate = pixelsPerSecond;
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
  sStickAimVelX = x * sStickAimRate;
  sStickAimVelY = -y * sStickAimRate;
  sMouseFrameX += sStickAimVelX * dt;
  sMouseFrameY += sStickAimVelY * dt;
}

int GyroMode() {
  EnsureInitialized();
  return sGyroMode;
}

void SetGyroMode(int mode) {
  EnsureInitialized();
  if (mode < 0 || mode > 2) {
    return;
  }
  sGyroMode = mode;
  MarkDirty();
}

int GyroSource() {
  EnsureInitialized();
  return sGyroSource;
}

void SetGyroSource(int source) {
  EnsureInitialized();
  if (source < 0 || source > 2) {
    return;
  }
  sGyroSource = source;
  MarkDirty();
}

float GyroRate() {
  EnsureInitialized();
  return sGyroRate;
}

void SetGyroRate(float pixelsPerSecondPerRad) {
  EnsureInitialized();
  if (std::isfinite(pixelsPerSecondPerRad) && pixelsPerSecondPerRad >= 20.f &&
      pixelsPerSecondPerRad <= 5000.f) {
    sGyroRate = pixelsPerSecondPerRad;
    MarkDirty();
  }
}

const char* GyroStatus() { return sGyroStatus; }

namespace {
// Quarter turns from portrait, as SDL numbers Android rotations (landscape is
// the phone's right side up).
int OrientationQuarters(SDL_DisplayOrientation orientation) {
  switch (orientation) {
  case SDL_ORIENTATION_LANDSCAPE:
    return 1;
  case SDL_ORIENTATION_PORTRAIT_FLIPPED:
    return 2;
  case SDL_ORIENTATION_LANDSCAPE_FLIPPED:
    return 3;
  default:
    return 0;
  }
}

// Pitch (x, positive tilts the far edge up) and yaw (positive turns right)
// rates in rad/s from the chosen source; false when there is none.
bool ReadGyroRates(float& pitch, float& yaw) {
  if (sGyroOverride) {
    pitch = sGyroOverridePitch;
    yaw = sGyroOverrideYaw;
    sGyroStatus = "console";
    return true;
  }
  const bool wantController = sGyroSource == 0 || sGyroSource == 1;
  const bool wantPhone = sGyroSource == 0 || sGyroSource == 2;
  bool haveRates = false;

  if (wantController) {
    if (SDL_Gamepad* pad = PADGetSDLGamepadForIndex(0)) {
      if (SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO)) {
        // Asked per pad, so a reconnected or swapped pad gets its gyro on too.
        if (!SDL_GamepadSensorEnabled(pad, SDL_SENSOR_GYRO)) {
          SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
        }
        float data[3];
        if (SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, data, 3)) {
          // Radians per second; x is pitch, y is yaw. Sensor rates are
          // counter-clockwise positive, so a positive y turns left.
          yaw = -data[1];
          pitch = data[0];
          haveRates = true;
          sGyroStatus = "controller";
        }
      }
    }
  }

  if (!haveRates && wantPhone) {
    if (sPhoneGyro == nullptr && !sPhoneGyroSearched) {
      sPhoneGyroSearched = true;
      int count = 0;
      if (SDL_SensorID* ids = SDL_GetSensors(&count)) {
        for (int i = 0; i < count; ++i) {
          if (SDL_GetSensorTypeForID(ids[i]) == SDL_SENSOR_GYRO) {
            sPhoneGyro = SDL_OpenSensor(ids[i]);
            break;
          }
        }
        SDL_free(ids);
      }
    }
    if (sPhoneGyro != nullptr) {
      float data[3];
      if (SDL_GetSensorData(sPhoneGyro, data, 3)) {
        // The phone reports its own axes (x right, y up in its natural
        // orientation), so turn them to the screen's, as SDL does for its
        // accelerometer: landscape would otherwise swap pitch and yaw. The
        // rate about the screen's up axis is negated, as for a pad.
        const SDL_DisplayID display = SDL_GetPrimaryDisplay();
        const int quarters = (OrientationQuarters(SDL_GetCurrentDisplayOrientation(display)) -
                              OrientationQuarters(SDL_GetNaturalDisplayOrientation(display)) + 4) %
                             4;
        switch (quarters) {
        case 1:
          pitch = -data[1];
          yaw = -data[0];
          break;
        case 2:
          pitch = -data[0];
          yaw = data[1];
          break;
        case 3:
          pitch = data[1];
          yaw = data[0];
          break;
        default:
          pitch = data[0];
          yaw = -data[1];
          break;
        }
        haveRates = true;
        sGyroStatus = "phone";
      }
    }
  }
  return haveRates;
}
} // namespace

void PollGyro() {
  EnsureInitialized();
  // Called once per presented frame, which is not once per tick when the
  // frame limiter is off, so the rates integrate over the real frame time.
  static uint64_t sLastNs = 0;
  const uint64_t nowNs = SDL_GetTicksNS();
  const float dt = sLastNs == 0 ? 0.f : std::min(float(double(nowNs - sLastNs) * 1e-9), 0.1f);
  sLastNs = nowNs;
  if (sSpringFlickLatch > 0.f) {
    sSpringFlickLatch -= dt;
  }
  // Gyro feeds the same aim state the mouse, twin stick and the touch layout's
  // drag aim use, so aiming only has an effect where that is driving the
  // camera. Flicks need no aim. The raw sTwinStick: TwinStick() reads false
  // while touch is in use, yet touch keeps the direct aim path.
  const bool aim = sGyroMode != 0 && (sMouseAim || sTwinStick || TouchDirectAim());
  if (!aim && !sSpringFlick) {
    sGyroStatus = sGyroMode == 0 ? "off" : "needs mouse aim, twin stick or touch controls";
    return;
  }

  float yaw = 0.f;
  float pitch = 0.f;
  if (!ReadGyroRates(pitch, yaw)) {
    sGyroStatus = "no gyro found";
    sSpringFlickArmed = true;
    return;
  }

  if (sSpringFlick) {
    // One flick per upward swing: it re-arms once the pitch speed has dropped
    // to half the threshold. The latch outlives the tick so a flick just before
    // the ball lands still springs.
    if (sSpringFlickArmed && pitch > sSpringFlickRate) {
      sSpringFlickLatch = 0.2f;
      sSpringFlickArmed = false;
    } else if (pitch < sSpringFlickRate * 0.5f) {
      sSpringFlickArmed = true;
    }
  }
  if (!aim) {
    if (sGyroMode != 0) {
      sGyroStatus = "flicks only (aim needs mouse aim, twin stick or touch controls)";
    }
    return;
  }

  bool active = sGyroMode == 2;
  if (!active) {
    // Hold to aim: right stick click on a pad, left ctrl on a keyboard.
    if (SDL_Gamepad* pad = PADGetSDLGamepadForIndex(0)) {
      active = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    }
    if (!active) {
      const bool* keys = SDL_GetKeyboardState(nullptr);
      active = keys != nullptr && keys[SDL_SCANCODE_LCTRL] != 0;
    }
  }
  if (!active) {
    sGyroStatus = "held off";
    return;
  }

  if (!std::isfinite(dt) || dt <= 0.f) {
    return;
  }
  // x right / y up, the same shape AddStickAim takes; the aim state expects
  // right/down positive.
  sGyroPendingX += yaw * sGyroRate * dt;
  sGyroPendingY -= pitch * sGyroRate * dt;
}

void ResetMouseAim() {
  sMouseAimState.Reset();
  sMouseGameplayActive = false;
  sMouseButtonGate.Reset();
  sMousePendingX = sMousePendingY = sMouseFrameX = sMouseFrameY = 0.f;
  sTouchLookFrameX = sTouchLookFrameY = 0.f;
  sGyroPendingX = sGyroPendingY = sStickAimVelX = sStickAimVelY = 0.f;
  std::lock_guard lock(sTouchAimMutex);
  sTouchAimPendingX = sTouchAimPendingY = 0.f;
}

void SetMouseCaptured(bool captured) {
  // A button held across a capture change can lose its release (Android drops
  // it when pointer capture ends mid-press, e.g. a boss dying under a charge
  // shot), and the weapon gate would then wait for a neutral that never comes.
  // Anything still physically held is ignored by the gate anyway.
  if (captured != sMouseCaptured) sMouseHeldButtons.Clear();
  sMouseCaptured = captured;
  if (!captured) {
    sMouseButtonGate.Reset();
    sMousePendingX = sMousePendingY = sMouseFrameX = sMouseFrameY = 0.f;
  }
}

bool MouseCaptured() { return sMouseCaptured; }
bool MouseGameplayActive() { return sMouseGameplayActive; }
void SetMouseGameplayActive(bool active) {
  sMouseGameplayActive = active;
  if (!active) ResetMouseAim();
}
bool MouseInvertX() { EnsureInitialized(); return sMouseInvertX; }
bool MouseInvertY() { EnsureInitialized(); return sMouseInvertY; }
bool MouseButtons() { EnsureInitialized(); return sMouseButtons; }
bool MouseCrosshair() { EnsureInitialized(); return sMouseCrosshair; }
int CrosshairSize() { EnsureInitialized(); return sCrosshairSize; }
void SetCrosshairSize(int percent) {
  EnsureInitialized();
  sCrosshairSize = std::clamp(percent, kCrosshairSizeMin, kCrosshairSizeMax);
  MarkDirty();
}
unsigned MouseWeaponButtons(unsigned held) {
  return sMouseButtonGate.Poll(MouseAim() && MouseButtons() && MouseGameplayActive() &&
                               MouseCaptured() && !Visible(), held);
}
unsigned MouseMenuButtons(unsigned held, bool focused) {
  return sMouseMenuGate.Poll(MouseAim() && MouseButtons() && !MouseGameplayActive() &&
                                 !Visible() && focused,
                             held);
}
void NoteMouseButton(bool synthetic, unsigned mask, bool down) {
  sMouseHeldButtons.Note(synthetic, mask, down);
}
void ClearMouseButtons() { sMouseHeldButtons.Clear(); }
unsigned MouseHeldButtons() { return sMouseHeldButtons.Held(); }

bool UpdateMouseAim(bool active, bool locked, float x, float y, float z) {
  SetMouseGameplayActive(active);
  const bool applied = sMouseAimState.Update(active, locked, x, y, z, sMouseFrameX, sMouseFrameY,
                                            MouseSensitivity(), MouseInvertX(), MouseInvertY());
  if (applied) sMouseFrameX = sMouseFrameY = 0.f;
  sAimAppliedLastTick = applied;
  return applied;
}
void SynchronizeMouseAim(float x, float y, float z) { sMouseAimState.Synchronize(x, y, z); }

float MouseSensitivity() {
  EnsureInitialized();
  return sMouseSensitivity;
}

void SetMouseSensitivity(float radiansPerPixel) {
  EnsureInitialized();
  if (std::isfinite(radiansPerPixel) && radiansPerPixel > 0.f) {
    sMouseSensitivity = radiansPerPixel;
  }
}

void AddMouseDelta(float dx, float dy) {
  if (!sMouseCaptured || !MouseAim() || Visible() || !std::isfinite(dx) || !std::isfinite(dy)) {
    return;
  }
  sMousePendingX += dx;
  sMousePendingY += dy;
}

bool TouchAim() {
  EnsureInitialized();
  return sTouchAim;
}

void SetTouchAim(bool on) {
  EnsureInitialized();
  sTouchAim = on;
  MarkDirty();
}

bool TouchMapTap() {
  EnsureInitialized();
  return sTouchMapTap;
}

void SetTouchMapTap(bool on) {
  EnsureInitialized();
  sTouchMapTap = on;
  MarkDirty();
}

bool TouchClassic() {
  EnsureInitialized();
  return sTouchClassic;
}

void SetTouchClassic(bool on) {
  EnsureInitialized();
  sTouchClassic = on;
  if (on) {
    sTouchTwinStick = false;
  }
  MarkDirty();
}

bool TouchTwinStick() {
  EnsureInitialized();
  return sTouchTwinStick;
}

void SetTouchTwinStick(bool on) {
  EnsureInitialized();
  sTouchTwinStick = on;
  if (on) {
    sTouchClassic = false;
  }
  MarkDirty();
}

bool TouchWheels() {
  EnsureInitialized();
  return sTouchWheels;
}

void SetTouchWheels(bool on) {
  EnsureInitialized();
  sTouchWheels = on;
  MarkDirty();
}

bool TouchVisorTapScan() {
  EnsureInitialized();
  return sTouchVisorTapScan;
}

void SetTouchVisorTapScan(bool on) {
  EnsureInitialized();
  sTouchVisorTapScan = on;
  MarkDirty();
}

// Game thread, once per frame from CPlayer::Think.
void SetWheelState(uint32_t mask) {
  sWheelMask.store(mask);
  sWheelStampNs.store(SDL_GetTicksNS());
}

// Not refreshed for a while (no player, paused) reads as 0: wheels disabled.
uint32_t WheelState() {
  const uint64_t now = SDL_GetTicksNS();
  return now - sWheelStampNs.load() < 500'000'000 ? sWheelMask.load() : 0;
}

// Game thread, once per frame from CInGameGuiManager::Update.
void SetPauseScreenOpen(bool open) {
  sPauseScreenOpen.store(open);
  sPauseScreenStampNs.store(SDL_GetTicksNS());
}

// Not refreshed for a while (left the game) counts as closed.
bool PauseScreenOpen() {
  return sPauseScreenOpen.load() && SDL_GetTicksNS() - sPauseScreenStampNs.load() < 300'000'000;
}

void RequestVisor(int visor) {
  if (visor < 0 || visor > 3) {
    return;
  }
  sVisorRequest.store(visor);
  sVisorRequestUntilNs.store(SDL_GetTicksNS() + kWheelRequestNs);
}

void RequestBeam(int beam) {
  if (beam < 0 || beam > 3) {
    return;
  }
  sBeamRequest.store(beam);
  sBeamRequestUntilNs.store(SDL_GetTicksNS() + kWheelRequestNs);
}

void CaptureWheelIcons(int wheel, CGuiModel* const* icons) {
  if (wheel < 0 || wheel > 1) {
    return;
  }
  for (int i = 0; i < 4; ++i) {
    {
      std::lock_guard<std::mutex> lock(sWheelIconMutex);
      if (!sWheelIcons[wheel][i].argb.empty()) {
        continue;
      }
    }
    if (icons[i] == nullptr || !icons[i]->GetModel().valid()) {
      continue;
    }
    // The model is locked at construction and caches on its first draw; an icon the player
    // doesn't own yet isn't drawn, so take it here once it has loaded.
    TCachedToken<CModel>& modelToken = const_cast<TCachedToken<CModel>&>(*icons[i]->GetModel());
    modelToken.TryCache();
    CModel* model = modelToken.GetObject();
    uint texId = 0;
    const CTexture* tex = model != nullptr ? model->PortFirstTexture(&texId) : nullptr;
    // A mod's picture replaces the texels the game holds; those are a stub. Load the retail
    // icon from the disc by id instead (the ids the retail models use).
    static const uint kRetailIcons[2][4] = {
        {0x2DDA38B8, 0x04503F39, 0xD518730E, 0x2EA5AE14},
        {0x8865D14F, 0x5C595218, 0x07183B57, 0xC156B36E},
    };
    static TCachedToken<CTexture>* retailTokens[2][4] = {};
    if (tex != nullptr && tex->PortNativeId() != 0 && gpSimplePool != nullptr) {
      if (retailTokens[wheel][i] == nullptr) {
        // Kept for the run: the icon texture stays loaded.
        retailTokens[wheel][i] = new TCachedToken<CTexture>(
            gpSimplePool->GetObj(SObjectTag('TXTR', kRetailIcons[wheel][i])));
        retailTokens[wheel][i]->Lock();
      }
      TCachedToken<CTexture>& tok = *retailTokens[wheel][i];
      if (!tok.TryCache()) {
        tok.ForceCache();
      }
      tex = tok.GetObject();
      texId = kRetailIcons[wheel][i];
    }
    if (tex == nullptr || tex->PortNativeId() != 0) {
      continue;
    }
    uint32_t w = 0;
    uint32_t h = 0;
    std::vector<uint8_t> rgba(256 * 256 * 4);
    if (!aurora_gx_texobj_rgba8(tex->PortTexObj(), &w, &h, rgba.data(), rgba.size())) {
      continue;
    }
    WheelIcon icon;
    icon.w = static_cast<int>(w);
    icon.h = static_cast<int>(h);
    icon.argb.resize(size_t(w) * h);
    // The HUD models map these textures bottom row first; flip to upright.
    for (uint32_t y = 0; y < h; ++y) {
      for (uint32_t x = 0; x < w; ++x) {
        const uint8_t* c = &rgba[(size_t(h - 1 - y) * w + x) * 4];
        icon.argb[size_t(y) * w + x] =
            uint32_t(c[3]) << 24 | uint32_t(c[0]) << 16 | uint32_t(c[1]) << 8 | c[2];
      }
    }
    PortLog::Write("port: touch wheel icon %d/%d: %ux%u fmt %d id %08X\n", wheel, i, w, h,
                  int(tex->GetTexelFormat()), texId);
    std::lock_guard<std::mutex> lock(sWheelIconMutex);
    sWheelIcons[wheel][i] = std::move(icon);
  }
}

bool VisorRequested(int visor) {
  return sVisorRequest.load() == visor && SDL_GetTicksNS() < sVisorRequestUntilNs.load();
}

bool BeamRequested(int beam) {
  return sBeamRequest.load() == beam && SDL_GetTicksNS() < sBeamRequestUntilNs.load();
}

// Game thread, once per HUD draw (valid=false when the minimap isn't shown).
void SetMinimapRect(bool valid, bool drawn, float x0, float y0, float x1, float y1) {
  std::lock_guard lock(sMinimapMutex);
  sMinimapDrawn = drawn;
  sMinimapValid = valid && std::isfinite(x0) && std::isfinite(y0) && std::isfinite(x1) &&
                  std::isfinite(y1);
  sMinimapRect[0] = x0;
  sMinimapRect[1] = y0;
  sMinimapRect[2] = x1;
  sMinimapRect[3] = y1;
  sMinimapStamp = std::chrono::steady_clock::now();
}

// A rect not refreshed for a while (HUD not drawn at all) counts as gone.
bool MinimapRect(float* out4, bool* drawn) {
  std::lock_guard lock(sMinimapMutex);
  if (!sMinimapValid ||
      std::chrono::steady_clock::now() - sMinimapStamp > std::chrono::milliseconds(300)) {
    return false;
  }
  if (out4 != nullptr) {
    std::copy(sMinimapRect, sMinimapRect + 4, out4);
  }
  if (drawn != nullptr) {
    *drawn = sMinimapDrawn;
  }
  return true;
}

void RequestMapTap() {
  sMapTapPending.fetch_add(1);
}

// Game thread, once per frame from CAutoMapper::Update.
void SetMapScreenOpen(bool open) {
  std::lock_guard lock(sMapPanMutex);
  sMapScreenOpen = open;
  sMapScreenStamp = std::chrono::steady_clock::now();
}

// Not refreshed for a while (paused, left the game) counts as closed.
bool MapScreenOpen() {
  std::lock_guard lock(sMapPanMutex);
  return sMapScreenOpen &&
         std::chrono::steady_clock::now() - sMapScreenStamp < std::chrono::milliseconds(300);
}

// UI thread. A call with zero deltas still marks the finger as down, for holdMs.
void AddMapPan(float dxDp, float dyDp, float viewHeightDp, int holdMs) {
  if (!std::isfinite(dxDp) || !std::isfinite(dyDp) || !(viewHeightDp > 0.f) ||
      !MapScreenOpen()) {
    return;
  }
  std::lock_guard lock(sMapPanMutex);
  sMapPanX += dxDp;
  sMapPanY += dyDp;
  sMapPanViewDp = viewHeightDp;
  sMapPanHeldUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(holdMs);
}

// UI thread: a pinch's finger-distance ratio (>1 zooms in); factors multiply.
void AddMapZoom(float ratio) {
  if (!std::isfinite(ratio) || !(ratio > 0.f) || !MapScreenOpen()) {
    return;
  }
  std::lock_guard lock(sMapPanMutex);
  sMapZoomPending = std::clamp(sMapZoomPending * std::clamp(ratio, 0.1f, 10.f), 0.01f, 100.f);
}

// UI thread: a twist's angle in radians; positive turns the map as the stick's
// right does. Angles add.
void AddMapRotate(float radians) {
  if (!std::isfinite(radians) || radians == 0.f || !MapScreenOpen()) {
    return;
  }
  std::lock_guard lock(sMapPanMutex);
  sMapRotatePending = std::clamp(sMapRotatePending + radians, -6.3f, 6.3f);
}

// Game thread: the pending twist (0 = none), cleared.
float TakeMapRotate() {
  std::lock_guard lock(sMapPanMutex);
  const float radians = sMapRotatePending;
  sMapRotatePending = 0.f;
  return radians;
}

// Game thread: the pending zoom factor (1 = none), cleared.
float TakeMapZoom() {
  std::lock_guard lock(sMapPanMutex);
  const float ratio = sMapZoomPending;
  sMapZoomPending = 1.f;
  return ratio;
}

// Game thread. Drains the pending pan; true while a finger is on the map (a pan
// within the last 250 ms), so the map doesn't drift back to its area meanwhile.
bool TakeMapPan(float* dxDp, float* dyDp, float* viewHeightDp) {
  std::lock_guard lock(sMapPanMutex);
  *dxDp = sMapPanX;
  *dyDp = sMapPanY;
  *viewHeightDp = sMapPanViewDp;
  sMapPanX = 0.f;
  sMapPanY = 0.f;
  return std::chrono::steady_clock::now() < sMapPanHeldUntil;
}

// Game thread, once per pad poll. Returns true for the poll that should read Z
// held: a tap presses for one poll and releases on the next, so each tap is
// one press edge.
bool ConsumeMapTapZ() {
  if (sMapTapHeld) {
    sMapTapHeld = false;
    return false;
  }
  if (sMapTapPending.load() > 0) {
    sMapTapPending.fetch_sub(1);
    sMapTapHeld = true;
    return true;
  }
  return false;
}

float TouchAimSpeed() {
  EnsureInitialized();
  return sTouchAimSpeed;
}

void SetTouchAimSpeed(float pixelsPerDp) {
  EnsureInitialized();
  if (std::isfinite(pixelsPerDp)) {
    sTouchAimSpeed = std::clamp(pixelsPerDp, 0.25f, 10.f);
    MarkDirty();
  }
}

// Called from the Android UI thread; the game thread drains it in
// BeginFrameMouse.
void AddTouchAim(float dxDp, float dyDp) {
  if ((!sTouchAim && sTouchClassic) || Visible() || !std::isfinite(dxDp) ||
      !std::isfinite(dyDp)) {
    return;
  }
  std::lock_guard lock(sTouchAimMutex);
  sTouchAimPendingX += dxDp * sTouchAimSpeed;
  sTouchAimPendingY += dyDp * sTouchAimSpeed;
}

void BeginFrameMouse() {
  float touchX = 0.f;
  float touchY = 0.f;
  {
    std::lock_guard lock(sTouchAimMutex);
    touchX = sTouchAimPendingX;
    touchY = sTouchAimPendingY;
    sTouchAimPendingX = sTouchAimPendingY = 0.f;
  }
  // On the direct aim path (mouse aim, twin stick, or the modern touch layout) the
  // touch travel joins the mouse's; in the classic GameCube scheme
  // CPlayer::UpdateTouchLook takes it instead (TakeTouchLook).
  const bool directAim = DirectAim();
  sTouchLookFrameX = directAim ? 0.f : touchX;
  sTouchLookFrameY = directAim ? 0.f : touchY;
  sMouseFrameX = sMousePendingX + sGyroPendingX + (directAim ? touchX : 0.f);
  sMouseFrameY = sMousePendingY + sGyroPendingY + (directAim ? touchY : 0.f);
  sMousePendingX = sMousePendingY = 0.f;
  sGyroPendingX = sGyroPendingY = 0.f;
  // AddStickAim sets it again during this tick's input update.
  sStickAimVelX = sStickAimVelY = 0.f;
  sAimAppliedLastTick = false;
}

void SetTouchAimDown(bool down) { sTouchAimDown = down; }

void HoldTouchAim(float seconds) {
  sTouchAimHoldUntilNs = SDL_GetTicksNS() + static_cast<uint64_t>(std::max(seconds, 0.f) * 1e9);
}

bool TouchAimDown() {
  return sTouchAimDown || SDL_GetTicksNS() < sTouchAimHoldUntilNs;
}

bool TakeTouchLook(float& dyaw, float& dpitch) {
  dyaw = dpitch = 0.f;
  const float x = sTouchLookFrameX;
  const float y = sTouchLookFrameY;
  sTouchLookFrameX = sTouchLookFrameY = 0.f;
  // Same signs and scale as the mouse aim: right/down travel in, world yaw/pitch out.
  dyaw = x * MouseSensitivity() * (MouseInvertX() ? 1.f : -1.f);
  dpitch = y * MouseSensitivity() * (MouseInvertY() ? 1.f : -1.f);
  return sTouchAim && !Visible();
}

bool PresentedAimDelta(float fraction, float& dyaw, float& dpitch) {
  dyaw = dpitch = 0.f;
  if (!FrameInterpolation() || !sMouseGameplayActive || !sAimAppliedLastTick ||
      !std::isfinite(fraction) || fraction < 0.f) {
    return false;
  }
  // What the next tick will consume: the mouse and gyro travel so far, plus the
  // stick's travel over the part of the tick already shown.
  const float ahead = std::min(fraction, 1.f) * TickPeriod();
  float touchX = 0.f;
  float touchY = 0.f;
  {
    std::lock_guard lock(sTouchAimMutex);
    touchX = sTouchAimPendingX;
    touchY = sTouchAimPendingY;
  }
  const float dx = sMousePendingX + sGyroPendingX + touchX + sStickAimVelX * ahead;
  const float dy = sMousePendingY + sGyroPendingY + touchY + sStickAimVelY * ahead;
  float yaw = 0.f;
  float pitch = 0.f;
  if (!sMouseAimState.Preview(dx, dy, MouseSensitivity(), MouseInvertX(), MouseInvertY(), yaw,
                              pitch)) {
    return false;
  }
  dyaw = static_cast<float>(std::remainder(double(yaw) - sMouseAimState.yaw, 2.0 * PortMouse::kPi));
  dpitch = pitch - sMouseAimState.pitch;
  return dyaw != 0.f || dpitch != 0.f;
}

bool FrameInterpolation() {
  EnsureInitialized();
  return sFrameInterpolation && !sOriginalExperience;
}

void SetFrameInterpolation(bool enabled) {
  EnsureInitialized();
  sFrameInterpolation = enabled;
}

bool SmoothFrames() {
  EnsureInitialized();
  return sSmoothFrames && !sOriginalExperience;
}

void SetSmoothFrames(bool enabled) {
  EnsureInitialized();
  sSmoothFrames = enabled;
  sFrameInterpolation = sActorInterpolation = sPoseInterpolation = sParticleInterpolation = enabled;
  MarkDirty();
}

bool ActorInterpolation() {
  EnsureInitialized();
  return sActorInterpolation && !sOriginalExperience;
}

void SetActorInterpolation(bool enabled) {
  EnsureInitialized();
  sActorInterpolation = enabled;
}

bool PoseInterpolation() {
  EnsureInitialized();
  return sPoseInterpolation && !sOriginalExperience;
}

void SetPoseInterpolation(bool enabled) {
  EnsureInitialized();
  sPoseInterpolation = enabled;
}

bool RoomGeoResident() {
  EnsureInitialized();
  return sRoomGeoResident;
}

void SetRoomGeoResident(bool enabled) {
  EnsureInitialized();
  sRoomGeoResident = enabled;
  MarkDirty();
}

bool RoomGeoResidentAtStartup() {
  std::ifstream file(SettingsFilePath());
  std::string line;
  bool on = kRoomGeoResidentDefault;
  while (std::getline(file, line)) {
    const size_t separator = line.find('=');
    if (separator == std::string::npos) {
      continue;
    }
    const std::string key = Trim(line.substr(0, separator));
    std::string value = line.substr(separator + 1);
    value.erase(std::min(value.find('#'), value.size()));
    if (key == "room_geo_gpu") {
      on = ParseBool(Trim(value));
    } else if (key == "room_geo_resident") {
      on = on || ParseBool(Trim(value));
    }
  }
  return on;
}

bool ParticleInterpolation() {
  EnsureInitialized();
  return sParticleInterpolation && !sOriginalExperience;
}

void SetParticleInterpolation(bool enabled) {
  EnsureInitialized();
  sParticleInterpolation = enabled;
}

bool PresentOverride(float& t) {
  EnsureInitialized();
  if (sPresentOverride < 0.f) {
    return false;
  }
  if (sPresentOverride == kPresentTick) {
    t = -1.f;
  } else if (sPresentOverride > 1.f) {
    t = 0.25f * static_cast< float >(sPresentCycleFrame++ & 3);
  } else {
    t = sPresentOverride;
  }
  return true;
}

void SetPresentOverride(float value) {
  EnsureInitialized();
  sPresentOverride = value < 0.f             ? -1.f
                     : value == kPresentTick ? kPresentTick
                     : value > 1.f           ? kPresentCycle
                                             : value;
  sPresentCycleFrame = 0;
}

float PresentOverrideValue() {
  EnsureInitialized();
  return sPresentOverride;
}

bool TickHold() { return sTickHold; }

void SetTickHold(bool held) {
  sTickHold = held;
  sHeldTicks = 0;
}

void StepTicks(unsigned count) { sHeldTicks += count; }

unsigned PendingHeldTicks() { return sHeldTicks; }

unsigned TakeHeldTicks() {
  // One tick per loop, like MP_TURBO=1, so each step is one drawn frame.
  if (sHeldTicks == 0) {
    return 0;
  }
  --sHeldTicks;
  return 1;
}

void GetFrameMouseDelta(float& dx, float& dy) {
  dx = sMouseFrameX;
  dy = sMouseFrameY;
}

float AimYaw() { return sMouseAimState.yaw; }
float AimPitch() { return sMouseAimState.pitch; }
bool AimInitialized() { return sMouseAimState.initialized; }

bool AiAudioEnabled() {
  EnsureInitialized();
  return AIPortOutputEnabled() != 0;
}

void SetAiAudioEnabled(bool enabled) {
  EnsureInitialized();
  if (sAiAudioEnabled == enabled && AiAudioEnabled() == enabled) {
    return;
  }
  sAiAudioEnabled = enabled;
  AIPortSetOutputEnabled(enabled ? 1 : 0);
}

bool MusyxAudioEnabled() {
  EnsureInitialized();
  return sMusyxAudioEnabled;
}

void SetMusyxAudioEnabled(bool enabled) {
  EnsureInitialized();
  if (sMusyxAudioEnabled == enabled) {
    return;
  }
  sMusyxAudioEnabled = enabled;
  salSetMuted(enabled ? 0 : 1);
}

void RequestReset() { sResetRequested = true; }

bool ConsumeResetRequest() {
  const bool requested = sResetRequested;
  sResetRequested = false;
  return requested;
}

void SetStateManager(CStateManager* mgr) {
  if (mgr != sStateManager) {
    // The tracker's room names come from the old world's PAKs.
    PortTracker::Reset();
  }
  sStateManager = mgr;
}
CStateManager* StateManager() { return sStateManager; }
bool ViewRay(float origin[3], float forward[3]) {
  if (sStateManager == nullptr || sStateManager->GetCameraManager() == nullptr) {
    return false;
  }
  const CTransform4f view =
      PortFreeCam::View(sStateManager->GetCameraManager()->GetCurrentCameraTransform(*sStateManager));
  const CVector3f at = view.GetTranslation();
  const CVector3f to = view.GetForward();
  origin[0] = at.GetX();
  origin[1] = at.GetY();
  origin[2] = at.GetZ();
  forward[0] = to.GetX();
  forward[1] = to.GetY();
  forward[2] = to.GetZ();
  return true;
}

namespace {
const char* const kPbrViews[] = {"off",     "albedo",     "normal", "rough",    "metal", "ao",
                                 "ambient", "reflection", "glow",   "exposure", "kind",
                                 "sun",     "drawid"};
// The last entry is not a shader view but Aurora's draw id mode (GXPortSetDrawIdMode).
constexpr int kDrawIdView = 12;
int sPbrView = 0;
} // namespace
void NoteDrawIdMode(bool on) { CCubeModel::PortSetDrawIds(on); }
int PbrViewCount() { return int(sizeof(kPbrViews) / sizeof(kPbrViews[0])); }
const char* PbrViewName(int view) { return view >= 0 && view < PbrViewCount() ? kPbrViews[view] : "?"; }
int PbrView() { return sPbrView; }
void SetPbrView(int view) {
  sPbrView = view >= 0 && view < PbrViewCount() ? view : 0;
  GXSetPBRDebugView(sPbrView == kDrawIdView ? 0u : u32(sPbrView));
  GXPortSetDrawIdMode(sPbrView == kDrawIdView ? GX_TRUE : GX_FALSE);
  PortDebug::NoteDrawIdMode(sPbrView == kDrawIdView);
}
void RequestTeleport(int areaId) { sPendingTeleport = areaId; }
bool ConsumeTeleportRequest(int& areaId) {
  if (sPendingTeleport < 0) {
    return false;
  }
  areaId = sPendingTeleport;
  sPendingTeleport = -1;
  return true;
}
void RequestWorldTeleport(uint32_t worldId, uint32_t areaAssetId) {
  sWorldTeleportWorld = worldId;
  sWorldTeleportArea = areaAssetId;
  sHasWorldTeleport = true;
}
bool ConsumeWorldTeleportRequest(uint32_t& worldId, uint32_t& areaAssetId) {
  if (!sHasWorldTeleport) {
    return false;
  }
  worldId = sWorldTeleportWorld;
  areaAssetId = sWorldTeleportArea;
  sHasWorldTeleport = false;
  return true;
}

void RequestWorldSweep() {
  if (sWorldSweepRequested || sWorldSweep.active || sHasWorldTeleport || sPendingTeleport >= 0)
    return;
  sWorldSweepRequested = true;
}

// Activates one layer of the current area and deactivates the rest, so the
// area's objects are built for that layer when it is next reconstructed.
// Layer 0 is the one a fresh save has, which is why a plain tour never sees
// anything behind another layer.
void SetSweepLayer(CStateManager& mgr, CWorld* world, TAreaId area, int layer) {
  rstl::rc_ptr< CScriptLayerManager >& layers = mgr.WorldLayerState();
  if (layers.IsNull())
    return;
  const int count = layers->GetAreaLayerCount(area);
  for (int i = 0; i < count; ++i)
    layers->SetLayerActive(area, TLayerId(i), i == layer);
  (void)world;
}

bool SweepLayersEnabled() {
  return port::EnvFlag("MP_RANDO_SWEEP_LAYERS");
}

bool ConsumeWorldSweepRequest(CStateManager& mgr) {
  // This entry point is called only by gameplay, never the frontend or UI.
  static const bool envChecked = [] {
    if (port::EnvFlag("MP_RANDO_SWEEP"))
      RequestWorldSweep();
    return true;
  }();
  (void)envChecked;
  WorldSweep& sweep = sWorldSweep;
  if ((!sWorldSweepRequested && !sweep.active) || mgr.GetWantsToQuit()) {
    return false;
  }
  if (sHasWorldTeleport || sPendingTeleport >= 0 || sResetRequested) {
    // A manual debug request wins; do not resume the tour behind the user's back.
    if (sweep.active) std::fputs("[sweep] cancelled: another debug teleport/reset\n", stderr);
    sweep = {};
    sWorldSweepRequested = false;
    return false;
  }
  CWorld* world = mgr.World();
  if (mgr.GetGameState() != CStateManager::kGS_Running || world == nullptr ||
      gpGameState == nullptr || gpMemoryCard == nullptr) return false;

  if (sWorldSweepRequested) {
    const auto& worlds = gpMemoryCard->GetMemoryWorlds();
    if (worlds.empty()) return false;
    sweep = {};
    // MP_RANDO_SWEEP_WORLDS=<hex id>[,<hex id>...] limits the tour to those
    // worlds, e.g. to redo one world without walking the seven before it.
    std::vector< uint32_t > only;
    if (const char* list = std::getenv("MP_RANDO_SWEEP_WORLDS")) {
      for (const char* p = list; *p != '\0';) {
        char* end = nullptr;
        const unsigned long id = std::strtoul(p, &end, 16);
        if (end == p) {
          ++p;
          continue;
        }
        only.push_back(static_cast< uint32_t >(id));
        p = end;
      }
    }
    const auto wanted = [&only](uint32_t id) {
      if (only.empty()) return true;
      for (uint32_t w : only)
        if (w == id) return true;
      return false;
    };
    // Visit the live world first so its MLVL can supply the first area list;
    // later worlds are entered at area zero, which also visits their first area.
    if (wanted(world->IGetWorldAssetId())) sweep.worlds.push_back(world->IGetWorldAssetId());
    for (const auto& entry : worlds) {
      if (entry.first != world->IGetWorldAssetId() && wanted(entry.first))
        sweep.worlds.push_back(entry.first);
    }
    sWorldSweepRequested = false;
    if (sweep.worlds.empty()) {
      std::fputs("[sweep] stopped: MP_RANDO_SWEEP_WORLDS matches no world\n", stderr);
      sweep = {};
      return false;
    }
    sweep.active = true;
    std::fprintf(stderr, "[sweep] begin: %zu worlds (use MP_RANDO_DUMP=1 for pickups)\n",
                 sweep.worlds.size());
    if (world->IGetWorldAssetId() != sweep.worlds[0]) {
      // The live world was filtered out: enter the first wanted world the same
      // way a finished world hands over to the next one.
      sweep.waiting = true;
      RequestWorldTeleport(sweep.worlds[0], 0u);
      return true;
    }
  }
  if (world->IGetWorldAssetId() != sweep.worlds[sweep.world]) {
    std::fputs("[sweep] cancelled: gameplay changed worlds\n", stderr);
    sweep = {};
    return false;
  }
  if (sweep.areas.empty()) {
    for (int i = 0; i < world->IGetAreaCount(); ++i)
      sweep.areas.push_back(world->IGetAreaAlways(TAreaId(i))->IGetAreaAssetId());
    if (sweep.areas.empty()) {
      std::fputs("[sweep] stopped: world has no areas\n", stderr);
      sweep = {};
      return false;
    }
    std::fprintf(stderr, "[sweep] world %zu/%zu: %08X, %zu areas\n", sweep.world + 1,
                 sweep.worlds.size(), sweep.worlds[sweep.world], sweep.areas.size());
    if (SweepLayersEnabled()) {
      // How many layers the widest area has, so every area gets a pass per
      // layer even where its own count is lower. An area with fewer layers
      // simply rebuilds the same thing, which costs a restart and nothing else.
      int widest = 1;
      for (int i = 0; i < world->IGetAreaCount(); ++i) {
        const int count = mgr.WorldLayerState()
                              ? mgr.WorldLayerState()->GetAreaLayerCount(TAreaId(i))
                              : 1;
        if (count > widest)
          widest = count;
      }
      sweep.layerCount = widest;
      std::fprintf(stderr, "[sweep] layers: up to %d per area\n", widest);
    }
  }

  // Count consecutive quiet simulation ticks, not rendered frames or a wall
  // clock timeout. Current-area construction alone does not imply that adjacent
  // areas, map tiles and factory requests have finished streaming.
  const TAreaId current = world->GetCurrentAreaId();
  const bool areaExists = world->DoesAreaExist(current);
  const bool areaValid = areaExists && world->GetArea(current)->IsValidated();
  const bool loadingIdle = world->GetChainHead(CWorld::kC_Loading) == CWorld::GetAliveAreasEnd();
  const bool freeingIdle = world->GetChainHead(CWorld::kC_ToDeallocate) == CWorld::GetAliveAreasEnd();
  const bool mapIdle = !world->GetMapWorld()->IsMapAreasStreaming();
  const bool loadsIdle = !gpResourceFactory->HasPendingLoads();
  const bool ready = areaExists && areaValid && loadingIdle && freeingIdle && mapIdle && loadsIdle;
  if (!ready) {
    // A tour that never settles is otherwise invisible: no output, no
    // progress, just frames. Say what is still outstanding, and how long it has
    // been that way, so the stall names itself instead of looking like work.
    if (sweep.settledTicks == 0) {
      std::fprintf(stderr,
                   "[sweep] waiting on area %d: %s%s%s%s%s (target %08X, pass %zu/%zu)\n",
                   static_cast<int>(current.Value()), areaExists ? "" : "no-area ",
                   areaValid ? "" : "unvalidated ", loadingIdle ? "" : "loading ",
                   freeingIdle ? "" : "freeing ", mapIdle ? "" : "map-streaming ",
                   sweep.areas.empty() ? 0u : sweep.areas[sweep.area], sweep.area + 1,
                   sweep.areas.size());
    }
    // 10 seconds of simulation. A pass that has not settled by then is not
    // going to: the loading chain stays put while the area is alive and
    // nothing else moves it. The window is short because the wait is measured
    // in ticks and a throttled run reaches a tick slowly.
    if (++sweep.stalledTicks == 600) {
      // One area that will not settle must not end a whole tour: the rest of
      // the world is still worth dumping, and this area's objects were built on
      // the way in even if the settle check never agreed. Report it, skip the
      // pass, and carry on from the next one.
      std::fprintf(stderr, "[sweep] giving up on %08X after 10s: %s%s%s%s%s\n",
                   sweep.areas.empty() ? 0u : sweep.areas[sweep.area],
                   areaExists ? "" : "no-area ", areaValid ? "" : "unvalidated ",
                   loadingIdle ? "" : "loading ", freeingIdle ? "" : "freeing ",
                   mapIdle ? "" : "map-streaming ");
      sweep.stalledTicks = 0;
      sweep.settledTicks = 0;
      sweep.revisiting = false;
      if (sweep.waiting) {
        ++sweep.completedAreas;
        ++sweep.area;
        sweep.layer = 0;
        if (sweep.area >= sweep.areas.size()) {
          sweep.areas.clear();
          sweep.area = 0;
          ++sweep.world;
        }
        sweep.waiting = false;
      }
      if (sweep.area >= sweep.areas.size() || sweep.world >= sweep.worlds.size()) {
        std::fprintf(stderr, "[sweep] complete: %zu worlds, %u areas\n", sweep.worlds.size(),
                     sweep.completedAreas);
        sweep = {};
        return false;
      }
      return true;
    }
    sweep.settledTicks = 0;
    return false;
  }
  sweep.stalledTicks = 0;
  if (++sweep.settledTicks < 30) return false;
  sweep.settledTicks = 0;
  if (sweep.waiting) {
    const uint32_t settled = world->IGetAreaAlways(current)->IGetAreaAssetId();
    if (sweep.revisiting) {
      // This is the hop away from the area being re-layered: its objects have
      // been dumped, so all that matters is that it is gone by the time we go
      // back. Wait here, then return to it with the new layer active.
      sweep.revisiting = false;
      std::fprintf(stderr, "[sweep] left %08X for layer %d; returning\n", settled,
                   sweep.layer + 1);
      RequestWorldTeleport(sweep.worlds[sweep.world], sweep.areas[sweep.area]);
      return true;
    }
    if (settled != sweep.areas[sweep.area]) {
      // The destination's own scripts moved the player on (Impact Crater's
      // spawn points do). Its objects, and so its dump, were already built.
      std::fprintf(stderr, "[sweep] note: %08X moved the player to %08X; continuing\n",
                   sweep.areas[sweep.area], settled);
    }
    // One area, one layer: the count reports passes, not distinct areas, so a
    // multi-layered area is visibly a few.
    ++sweep.completedAreas;
    std::fprintf(stderr, "[sweep] area %zu/%zu: %08X layer %d/%d (total %u)\n", sweep.area + 1,
                 sweep.areas.size(), sweep.areas[sweep.area], sweep.layer + 1, sweep.layerCount,
                 sweep.completedAreas);
    if (++sweep.layer < sweep.layerCount) {
      // Another layer of the area just built. The layer state has to be set
      // before the area is reconstructed, because CGameArea builds the objects
      // of the layers that are active at construction time - which is why a
      // pickup behind an inactive layer is not in the dump at all.
      ++sweep.layerPasses;
      SetSweepLayer(mgr, world, current, sweep.layer);
      std::fprintf(stderr, "[sweep] re-entering %08X with layer %d active\n",
                   sweep.areas[sweep.area], sweep.layer);
      // The area has to be unloaded before it is rebuilt: SetLayerActive is a
      // bit flip, and travelling straight back onto a live area re-enters it
      // without ever scheduling its load again, so the pass never settles. Two
      // hops: out to the world's first area, which unloads this one, and the
      // return below lands on it with the new layer active.
      sweep.revisiting = true;
      RequestWorldTeleport(sweep.worlds[sweep.world], 0u);
      return true;
    }
    sweep.layer = 0;
    if (++sweep.area == sweep.areas.size()) {
      if (++sweep.world == sweep.worlds.size()) {
        std::fprintf(stderr, "[sweep] complete: %zu worlds, %u areas\n", sweep.worlds.size(),
                     sweep.completedAreas);
        sweep = {};
        return false;
      }
      sweep.areas.clear();
      sweep.area = 0;
    }
  }
  // QuitGame is consumed in this same tick. The next call with !GetWantsToQuit
  // belongs to the new manager even if the allocator reused its old address.
  sweep.waiting = true;
  RequestWorldTeleport(sweep.worlds[sweep.world],
                       sweep.areas.empty() ? 0u : sweep.areas[sweep.area]);
  return true;
}

bool Visible() {
  EnsureInitialized();
  return sVisible;
}

bool OverlayVisible() { return sOverlayVisible.load(std::memory_order_acquire); }

bool TouchColorsFlag() { return sTouchColorsFlag.load(std::memory_order_acquire); }
bool TouchLabelsFlag() { return sTouchLabelsFlag.load(std::memory_order_acquire); }
bool TouchTurboFlag() { return sTouchTurboFlag.load(std::memory_order_acquire); }
bool TouchFloatingStickFlag() { return sTouchFloatingStickFlag.load(std::memory_order_acquire); }
float TouchSideMarginDp() { return sTouchSideMargin.load(); }
float TouchStickInsetDp() { return sTouchStickInset.load(); }
float TouchButtonInsetDp() { return sTouchButtonInset.load(); }

std::string TouchLayout() {
  std::lock_guard< std::mutex > lock(sTouchLayoutMutex);
  return sTouchLayout;
}

void SetTouchLayout(const std::string& layout) {
  if (layout.size() > kTouchLayoutMaxLen) {
    return;
  }
  {
    std::lock_guard< std::mutex > lock(sTouchLayoutMutex);
    sTouchLayout = layout;
  }
  sTouchLayoutSavePending.store(true, std::memory_order_release);
}

bool TakeTouchEditRequested() { return sTouchEditRequested.exchange(false, std::memory_order_acq_rel); }

void SaveSettingsNow() {
  EnsureInitialized();
  SaveSettings();
}

// Set while the disc is mounted, before the first frame.
bool sDiscReadFailedLastSession = false;

void NoteDiscReadFailedLastSession() { sDiscReadFailedLastSession = true; }

// The overlay is a full-screen panel with a page list instead of tabs, which
// fits a touchscreen and reads better on the desktop too. The desktop can go
// back to the old floating tabbed window (System > Overlay as a floating
// window); MP_TOUCH_UI forces the page layout regardless.
bool PageLayout() {
#if defined(__ANDROID__)
  return true;
#else
  static const bool sForced = port::EnvFlag("MP_TOUCH_UI");
  return sForced || !sOverlayWindowed;
#endif
}

SDL_Window* MainWindow() {
  static SDL_Window* sWindow = nullptr;
  if (sWindow == nullptr) {
    int windowCount = 0;
    if (SDL_Window** windows = SDL_GetWindows(&windowCount)) {
      if (windowCount > 0) {
        sWindow = windows[0];
      }
      SDL_free(windows);
    }
  }
  return sWindow;
}

bool WindowSize(int& width, int& height) {
  SDL_Window* window = MainWindow();
  return window != nullptr && SDL_GetWindowSize(window, &width, &height) && width > 0 &&
         height > 0;
}

// Sizes in unscaled pixels; UpdateUiScale multiplies them by the display scale
// like the defaults. A fingertip covers far more than a cursor does, so frames,
// grabs and scrollbars grow, and TouchExtraPadding widens every hit box a
// little beyond what is drawn.
void ApplyTouchStyle(ImGuiStyle& style) {
  style.WindowPadding = ImVec2(10.f, 10.f);
  style.FramePadding = ImVec2(10.f, 7.f);
  style.ItemSpacing = ImVec2(10.f, 8.f);
  style.ItemInnerSpacing = ImVec2(8.f, 6.f);
  style.CellPadding = ImVec2(6.f, 4.f);
  style.TouchExtraPadding = ImVec2(3.f, 3.f);
  style.IndentSpacing = 22.f;
  style.ScrollbarSize = 18.f;
  style.GrabMinSize = 18.f;
  style.FrameRounding = 5.f;
  style.GrabRounding = 5.f;
  style.ScrollbarRounding = 9.f;
  style.TabRounding = 5.f;
}

// --- F1 overlay themes ---------------------------------------------------------------------------

enum UiThemeId { kThemeAuto = 0, kThemePrime = 1, kThemeRemastered = 2, kThemePlain = 3 };

// The scale UpdateUiScale has applied to the style so far. Border sizes and rounding are set
// here in unscaled units times this, so they stay consistent with the sizes ScaleAllSizes made.
float sUiAppliedScale = 1.f;
int sThemeApplied = -1;        // the resolved theme the style holds, -1 = none yet
float sThemeAppliedScale = 0.f;
int sThemeResolved = kThemePrime; // what Auto resolves to now; read by the colour helpers

// Whether the Remastered import is loaded: the mods are on, and a mod made by the import is
// enabled. CurrentStatus is in memory (rescanned only on a reload), so this is cheap per frame.
bool RemasteredImportLoaded() {
  const PortMods::Status& status = PortMods::CurrentStatus();
  if (!status.active || PortMods::Suspended()) {
    return false;
  }
  for (const PortMods::ModInfo& mod : status.mods) {
    if (mod.import && mod.enabled) {
      return true;
    }
  }
  return false;
}

int ResolveTheme() {
  if (sUiTheme != kThemeAuto) {
    return sUiTheme;
  }
  return RemasteredImportLoaded() ? kThemeRemastered : kThemePrime;
}

// Colours for one theme. Every ImGuiCol_ is derived from these so none is left at the stock value.
struct ThemeRgb {
  float x, y, z;
  ThemeRgb() : x(0.f), y(0.f), z(0.f) {}
  ThemeRgb(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};

struct ThemePalette {
  ThemeRgb bg;     // window background (alpha below)
  float bgAlpha;
  ThemeRgb deep;   // child / title / menu background
  ThemeRgb frame;  // frames, buttons, tabs
  ThemeRgb frameHi; // hovered frames
  ThemeRgb accent;
  ThemeRgb text;
  ThemeRgb textDim;
};

ImVec4 Rgba(const ThemeRgb& c, float a) { return ImVec4(c.x, c.y, c.z, a); }

void FillThemeColors(ImGuiStyle& style, const ThemePalette& p) {
  ImVec4* c = style.Colors;
  const ThemeRgb a = p.accent;
  c[ImGuiCol_Text] = Rgba(p.text, 1.f);
  c[ImGuiCol_TextDisabled] = Rgba(p.textDim, 1.f);
  c[ImGuiCol_WindowBg] = Rgba(p.bg, p.bgAlpha);
  c[ImGuiCol_ChildBg] = Rgba(p.deep, 0.55f);
  c[ImGuiCol_PopupBg] = Rgba(p.bg, 0.97f);
  c[ImGuiCol_Border] = Rgba(a, 0.55f);
  c[ImGuiCol_BorderShadow] = ImVec4(0.f, 0.f, 0.f, 0.f);
  c[ImGuiCol_FrameBg] = Rgba(p.frame, 0.90f);
  c[ImGuiCol_FrameBgHovered] = Rgba(p.frameHi, 0.95f);
  c[ImGuiCol_FrameBgActive] = Rgba(a, 0.35f);
  c[ImGuiCol_TitleBg] = Rgba(p.deep, 1.f);
  c[ImGuiCol_TitleBgActive] = Rgba(p.frame, 1.f);
  c[ImGuiCol_TitleBgCollapsed] = Rgba(p.deep, 0.75f);
  c[ImGuiCol_MenuBarBg] = Rgba(p.deep, 1.f);
  c[ImGuiCol_ScrollbarBg] = ImVec4(0.f, 0.f, 0.f, 0.30f);
  c[ImGuiCol_ScrollbarGrab] = Rgba(p.frameHi, 1.f);
  c[ImGuiCol_ScrollbarGrabHovered] = Rgba(a, 0.60f);
  c[ImGuiCol_ScrollbarGrabActive] = Rgba(a, 0.90f);
  c[ImGuiCol_CheckMark] = Rgba(a, 1.f);
  c[ImGuiCol_SliderGrab] = Rgba(a, 0.85f);
  c[ImGuiCol_SliderGrabActive] = Rgba(a, 1.f);
  c[ImGuiCol_Button] = Rgba(p.frame, 0.90f);
  c[ImGuiCol_ButtonHovered] = Rgba(p.frameHi, 1.f);
  c[ImGuiCol_ButtonActive] = Rgba(a, 0.55f);
  c[ImGuiCol_Header] = Rgba(p.frame, 0.80f);
  c[ImGuiCol_HeaderHovered] = Rgba(a, 0.35f);
  c[ImGuiCol_HeaderActive] = Rgba(a, 0.55f);
  c[ImGuiCol_Separator] = Rgba(a, 0.35f);
  c[ImGuiCol_SeparatorHovered] = Rgba(a, 0.70f);
  c[ImGuiCol_SeparatorActive] = Rgba(a, 1.f);
  c[ImGuiCol_ResizeGrip] = Rgba(a, 0.30f);
  c[ImGuiCol_ResizeGripHovered] = Rgba(a, 0.65f);
  c[ImGuiCol_ResizeGripActive] = Rgba(a, 0.90f);
  c[ImGuiCol_TabHovered] = Rgba(a, 0.50f);
  c[ImGuiCol_Tab] = Rgba(p.frame, 0.85f);
  c[ImGuiCol_TabSelected] = Rgba(a, 0.40f);
  c[ImGuiCol_TabSelectedOverline] = Rgba(a, 1.f);
  c[ImGuiCol_TabDimmed] = Rgba(p.deep, 0.90f);
  c[ImGuiCol_TabDimmedSelected] = Rgba(a, 0.25f);
#ifdef IMGUI_HAS_DOCK
  c[ImGuiCol_TabDimmedSelectedOverline] = Rgba(a, 0.55f);
  c[ImGuiCol_DockingPreview] = Rgba(a, 0.45f);
  c[ImGuiCol_DockingEmptyBg] = Rgba(p.deep, 1.f);
#endif
  c[ImGuiCol_PlotLines] = Rgba(a, 0.90f);
  c[ImGuiCol_PlotLinesHovered] = Rgba(p.text, 1.f);
  c[ImGuiCol_PlotHistogram] = Rgba(a, 0.80f);
  c[ImGuiCol_PlotHistogramHovered] = Rgba(p.text, 1.f);
  c[ImGuiCol_TableHeaderBg] = Rgba(p.frame, 1.f);
  c[ImGuiCol_TableBorderStrong] = Rgba(a, 0.50f);
  c[ImGuiCol_TableBorderLight] = Rgba(a, 0.20f);
  c[ImGuiCol_TableRowBg] = ImVec4(0.f, 0.f, 0.f, 0.f);
  c[ImGuiCol_TableRowBgAlt] = ImVec4(1.f, 1.f, 1.f, 0.035f);
  c[ImGuiCol_TextLink] = Rgba(a, 1.f);
  c[ImGuiCol_TextSelectedBg] = Rgba(a, 0.35f);
  c[ImGuiCol_DragDropTarget] = Rgba(a, 0.90f);
  c[ImGuiCol_NavCursor] = Rgba(a, 1.f);
  c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.f, 1.f, 1.f, 0.70f);
  c[ImGuiCol_NavWindowingDimBg] = Rgba(p.bg, 0.55f);
  c[ImGuiCol_ModalWindowDimBg] = Rgba(p.bg, 0.60f);
}

// Only colours, borders and rounding; paddings and sizes stay ApplyTouchStyle's. Border and
// rounding values are unscaled units times the scale applied so far (ScaleAllSizes never runs here).
void ApplyPrimeTheme(ImGuiStyle& style, int theme) {
  const float scale = sUiAppliedScale;
  if (theme == kThemePlain) {
    ImGui::StyleColorsDark(&style);
    style.WindowBorderSize = style.ChildBorderSize = style.PopupBorderSize = std::round(scale);
    style.FrameBorderSize = 0.f;
    style.TabBorderSize = 0.f;
    style.WindowRounding = style.ChildRounding = style.PopupRounding = 0.f;
    // ApplyTouchStyle's values, which a switch from another theme must restore.
    style.FrameRounding = style.GrabRounding = style.TabRounding = 5.f * scale;
    style.ScrollbarRounding = 9.f * scale;
    return;
  }
  ThemePalette p;
  if (theme == kThemeRemastered) {
    // Cooler glass HUD: dark navy, ice-blue accent.
    p.bg = ThemeRgb(0.035f, 0.055f, 0.105f);
    p.bgAlpha = 0.88f;
    p.deep = ThemeRgb(0.045f, 0.075f, 0.140f);
    p.frame = ThemeRgb(0.075f, 0.140f, 0.240f);
    p.frameHi = ThemeRgb(0.110f, 0.210f, 0.340f);
    p.accent = ThemeRgb(0.373f, 0.847f, 1.000f); // #5FD8FF
    p.text = ThemeRgb(0.97f, 0.99f, 1.00f);
    p.textDim = ThemeRgb(0.50f, 0.60f, 0.72f);
  } else {
    // Retail pause / scan visor: near-black blue, teal frames, amber accent.
    p.bg = ThemeRgb(0.020f, 0.063f, 0.102f); // #05101A
    p.bgAlpha = 0.92f;
    p.deep = ThemeRgb(0.030f, 0.090f, 0.140f);
    p.frame = ThemeRgb(0.045f, 0.170f, 0.230f);
    p.frameHi = ThemeRgb(0.070f, 0.260f, 0.340f);
    p.accent = ThemeRgb(1.000f, 0.604f, 0.180f); // #FF9A2E
    p.text = ThemeRgb(0.95f, 0.91f, 0.82f);
    p.textDim = ThemeRgb(0.45f, 0.55f, 0.65f);
  }
  FillThemeColors(style, p);
  const float round = theme == kThemeRemastered ? 8.f : 2.f;
  style.WindowBorderSize = style.ChildBorderSize = style.PopupBorderSize = style.FrameBorderSize =
      std::max(1.f, std::round(scale));
  style.TabBorderSize = 0.f;
  style.WindowRounding = style.ChildRounding = style.PopupRounding = round * scale;
  style.FrameRounding = style.GrabRounding = style.TabRounding = (theme == kThemeRemastered ? 7.f : 2.f) * scale;
  style.ScrollbarRounding = (theme == kThemeRemastered ? 9.f : 2.f) * scale;
}

// Called every frame: restyles only when the resolved theme or the UI scale changed.
void ApplyThemeIfChanged() {
  if (ImGui::GetCurrentContext() == nullptr) {
    return;
  }
  sThemeResolved = ResolveTheme();
  if (sThemeResolved == sThemeApplied && sUiAppliedScale == sThemeAppliedScale) {
    return;
  }
  sThemeApplied = sThemeResolved;
  sThemeAppliedScale = sUiAppliedScale;
  ApplyPrimeTheme(ImGui::GetStyle(), sThemeResolved);
}

// Status colours that stay legible on the current theme. Plain returns `plain`, the old values.
ImVec4 ThemeWarnColor(const ImVec4& plain = ImVec4(1.f, 0.8f, 0.3f, 1.f)) {
  switch (sThemeResolved) {
  case kThemePrime: return ImVec4(1.f, 0.88f, 0.32f, 1.f); // yellow, apart from the amber accent
  case kThemeRemastered: return ImVec4(1.f, 0.82f, 0.38f, 1.f);
  default: return plain;
  }
}

ImVec4 ThemeGoodColor(const ImVec4& plain = ImVec4(0.4f, 1.f, 0.4f, 1.f)) {
  switch (sThemeResolved) {
  case kThemePrime: return ImVec4(0.45f, 0.95f, 0.55f, 1.f);
  case kThemeRemastered: return ImVec4(0.45f, 1.f, 0.80f, 1.f);
  default: return plain;
  }
}

ImVec4 ThemeBadColor(const ImVec4& plain = ImVec4(1.f, 0.5f, 0.3f, 1.f)) {
  switch (sThemeResolved) {
  case kThemePrime: return ImVec4(1.f, 0.42f, 0.35f, 1.f);
  case kThemeRemastered: return ImVec4(1.f, 0.45f, 0.52f, 1.f);
  default: return plain;
  }
}

// --- F1 overlay sounds ---------------------------------------------------------------------------
//
// The game's own UI effects (group UI_AGSC 40, preloaded at boot, so loaded at the title screen and
// in game), started through CSfxManager like the pause screen does. Chosen by name and by what
// CPauseScreenBlur does:
//   open  -> SFXui_x_pause_00   (the map/pause screen opening)
//   close -> SFXui_x_pause_01   (the same screen closing)
//   click / activate (button, checkbox, combo, tab, selectable) -> SFXui_x_invchoos_00 (choose)
//   keyboard/pad focus move  -> SFXui_x_invscrol_00 (scroll between inventory items)
//   back (Esc, pad B)        -> SFXui_x_invback_00
// CSfxManager::Update runs in the main loop even while the overlay is open (the overlay holds the
// simulation, not the audio), so these play with F1 up. They queue into the current channel.
enum MenuSound { kMenuOpen, kMenuClose, kMenuChoose, kMenuMove, kMenuBack };

// `always` skips the rate limit: opening and closing are rare and must not be swallowed by the
// press that caused them.
void PlayMenuSound(MenuSound sound, bool always = false) {
  if (!sUiSounds || sThemeResolved == kThemePlain || !sMainLoopRan) {
    return;
  }
  static uint64_t sLastMs = 0;
  const uint64_t now = SDL_GetTicks();
  if (!always && sLastMs != 0 && now - sLastMs < 50) {
    return;
  }
  sLastMs = now;
  ushort id = 0;
  switch (sound) {
  case kMenuOpen: id = SFXui_x_pause_00; break;
  case kMenuClose: id = SFXui_x_pause_01; break;
  case kMenuChoose: id = SFXui_x_invchoos_00; break;
  case kMenuMove: id = SFXui_x_invscrol_00; break;
  case kMenuBack: id = SFXui_x_invback_00; break;
  }
  CSfxManager::SfxStart(id, 127, 64, false, CSfxManager::kMedPriority);
}

// Derives sounds from ImGui's own state once a frame instead of hooking widgets: a new active item
// is a press (a held slider is one press, not one per frame), and a nav focus change with the nav
// cursor showing is a keyboard or pad move (mouse hover never moves nav focus or shows the cursor).
// `visible` is whether the overlay is open this frame; call it at the end of DrawUI.
void UpdateMenuSounds(bool visible) {
  static bool sWasVisible = false;
  static ImGuiID sPrevActive = 0;
  static ImGuiID sPrevNav = 0;
  if (visible != sWasVisible) {
    sWasVisible = visible;
    sPrevActive = sPrevNav = 0;
    // A back press that closes the overlay is the close, not a second sound: this branch returns
    // before the back check below, and the close ignores the rate limit.
    PlayMenuSound(visible ? kMenuOpen : kMenuClose, true);
    return;
  }
  if (!visible || ImGui::GetCurrentContext() == nullptr) {
    return;
  }
  ImGuiContext& g = *ImGui::GetCurrentContext();
  const ImGuiID active = g.ActiveId;
  const ImGuiID nav = g.NavId;
  bool pressed = active != 0 && active != sPrevActive;
  if (pressed && g.ActiveIdWindow != nullptr) {
    // Dragging a window by its title, or a scrollbar, is not a choice.
    ImGuiWindow* window = g.ActiveIdWindow;
    pressed = active != window->MoveId && active != ImGui::GetWindowScrollbarID(window, ImGuiAxis_X) &&
              active != ImGui::GetWindowScrollbarID(window, ImGuiAxis_Y);
  }
  const bool moved = nav != 0 && nav != sPrevNav && g.NavCursorVisible && active == 0;
  const bool back = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
  sPrevActive = active;
  sPrevNav = nav;
  if (back) {
    PlayMenuSound(kMenuBack);
  } else if (pressed) {
    PlayMenuSound(kMenuChoose);
  } else if (moved) {
    PlayMenuSound(kMenuMove);
  }
}

// Phones report a density of roughly 3, which leaves ImGui's default 13px font
// unreadably small, so scale the overlay to match the display. The scale is not
// known on the first frame, so keep watching for it instead of latching once.
void UpdateUiScale() {
  if (ImGui::GetCurrentContext() == nullptr) {
    return;
  }
  static bool sInitialized = false;
  if (!sInitialized) {
    sInitialized = true;
    // The overlay has to size itself to the scaled font, so do not restore a
    // window size remembered from a previous, smaller run.
    ImGui::GetIO().IniFilename = nullptr;
    // Before any scaling, so the scaling applies to these sizes too. Both
    // layouts use it, so switching layout needs no restyle.
    ApplyTouchStyle(ImGui::GetStyle());
  }
  SDL_Window* window = MainWindow();
  if (window == nullptr) {
    return;
  }
  const float displayScale = SDL_GetWindowDisplayScale(window);
  const float uiScale = std::clamp(displayScale, 1.f, 4.f);
  if (uiScale == sUiAppliedScale) {
    return;
  }
  const float ratio = uiScale / sUiAppliedScale;
  sUiAppliedScale = uiScale;
  ImGui::GetStyle().ScaleAllSizes(ratio);
  ImGui::GetIO().FontGlobalScale *= ratio;
}

void RequestToggle() { sToggleRequested.store(true, std::memory_order_release); }

static std::atomic<bool> sGpuSelfTestRequested{false};

void RequestGpuSelfTest() { sGpuSelfTestRequested.store(true, std::memory_order_release); }

void RunGpuSelfTestIfRequested() {
  // Pipelines compile asynchronously and a draw is skipped until its pipeline is ready, so a request first
  // runs a silent warm-up pass, waits for the compile queue to drain, then runs the logged pass.
  enum class Stage { Idle, WarmingUp };
  static Stage stage = Stage::Idle;
  static unsigned waited = 0;
  static unsigned frames = 0;
  static const bool envRun = port::EnvFlag("MP_GPU_SELFTEST");
  if (envRun && ++frames == 120) {
    RequestGpuSelfTest();
  }
  bool runReal = false;
  if (stage == Stage::WarmingUp) {
    if (aurora_gpu_selftest_pending()) {
      return;
    }
    // Let the pipelines the warm-up queued finish (a few frames at least, and not forever).
    const bool drained = aurora_get_stats()->queuedPipelines == 0;
    if (++waited < 3 || (!drained && waited < 1800)) {
      return;
    }
    stage = Stage::Idle;
    runReal = true;
  } else if (sGpuSelfTestRequested.exchange(false, std::memory_order_acq_rel)) {
    if (aurora_gpu_selftest_run(true)) {
      stage = Stage::WarmingUp;
      waited = 0;
      CGX::ResetGXStatesFull();
      CGraphics::SetViewMatrix();
    }
    return;
  }
  if (runReal && aurora_gpu_selftest_run(false)) {
    // The test left its own GX state behind: drop the game's cached copy of it.
    CGX::ResetGXStatesFull();
    CGraphics::SetViewMatrix();
  }
}

void UpdateControllerNav() {
  EnsureInitialized();
  // Registered here rather than in EnsureInitialized so the event system is only
  // touched from the game thread; the Java visibility query can reach that
  // initialization from the UI thread.
  static bool sEventWatchRegistered = false;
  if (!sEventWatchRegistered) {
    sEventWatchRegistered = true;
    SDL_AddEventWatch(debug_event_watch, nullptr);
  }
  UpdateUiScale();
  ApplyThemeIfChanged();
  if (sToggleRequested.exchange(false, std::memory_order_acq_rel)) {
    Toggle();
  }
  if (const int hotkey = sSaveStateHotkey.exchange(0, std::memory_order_acq_rel);
      hotkey != 0 && sSaveStateHotkeys) {
    if (hotkey == 1) {
      PortSaveState::RequestSave(PortSaveState::SelectedSlot());
    } else {
      PortSaveState::RequestLoad(PortSaveState::SelectedSlot());
    }
  }
  sOverlayVisible.store(sVisible, std::memory_order_release);
  sTouchColorsFlag.store(sTouchColors, std::memory_order_release);
  sTouchLabelsFlag.store(sTouchLabels, std::memory_order_release);
  sTouchTurboFlag.store(sTouchTurbo && !sOriginalExperience, std::memory_order_release);
  sTouchFloatingStickFlag.store(sTouchFloatingStick, std::memory_order_release);

  ImGuiIO& io = ImGui::GetIO();
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  // ImGui applies one change per key and frame and holds everything queued
  // behind a second one. Whatever queues changes faster than that stalls the
  // overlay's input until it stops, and then the backlog replays. Nothing
  // should; if something does, take the queue in one go and say so.
  {
    const int queued = ImGui::GetCurrentContext()->InputEventsQueue.Size;
    io.ConfigInputTrickleEventQueue = queued <= 64;
    static uint64_t sLastLogNs = 0;
    const uint64_t now = SDL_GetTicksNS();
    if (queued > 64 && (sLastLogNs == 0 || now - sLastLogNs > 2000000000ull)) {
      sLastLogNs = now;
      PortLog::Write("metroid_prime_port: %d overlay input events were queued; flushed\n", queued);
    }
  }
  // The SDL3 backend calls SDL_ShowCursor on every NewFrame, and the main loop
  // hides the cursor again during play, so it flickered wherever relative
  // mouse mode wasn't hiding it (Android, menus, cutscenes). Let the backend
  // own the cursor only while the overlay is open.
  if (sVisible) {
    io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
  } else {
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
  }

  // This is the only writer of the gamepad keys (Aurora turns the backend's
  // own pad polling off): a second one that disagrees about a key, as the
  // backend did with a stick held as a d-pad or with another pad than the
  // player's, queues two changes a frame where ImGui applies one.
  SDL_Gamepad* pad = PADGetSDLGamepadForIndex(0);
  // While the Controls tab captures a pad input, the pad binds instead of
  // navigating (releasing everything here also ends any nav press in progress).
  // A pad that went away releases everything the same way.
  const bool capturing = pad == nullptr || PortControls::Capturing();
  const auto held = [pad, capturing](SDL_GamepadButton button) {
    return !capturing && SDL_GetGamepadButton(pad, button);
  };
  const auto axis = [pad, capturing](SDL_GamepadAxis a) {
    return capturing ? Sint16{0} : SDL_GetGamepadAxis(pad, a);
  };
  constexpr Sint16 kStickThreshold = 16000;
  io.AddKeyEvent(ImGuiKey_GamepadDpadUp,
                 held(SDL_GAMEPAD_BUTTON_DPAD_UP) || axis(SDL_GAMEPAD_AXIS_LEFTY) < -kStickThreshold);
  io.AddKeyEvent(ImGuiKey_GamepadDpadDown,
                 held(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || axis(SDL_GAMEPAD_AXIS_LEFTY) > kStickThreshold);
  io.AddKeyEvent(ImGuiKey_GamepadDpadLeft,
                 held(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || axis(SDL_GAMEPAD_AXIS_LEFTX) < -kStickThreshold);
  io.AddKeyEvent(ImGuiKey_GamepadDpadRight,
                 held(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || axis(SDL_GAMEPAD_AXIS_LEFTX) > kStickThreshold);
  io.AddKeyEvent(ImGuiKey_GamepadFaceDown, held(SDL_GAMEPAD_BUTTON_SOUTH));
  io.AddKeyEvent(ImGuiKey_GamepadFaceRight, held(SDL_GAMEPAD_BUTTON_EAST));
  io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, held(SDL_GAMEPAD_BUTTON_WEST));
  io.AddKeyEvent(ImGuiKey_GamepadFaceUp, held(SDL_GAMEPAD_BUTTON_NORTH));
  io.AddKeyEvent(ImGuiKey_GamepadL1, held(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
  io.AddKeyEvent(ImGuiKey_GamepadR1, held(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER));

  static bool sChordHeld = false;
  const bool chord = held(SDL_GAMEPAD_BUTTON_START) && held(SDL_GAMEPAD_BUTTON_BACK);
  if (chord && !sChordHeld) {
    Toggle();
  }
  sChordHeld = chord;
}

void Toggle() {
  EnsureInitialized();
  sVisible = !sVisible;
  SetMouseCaptured(false);
  // A press that never got its release (seen on the Android tablet with a mouse
  // while the pointer was captured) leaves ImGui holding the button, and every
  // later tap is ignored. Opening the overlay drops held mouse buttons and keys,
  // as a focus loss would.
  if (sVisible && ImGui::GetCurrentContext() != nullptr) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddFocusEvent(false);
    io.AddFocusEvent(true);
  }
}

namespace {
// What a setting does. A tooltip on the desktop; a tap shows none, so Android
// prints it, dimmed, under the setting instead.
#if defined(__ANDROID__)
constexpr bool kInlineHelp = true;
#else
constexpr bool kInlineHelp = false;
#endif

void ItemHelp(const char* text) {
  if (kInlineHelp) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
  } else if (ImGui::BeginItemTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

// SameLine, except after an ItemHelp that printed its text on Android.
void SameLineAfterHelp() {
  if (!kInlineHelp) {
    ImGui::SameLine();
  }
}
} // namespace

// Rows the Original experience overrides: shown with their saved values, which
// come back when it's turned off.
bool BeginOriginalLocked() {
  if (!sOriginalExperience) {
    return false;
  }
  ImGui::TextDisabled("Original experience is on: these keep their values for later.");
  ImGui::BeginDisabled();
  return true;
}

void EndOriginalLocked(bool locked) {
  if (locked) {
    ImGui::EndDisabled();
  }
}

void DrawPerformanceTab() {
  ImGui::SeparatorText("Frame rate");
  const bool locked = BeginOriginalLocked();
  bool frameLimit = sFrameLimitEnabled;
  if (ImGui::Checkbox("60 FPS cap (target)", &frameLimit)) {
    SetFrameLimitEnabled(frameLimit);
    MarkDirty();
  }
  // Both, because they answer different questions. Throughput is what the
  // machine produces once the pacing wait is excluded; presented is what
  // reaches the screen. A player seeing stutter wants the second one, and the
  // gap between them is the headroom.
  ImGui::Text("Presented %.1f FPS    Throughput %.1f FPS    Frame %.2f ms", sActualFps, sThroughputFps,
              static_cast< double >(ImGui::GetIO().DeltaTime) * 1000.0);
  ImGui::SetItemTooltip("Presented is what reaches the screen. Throughput leaves out the wait for the\n"
                        "frame cap: the gap between the two is the headroom at the current frame cost.");
  if (SimAdaptive()) {
    ImGui::Text("Simulation %.1f ticks/s (adaptive)", sActualTps);
  } else {
    ImGui::Text("Simulation %.1f ticks/s (target %u)", sActualTps, SimRate());
  }

  bool smooth = sSmoothFrames;
  if (ImGui::Checkbox("Smooth uncapped frames", &smooth)) {
    PortDebug::SetSmoothFrames(smooth);
  }
  ItemHelp("With the FPS cap off, frames between the game's 60 Hz ticks turn the view with "
           "mouse, gyro and twin-stick look, and draw moving objects, animation and particles "
           "between their last two ticks. The game itself still runs at 60 Hz, so aim and "
           "shots are unchanged.");

  ImGui::SeparatorText("Simulation rate (very experimental)");
  ImGui::TextColored(ThemeWarnColor(), "For testing only. The game was written for 60 Hz: other rates can\n"
                                       "break physics, enemy behaviour, cutscenes and scripted events.");
  bool adaptive = sSimAdaptive;
  if (ImGui::Checkbox("Adaptive (follow frame rate)", &adaptive)) {
    PortDebug::SetSimAdaptive(adaptive);
  }
  ItemHelp("One step per frame with dt = the measured frame time (clamped 30-480 Hz), so a "
           "variable frame rate is matched exactly. Leave the FPS cap off.");
  ImGui::BeginDisabled(adaptive);
  int simRate = static_cast< int >(sSimRate);
  if (ImGui::SliderInt("Sim Hz", &simRate, 30, 480)) {
    PortDebug::SetSimRate(static_cast< unsigned >(simRate));
  }
  ImGui::EndDisabled();
  ItemHelp("60 Hz is console-accurate and the only rate that's supported. Other values step the "
           "game logic at that rate instead of smoothing frames between 60 Hz ticks; leave the "
           "FPS cap off for it to matter. For smooth high frame rates, use Smooth uncapped "
           "frames instead.");
  EndOriginalLocked(locked);
}

// Memory card transfer (port_gci.h). The work runs on the main thread; the
// file dialogs answer on another one, so their picks wait in sCardPicks.
namespace {
enum CardPick { kCardPick_Import, kCardPick_ExportFolder, kCardPick_ExportFile };
std::mutex sCardPickMutex;
std::vector<std::pair<CardPick, std::string>> sCardPicks;
std::atomic<bool> sCardDialogOpen{false};
std::string sCardStatus;
// Android has no folder dialog: each file gets its own save dialog, in turn.
std::vector<std::filesystem::path> sCardExportQueue;

// The last path segment, for messages; content:// URIs keep theirs escaped.
std::string CardDisplayName(const std::string& path) {
  std::string name = path.substr(path.find_last_of("/\\") + 1);
  const size_t escaped = name.rfind("%2F");
  return escaped == std::string::npos ? name : name.substr(escaped + 3);
}

std::string CardImportTarget(std::filesystem::path& folder) {
  if (sStateManager != nullptr) {
    return "Return to the title screen to import: saving the game in progress would "
           "overwrite the imported save.";
  }
  folder = PortGci::MountedCardFolder();
  if (folder.empty()) {
    return "The memory card is not a GCI folder, so there is nowhere to import to.";
  }
  return {};
}

std::string CardExportSource(std::filesystem::path& folder) {
  folder = PortGci::MountedCardFolder();
  if (folder.empty()) {
    return "The memory card is not a GCI folder.";
  }
  if (PortGci::GameFiles(folder).empty()) {
    return "There are no saves on the memory card yet.";
  }
  return {};
}

std::string FinishCardImport(const PortGci::Report& report) {
  if (report.copied > 0) {
    // The save screen remounts the card and reads it again once it is idle.
    PortGci::MarkCardChanged();
  }
  return report.Summary("Imported");
}
} // namespace

std::string CardList() {
  const std::filesystem::path folder = PortGci::MountedCardFolder();
  if (folder.empty()) {
    return "The memory card is not a GCI folder.";
  }
  std::string text = "card: " + PortGci::PathString(folder);
  for (const std::filesystem::path& file : PortGci::GameFiles(folder)) {
    std::error_code ec;
    text += "\n  " + PortGci::PathString(file.filename()) + " (" +
            std::to_string(std::filesystem::file_size(file, ec)) + " bytes)";
  }
  const PortGci::DolphinCard dolphin = PortGci::FindDolphinCard();
  if (!dolphin.gciFolder.empty()) {
    text += "\ndolphin folder: " + PortGci::PathString(dolphin.gciFolder);
  }
  if (!dolphin.rawImage.empty()) {
    text += "\ndolphin raw: " + PortGci::PathString(dolphin.rawImage);
  }
  return text;
}

std::string CardImport(const std::string& path) {
  std::filesystem::path folder;
  const std::string refusal = CardImportTarget(folder);
  if (!refusal.empty()) {
    return refusal;
  }
  std::error_code ec;
  if (std::filesystem::is_directory(PortGci::PathFromString(path), ec)) {
    return FinishCardImport(PortGci::ImportFolder(PortGci::PathFromString(path), folder));
  }
  // SDL reads content:// URIs from the Android picker as well as paths.
  size_t size = 0;
  void* data = SDL_LoadFile(path.c_str(), &size);
  if (data == nullptr) {
    return "Could not read " + CardDisplayName(path) + ": " + SDL_GetError();
  }
  const uint8_t* bytes = static_cast< const uint8_t* >(data);
  const std::vector<uint8_t> contents(bytes, bytes + size);
  SDL_free(data);
  return FinishCardImport(
      PortGci::ImportBytes(contents, CardDisplayName(path), folder, folder.parent_path()));
}

std::string CardExport(const std::string& dest) {
  std::filesystem::path folder;
  const std::string refusal = CardExportSource(folder);
  if (!refusal.empty()) {
    return refusal;
  }
  const std::filesystem::path target = PortGci::PathFromString(dest);
  if (target.extension() == ".raw") {
    return PortGci::ExportRaw(folder, target).Summary("Exported");
  }
  return PortGci::ExportFolder(folder, target).Summary("Exported");
}

std::string CardImportDolphin() {
  std::filesystem::path folder;
  const std::string refusal = CardImportTarget(folder);
  if (!refusal.empty()) {
    return refusal;
  }
  const PortGci::DolphinCard dolphin = PortGci::FindDolphinCard();
  if (!dolphin.Found()) {
    return "No Dolphin memory card found (GC/USA/Card A or GC/MemoryCardA.USA.raw in "
           "Dolphin's user folder).";
  }
  // Dolphin uses one or the other, per its settings: try the one written last.
  std::error_code ec;
  std::filesystem::file_time_type folderTime = std::filesystem::file_time_type::min();
  for (const std::filesystem::path& file : PortGci::GameFiles(dolphin.gciFolder)) {
    folderTime = std::max(folderTime, std::filesystem::last_write_time(file, ec));
  }
  const auto rawTime = dolphin.rawImage.empty() ? std::filesystem::file_time_type::min()
                                                : std::filesystem::last_write_time(dolphin.rawImage, ec);
  std::vector<std::filesystem::path> sources;
  if (!dolphin.gciFolder.empty() && folderTime != std::filesystem::file_time_type::min()) {
    sources.push_back(dolphin.gciFolder);
  }
  if (!dolphin.rawImage.empty()) {
    sources.insert(rawTime > folderTime ? sources.begin() : sources.end(), dolphin.rawImage);
  }
  for (const std::filesystem::path& source : sources) {
    const PortGci::Report report = source == dolphin.gciFolder
                                       ? PortGci::ImportFolder(source, folder)
                                       : PortGci::ImportFile(source, folder, folder.parent_path());
    if (report.copied > 0 || !report.errors.empty()) {
      return "From " + PortGci::PathString(source) + ": " + FinishCardImport(report);
    }
  }
  return "Dolphin's memory card holds no Metroid Prime saves.";
}

std::string CardExportDolphin() {
  std::filesystem::path folder;
  const std::string refusal = CardExportSource(folder);
  if (!refusal.empty()) {
    return refusal;
  }
  const PortGci::DolphinCard dolphin = PortGci::FindDolphinCard();
  if (!dolphin.Found()) {
    return "No Dolphin memory card found (GC/USA/Card A or GC/MemoryCardA.USA.raw in "
           "Dolphin's user folder); start a GameCube game in Dolphin once to create it.";
  }
  std::string text;
  if (!dolphin.gciFolder.empty()) {
    text = "To " + PortGci::PathString(dolphin.gciFolder) + ": " +
           PortGci::ExportFolder(folder, dolphin.gciFolder).Summary("Exported");
  }
  if (!dolphin.rawImage.empty()) {
    text += std::string(text.empty() ? "" : "\n") + "To " +
            PortGci::PathString(dolphin.rawImage) + ": " +
            PortGci::ExportRaw(folder, dolphin.rawImage).Summary("Exported");
  }
  return text;
}

namespace {
void OpenCardDialog(CardPick pick) {
  int windowCount = 0;
  SDL_Window** windows = SDL_GetWindows(&windowCount);
  SDL_Window* window = windows != nullptr && windowCount > 0 ? windows[0] : nullptr;
  SDL_free(windows);
  const SDL_DialogFileCallback done = [](void* userdata, const char* const* files, int) {
    std::lock_guard lock(sCardPickMutex);
    const CardPick kind = static_cast< CardPick >(reinterpret_cast< intptr_t >(userdata));
    // An empty path means cancelled (or a failed dialog, with the error set).
    sCardPicks.emplace_back(kind, files != nullptr && files[0] != nullptr ? files[0] : "");
    if (files == nullptr) {
      sCardPicks.back().second = std::string("\x01") + SDL_GetError();
    }
    sCardDialogOpen = false;
  };
  void* userdata = reinterpret_cast< void* >(static_cast< intptr_t >(pick));
  sCardDialogOpen = true;
  switch (pick) {
  case kCardPick_Import: {
#if defined(__ANDROID__)
    // Android turns filters into MIME types, and .gci has none.
    SDL_ShowOpenFileDialog(done, userdata, window, nullptr, 0, nullptr, false);
#else
    static const SDL_DialogFileFilter filters[] = {
        {"GameCube saves (.gci, card images)", "gci;raw;mcp;sav"},
        {"All files", "*"},
    };
    SDL_ShowOpenFileDialog(done, userdata, window, filters, 2, nullptr, false);
#endif
    break;
  }
  case kCardPick_ExportFolder:
    SDL_ShowOpenFolderDialog(done, userdata, window, nullptr, false);
    break;
  case kCardPick_ExportFile: {
    static std::string location;
    location = sCardExportQueue.empty()
                   ? std::string()
                   : PortGci::PathString(sCardExportQueue.front().filename());
    SDL_ShowSaveFileDialog(done, userdata, window, nullptr, 0,
                           location.empty() ? nullptr : location.c_str());
    break;
  }
  }
}

std::string CardSaveTo(const std::filesystem::path& source, const std::string& target) {
  std::ifstream in(source, std::ios::binary);
  const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  SDL_IOStream* out = in ? SDL_IOFromFile(target.c_str(), "wb") : nullptr;
  bool ok = out != nullptr && SDL_WriteIO(out, bytes.data(), bytes.size()) == bytes.size();
  if (out != nullptr) {
    ok = SDL_CloseIO(out) && ok;
  }
  return ok ? "Saved " + PortGci::PathString(source.filename()) + " as " + CardDisplayName(target) + "."
            : "Could not save " + PortGci::PathString(source.filename()) + ": " + SDL_GetError();
}

// Runs every frame, overlay open or not, so a dialog's answer is acted on.
void ProcessCardPicks() {
  std::vector<std::pair<CardPick, std::string>> picks;
  {
    std::lock_guard lock(sCardPickMutex);
    picks.swap(sCardPicks);
  }
  for (const auto& [kind, path] : picks) {
    if (path.empty() || path[0] == '\x01') {
      if (path.size() > 1) {
        sCardStatus = "The file dialog failed: " + path.substr(1);
      }
      sCardExportQueue.clear();
      continue;
    }
    switch (kind) {
    case kCardPick_Import:
      sCardStatus = CardImport(path);
      break;
    case kCardPick_ExportFolder:
      sCardStatus = CardExport(path);
      break;
    case kCardPick_ExportFile:
      if (!sCardExportQueue.empty()) {
        sCardStatus = CardSaveTo(sCardExportQueue.front(), path);
        sCardExportQueue.erase(sCardExportQueue.begin());
        if (!sCardExportQueue.empty()) {
          OpenCardDialog(kCardPick_ExportFile);
        }
      }
      break;
    }
  }
}

void DrawMemoryCard() {
  ImGui::SeparatorText("Memory card");
  const std::filesystem::path folder = PortGci::MountedCardFolder();
  if (folder.empty()) {
    ImGui::TextDisabled("The card is not a GCI folder; nothing to import to or export.");
    return;
  }
  const size_t saves = PortGci::GameFiles(folder).size();
  ImGui::TextWrapped("Card: %s (%zu save file%s)", PortGci::PathString(folder).c_str(), saves,
                     saves == 1 ? "" : "s");
  const bool inGame = sStateManager != nullptr;
  const bool busy = sCardDialogOpen;
  ImGui::BeginDisabled(inGame || busy);
  if (ImGui::Button("Import file...")) {
    OpenCardDialog(kCardPick_Import);
  }
  ItemHelp("Dolphin .gci saves or a whole card image (.raw). An import replaces the card's saves; "
           "the old ones move to _replaced in the card folder.");
#if !defined(__ANDROID__)
  ImGui::SameLine();
  if (ImGui::Button("Import from Dolphin")) {
    sCardStatus = CardImportDolphin();
  }
  ItemHelp("Dolphin's card is looked for in its user folder (GC/USA/Card A, GC/MemoryCardA.USA.raw).");
#endif
  ImGui::EndDisabled();
  ImGui::BeginDisabled(busy || saves == 0);
#if defined(__ANDROID__)
  if (ImGui::Button("Export...")) {
    sCardExportQueue = PortGci::GameFiles(folder);
    OpenCardDialog(kCardPick_ExportFile);
  }
  ItemHelp("Saves each file in turn; keep Dolphin's names (01-GM8E-MetroidPrime A.gci) for its GCI "
           "folder.");
#else
  if (ImGui::Button("Export to folder...")) {
    OpenCardDialog(kCardPick_ExportFolder);
  }
  ImGui::SameLine();
  if (ImGui::Button("Export to Dolphin")) {
    sCardStatus = CardExportDolphin();
  }
  ItemHelp("Close Dolphin first. A raw card is backed up to .raw.bak before it is written.");
#endif
  ImGui::EndDisabled();
#if !defined(__ANDROID__)
  ImGui::SameLine();
  if (ImGui::Button("Open card folder")) {
    // "Card A" has a space, which a URL can't carry as is.
    const std::string path = PortGci::PathString(folder);
    std::string url = path.front() == '/' ? "file://" : "file:///"; // C:\ on Windows
    for (const char c : path) {
      url += c == ' ' ? std::string("%20") : std::string(1, c == '\\' ? '/' : c);
    }
    SDL_OpenURL(url.c_str());
  }
#endif
  if (inGame) {
    ImGui::TextColored(ThemeWarnColor(), "Return to the title screen to import.");
  }
  if (!sCardStatus.empty()) {
    ImGui::TextWrapped("%s", sCardStatus.c_str());
  }
}

// The Remastered import (port_remastered_import.h): the user's own image and
// key file, converted here into the remastered-models mod.
std::mutex sRemasteredPickMutex;
std::vector<std::pair<int, std::string>> sRemasteredPicks;

#if defined(__ANDROID__)
// Not SDL_ShowOpenFileDialog: Android often kills the game behind the picker
// for its memory (seen on a tablet: the pick came back to a new process), and
// SDL's callback dies with it. MetroidPrimeActivity.pickRemasteredFile writes
// the picked address to this file, which the panel reads, in this process or
// the next one.
std::string RemasteredPickFile(int which) {
  const char* root = SDL_GetAndroidInternalStoragePath();
  return std::string(root != nullptr ? root : ".") + "/remastered_pick_" + std::to_string(which) + ".txt";
}

void TakeRemasteredPickFiles() {
  for (int which = 0; which < 2; ++which) {
    std::ifstream in(RemasteredPickFile(which));
    std::string uri;
    if (std::getline(in, uri) && !uri.empty()) {
      sRemasteredPicks.emplace_back(which, uri);
    }
  }
}

void OpenRemasteredDialog(int which) {
  JNIEnv* env = static_cast< JNIEnv* >(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast< jobject >(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return;
  }
  jclass cls = env->GetObjectClass(activity);
  jmethodID method = env->GetMethodID(cls, "pickRemasteredFile", "(I)V");
  if (method != nullptr) {
    env->CallVoidMethod(activity, method, jint(which));
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
  }
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(activity);
}
#else
void OpenRemasteredDialog(int which) {
  int windowCount = 0;
  SDL_Window** windows = SDL_GetWindows(&windowCount);
  SDL_Window* window = windows != nullptr && windowCount > 0 ? windows[0] : nullptr;
  SDL_free(windows);
  const SDL_DialogFileCallback done = [](void* userdata, const char* const* files, int) {
    if (files != nullptr && files[0] != nullptr) {
      std::lock_guard lock(sRemasteredPickMutex);
      sRemasteredPicks.emplace_back(int(reinterpret_cast< intptr_t >(userdata)), files[0]);
    }
  };
  static const SDL_DialogFileFilter imageFilters[] = {{"Switch images (.nsp, .xci)", "nsp;xci"}, {"All files", "*"}};
  static const SDL_DialogFileFilter keyFilters[] = {{"Key files (.keys)", "keys"}, {"All files", "*"}};
  SDL_ShowOpenFileDialog(done, reinterpret_cast< void* >(static_cast< intptr_t >(which)), window,
                         which == 0 ? imageFilters : keyFilters, 2, nullptr, false);
}
#endif

#if defined(__ANDROID__)
// Android's picker gives a content:// address, which only the system can open.
// The image is several GB, so it is not copied as the disc is: the file is
// opened here and the import reads it through the descriptor ("fd:<n>", see
// SourceFile). One descriptor per field, closed when another file is picked.
std::string OpenRemasteredPick(int which, const std::string& uri) {
  static int sHeld[2] = {-1, -1};
  if (sHeld[which] >= 0) {
    close(sHeld[which]);
    sHeld[which] = -1;
  }
  SDL_IOStream* io = SDL_IOFromFile(uri.c_str(), "rb");
  if (io == nullptr) {
    PortLog::Write("metroid_prime_port: could not open the picked file: %s: %s\n", uri.c_str(), SDL_GetError());
    return {};
  }
  const int fd = int(SDL_GetNumberProperty(SDL_GetIOProperties(io), SDL_PROP_IOSTREAM_FILE_DESCRIPTOR_NUMBER, -1));
  sHeld[which] = fd >= 0 ? dup(fd) : -1;
  SDL_CloseIO(io);
  return sHeld[which] >= 0 ? "fd:" + std::to_string(sHeld[which]) : std::string();
}

// "content://.../document/primary%3ADownload%2Fgame.nsp" -> "game.nsp".
std::string RemasteredPickName(const std::string& uri) {
  std::string text;
  for (size_t i = 0; i < uri.size(); ++i) {
    if (uri[i] == '%' && i + 2 < uri.size() && std::isxdigit(static_cast< unsigned char >(uri[i + 1])) &&
        std::isxdigit(static_cast< unsigned char >(uri[i + 2]))) {
      text += char(std::stoi(uri.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else {
      text += uri[i];
    }
  }
  const size_t cut = text.find_last_of("/:");
  return cut == std::string::npos ? text : text.substr(cut + 1);
}
#endif

// What the last "Reload mods" came to: it waits for a game to start when none
// is loaded, and is refused where the game can't be saved.
void DrawModReloadMessage() {
  const std::string message = PortSaveState::LastMessage();
  if (message.find("mods") != std::string::npos || message.find("Mods") != std::string::npos) {
    ImGui::TextWrapped("%s", message.c_str());
  }
}

} // namespace

// An import from the panel unloads the mods while it runs (on a tablet, the
// game holding a room-geometry mod plus the import ran out of memory) and
// loads them again when it ends, the new import in place of the old.
static bool sImportWatched = false;
static bool sImportUnloadedMods = false;
// A reload is refused where the game can't save (a cutscene, say), so these
// are asked again every two seconds until one runs.
static int sReloadAwaited = -1;
static double sReloadRetry = 0.0;

static void ReloadUntilDone() {
  sReloadAwaited = PortSaveState::ModReloads();
  sReloadRetry = ImGui::GetTime() + 2.0;
  PortSaveState::RequestModReload();
}

bool StartRemasteredImport(const std::string& image, const std::string& keys) {
#if defined(__ANDROID__)
  // Each worker holds a world's models while it converts them; a phone has
  // the memory for two of those, not for one per core.
  const bool started = PortRemastered::StartImport(image, keys, 2);
#else
  const bool started = PortRemastered::StartImport(image, keys);
#endif
  if (!started) {
    return false;
  }
  sImportWatched = true;
  const PortMods::Status& status = PortMods::CurrentStatus();
  if (std::any_of(status.mods.begin(), status.mods.end(), [](const PortMods::ModInfo& mod) { return mod.enabled; })) {
    PortMods::SetSuspended(true);
    ReloadUntilDone();
    sImportUnloadedMods = true;
  }
  return true;
}

// Every frame, so the mods come back with the panel closed too.
static void FinishRemasteredImport() {
  if (sReloadAwaited >= 0) {
    if (PortSaveState::ModReloads() != sReloadAwaited) {
      sReloadAwaited = -1;
    } else if (ImGui::GetTime() >= sReloadRetry) {
      sReloadRetry = ImGui::GetTime() + 2.0;
      PortSaveState::RequestModReload();
    }
  }
  if (!sImportWatched) {
    return;
  }
  const PortRemastered::ImportState state = PortRemastered::ImportStatus();
  if (state.running) {
    return;
  }
  sImportWatched = false;
  if (state.ok) {
    // A mod just imported is wanted, even if it was switched off to import.
    std::vector<std::string> disabled = PortMods::SplitDisabled(sModsDisabled);
    const auto kept = std::remove(disabled.begin(), disabled.end(), std::string(PortRemastered::kImportModName));
    if (kept != disabled.end()) {
      disabled.erase(kept, disabled.end());
      SetModsDisabled(PortMods::JoinDisabled(disabled));
    }
  }
  if (state.ok || sImportUnloadedMods) {
    PortMods::SetSuspended(false);
    ReloadUntilDone();
  }
  sImportUnloadedMods = false;
}

namespace {

// The Remastered gallery (port_gallery.h): the concept art the Remastered import put in the mods, shown one picture at
// a time in a window of its own. Only the current picture is decoded.
bool sGalleryOpen = false;
std::vector<std::string> sGalleryPaths;
int sGalleryIndex = 0;
int sGalleryLoaded = -1;      // the picture sGalleryTexture holds
ImTextureID sGalleryTexture = 0;
ImVec2 sGallerySize{0.0f, 0.0f};
bool sGalleryFailed = false;
// Textures no longer shown, freed once the frames that drew them are done with.
struct RetiredTexture {
  ImTextureID texture;
  int frames;
};
std::vector<RetiredTexture> sGalleryRetired;

void ReleaseGalleryTexture() {
  if (sGalleryTexture != 0) {
    sGalleryRetired.push_back({sGalleryTexture, 0});
    sGalleryTexture = 0;
  }
  sGalleryLoaded = -1;
}

void FreeRetiredGalleryTextures(bool all) {
  for (size_t i = 0; i < sGalleryRetired.size();) {
    // Two frames: the one that drew it may still be on its way to the GPU.
    if (all || ++sGalleryRetired[i].frames > 2) {
      aurora_imgui_remove_texture(sGalleryRetired[i].texture);
      sGalleryRetired.erase(sGalleryRetired.begin() + i);
    } else {
      ++i;
    }
  }
}

void CloseGallery() {
  sGalleryOpen = false;
  // Retired, not freed: this frame may already have drawn it. DrawUI frees it a few frames on.
  ReleaseGalleryTexture();
  sGalleryPaths.clear();
}

void LoadGalleryPicture() {
  ReleaseGalleryTexture();
  sGalleryFailed = true;
  sGalleryLoaded = sGalleryIndex;
  std::ifstream file(PortGci::PathFromString(sGalleryPaths[sGalleryIndex]), std::ios::binary);
  const std::vector<uint8_t> jpeg((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;
  if (!PortGallery::DecodeGalleryJpeg(jpeg.data(), jpeg.size(), width, height, rgba)) {
    return;
  }
  sGalleryTexture = aurora_imgui_add_texture(uint32_t(width), uint32_t(height), rgba.data());
  sGallerySize = ImVec2(float(width), float(height));
  sGalleryFailed = false;
}

void DrawGalleryWindow() {
  FreeRetiredGalleryTextures(false);
  if (!sGalleryOpen) {
    return;
  }
  const int count = int(sGalleryPaths.size());
  const ImVec2 display = ImGui::GetMainViewport()->Size;
  ImGui::SetNextWindowSize(ImVec2(display.x * 0.8f, display.y * 0.8f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  bool open = true;
  if (ImGui::Begin("Gallery##port", &open)) {
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    int step = 0;
    if (ImGui::Button("Previous") || (focused && ImGui::IsKeyPressed(ImGuiKey_LeftArrow))) {
      step = -1;
    }
    ImGui::SameLine();
    if (ImGui::Button("Next") || (focused && ImGui::IsKeyPressed(ImGuiKey_RightArrow))) {
      step = 1;
    }
    ImGui::SameLine();
    ImGui::Text("%d / %d", sGalleryIndex + 1, count);
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
      open = false;
    }
    if (step != 0) {
      sGalleryIndex = (sGalleryIndex + step + count) % count;
    }
    if (sGalleryLoaded != sGalleryIndex) {
      LoadGalleryPicture();
    }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (sGalleryFailed) {
      ImGui::TextDisabled("Cannot read this picture.");
    } else if (avail.x > 1.0f && avail.y > 1.0f) {
      const float scale = std::min(avail.x / sGallerySize.x, avail.y / sGallerySize.y);
      const ImVec2 shown(sGallerySize.x * scale, sGallerySize.y * scale);
      const ImVec2 cursor = ImGui::GetCursorPos();
      ImGui::SetCursorPos(ImVec2(cursor.x + (avail.x - shown.x) * 0.5f, cursor.y + (avail.y - shown.y) * 0.5f));
      // The import writes the TXTR rows as stored, bottom row first, so draw them flipped.
      ImGui::Image(sGalleryTexture, shown, ImVec2(0.f, 1.f), ImVec2(1.f, 0.f));
    }
  }
  ImGui::End();
  if (!open) {
    CloseGallery();
  }
}

// Once per launch, a few seconds, no input taken: a Remastered import from an older importer.
void DrawStaleImportToast() {
  static double sShownAt = -1.0;
  static bool sDone = false;
  if (sDone) {
    return;
  }
  const char* name = PortMods::StaleRemasteredImport();
  if (name == nullptr) {
    return;
  }
  const double now = ImGui::GetTime();
  if (sShownAt < 0.0) {
    sShownAt = now;
  }
  if (now - sShownAt > 12.0) {
    sDone = true;
    return;
  }
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y - 24.f),
                          ImGuiCond_Always, ImVec2(0.5f, 1.f));
  ImGui::SetNextWindowSize(ImVec2(std::min(viewport->Size.x - 32.f, 560.f), 0.f));
  ImGui::SetNextWindowBgAlpha(0.8f);
  if (ImGui::Begin("##stale-import-toast", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::TextWrapped("%s was imported by an older version. Re-import Remastered (F1 > Remastered) to get the latest fixes.",
                       name);
  }
  ImGui::End();
}

// Once per launch, for 12 s: a newer release than this build is out.
// Taps arrive on Android's UI thread (TouchControlsView), so the rect is shared under
// a lock and a hit only sets a flag; the page opens on the game thread.
bool sUpdateToastDone = false;
std::mutex sUpdateToastMutex;
bool sUpdateToastShowing = false;
ImVec4 sUpdateToastRect; // last drawn: x0, y0, x1, y1 as fractions of the window
std::atomic<bool> sUpdateToastTapped{false};

void SetUpdateToastShowing(bool showing, const ImVec4& rect = ImVec4()) {
  std::lock_guard lock(sUpdateToastMutex);
  sUpdateToastShowing = showing;
  sUpdateToastRect = rect;
}

void OpenUpdateToastRelease() {
  SDL_OpenURL(PortUpdateCheck::Latest().url.c_str());
  sUpdateToastDone = true;
  SetUpdateToastShowing(false);
}

void DrawUpdateToast() {
  static double sShownAt = -1.0;
  SetUpdateToastShowing(false);
  if (sUpdateToastDone || PortUpdateCheck::Status() != PortUpdateCheck::kStatus_Available) {
    return;
  }
  if (sUpdateToastTapped.exchange(false)) {
    OpenUpdateToastRelease();
    return;
  }
  const double now = ImGui::GetTime();
  if (sShownAt < 0.0) {
    sShownAt = now;
  }
  if (now - sShownAt > 12.0) {
    sUpdateToastDone = true;
    return;
  }
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + 24.f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.f));
  ImGui::SetNextWindowSize(ImVec2(std::min(viewport->Size.x - 32.f, 560.f), 0.f));
  ImGui::SetNextWindowBgAlpha(0.8f);
  if (ImGui::Begin("##update-toast", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::TextWrapped("Version %s is out (this is %s). Click or tap here to open its release page.",
                       PortUpdateCheck::Latest().version.c_str(), MP_BUILD_VERSION);
    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    if (viewport->Size.x > 0.f && viewport->Size.y > 0.f) {
      SetUpdateToastShowing(true, ImVec4((pos.x - viewport->Pos.x) / viewport->Size.x,
                                         (pos.y - viewport->Pos.y) / viewport->Size.y,
                                         (pos.x + size.x - viewport->Pos.x) / viewport->Size.x,
                                         (pos.y + size.y - viewport->Pos.y) / viewport->Size.y));
    }
    // A mouse click; taps go through PortDebug::TapUpdateToast.
    if (ImGui::IsWindowHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      OpenUpdateToastRelease();
    }
  }
  ImGui::End();
}

// While pipelines compile in the background (the shipped seed on a first start, then new
// shaders as they appear): after half a second, a count and a bar since the toast opened.
void DrawShaderCompilationToast() {
  static double sQueuedSince = -1.0;
  static uint32_t sBase = 0; // createdPipelines when the toast opened
  const AuroraStats* stats = aurora_get_stats();
  if (!sShowShaderCompilation || stats == nullptr || stats->queuedPipelines == 0) {
    sQueuedSince = -1.0;
    return;
  }
  const double now = ImGui::GetTime();
  if (sQueuedSince < 0.0) {
    sQueuedSince = now;
    sBase = stats->createdPipelines;
  }
  if (now - sQueuedSince < 0.5) {
    return;
  }
  const uint32_t done = stats->createdPipelines >= sBase ? stats->createdPipelines - sBase : 0;
  const uint32_t total = done + stats->queuedPipelines;
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  // Top centre: the Android touch overlay has its pause/F1 buttons in the bottom corners.
  ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + 16.f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.f));
  ImGui::SetNextWindowBgAlpha(0.7f);
  // Sized to its text: the font scale differs per platform (Android's is larger).
  if (ImGui::Begin("##shader-compilation-toast", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_AlwaysAutoResize)) {
    char label[64];
    std::snprintf(label, sizeof(label), "Compiling shaders %u / %u", total, total); // widest, so the count doesn't jitter
    const float width = ImGui::CalcTextSize(label).x;
    std::snprintf(label, sizeof(label), "Compiling shaders %u / %u", done, total);
    ImGui::TextUnformatted(label);
    ImGui::ProgressBar(total != 0 ? static_cast<float>(done) / static_cast<float>(total) : 0.f,
                       ImVec2(width, std::max(6.f, ImGui::GetFontSize() * 0.4f)), "");
  }
  ImGui::End();
}

// Once per launch, for longer and in red: the last session ended on a failed disc read.
void DrawDiscReadFailedAlert() {
  static double sShownAt = -1.0;
  if (!sDiscReadFailedLastSession) {
    return;
  }
  const double now = ImGui::GetTime();
  if (sShownAt < 0.0) {
    sShownAt = now;
  }
  if (now - sShownAt > 20.0) {
    sDiscReadFailedLastSession = false;
    return;
  }
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + 24.f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.f));
  ImGui::SetNextWindowSize(ImVec2(std::min(viewport->Size.x - 32.f, 640.f), 0.f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.6f, 0.05f, 0.05f, 0.92f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 0.35f, 0.35f, 1.f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.f, 10.f));
  if (ImGui::Begin("##disc-read-failed-alert", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::SetWindowFontScale(1.4f);
    ImGui::TextWrapped("The last session crashed: the disc image is damaged.");
    ImGui::SetWindowFontScale(1.f);
    ImGui::TextWrapped("Part of it couldn't be read. If the game stops again, copy the image again or check it in "
                       "Dolphin (Properties > Verify).");
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(3);
}

void DrawRemasteredImport() {
  static char sImage[1024] = "";
  static char sKeys[1024] = "";
#if defined(__ANDROID__)
  static std::string sPickNames[2];
#endif
  static bool sFilled = false;
  const auto remember = [](int which, const std::string& path) {
    std::string& saved = which == 0 ? sRemasteredImagePath : sRemasteredKeysPath;
    if (!path.empty() && saved != path) {
      saved = path;
      MarkDirty();
    }
  };
  if (!sFilled) {
    sFilled = true;
    std::snprintf(sKeys, sizeof(sKeys), "%s", PortRemastered::DefaultKeysPath().c_str());
#if defined(__ANDROID__)
    // The files picked last time open again as if picked now.
    std::lock_guard lock(sRemasteredPickMutex);
    if (!sRemasteredImagePath.empty()) {
      sRemasteredPicks.emplace_back(0, sRemasteredImagePath);
    }
    if (!sRemasteredKeysPath.empty()) {
      sRemasteredPicks.emplace_back(1, sRemasteredKeysPath);
    }
    // A pick the previous process never saw: after the remembered ones, so it wins.
    TakeRemasteredPickFiles();
#else
    if (!sRemasteredImagePath.empty()) {
      std::snprintf(sImage, sizeof(sImage), "%s", sRemasteredImagePath.c_str());
    }
    if (!sRemasteredKeysPath.empty()) {
      std::snprintf(sKeys, sizeof(sKeys), "%s", sRemasteredKeysPath.c_str());
    }
#endif
  }
  {
    std::lock_guard lock(sRemasteredPickMutex);
#if defined(__ANDROID__)
    for (const auto& [which, path] : sRemasteredPicks) {
      const std::string opened = OpenRemasteredPick(which, path);
      std::snprintf(which == 0 ? sImage : sKeys, sizeof(sImage), "%s", opened.c_str());
      sPickNames[which] = opened.empty() ? "could not be opened" : RemasteredPickName(path);
      if (!opened.empty()) {
        remember(which, path);
      }
      std::remove(RemasteredPickFile(which).c_str());
    }
#else
    for (const auto& [which, path] : sRemasteredPicks) {
      std::snprintf(which == 0 ? sImage : sKeys, sizeof(sImage), "%s", path.c_str());
      remember(which, path);
    }
#endif
    sRemasteredPicks.clear();
  }
  if (!ImGui::CollapsingHeader("Metroid Prime Remastered models")) {
    return;
  }
  const PortRemastered::ImportState state = PortRemastered::ImportStatus();
  ImGui::TextWrapped("Converts the models of your own copy of Metroid Prime Remastered into a mod. It needs the "
                     "game's .nsp or .xci and your console's key file (prod.keys), and takes a few minutes.");
  if (PortMods::StaleRemasteredImport() != nullptr) {
    ImGui::TextColored(ThemeWarnColor(),
                       "Re-import needed: the installed models were made by an older version.");
  }
  ImGui::PushStyleColor(ImGuiCol_Text, ThemeWarnColor(ImVec4(1.f, 0.75f, 0.3f, 1.f)));
  ImGui::TextWrapped("Very experimental and currently unsupported: expect wrong or missing models, crashes and "
                     "heavy memory use. Remove mods/remastered-models to get the retail game back.");
  ImGui::PopStyleColor();
  ImGui::BeginDisabled(state.running);
#if defined(__ANDROID__)
  // No path to type here: the files are picked, and shown by name.
  if (ImGui::Button("Pick the .nsp/.xci...##remastered-image")) {
    OpenRemasteredDialog(0);
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(sPickNames[0].empty() ? "Metroid Prime Remastered .nsp or .xci" : sPickNames[0].c_str());
  if (ImGui::Button("Pick the keys...##remastered-keys")) {
    OpenRemasteredDialog(1);
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(sPickNames[1].empty() ? "prod.keys" : sPickNames[1].c_str());
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("Android may close the game while you pick a file. The pick is kept: open this page again.");
  ImGui::PopStyleColor();
#else
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
  ImGui::InputTextWithHint("##remastered-image", "Metroid Prime Remastered .nsp or .xci", sImage, sizeof(sImage));
  ImGui::SameLine();
  if (ImGui::Button("Browse...##remastered-image")) {
    OpenRemasteredDialog(0);
  }
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
  ImGui::InputTextWithHint("##remastered-keys", "prod.keys", sKeys, sizeof(sKeys));
  ImGui::SameLine();
  if (ImGui::Button("Browse...##remastered-keys")) {
    OpenRemasteredDialog(1);
  }
#endif
  if (ImGui::Checkbox("Room geometry too##remastered", &sImportGeometry)) {
    MarkDirty();
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Also converts the rooms themselves, not only the models in them. About 6.5 GB in place of "
                      "1 GB, and twice as long.");
  }
#if defined(__ANDROID__)
  // A tap shows no tooltip, so the warning is spelled out.
  if (sImportGeometry) {
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeWarnColor(ImVec4(1.f, 0.75f, 0.3f, 1.f)));
    ImGui::TextWrapped("Untested on phones: needs about 6.5 GB free and lots of RAM, and the game may run slowly "
                       "or be closed by Android. Remove mods/remastered-models to go back.");
    ImGui::PopStyleColor();
  }
#endif
  PortRemastered::SetImportGeometry(sImportGeometry);
  if (ImGui::Checkbox("Particle effects (experimental)##remastered", &sImportEffects)) {
    MarkDirty();
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Also replaces the disc's particle effects with Remastered's where the two match. Some "
                      "effects may look wrong. Reload the mods afterwards, as for any import.");
  }
  PortRemastered::SetImportEffects(sImportEffects);
  static bool sReconvert = false;
  ImGui::Checkbox("Reconvert everything##remastered", &sReconvert);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("A re-import normally keeps what the last import made where nothing it depends on has "
                      "changed. This makes everything anew, as the first import did.");
  }
  PortRemastered::SetImportReuse(!sReconvert);
  ImGui::EndDisabled();
  if (state.running) {
    ImGui::ProgressBar(state.total > 0 ? float(state.done) / float(state.total) : 0.f, ImVec2(-1.f, 0.f),
                       state.message.c_str());
    if (sImportUnloadedMods) {
      ImGui::TextDisabled("The mods are unloaded until the import ends.");
      DrawModReloadMessage();
    }
    if (ImGui::Button("Cancel##remastered")) {
      PortRemastered::CancelImport();
    }
  } else {
    ImGui::BeginDisabled(sImage[0] == '\0' || sKeys[0] == '\0');
#if !defined(__ANDROID__)
    // A typed path is kept once it is used (Android keeps what was picked).
    const auto rememberTyped = [&] {
      remember(0, sImage);
      remember(1, sKeys);
    };
#else
    const auto rememberTyped = [] {};
#endif
    if (ImGui::Button("Import##remastered")) {
      rememberTyped();
      StartRemasteredImport(sImage, sKeys);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("The mods are unloaded while it runs, and loaded again with the new import when it ends.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Import movies##remastered")) {
      rememberTyped();
      PortRemastered::StartMovieImport(sImage, sKeys);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
#if defined(__ANDROID__)
      ImGui::SetTooltip("Only the menu movies and the gallery, added to the mod already imported.");
#else
      ImGui::SetTooltip("Only the menu movies (needs ffmpeg) and the gallery, added to the mod already imported.");
#endif
    }
    ImGui::EndDisabled();
    if (state.finished && state.ok) {
      ImGui::PushStyleColor(ImGuiCol_Text, ThemeGoodColor(ImVec4(0.5f, 1.f, 0.5f, 1.f)));
      ImGui::TextWrapped("%s", state.message.c_str());
      ImGui::PopStyleColor();
      // It loads on its own when the import ends; this is to load it again
      // by hand.
      if (ImGui::Button("Load it again##remastered")) {
        PortSaveState::RequestModReload();
      }
      DrawModReloadMessage();
    } else if (state.finished && state.cancelled) {
      ImGui::TextDisabled("The import was cancelled.");
    } else if (state.finished) {
      ImGui::PushStyleColor(ImGuiCol_Text, ThemeBadColor());
      ImGui::TextWrapped("The import failed: %s", state.message.c_str());
      ImGui::PopStyleColor();
    }
  }
  if (!state.lines.empty() && ImGui::TreeNode("remastered-log", "%d notes", int(state.lines.size()))) {
    for (const std::string& line : state.lines) {
      ImGui::TextWrapped("%s", line.c_str());
    }
    ImGui::TreePop();
  }
}

#if !defined(__ANDROID__)
// Importers (port_importers.h): the user's own programs that build a mod.
// Nothing is drawn until the importers folder holds one.
void DrawImporters() {
  // The folder is read when the panel opens and after a run, not every frame.
  static std::vector<std::string> sNames;
  static int sListedFrame = -2;
  static char sArgument[512] = "";
  const int frame = ImGui::GetFrameCount();
  const PortImporters::State& state = PortImporters::Poll();
  if (sListedFrame != frame - 1 || ImGui::IsWindowAppearing()) {
    sNames = PortImporters::List();
  }
  sListedFrame = frame;
  if (sNames.empty() && !state.running && !state.finished) {
    return;
  }
  ImGui::SeparatorText("Importers");
  ImGui::BeginDisabled(state.running);
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
  ImGui::InputTextWithHint("Argument", "optional, e.g. the file to import", sArgument, sizeof(sArgument));
  ItemHelp("An importer is a program in the importers folder beside the mods folder; it builds a mod "
           "from files of your own. The argument is passed to it.");
  for (const std::string& name : sNames) {
    if (ImGui::Button(("Run " + name).c_str())) {
      PortImporters::Start(name, sArgument);
    }
  }
  ImGui::EndDisabled();
  if (state.running) {
    ImGui::TextColored(ThemeWarnColor(), "%s is running...", state.name.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      PortImporters::Cancel();
    }
  } else if (state.finished && state.exitCode == 0) {
    ImGui::TextColored(ThemeGoodColor(ImVec4(0.5f, 1.f, 0.5f, 1.f)), "%s finished. Reload the mods to load it.",
                       state.name.c_str());
  } else if (state.finished && state.cancelled) {
    ImGui::TextDisabled("%s was cancelled.", state.name.c_str());
  } else if (state.finished) {
    ImGui::TextColored(ThemeBadColor(), "%s failed (exit code %d).", state.name.c_str(), state.exitCode);
  }
  if (!state.lines.empty()) {
    // The tail of the output; a failure shows more of it.
    const size_t shown = std::min<size_t>(state.lines.size(), state.finished && state.exitCode != 0 && !state.cancelled ? 12 : 3);
    for (size_t i = state.lines.size() - shown; i < state.lines.size(); ++i) {
      ImGui::TextWrapped("%s", state.lines[i].c_str());
    }
  }
}
#endif

void DrawMods() {
  ImGui::SeparatorText("Mods");
  const PortMods::Status& status = PortMods::CurrentStatus();
  if (sOriginalExperience) {
    ImGui::TextDisabled("Original experience is on: no mods are loaded.");
  }
  bool enabled = sModsEnabled;
  if (ImGui::Checkbox("Load mods", &enabled)) {
    SetModsEnabled(enabled);
  }
  ItemHelp("Each folder in the mods folder is a mod; later names win. A file at a disc path "
           "(Metroid1.pak, Audio/..., Video/...) replaces that file, and a resource named by id and "
           "type (1A2B3C4D.TXTR) replaces it in every PAK. Mods load at startup.");
  // What the settings would load next time, against what this launch loaded.
  std::vector<std::string> disabled = PortMods::SplitDisabled(sModsDisabled);
  bool changed = ModsEnabled() != status.active;
  if (status.mods.empty()) {
    ImGui::TextDisabled("No mods in the folder.");
  }
  for (const PortMods::ModInfo& mod : status.mods) {
    const auto found = std::find(disabled.begin(), disabled.end(), mod.name);
    bool on = found == disabled.end();
    ImGui::BeginDisabled(!sModsEnabled);
    if (ImGui::Checkbox(mod.name.c_str(), &on)) {
      if (on) {
        disabled.erase(std::remove(disabled.begin(), disabled.end(), mod.name), disabled.end());
      } else {
        disabled.push_back(mod.name);
      }
      SetModsDisabled(PortMods::JoinDisabled(disabled));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (mod.enabled) {
      if (mod.textures > 0) {
        ImGui::TextDisabled("%d file(s), %d resource(s), %d native texture(s)", mod.files, mod.resources, mod.textures);
      } else {
        ImGui::TextDisabled("%d file(s), %d resource(s)", mod.files, mod.resources);
      }
    } else {
      ImGui::TextDisabled("not loaded");
    }
    if (mod.importStale) {
      ImGui::SameLine();
      ImGui::TextColored(ThemeWarnColor(), "re-import needed");
    }
    changed = changed || (ModsEnabled() && on) != mod.enabled;
  }
  if (PortMods::Suspended()) {
    ImGui::TextColored(ThemeWarnColor(), "Unloaded while the Remastered import runs; they load again when it ends.");
  } else if (changed) {
    ImGui::TextColored(ThemeWarnColor(), "Reload the mods to apply.");
  }
  if (ImGui::Button("Reload mods")) {
    PortSaveState::RequestModReload();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Reads the mods folder again and reloads the room, as a save state does.");
  }
#if !defined(__ANDROID__)
  if (!status.folder.empty()) {
    ImGui::SameLine();
    if (ImGui::Button("Open mods folder")) {
      std::string url = status.folder.front() == '/' ? "file://" : "file:///";
      for (const char c : status.folder) {
        url += c == ' ' ? std::string("%20") : std::string(1, c == '\\' ? '/' : c);
      }
      SDL_OpenURL(url.c_str());
    }
  }
#endif
  DrawModReloadMessage();
  for (const std::string& message : status.messages) {
    ImGui::TextColored(ThemeBadColor(), "%s", message.c_str());
  }
  if (PortMods::NativeTextureCount() > 0) {
    ImGui::TextDisabled("Native textures: %zu, %zu in use", PortMods::NativeTextureCount(), PortMods::NativeTexturesBound());
  }
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("Folder: %s", status.folder.c_str());
  ImGui::PopStyleColor();
#if !defined(__ANDROID__)
  DrawImporters();
#endif
}
} // namespace

bool UpdateToastShowing() {
  std::lock_guard lock(sUpdateToastMutex);
  return sUpdateToastShowing;
}

bool TapUpdateToast(float x, float y) {
  std::lock_guard lock(sUpdateToastMutex);
  const ImVec4& r = sUpdateToastRect;
  if (!sUpdateToastShowing || x < r.x || x > r.z || y < r.y || y > r.w) {
    return false;
  }
  sUpdateToastTapped.store(true);
  return true;
}

void DrawGameSection();

void DrawCutscenesSection() {
  ImGui::SeparatorText("Cutscenes");
  const bool locked = BeginOriginalLocked();
  bool skippable = sSkippableCutscenes;
  if (ImGui::Checkbox("Skippable cutscenes", &skippable)) {
    SetSkippableCutscenes(skippable);
  }
  ItemHelp("Every cutscene can be skipped with Start or A, including the ones the game never "
           "lets you skip (randomprime's room patches). Applies to rooms loaded after the change.");
  if (PortSkipCutscenes::Forced()) {
    SameLineAfterHelp();
    ImGui::TextDisabled("(on in randomized games)");
  }
  int elevatorRide = sElevatorRide;
  if (ImGui::Combo("Elevator ride", &elevatorRide, "Original\0Fast\0Skip\0")) {
    SetElevatorRide(static_cast< EElevatorRide >(elevatorRide));
  }
  ItemHelp("The ride shown between worlds. Original lasts at least 5 s even though the port loads "
           "far faster; Fast ends it about 2 s in, once the next area is loaded; Skip shows black "
           "until the area is loaded. The cinematic played in the elevator room before the ride is "
           "not affected.");
  EndOriginalLocked(locked);

}

void DrawUnlocksSection() {
  ImGui::SeparatorText("Unlocks");
  const bool locked = BeginOriginalLocked();
  bool hardMode = sUnlockHardMode;
  if (ImGui::Checkbox("Hard mode", &hardMode)) {
    SetUnlockHardMode(hardMode);
  }
  ImGui::SetItemTooltip("Offered when starting a file, as after finishing the game.");
  ImGui::SameLine();
  bool fusionSuit = sUnlockFusionSuit;
  if (ImGui::Checkbox("Fusion Suit", &fusionSuit)) {
    SetUnlockFusionSuit(fusionSuit);
    // The suit choice itself is saved; without the unlock (or a real link)
    // there is no menu left to switch it back off, so drop it here.
    if (!fusionSuit && gpGameState != nullptr &&
        !(gpGameState->SystemState().GetFusionLinked() &&
          gpGameState->SystemState().GetNormalModeBeat())) {
      gpGameState->SystemState().SetHasFusion(false);
      gpGameState->PlayerState()->SetIsFusionEnabled(false);
    }
  }
  ImGui::SetItemTooltip("Under Fusion Bonus; retail needs a GBA link to Metroid Fusion.");
  ImGui::SameLine();
  bool galleries = sUnlockGalleries;
  if (ImGui::Checkbox("Image galleries", &galleries)) {
    SetUnlockGalleries(galleries);
  }
  ImGui::SetItemTooltip("All four of them.");
  ImGui::TextDisabled("What finishing the game unlocks. Not written into the save.");
  ItemHelp("Turning an option off locks it again. Metroid (NES) stays locked: its emulator can't run "
           "in the port.");
  EndOriginalLocked(locked);

}

void DrawSpeedrunSection() {
  ImGui::SeparatorText("Speedrun");
  bool timer = sSpeedrunTimer;
  if (ImGui::Checkbox("On-screen in-game time", &timer)) {
    SetSpeedrunTimer(timer);
  }
  ItemHelp("The play time the save file shows: it stops in cutscenes, menus and loads.");
  bool liveSplit = sLiveSplit;
  if (ImGui::Checkbox("LiveSplit", &liveSplit)) {
    SetLiveSplit(liveSplit);
  }
  ItemHelp("In LiveSplit: right-click, Control > Start TCP Server (port 16834), and compare against "
           "Game Time. A new file resets and starts the timer, the game time follows the in-game "
           "time, and it splits on the final blow.");
  SameLineAfterHelp();
  switch (PortLiveSplit::Status()) {
  case PortLiveSplit::kStatus_Off:
    ImGui::TextDisabled("off");
    break;
  case PortLiveSplit::kStatus_Connecting:
    ImGui::TextUnformatted("connecting...");
    break;
  case PortLiveSplit::kStatus_Connected:
    ImGui::TextColored(ThemeGoodColor(), "connected");
    break;
  case PortLiveSplit::kStatus_Failed:
    ImGui::TextColored(ThemeBadColor(), "%s", PortLiveSplit::LastError().c_str());
    break;
  }
  // Applied when the field loses focus; until then the text is left alone.
  static char address[128];
  static bool editingAddress = false;
  if (!editingAddress) {
    std::snprintf(address, sizeof(address), "%s", sLiveSplitAddress.c_str());
  }
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.f);
  ImGui::InputText("Server (host:port)", address, sizeof(address));
  editingAddress = ImGui::IsItemActive();
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    SetLiveSplitAddress(address);
  }
  ImGui::SameLine();
  bool splitUpgrades = sLiveSplitSplitUpgrades;
  if (ImGui::Checkbox("Split on upgrades", &splitUpgrades)) {
    sLiveSplitSplitUpgrades = splitUpgrades;
    ApplyLiveSplit();
    MarkDirty();
  }
  ImGui::SetItemTooltip("Also splits on each new upgrade or artifact (not expansions or energy tanks).");

}

void DrawDiscordSection() {
  if (PortDiscord::Supported()) {
    ImGui::SeparatorText("Discord");
    bool discord = sDiscord;
    if (ImGui::Checkbox("Rich Presence", &discord)) {
      SetDiscordPresence(discord);
    }
    ImGui::SetItemTooltip("Shows the room, world, energy, missiles and item percentage on your "
                          "Discord profile while the Discord app runs.");
    ImGui::SameLine();
    switch (PortDiscord::Status()) {
    case PortDiscord::kStatus_Off:
      ImGui::TextDisabled("off");
      break;
    case PortDiscord::kStatus_Connecting:
      ImGui::TextUnformatted("connecting...");
      break;
    case PortDiscord::kStatus_Connected:
      ImGui::TextColored(ThemeGoodColor(), "connected");
      break;
    case PortDiscord::kStatus_Failed:
      ImGui::TextColored(ThemeBadColor(), "%s", PortDiscord::LastError().c_str());
      break;
    }
    if (sDiscord) {
      ImGui::TextDisabled("Showing: %s", PortDiscord::CurrentText().c_str());
    }
  }

}

void DrawUpdateSection() {
  ImGui::SeparatorText("Updates");
  bool check = sUpdateCheck;
  if (ImGui::Checkbox("Check for updates", &check)) {
    sUpdateCheck = check;
    ApplyUpdateCheck();
    PortUpdateCheck::CheckNow(); // switched on: check now, not next launch
    MarkDirty();
  }
  ImGui::SetItemTooltip("Asks GitHub at launch whether a newer release is out. Nothing else is sent.");
  ImGui::SameLine();
  const PortUpdateCheck::EStatus status = PortUpdateCheck::Status();
  switch (status) {
  case PortUpdateCheck::kStatus_Off:
    ImGui::TextDisabled(sUpdateCheck ? "off (MP_UPDATE_CHECK=0)" : "off");
    break;
  case PortUpdateCheck::kStatus_Checking:
    ImGui::TextUnformatted("checking...");
    break;
  case PortUpdateCheck::kStatus_UpToDate:
    ImGui::TextColored(ThemeGoodColor(), "up to date");
    break;
  case PortUpdateCheck::kStatus_Available:
    ImGui::TextColored(ThemeGoodColor(), "%s is out", PortUpdateCheck::Latest().version.c_str());
    break;
  case PortUpdateCheck::kStatus_Failed:
    ImGui::TextColored(ThemeBadColor(), "%s", PortUpdateCheck::LastError().c_str());
    break;
  }
  if (status == PortUpdateCheck::kStatus_Off) {
    return;
  }
  ImGui::BeginDisabled(status == PortUpdateCheck::kStatus_Checking);
  if (ImGui::Button("Check now")) {
    PortUpdateCheck::CheckNow();
  }
  ImGui::EndDisabled();
  if (status == PortUpdateCheck::kStatus_Available) {
    ImGui::SameLine();
    if (ImGui::Button("Open release page")) {
      SDL_OpenURL(PortUpdateCheck::Latest().url.c_str());
    }
  }
  if (const int64_t checked = PortUpdateCheck::LastChecked(); checked > 0) {
    const time_t when = static_cast< time_t >(checked);
    char text[64] = {};
    if (const std::tm* local = std::localtime(&when)) {
      std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", local);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("last checked %s", text);
  }
}

#if defined(__ANDROID__)
// The folder picker's progress, set from the Java copy thread.
std::mutex sTexturePackStatusMutex;
std::string sTexturePackStatus;

std::string TexturePackStatus() {
  std::lock_guard lock(sTexturePackStatusMutex);
  return sTexturePackStatus;
}

void SetTexturePackStatus(std::string status) {
  std::lock_guard lock(sTexturePackStatusMutex);
  sTexturePackStatus = std::move(status);
}

// MetroidPrimeActivity.pickTexturePack opens the system folder picker and
// copies the chosen folder in the background, reporting back through the
// nativeTexturePack* functions below.
void PickTexturePack() {
  JNIEnv* env = static_cast< JNIEnv* >(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast< jobject >(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    SetTexturePackStatus("Could not open the folder picker.");
    return;
  }
  jclass cls = env->GetObjectClass(activity);
  jmethodID method = env->GetMethodID(cls, "pickTexturePack", "(Ljava/lang/String;)V");
  if (method != nullptr) {
    // The pack is copied into the data folder, wherever that is.
    jstring folder = env->NewStringUTF(PortPaths::UserFolder().c_str());
    env->CallVoidMethod(activity, method, folder);
    env->DeleteLocalRef(folder);
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    SetTexturePackStatus("Could not open the folder picker.");
  }
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(activity);
}
#endif

void DrawTexturePack();

// The look of this overlay: the floating window, the theme and the menu sounds.
void DrawOverlaySection() {
  ImGui::SeparatorText("Overlay");
#if !defined(__ANDROID__)
  if (ImGui::Checkbox("Overlay as a floating window", &sOverlayWindowed)) {
    MarkDirty();
  }
#endif
  {
    int theme = sUiTheme;
    if (ImGui::Combo("Menu theme", &theme, "Auto\0Metroid Prime\0Remastered\0Plain\0")) {
      sUiTheme = theme;
      MarkDirty();
    }
    ItemHelp("The look of this overlay. Auto uses the Remastered style while the Remastered import is "
             "loaded and the Metroid Prime pause-screen style otherwise; Plain is stock Dear ImGui.");
    if (ImGui::Checkbox("Menu sounds", &sUiSounds)) {
      MarkDirty();
    }
    ItemHelp("The game's own menu sounds for opening, choosing and moving through the overlay. Off "
             "with the Plain theme.");
  }
}

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
  ImGui::SetItemTooltip("A small progress bar while shaders compile in the background (mostly the first start\n"
                        "after an install or update). Draws whose shader isn't ready yet are skipped.");
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
    ImGui::SetItemTooltip("Lowers the EFB scale, down to the lowest scale below, while the frame rate\n"
                          "is below the target, and raises it back to the scale above when there is\n"
                          "room. Each change stutters for a frame, so it changes at most once every\n"
                          "couple of seconds.");
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
                         live == BACKEND_VULKAN ? "Vulkan" : "another API");
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
    const bool pending = aurora_gpu_selftest_pending();
    ImGui::BeginDisabled(pending);
    if (ImGui::Button("GPU self-test")) {
      RequestGpuSelfTest();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Renders known patterns offscreen through the game's own GX path, reads them back\n"
                          "and logs one PASS or FAIL line per feature (indexed vertices, TEV, textures, blend,\n"
                          "depth, EFB copy). For GPU driver bugs such as a black world; attach the log to a report.");
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

void DrawControlsOptions() {
  ImGui::SeparatorText("Buttons");
  const bool locked = BeginOriginalLocked();
  bool lockOnToggle = sLockOnToggle;
  if (ImGui::Checkbox("Toggle Lock-On", &lockOnToggle)) {
    SetLockOnToggle(lockOnToggle);
  }
  ItemHelp("Press L once to lock on, scan, strafe or grapple, and again to let go. The lock also "
           "lets go by itself when its target is gone.");
  bool stickyCharge = sStickyCharge;
  if (ImGui::Checkbox("Sticky Charge", &stickyCharge)) {
    SetStickyCharge(stickyCharge);
  }
  ItemHelp("Taps fire as usual. Hold fire for a moment and let go, and the beam keeps charging; press "
           "fire again to shoot. Needs the Charge Beam.");
  bool rapidCharge = sRapidCharge;
  if (ImGui::Checkbox("Remastered charge (rapid fire)", &rapidCharge)) {
    SetRapidCharge(rapidCharge);
  }
  ItemHelp("As in Metroid Prime Remastered: holding fire fires a few shots before charging (3 in all "
           "with Power, 2 with Wave or Plasma, 1 with Ice), then charges faster, so a full charge takes about as "
           "long as before. Without the Charge Beam it fires the same shots, then waits for you to let go.");
  bool swapScanXray = sSwapScanXray;
  if (ImGui::Checkbox("Swap the Scan and X-Ray visor buttons", &swapScanXray)) {
    SetSwapScanXray(swapScanXray);
  }
  ImGui::SetItemTooltip("Each takes the other's D-pad direction, as in Metroid Prime\n"
                        "Remastered's Dual Sticks layout. The Remastered controller preset\n"
                        "turns it on and the other presets off.");

  ImGui::SeparatorText("Morph ball");
  bool fastMorph = sFastMorph;
  if (ImGui::Checkbox("Fast Morph", &fastMorph)) {
    SetFastMorph(fastMorph);
  }
  ItemHelp("Morphing and unmorphing take a fraction of a second and keep your momentum, as in "
           "Metroid Prime 4. Unmorphing on the ground caps speed at walking speed; in the air the "
           "whole jump arc carries over.");
  const int springRule = PortAp::SpringBallRule();
  ImGui::BeginDisabled(springRule >= 0);
  bool springBall = sSpringBall;
  if (ImGui::Checkbox("Spring Ball (C-stick up)", &springBall)) {
    SetSpringBall(springBall);
  }
  ImGui::EndDisabled();
  if (springRule >= 0) {
    ImGui::SameLine();
    ImGui::TextDisabled("(set by the Archipelago seed: %s)", springRule == 0   ? "not received yet"
                                                             : springRule == 1 ? "with the Bombs"
                                                                               : "unlocked");
  } else {
    ItemHelp("A small jump in morph ball, as in Metroid Prime Trilogy, once the Morph Ball Bombs are "
             "held. Twin stick still passes the right stick up to it, and the beam shift (X in the "
             "Remastered preset) springs too.");
  }
  EndOriginalLocked(locked);
}

void DrawControlsKeyboardMouse() {
  ImGui::SeparatorText("Mouse aim");
  bool mouseAim = sMouseAim;
  if (ImGui::Checkbox("Mouse aim", &mouseAim)) {
    SetMouseAim(mouseAim);
    MarkDirty();
  }
  if (ImGui::SliderFloat("Sensitivity", &sMouseSensitivity, 0.0005f, 0.02f, "%.4f rad/px",
                         ImGuiSliderFlags_Logarithmic)) {
    MarkDirty();
  }
  if (ImGui::Checkbox("Invert X", &sMouseInvertX)) {
    MarkDirty();
  }
  ImGui::SameLine();
  if (ImGui::Checkbox("Invert Y", &sMouseInvertY)) {
    MarkDirty();
  }
  ImGui::SameLine();
  if (ImGui::Checkbox("Weapon buttons", &sMouseButtons)) {
    sMouseButtonGate.Reset();
    MarkDirty();
  }
  ImGui::SetItemTooltip("Mouse buttons are set in Controls > Keyboard & mouse. Existing "
                        "keyboard/controller weapon bindings also work.");
  if (ImGui::Checkbox("Crosshair", &sMouseCrosshair)) {
    MarkDirty();
  }
  ImGui::SetItemTooltip("A crosshair at the aim point while mouse aiming.");
  ImGui::SameLine();
  int crosshairSize = sCrosshairSize;
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.f);
  if (ImGui::SliderInt("Size", &crosshairSize, kCrosshairSizeMin, kCrosshairSizeMax, "%d%%")) {
    SetCrosshairSize(crosshairSize);
  }
  ImGui::SetItemTooltip("Applies under mouse aim and twin stick.");

  PortControls::DrawKeyboardMouse();
}

void DrawControlsController() {
  ImGui::SeparatorText("Stick aim");
  bool twinStick = sTwinStick;
  if (ImGui::Checkbox("Twin stick (right stick aims)", &twinStick)) {
    SetTwinStick(twinStick);
    MarkDirty();
  }
  ItemHelp("Twin stick uses the right stick as a direct camera aim (the same path as the mouse) and "
           "consumes it, so it no longer free-looks. Fire stays on whatever is bound to A; remap it "
           "in Controls > Controller.");
  ImGui::BeginDisabled(!sTwinStick);
  float stickRate = sStickAimRate;
  if (ImGui::SliderFloat("Stick aim speed", &stickRate, 100.f, 3000.f, "%.0f px/s",
                         ImGuiSliderFlags_Logarithmic)) {
    SetStickAimRate(stickRate);
  }
  // The game's own option, saved with its settings; free look uses it too.
  ImGui::BeginDisabled(gpGameState == nullptr);
  bool invertY = gpGameState != nullptr && gpGameState->GameOptions().GetInvertYAxis();
  if (ImGui::Checkbox("Invert stick aim Y (the game's Reverse Y Axis)", &invertY)) {
    gpGameState->GameOptions().SetInvertYAxis(invertY);
  }
  ImGui::EndDisabled();
  ImGui::EndDisabled();
  PortControls::DrawController();
}

void DrawControlsTouchGyro() {
#if defined(__ANDROID__)
  ImGui::SeparatorText("Touch controls");
  if (ImGui::Checkbox("Coloured buttons", &sTouchColors)) {
    MarkDirty();
  }
  ItemHelp("Draws the on-screen buttons in the GameCube pad's colours: green A, red B, yellow "
           "C-stick, purple Z. Off, they are plain and see-through.");
  if (ImGui::Checkbox("Button descriptions", &sTouchLabels)) {
    MarkDirty();
  }
  ItemHelp("Writes what each on-screen button does next to its letter (Fire, Jump, Lock...). "
           "Off, only the letters are shown.");
  ImGui::BeginDisabled(sOriginalExperience);
  if (ImGui::Checkbox("Turbo fire button", &sTouchTurbo)) {
    if (!sTouchTurbo) {
      sTouchTurboFire.store(false, std::memory_order_release);
    }
    MarkDirty();
  }
  ImGui::EndDisabled();
  ItemHelp("Adds a Turbo button next to Fire: holding it fires as if Fire were tapped as fast as the "
           "game accepts. It can be moved and resized in Edit layout.");
  if (ImGui::Checkbox("Floating left stick", &sTouchFloatingStick)) {
    MarkDirty();
  }
  ItemHelp("Hides the left stick until a finger touches a free spot on the left half of the screen, "
           "then centres it under that finger. Another finger on the left half aims, like the rest "
           "of the free area. The map screen keeps the fixed stick.");
  int touchLayout = sTouchClassic ? 1 : sTouchTwinStick ? 2 : 0;
  static const char* const kTouchLayouts[] = {"Default", "Classic GameCube", "Twin stick (Remastered)"};
  if (ImGui::Combo("Layout", &touchLayout, kTouchLayouts, 3)) {
    SetTouchClassic(touchLayout == 1);
    SetTouchTwinStick(touchLayout == 2);
    if (touchLayout == 2) {
      // Remastered picks beams with Y + the D-pad, so twin starts with the D-pad, not the wheels.
      SetTouchWheels(false);
    }
  }
  ItemHelp("Default: the GameCube pad without a C-stick; drag the free screen area to aim like a "
           "mouse (the left stick strafes), and beams and visors come from the Visor and Beam "
           "wheels. Classic GameCube: brings back the C-stick and the D-pad. Twin stick "
           "(Remastered): a right stick that aims, with Remastered's Dual Sticks buttons (Jump, "
           "Fire, Morph, Missile, LT Lock) and a D-pad for visors; hold Y (Beam) and press the D-pad to pick "
           "a beam. The free-area drag stays on; the wheels can replace the D-pad.");
  ImGui::BeginDisabled(!sTouchClassic);
  bool touchAim = sTouchAim;
  if (ImGui::Checkbox("Touch aim", &touchAim)) {
    SetTouchAim(touchAim);
  }
  ItemHelp("Drag a finger on the free screen area. Classic layout with twin stick off: dragging "
           "sideways turns Samus, dragging up or down looks up or down like R free look and "
           "levels out when you let go. Otherwise the view aims like a mouse, by the distance "
           "dragged. Not while locked on or in the ball.");
  ImGui::EndDisabled();
  bool touchMapTap = sTouchMapTap;
  if (ImGui::Checkbox("Tap minimap for map", &touchMapTap)) {
    SetTouchMapTap(touchMapTap);
  }
  ItemHelp("Tapping the minimap opens the map; hides the GameCube layout's Z button.");
  ImGui::BeginDisabled(!sTouchClassic && !sTouchTwinStick);
  bool touchWheels = sTouchWheels;
  if (ImGui::Checkbox("Beam and visor wheels", &touchWheels)) {
    SetTouchWheels(touchWheels);
  }
  ItemHelp("Replaces the D-pad with a Visor and a Beam button. Hold one, slide to a sector, "
           "let go to pick. Letting go in the middle cancels. Off, the D-pad is back.");
  ImGui::EndDisabled();
  ImGui::BeginDisabled(!((!sTouchClassic && !sTouchTwinStick) || sTouchWheels));
  bool touchVisorTapScan = sTouchVisorTapScan;
  if (ImGui::Checkbox("Tap Visor for Scan Visor", &touchVisorTapScan)) {
    SetTouchVisorTapScan(touchVisorTapScan);
  }
  ItemHelp("A quick tap on the Visor button (no slide) selects the Scan Visor.");
  ImGui::EndDisabled();
  ImGui::BeginDisabled(!(!sTouchClassic || sTouchAim));
  float touchAimSpeed = sTouchAimSpeed;
  if (ImGui::SliderFloat("Touch aim speed", &touchAimSpeed, 0.5f, 6.f, "%.2f px/dp",
                         ImGuiSliderFlags_Logarithmic)) {
    SetTouchAimSpeed(touchAimSpeed);
  }
  ItemHelp("How far the view turns per dp of finger travel. The default turns about 180 degrees "
           "over a 400 dp drag at the default mouse sensitivity.");
  ImGui::EndDisabled();
  float sideMargin = sTouchSideMargin.load();
  if (ImGui::SliderFloat("Side margin", &sideMargin, 0.f, kTouchMarginMaxDp, "%.0f dp")) {
    sTouchSideMargin.store(sideMargin);
    MarkDirty();
  }
  ItemHelp("Moves every on-screen control in from the left and right edges, for curved screen "
           "edges or a case.");
  float stickInset = sTouchStickInset.load();
  if (ImGui::SliderFloat("Stick inset", &stickInset, 0.f, kTouchMarginMaxDp, "%.0f dp")) {
    sTouchStickInset.store(stickInset);
    MarkDirty();
  }
  ItemHelp("Extra room between the left stick and the screen's left edge, on top of the side "
           "margin.");
  float buttonInset = sTouchButtonInset.load();
  if (ImGui::SliderFloat("Button inset", &buttonInset, 0.f, kTouchMarginMaxDp, "%.0f dp")) {
    sTouchButtonInset.store(buttonInset);
    MarkDirty();
  }
  ItemHelp("Extra room between the face buttons (and the classic C-stick) and the screen's right "
           "edge, on top of the side margin.");
  if (ImGui::Button("Reset margins")) {
    sTouchSideMargin.store(kTouchSideMarginDefault);
    sTouchStickInset.store(kTouchStickInsetDefault);
    sTouchButtonInset.store(kTouchButtonInsetDefault);
    MarkDirty();
  }
  if (ImGui::Button("Edit layout")) {
    // The touch view takes the request and opens its editor over the game.
    sTouchEditRequested.store(true, std::memory_order_release);
    RequestToggle();
  }
  ItemHelp("Closes this menu and lets you drag each on-screen control to where you want it and "
           "pinch it to resize. Done saves; Reset all puts everything back.");
#endif

  ImGui::SeparatorText("Gyro aim");
  const char* gyroModes[] = {"Off", "Hold to aim", "Always aim"};
  int gyroMode = sGyroMode;
  if (ImGui::Combo("Mode", &gyroMode, gyroModes, 3)) {
    SetGyroMode(gyroMode);
  }
  ItemHelp("Tilt the pad or the phone to aim. Hold to aim uses right stick click or left ctrl. Needs "
           "mouse aim, twin stick or the touch controls (not the classic layout), since the gyro "
           "feeds that same aim.");
  ImGui::BeginDisabled(sGyroMode == 0 && !sSpringFlick);
  const char* gyroSources[] = {"Auto", "Controller", "Phone"};
  int gyroSource = sGyroSource;
  if (ImGui::Combo("Source", &gyroSource, gyroSources, 3)) {
    SetGyroSource(gyroSource);
  }
  ImGui::Text("Gyro: %s", GyroStatus());
  ImGui::EndDisabled();
  ImGui::BeginDisabled(sGyroMode == 0);
  float gyroRate = sGyroRate;
  // The ## suffix keeps its ImGui id apart from the mouse Sensitivity slider.
  if (ImGui::SliderFloat("Sensitivity##gyro", &gyroRate, 50.f, 3000.f, "%.0f px/s per rad/s",
                         ImGuiSliderFlags_Logarithmic)) {
    SetGyroRate(gyroRate);
  }
  ImGui::EndDisabled();

  bool springFlick = sSpringFlick;
  if (ImGui::Checkbox("Spring Ball on gyro flick", &springFlick)) {
    SetSpringBallFlick(springFlick);
  }
  ItemHelp("Tilt the pad or phone up sharply to spring, like Trilogy's nunchuk flick. Uses the gyro "
           "source below; gyro aim can stay off. Raise the strength if it springs by accident.");
  ImGui::BeginDisabled(!sSpringFlick);
  float flickRate = sSpringFlickRate;
  if (ImGui::SliderFloat("Flick strength", &flickRate, 2.f, 20.f, "%.1f rad/s")) {
    SetSpringBallFlickRate(flickRate);
  }
  ImGui::EndDisabled();
}

void DrawAudio() {
  bool ai = AiAudioEnabled();
  if (ImGui::Checkbox("Streamed audio (music/movies)", &ai)) {
    SetAiAudioEnabled(ai);
    MarkDirty();
  }
  bool musyx = sMusyxAudioEnabled;
  if (ImGui::Checkbox("MusyX audio (effects/streams)", &musyx)) {
    SetMusyxAudioEnabled(musyx);
    MarkDirty();
  }
}

void DrawVoices() {
  struct Agg {
    PortMusyxVoice voice;
    int instances;
  };

  // Held between frames so "Freeze list" can keep the last collection on screen.
  // Ordered by id, not loudness: sorting by rms made rows swap places every
  // frame as the levels moved, which made the checkboxes hard to hit and hid
  // which sample a row belonged to.
  static std::vector< Agg > aggs;
  static bool freeze = false;

  ImGui::Checkbox("Freeze list", &freeze);
  ImGui::SameLine();
  ImGui::TextUnformatted("(hold the current list so its rows stay put)");
  ImGui::SetItemTooltip("Freezing keeps the samples and levels shown right now, so the rows\n"
                        "stay readable while you look for one. Mute still applies: the\n"
                        "checkbox reads and writes the live mute state, so you can silence\n"
                        "a sample whether or not it is still playing. Untick to follow the\n"
                        "live voices again. Not saved between runs.");

  if (!freeze) {
    aggs.clear();
    PortMusyxVoice voices[64];
    const int count = MusyxPortCopyVoices(voices, 64);
    for (int i = 0; i < count; ++i) {
      bool found = false;
      for (Agg& agg : aggs) {
        if (agg.voice.smpId == voices[i].smpId) {
          ++agg.instances;
          if (voices[i].rms > agg.voice.rms) {
            agg.voice = voices[i];
          }
          found = true;
          break;
        }
      }
      if (!found) {
        aggs.push_back(Agg{voices[i], 1});
      }
    }
    std::sort(aggs.begin(), aggs.end(),
              [](const Agg& a, const Agg& b) { return a.voice.smpId < b.voice.smpId; });
  }

  ImGui::TextUnformatted("Active samples, lowest id first. Mute one to isolate it.");
  if (aggs.empty()) {
    ImGui::TextUnformatted(freeze ? "No samples in the frozen list." : "No active MusyX voices.");
  }
  for (const Agg& agg : aggs) {
    ImGui::PushID(static_cast< int >(agg.voice.smpId));
    bool muted = MusyxPortIsSampleMuted(agg.voice.smpId) != 0;
    if (ImGui::Checkbox("##mute", &muted)) {
      MusyxPortSetSampleMuted(agg.voice.smpId, muted ? 1 : 0);
      MarkDirty();
    }
    ImGui::SameLine();
    ImGui::Text("smp %u  %s  len %u  pitch %u  rms %d  vol %u/%u  x%d", agg.voice.smpId,
                agg.voice.looped ? "loop" : "one-shot", agg.voice.length, agg.voice.pitch,
                agg.voice.rms, agg.voice.volL, agg.voice.volR, agg.instances);
    ImGui::PopID();
  }
  // Deliberately not behind the empty-list case above: with nothing playing
  // there is still a mute set to clear.
  if (ImGui::Button("Unmute all")) {
    MusyxPortClearSampleMutes();
    MarkDirty();
  }
}

// Archipelago's Connect screen: the room's address, the slot name and the
// room password, saved to archipelago.json. The built-in Metroid Prime tables
// mean nothing else is needed, so this works where editing a file does not.
void DrawArchipelagoConnect() {
  static bool sLoaded = false;
  static char sServer[256];
  static char sSlot[64];
  static char sPassword[128];
  static std::string sResult;
  if (!sLoaded) {
    sLoaded = true;
    const PortAp::ConnectionDetails saved = PortAp::SavedConnection();
    SDL_strlcpy(sServer, saved.server.c_str(), sizeof(sServer));
    SDL_strlcpy(sSlot, saved.slot.c_str(), sizeof(sSlot));
    SDL_strlcpy(sPassword, saved.password.c_str(), sizeof(sPassword));
  }

  ImGui::SeparatorText("Connect");
  // The server doesn't send this option, so it is set here to match the seed.
  int suitDamage = sApSuitDamage;
  if (ImGui::Combo("Staggered suit damage", &suitDamage, "Default\0Progressive\0Additive\0")) {
    SetApSuitDamage(suitDamage);
  }
  ImGui::SetItemTooltip("Set it to your YAML's staggered_suit_damage (the apworld's default is\n"
                        "Progressive: damage reduction by how many suits you have).");
  ImGui::InputTextWithHint("Server", "archipelago.gg:38281", sServer, sizeof(sServer));
  ImGui::InputTextWithHint("Slot name", "your player name in the seed", sSlot, sizeof(sSlot));
  ImGui::InputTextWithHint("Password", "only if the room has one", sPassword, sizeof(sPassword),
                           ImGuiInputTextFlags_Password);
  if (ImGui::Button(PortAp::Enabled() ? "Reconnect" : "Connect")) {
    PortAp::ConnectionDetails details;
    details.server = sServer;
    details.slot = sSlot;
    details.password = sPassword;
    std::string error;
    sResult = PortAp::Connect(details, error) ? std::string() : error;
    // The saved form (trimmed, "/connect " dropped) goes back into the fields.
    if (sResult.empty())
      sLoaded = false;
  }
  if (PortAp::Enabled()) {
    ImGui::SameLine();
    if (ImGui::Button("Disconnect")) {
      std::string error;
      sResult = PortAp::Disconnect(error) ? std::string() : error;
    }
  }
  if (!sResult.empty())
    ImGui::TextColored(ThemeBadColor(ImVec4(1.f, 0.45f, 0.4f, 1.f)), "%s", sResult.c_str());
  ImGui::TextDisabled("Start a new game after connecting to a new seed.");

  // Games played before, each with its own save card. The list is re-read now
  // and then: a game is recorded when its server answers, after Connect.
  static std::vector<PortAp::ConnectionDetails> sRecent;
  static uint64_t sRecentReadAt = 0;
  if (!ImGui::CollapsingHeader("Recent Archipelago games", ImGuiTreeNodeFlags_DefaultOpen)) {
    sRecentReadAt = 0;
    return;
  }
  const uint64_t now = SDL_GetTicks();
  if (sRecentReadAt == 0 || now - sRecentReadAt > 2000) {
    sRecent = PortAp::RecentGames();
    sRecentReadAt = now == 0 ? 1 : now;
  }
  if (sRecent.empty()) {
    ImGui::TextDisabled("None yet. A game is listed once its server has answered.");
    return;
  }
  // The save card only changes on the title screen, so a game in progress
  // would keep saving to the card it was loaded from.
  const bool inGame = StateManager() != nullptr;
  if (inGame)
    ImGui::TextDisabled("Quit to the title screen to resume another game.");
  const PortAp::ConnectionDetails current = PortAp::SavedConnection();
  for (size_t i = 0; i < sRecent.size(); ++i) {
    const PortAp::ConnectionDetails& game = sRecent[i];
    const bool isCurrent = PortAp::Enabled() && current.server == game.server &&
                           current.slot == game.slot && current.seed == game.seed;
    ImGui::PushID(static_cast<int>(i));
    ImGui::BeginDisabled(inGame || isCurrent);
    if (ImGui::Button(isCurrent ? "Current" : "Resume")) {
      std::string error;
      sResult = PortAp::Connect(game, error) ? std::string() : error;
      if (sResult.empty()) {
        sLoaded = false;
        sRecentReadAt = 0;
      }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    char played[32] = "";
    const time_t when = static_cast<time_t>(game.lastPlayed);
    if (const std::tm* local = game.lastPlayed > 0 ? std::localtime(&when) : nullptr)
      std::strftime(played, sizeof(played), "%Y-%m-%d %H:%M", local);
    ImGui::Text("%s @ %s", game.slot.c_str(), game.server.c_str());
    ImGui::Indent();
    ImGui::TextDisabled("%s%s%s", game.seed.c_str(), played[0] != '\0' ? "  -  " : "", played);
    ImGui::Unindent();
    ImGui::PopID();
  }
}

ImVec4 ChatLineColor(const std::string& type) {
  if (type == "Chat")
    return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
  if (type == "ServerChat" || type == "CommandResult" || type == "AdminCommandResult")
    return ImVec4(0.55f, 0.85f, 1.0f, 1.0f);
  if (type == "Hint")
    return ThemeWarnColor(ImVec4(1.0f, 0.85f, 0.4f, 1.0f));
  if (type == "ItemSend" || type == "ItemCheat")
    return ThemeGoodColor(ImVec4(0.6f, 0.95f, 0.6f, 1.0f));
  if (type == "Goal" || type == "Release" || type == "Collect" || type == "Countdown")
    return ImVec4(1.0f, 0.6f, 1.0f, 1.0f);
  if (type == "port")
    return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
  return ImVec4(0.8f, 0.8f, 0.8f, 1.0f); // Join, Part, TagsChanged, Tutorial, ...
}

// The multiworld's chat: every PrintJSON message the server sent, and a box that
// sends Say, so server commands such as !hint work from inside the game.
void DrawChatTab() {
  static char sInput[512] = {};
  static std::string sError;
  static uint64_t sSeenSerial = ~uint64_t{0};

  if (!PortAp::Enabled()) {
    ImGui::TextWrapped("Connect to an Archipelago room on the Connection page to chat.");
    return;
  }
  uint64_t serial = 0;
  const std::vector< PortAp::ChatLine > log = PortAp::ChatLog(&serial);
  // The log takes the tab down to the input row, the error and the help line.
  const ImGuiStyle& style = ImGui::GetStyle();
  const float below = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() * 3.0f;
  // The window may reach past a small display, so stop at whichever ends first.
  const float room = std::min(ImGui::GetContentRegionAvail().y,
                              ImGui::GetIO().DisplaySize.y - style.WindowPadding.y -
                                  ImGui::GetCursorScreenPos().y);
  const float logHeight = std::max(ImGui::GetTextLineHeightWithSpacing() * 6.0f, room - below);
  if (ImGui::BeginChild("apChat", ImVec2(0.0f, logHeight), ImGuiChildFlags_Borders)) {
    // Follow new lines only while the log is scrolled to the bottom, so reading
    // back is not interrupted.
    const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
    if (log.empty())
      ImGui::TextDisabled("No messages yet.");
    ImGui::PushTextWrapPos(0.0f);
    for (const PortAp::ChatLine& line : log) {
      ImGui::PushStyleColor(ImGuiCol_Text, ChatLineColor(line.type));
      ImGui::TextUnformatted(line.text.c_str());
      ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    if (serial != sSeenSerial && (atBottom || sSeenSerial == ~uint64_t{0}))
      ImGui::SetScrollHereY(1.0f);
    sSeenSerial = serial;
  }
  ImGui::EndChild();

  const bool connected = PortAp::Connected();
  ImGui::BeginDisabled(!connected);
  const float sendWidth = ImGui::CalcTextSize("Send").x + style.FramePadding.x * 2.0f;
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - sendWidth - style.ItemSpacing.x);
  bool send = ImGui::InputTextWithHint("##apSay", connected ? "Message or !command" : "Not connected",
                                       sInput, sizeof(sInput), ImGuiInputTextFlags_EnterReturnsTrue);
  if (send)
    ImGui::SetKeyboardFocusHere(-1); // Enter keeps the box focused for the next line
  ImGui::SameLine();
  send = ImGui::Button("Send") || send;
  ImGui::EndDisabled();
  if (send && sInput[0] != '\0') {
    if (PortAp::SendChat(sInput, sError))
      sInput[0] = '\0';
  }
  if (!sError.empty())
    ImGui::TextColored(ThemeBadColor(ImVec4(1.f, 0.5f, 0.5f, 1.f)), "%s", sError.c_str());
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("Commands: !hint <item>, !hint_location <location>, !remaining, !release, "
                     "!collect, !help");
  ImGui::PopStyleColor();
}

// Restart, screenshot and exit, plus the settings file; the top of the Game page (settings are on System).
void DrawGameSection() {
  ImGui::SeparatorText("Game");
  if (ImGui::Button("Restart to menu")) {
    RequestReset();
  }
  ImGui::SameLine();
  if (ImGui::Button("Screenshot (F12)")) {
    aurora::request_screenshot();
  }
  ImGui::SameLine();
  if (ImGui::Button("Exit game")) {
    ImGui::OpenPopup("Exit game?");
  }
  if (ImGui::BeginPopupModal("Exit game?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("Progress since the last save station is lost.");
    if (ImGui::Button("Exit")) {
      // The same path as closing the window: the main loop sees AURORA_EXIT
      // and shuts down cleanly.
      SaveSettings();
      SDL_Event quit{};
      quit.type = SDL_EVENT_QUIT;
      SDL_PushEvent(&quit);
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

void DrawSettingsSection() {
  ImGui::SeparatorText("Settings");
  if (ImGui::Button("Save settings now")) {
    sSettingsDirty = true;
    SaveSettings();
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(sSettingsDirty ? "Unsaved changes" : "Saved");
  ImGui::SetItemTooltip("Settings are saved automatically when changed.");
  const std::string path = SettingsFilePath();
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("File: %s", path.c_str());
  ImGui::PopStyleColor();
}

// The connection page of the Archipelago tab: the Connect form, recent games,
// the session's status and the items received.
void DrawArchipelagoSession() {
  DrawArchipelagoConnect();
  if (PortAp::Enabled()) {
    ImGui::SeparatorText("Archipelago status");
    if (PortAp::Connected()) {
      ImGui::TextColored(ThemeGoodColor(), "connected");
    } else {
      ImGui::TextDisabled("not connected");
    }
    ImGui::SameLine();
    ImGui::TextWrapped("%s", PortAp::StatusText());
    const char* seedName = PortAp::SeedName();
    if (seedName != nullptr && seedName[0] != '\0')
      ImGui::TextWrapped("Seed: %s", seedName);
    ImGui::Text("Items received: %d    Location checks sent: %d", PortAp::ItemCount(),
                PortAp::CheckCount());
    const char* lastMessage = PortAp::LastMessage();
    if (lastMessage != nullptr && lastMessage[0] != '\0')
      ImGui::TextWrapped("Last message: %s", lastMessage);

    // Item tracker: the session's receipts, so an item that arrived while the
    // player was not looking at the HUD is still readable here.
    const std::vector< PortAp::TrackedItem > tracked = PortAp::TrackedItems();
    ImGui::SeparatorText("Received items");
    if (tracked.empty()) {
      ImGui::TextDisabled("Nothing yet.");
    } else {
      // Half the space the section has left, so the table does not push the
      // settings below it off the tab.
      if (ImGui::BeginTable("apTracked", 3,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                ImGuiTableFlags_BordersInnerH,
                            ImVec2(0.0f, ImGui::GetContentRegionAvail().y * 0.5f))) {
        ImGui::TableSetupColumn("Item");
        ImGui::TableSetupColumn("From");
        ImGui::TableSetupColumn("Step");
        ImGui::TableHeadersRow();
        for (const PortAp::TrackedItem& item : tracked) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(item.name.c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(item.from.empty() ? "(your item)" : item.from.c_str());
          ImGui::TableNextColumn();
          if (item.total > 1) {
            ImGui::Text("%d of %d", item.step, item.total);
          } else {
            ImGui::TextDisabled("-");
          }
        }
        ImGui::EndTable();
      }
    }
  }
}

// Everything Archipelago in one tab: the connection and the multiworld's chat.
bool SubTab(const char* page, const char* name);

void DrawArchipelagoTab() {
  if (!ImGui::BeginTabBar("apPages")) {
    return;
  }
  if (SubTab("Archipelago", "Connection")) {
    DrawArchipelagoSession();
    ImGui::EndTabItem();
  }
  if (SubTab("Archipelago", "Chat")) {
    DrawChatTab();
    ImGui::EndTabItem();
  }
  ImGui::EndTabBar();
}

void GrantItem(CPlayerState& ps, CPlayerState::EItemType type, int amount, int capacity) {
  ps.SetPowerUp(type, capacity);
  ps.SetPickup(type, amount);
}

void DrawCheats() {
  CStateManager* mgr = sStateManager;
  if (mgr == nullptr) {
    ImGui::TextUnformatted("Waiting for gameplay...");
    return;
  }
  CPlayerState* ps = mgr->PlayerState();
  if (ps == nullptr) {
    ImGui::TextUnformatted("No player state.");
    return;
  }

  ImGui::Text("Health: %.0f / %.0f", ps->HealthInfo()->GetHP(), ps->CalculateHealth());
  if (ImGui::Button("Full health")) {
    ps->HealthInfo()->SetHP(ps->CalculateHealth());
  }
  ImGui::SameLine();
  if (ImGui::Button("Grant everything")) {
    for (int i = CPlayerState::kIT_PowerBeam; i < CPlayerState::kIT_Max; ++i) {
      GrantItem(*ps, static_cast< CPlayerState::EItemType >(i), 1, 1);
    }
    GrantItem(*ps, CPlayerState::kIT_Missiles, 250, 250);
    GrantItem(*ps, CPlayerState::kIT_PowerBombs, 8, 8);
    GrantItem(*ps, CPlayerState::kIT_EnergyTanks, 14, 14);
    ps->HealthInfo()->SetHP(ps->CalculateHealth());
  }
  ImGui::SameLine();
  bool invulnerable = Invulnerable();
  if (ImGui::Checkbox("Invulnerable", &invulnerable)) {
    SetInvulnerable(invulnerable);
  }
  ImGui::SetItemTooltip("Samus takes no damage from anything. Stays on until unticked.");

  ImGui::SeparatorText("Abilities");
  struct SItemToggle {
    const char* name;
    CPlayerState::EItemType type;
  };
  static const SItemToggle kItems[] = {
      {"Power Beam", CPlayerState::kIT_PowerBeam},
      {"Ice Beam", CPlayerState::kIT_IceBeam},
      {"Wave Beam", CPlayerState::kIT_WaveBeam},
      {"Plasma Beam", CPlayerState::kIT_PlasmaBeam},
      {"Charge Beam", CPlayerState::kIT_ChargeBeam},
      {"Super Missile", CPlayerState::kIT_SuperMissile},
      {"Ice Spreader", CPlayerState::kIT_IceSpreader},
      {"Wavebuster", CPlayerState::kIT_Wavebuster},
      {"Flamethrower", CPlayerState::kIT_Flamethrower},
      {"Combat Visor", CPlayerState::kIT_CombatVisor},
      {"Scan Visor", CPlayerState::kIT_ScanVisor},
      {"Thermal Visor", CPlayerState::kIT_ThermalVisor},
      {"X-Ray Visor", CPlayerState::kIT_XRayVisor},
      {"Morph Ball", CPlayerState::kIT_MorphBall},
      {"Morph Ball Bombs", CPlayerState::kIT_MorphBallBombs},
      {"Boost Ball", CPlayerState::kIT_BoostBall},
      {"Spider Ball", CPlayerState::kIT_SpiderBall},
      {"Space Jump Boots", CPlayerState::kIT_SpaceJumpBoots},
      {"Grapple Beam", CPlayerState::kIT_GrappleBeam},
      {"Gravity Suit", CPlayerState::kIT_GravitySuit},
      {"Varia Suit", CPlayerState::kIT_VariaSuit},
      {"Phazon Suit", CPlayerState::kIT_PhazonSuit},
  };
  // As many columns as the widest name allows.
  float nameWidth = 0.f;
  for (const SItemToggle& item : kItems) {
    nameWidth = std::max(nameWidth, ImGui::CalcTextSize(item.name).x);
  }
  nameWidth += ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::GetStyle().ItemSpacing.x * 2.f;
  const int columns = std::clamp(int(ImGui::GetContentRegionAvail().x / nameWidth), 1, 4);
  if (ImGui::BeginTable("##abilities", columns)) {
    for (const SItemToggle& item : kItems) {
      ImGui::TableNextColumn();
      bool owned = ps->HasPowerUp(item.type);
      if (ImGui::Checkbox(item.name, &owned)) {
        if (owned) {
          GrantItem(*ps, item.type, 1, 1);
        } else {
          ps->SetPowerUp(item.type, 0);
          ps->SetPickup(item.type, 0);
        }
      }
    }
    ImGui::EndTable();
  }

  int missiles = ps->GetItemAmount(CPlayerState::kIT_Missiles);
  if (ImGui::SliderInt("Missiles", &missiles, 0, 250)) {
    GrantItem(*ps, CPlayerState::kIT_Missiles, missiles, 250);
  }
  int powerBombs = ps->GetItemAmount(CPlayerState::kIT_PowerBombs);
  if (ImGui::SliderInt("Power Bombs", &powerBombs, 0, 8)) {
    GrantItem(*ps, CPlayerState::kIT_PowerBombs, powerBombs, 8);
  }
  int tanks = ps->GetItemAmount(CPlayerState::kIT_EnergyTanks);
  if (ImGui::SliderInt("Energy Tanks", &tanks, 0, 14)) {
    GrantItem(*ps, CPlayerState::kIT_EnergyTanks, tanks, 14);
    ps->HealthInfo()->SetHP(ps->CalculateHealth());
  }

  ImGui::SeparatorText("Teleport");
  CWorld* world = mgr->World();
  if (world == nullptr) {
    ImGui::TextUnformatted("No world.");
    return;
  }
  if (mgr->GetGameState() != CStateManager::kGS_Running) {
    ImGui::TextUnformatted("(waiting for gameplay)");
  }
  const int areaCount = world->IGetAreaCount();
  const int current = world->GetCurrentAreaId().Value();
  const std::vector< std::string >& roomNames = PortTracker::RoomNames(*world);
  const auto roomLabel = [&](int i) {
    char label[160];
    if (i >= 0 && i < int(roomNames.size()) && !roomNames[i].empty()) {
      std::snprintf(label, sizeof(label), "%d  %s", i, roomNames[i].c_str());
    } else {
      std::snprintf(label, sizeof(label), "Area %d", i);
    }
    return std::string(label);
  };
  // Only loaded areas (the current one and its neighbours) can be entered in place; any
  // other area reloads the world straight into it.
  const auto goToArea = [&](int i) {
    const bool loaded = world->GetArea(TAreaId(i))->IsPostConstructed();
    if (loaded || gpGameState == nullptr) {
      PortDebug::RequestTeleport(i);
    } else {
      PortDebug::RequestWorldTeleport(
          static_cast< uint32_t >(gpGameState->CurrentWorldAssetId()),
          static_cast< uint32_t >(world->IGetAreaAlways(TAreaId(i))->IGetAreaAssetId()));
    }
  };
  static char sRoomFilter[64] = {};
  const float goWidth = ImGui::CalcTextSize("Reload").x + ImGui::GetStyle().FramePadding.x * 2.f;
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - goWidth - ImGui::GetStyle().ItemSpacing.x);
  if (ImGui::BeginCombo("##room", roomLabel(current).c_str(), ImGuiComboFlags_HeightLarge)) {
#if !defined(__ANDROID__) // it would raise the on-screen keyboard every time
    if (ImGui::IsWindowAppearing()) {
      ImGui::SetKeyboardFocusHere();
    }
#endif
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##filter", "Filter rooms", sRoomFilter, sizeof(sRoomFilter));
    for (int i = 0; i < areaCount; ++i) {
      const std::string label = roomLabel(i);
      if (sRoomFilter[0] != '\0' && SDL_strcasestr(label.c_str(), sRoomFilter) == nullptr) {
        continue;
      }
      const bool loaded = world->GetArea(TAreaId(i))->IsPostConstructed();
      ImGui::PushID(i);
      if (!loaded) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      }
      if (ImGui::Selectable(label.c_str(), i == current)) {
        goToArea(i);
      }
      if (!loaded) {
        ImGui::PopStyleColor();
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip("Pick a room to go there. Greyed rooms aren't loaded: going there reloads the world.");
  ImGui::SameLine();
  if (ImGui::Button("Reload")) {
    goToArea(current);
  }

  if (gpMemoryCard == nullptr) {
    ImGui::TextUnformatted("(memory card not ready)");
    return;
  }
  static bool sWorldListBuilt = false;
  static std::vector< std::pair< uint32_t, std::string > > sWorldList;
  if (!sWorldListBuilt && !gpMemoryCard->GetMemoryWorlds().empty()) {
    sWorldListBuilt = true;
    const rstl::vector< CMemoryCard::MemoryWorld >& worlds = gpMemoryCard->GetMemoryWorlds();
    for (int i = 0; i < worlds.size(); ++i) {
      const uint32_t id = static_cast< uint32_t >(worlds[i].first);
      std::string name;
      const wchar_t* wide = worlds[i].second.GetFrontEndName();
      if (wide != nullptr) {
        for (const wchar_t* p = wide; *p != 0; ++p) {
          name.push_back(static_cast< char >(*p));
        }
      }
      if (name.empty()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "MLVL %08X", static_cast< unsigned >(id));
        name = buf;
      }
      sWorldList.emplace_back(id, name);
    }
  }
  if (!sWorldListBuilt) {
    ImGui::TextUnformatted("(loading worlds...)");
    return;
  }
  const char* currentWorld = "(another world)";
  for (const std::pair< uint32_t, std::string >& entry : sWorldList) {
    if (gpGameState != nullptr && gpGameState->CurrentWorldAssetId() == entry.first) {
      currentWorld = entry.second.c_str();
    }
  }
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::BeginCombo("##world", currentWorld)) {
    for (const std::pair< uint32_t, std::string >& entry : sWorldList) {
      ImGui::PushID(static_cast< int >(entry.first));
      const bool isCurrent = gpGameState != nullptr && gpGameState->CurrentWorldAssetId() == entry.first;
      if (ImGui::Selectable(entry.second.c_str(), isCurrent)) {
        PortDebug::RequestWorldTeleport(entry.first, 0u);
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip("Pick a world to go to its start.");
}

// The free camera: the view leaves the player, who stands still meanwhile.
void DrawFreeCam() {
  CStateManager* mgr = StateManager();
  bool on = PortFreeCam::Active();
  ImGui::BeginDisabled(mgr == nullptr && !on);
  if (ImGui::Checkbox("Free camera", &on)) {
    PortFreeCam::SetActive(on, mgr);
  }
  ImGui::EndDisabled();
  if (!PortFreeCam::Active()) {
    return;
  }
  ImGui::SameLine();
  bool frozen = PortFreeCam::Frozen();
  if (ImGui::Checkbox("Freeze the game", &frozen)) {
    PortFreeCam::SetFrozen(frozen);
  }
  ImGui::SameLine();
  bool showPlayer = PortFreeCam::ShowPlayer();
  if (ImGui::Checkbox("Show Samus", &showPlayer)) {
    PortFreeCam::SetShowPlayer(showPlayer);
  }
  float speed = PortFreeCam::Speed();
  if (ImGui::SliderFloat("Speed", &speed, 1.f, 100.f, "%.0f m/s", ImGuiSliderFlags_Logarithmic)) {
    PortFreeCam::SetSpeed(speed);
  }
  PortFreeCam::Pose pose = PortFreeCam::GetPose();
  float pos[3] = {pose.x, pose.y, pose.z};
  float look[2] = {pose.yaw, pose.pitch};
  bool moved = ImGui::InputFloat3("Position", pos, "%.2f");
  moved |= ImGui::InputFloat2("Yaw, pitch", look, "%.1f");
  if (moved) {
    pose.x = pos[0];
    pose.y = pos[1];
    pose.z = pos[2];
    pose.yaw = look[0];
    pose.pitch = look[1];
    PortFreeCam::SetPose(pose);
  }
  ImGui::TextWrapped("Close this menu to fly: stick moves, C stick or mouse looks, Z / D-pad up "
                     "rises, L / D-pad down sinks, R goes four times as fast.");
}

// The switches and readouts for working on what mods draw: room geometry, room
// environments, PBR.
void DrawRendering() {
  ImGui::TextDisabled("Hover a setting for what it does and the recommended value. They reset at each start.");
  static const char* const kCollisionModes[] = {"off", "on top of the world", "only (hide the world)"};
  int collision = int(PortCollisionView::GetMode());
  if (ImGui::Combo("Collision", &collision, kCollisionModes, 3)) {
    PortCollisionView::SetMode(PortCollisionView::Mode(collision));
  }
  ImGui::SetItemTooltip("Recommended: off.\n"
                        "Draws what Samus collides with: walls grey, floors blue, ceilings red,\n"
                        "lava orange, grates yellow, solid objects as orange boxes.");

  if (ImGui::CollapsingHeader("Frame statistics")) {
    if (const AuroraStats* stats = aurora_get_stats()) {
      ImGui::Text("%.0f fps, %u draws (%u merged), %u PBR, %u passes", aurora_get_fps(), stats->drawCallCount,
                  stats->mergedDrawCallCount, CCubeMaterial::sPortPBRDraws, stats->renderPassCount);
      ImGui::Text("vertices %.1f MiB, indices %.1f, arrays %.1f, uniforms %.1f, texture uploads %.1f",
                  stats->lastVertSize / 1048576.f, stats->lastIndexSize / 1048576.f,
                  stats->lastStorageSize / 1048576.f, stats->lastUniformSize / 1048576.f,
                  stats->lastTextureUploadSize / 1048576.f);
      if (const uint32_t resident = aurora_get_resident_geometry_mib()) {
        ImGui::Text("kept on the GPU %.1f of %u MiB", aurora_get_resident_geometry_used() / 1048576.f, resident);
      }
      ImGui::Text("pipelines %u made, %u waiting", stats->createdPipelines, stats->queuedPipelines);
    }
    AuroraTextureStats textures{};
    aurora_get_texture_stats(&textures);
    ImGui::Text("textures %u, %.0f MiB; render targets %u, %.0f MiB", textures.count[0],
                textures.bytes[0] / 1048576.f, textures.count[1], textures.bytes[1] / 1048576.f);
    int areas = 0;
    int instances = 0;
    int models = 0;
    int loaded = 0;
    int drawn = 0;
    PortRoomGeo::Stats(areas, instances, models, loaded, drawn);
    ImGui::Text("room geometry: %d area(s), %d of %d model(s) loaded, %d of %d instance(s) drawn", areas, loaded,
                models, drawn, instances);
  }
}
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
    ImGui::TextWrapped("Remastered support is unfinished and broken in places. Expect wrong materials, missing or "
                       "misplaced models and effects, visual glitches, slowdowns and crashes. Re-imports are often "
                       "needed after updates.");
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
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
    ItemHelp("The language of the game's text. Only English is on the disc: the others come with the "
             "Remastered import (its text), and any text it lacks stays English. Text already on "
             "screen changes the next time its menu or screen opens.");
  }

}

// What a player sets for Remastered's rooms; the rest is under Debug.
void DrawRemasteredRoomModels() {
  ImGui::SeparatorText("Room models");
  // The frame's buffers are only sized for room geometry when the game started with some.
  const bool geoReady = PortRoomGeo::BuffersReady();
  if (!geoReady && PortMods::RoomGeometryLoaded()) {
    ImGui::TextColored(ThemeWarnColor(), "Restart the game to see the Remastered rooms.");
    ImGui::SetItemTooltip("The game started without room geometry installed, so it set no room aside\n"
                          "for it. Once it starts with some, mods can be changed without a restart.");
  }
  ImGui::BeginDisabled(!geoReady);
  static const char* const kModes[] = {"off", "in place of the room", "on top of the room"};
  int mode = int(PortRoomGeo::GetMode());
  if (ImGui::Combo("Room geometry", &mode, kModes, 3)) {
    PortRoomGeo::SetMode(PortRoomGeo::Mode(mode));
  }
  ImGui::SetItemTooltip("Recommended: in place of the room.\n"
                        "Draws a mod's Remastered room models instead of the original rooms, or on\n"
                        "top of them to compare the two. Does nothing without a room geometry mod.");
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
    ImGui::SetItemTooltip("Turns anti-aliasing off and sets the EFB scale to 2x (the Quality settings).\n"
                          "The Remastered rooms draw many more models, and a phone's GPU pays for\n"
                          "each one at every pixel it renders.");
  }
#endif
  bool areaLights = PortRoomGeo::AreaLights();
  if (ImGui::Checkbox("Take the area's lights", &areaLights)) {
    PortRoomGeo::SetAreaLights(areaLights);
  }
  ImGui::SetItemTooltip("Recommended: off.\n"
                        "Also lights the Remastered rooms with the game's own lights where the room\n"
                        "has baked light. Off matches Remastered.");
  float minPixels = PortRoomGeo::MinPixels();
  if (ImGui::SliderFloat("Skip small models", &minPixels, 0.f, 8.f, minPixels > 0.f ? "under %.1f px" : "off")) {
    PortRoomGeo::SetMinPixels(minPixels);
    MarkDirty();
  }
  ImGui::SetItemTooltip("Leaves out room models that look smaller than this on screen (in the game's\n"
                        "own 480-line pixels, whatever the render scale). Raise it if frames are\n"
                        "slow; 0 draws everything.");
  float lodDistance = PortRoomGeo::LodDistance();
  if (ImGui::SliderFloat("Detail distance", &lodDistance, 0.f, 4.f, lodDistance > 0.f ? "x%.2f" : "full detail")) {
    PortRoomGeo::SetLodDistance(lodDistance);
    MarkDirty();
  }
  ImGui::SetItemTooltip("Where room models switch to Remastered's simpler versions of themselves\n"
                        "farther away: 1 is Remastered's own distances, higher keeps the full models\n"
                        "farther out, 0 never switches. Lower it if frames are slow. Needs an import\n"
                        "made by this version.");
  ImGui::EndDisabled();
  bool resident = sRoomGeoResident;
  if (ImGui::Checkbox("Keep on the GPU (next start)", &resident)) {
    PortDebug::SetRoomGeoResident(resident);
  }
  ImGui::SetItemTooltip(
      "Recommended: on.\n"
      "Uploads a room geometry mod's models once when they load instead of\n"
      "every frame, so a frame's buffers can be smaller. Takes effect from the\n"
      "next start.");

}

void DrawRemasteredTab() {
  DrawRemasteredWarning();
  ImGui::SeparatorText("Import");
  DrawRemasteredImport();

  ImGui::SeparatorText("Gallery");
  {
    const std::vector<std::string> pictures = PortMods::GalleryPaths();
    if (pictures.empty()) {
      ImGui::TextDisabled("Remastered's concept art. It comes with the Remastered import.");
    } else if (ImGui::Button(("Open gallery (" + std::to_string(pictures.size()) + " pictures)").c_str())) {
      sGalleryPaths = pictures;
      sGalleryIndex = std::min(sGalleryIndex, int(pictures.size()) - 1);
      ReleaseGalleryTexture();
      sGalleryOpen = true;
    }
  }

  DrawRemasteredRoomModels();
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
      ImGui::SetItemTooltip("The game started without room geometry installed, so it set no room aside\n"
                            "for it. Once it starts with some, mods can be changed without a restart.");
    }
    ImGui::BeginDisabled(!geoReady);
    bool merged = PortRoomGeo::MergedDraws();
    if (ImGui::Checkbox("Draw merged copies", &merged)) {
      PortRoomGeo::SetMergedDraws(merged);
    }
    ImGui::SetItemTooltip("Recommended: on. Not saved.\n"
                          "Draws repeated props (rocks, grass) near each other as one model, lit as\n"
                          "a group. Off draws each copy on its own, with its own lights: slower, but a\n"
                          "way to check whether the merge changes how something looks.");
    bool frontToBack = PortRoomGeo::FrontToBack();
    if (ImGui::Checkbox("Draw nearest first", &frontToBack)) {
      PortRoomGeo::SetFrontToBack(frontToBack);
    }
    ImGui::SetItemTooltip("Recommended: on. Not saved.\n"
                          "Draws the room's solid models nearest first, so the GPU skips shading what\n"
                          "they hide. Looks the same either way; off is for comparing frame rates.");
    bool prepass = PortRoomGeo::DepthPrepass();
    if (ImGui::Checkbox("Depth pre-pass for cut-outs", &prepass)) {
      PortRoomGeo::SetDepthPrepass(prepass);
    }
    ImGui::SetItemTooltip("Recommended: off. Not saved. Needs \"Draw nearest first\".\n"
                          "Draws grass and leaves once for depth only, then shades just the parts that\n"
                          "stay in front. Looks the same either way; slower on the phones tried so far.");
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
      GXPortSetGpuTimes(gpuTimes ? GX_TRUE : GX_FALSE);
    }
    ImGui::SetItemTooltip("Not saved. Times each render pass on the GPU (timestamp queries), averaged\n"
                          "over 60 frames. Total is the sum of the passes; span is first start to last end.");
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
    ImGui::SetItemTooltip("Not saved. A speed test: leaves out one part of the Remastered surfaces'\n"
                          "shading (Flat leaves out all of it but the colour map), so the frame rate\n"
                          "shows what that part costs. Maps only is Flat with every map still read, to\n"
                          "tell reading the maps from the maths. Looks wrong on purpose. The two\n"
                          "without anisotropy shade in full but filter the maps more cheaply, and look a\n"
                          "little softer. The rest time the frame around the shading: no bloom and\n"
                          "colour grade, no copies of the screen for effects such as heat haze, and\n"
                          "then one part of the post-processing at a time: the bloom (the grade stays),\n"
                          "its copy of the frame, and reloading depth for the HUD after it. The last\n"
                          "looks right: it reloads the depth even when the HUD doesn't need it.");
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
    ImGui::SetItemTooltip("Recommended: the shaded result.\n"
                          "The other choices show one input of a mod's PBR materials alone: base colour\n"
                          "(albedo), normals, roughness, metalness, ambient occlusion, ambient light,\n"
                          "reflection, glow, exposure, or the material kind.");
    static const char* const kProbes[] = {"off", "on", "mirror", "window"};
    int probe = std::clamp(CCubeMaterial::sPortPBRProbeMode, 0, 3);
    if (ImGui::Combo("Reflection probe", &probe, kProbes, 4)) {
      CCubeMaterial::sPortPBRProbeMode = probe;
    }
    ImGui::SetItemTooltip("Recommended: on.\n"
                          "Where shiny PBR surfaces get their reflections: the room's own cube when\n"
                          "the mod has one, otherwise a live capture of the world around the camera.\n"
                          "Mirror and window draw surfaces as perfect mirrors of it, or windows onto it,\n"
                          "to check what it holds.");

    bool env = PortRoomEnv::Enabled();
    if (ImGui::Checkbox("Room environments", &env)) {
      PortRoomEnv::SetEnabled(env);
    }
    ImGui::SetItemTooltip("Recommended: on.\n"
                          "Uses the lighting a mod brings for each room: its reflection cubes, baked\n"
                          "ambient light, exposure and bloom.");
    ImGui::BeginDisabled(!env);
    ImGui::SameLine();
    bool exposed = PortRoomEnv::RoomExposed();
    if (ImGui::Checkbox("Exposure by room", &exposed)) {
      PortRoomEnv::SetRoomExposed(exposed);
    }
    ImGui::SetItemTooltip("Recommended: on.\n"
                          "Sets the brightness as Remastered does, from the room the camera is in and\n"
                          "its tone curve. Off exposes every reflection cube to a neutral middle grey.");
    ImGui::SameLine();
    bool volumes = PortRoomEnv::VolumesEnabled();
    if (ImGui::Checkbox("Baked light per pixel", &volumes)) {
      PortRoomEnv::SetVolumesEnabled(volumes);
    }
    ImGui::SetItemTooltip("Recommended: on.\n"
                          "Lights the Remastered rooms from the room's grid of baked light, so the light\n"
                          "changes across a surface. Off gives each model one ambient colour.");
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

void DrawLogSection() {
  ImGui::SeparatorText("Log");
  bool logFile = sLogFile || PortLogFile::Active();
  if (ImGui::Checkbox("Write the log to a file", &logFile)) {
    SetLogFile(logFile);
    if (logFile) {
      PortLogFile::Start();
    }
  }
  const std::string logPath = PortLogFile::Path();
  if (!logPath.empty()) {
    ImGui::SameLine();
    if (ImGui::Button("Copy log path")) {
      ImGui::SetClipboardText(logPath.c_str());
    }
  }
  if (PortLogFile::Active()) {
    ImGui::TextWrapped("Writing to %s (last run's: metroid_prime_port.old.log).", logPath.c_str());
    if (!sLogFile) {
      ImGui::TextDisabled("Stops at the next start.");
    }
  } else {
    ImGui::TextWrapped("Everything the game logs, including the reason for a crash, goes to %s.",
                       logPath.empty() ? "(no user folder)" : logPath.c_str());
  }
  if (const std::string shared = PortLogFile::SharedPath(); !shared.empty()) {
    ImGui::TextWrapped("A copy goes to %s, which the phone's file manager can open.", shared.c_str());
  }

}

void DrawDebugTab() {
  ImGui::SeparatorText("Cheats");
  bool cheats = sCheats;
  if (ImGui::Checkbox("Show cheats (items, health, teleport)", &cheats)) {
    sCheats = cheats;
    MarkDirty();
  }
  if (sCheats) {
    DrawCheats();
  }

  ImGui::SeparatorText("Camera");
  DrawFreeCam();

  ImGui::SeparatorText("Rendering");
  DrawRendering();

  ImGui::SeparatorText("Audio backends");
  DrawAudio();
  if (ImGui::CollapsingHeader("Sounds playing")) {
    DrawVoices();
  }
}

void DrawTrackerCount(const char* label, const PortTracker::Count& count) {
  const bool done = count.total > 0 && count.have >= count.total;
  if (done) {
    ImGui::TextColored(ThemeGoodColor(), "%s %d/%d", label, count.have, count.total);
  } else {
    ImGui::Text("%s %d/%d", label, count.have, count.total);
  }
}

// The Archipelago checks within reach, coloured as on the map.
void DrawTrackerLogic() {
  static PortAp::LogicState state;
  if (!PortAp::Logic(state)) {
    return;
  }
  ImGui::SeparatorText("Archipelago checks");
  bool colors = sMapLogicColors;
  if (ImGui::Checkbox("Colour the map's dots by logic", &colors)) {
    SetMapLogicColors(colors);
  }
  ItemHelp("Worked out from the items received and the seed's logic options, with the rules of the "
           "Metroid Prime Archipelago tracker pack. Green is in logic; yellow can be reached with a "
           "trick the seed doesn't count on; blue can be seen but not collected.");
  static const ImVec4 kColors[] = {
      ImVec4(0.95f, 0.30f, 0.30f, 1.f), // out of logic
      ImVec4(0.35f, 0.60f, 1.00f, 1.f), // inspect
      ImVec4(1.00f, 0.85f, 0.25f, 1.f), // sequence break
      ImVec4(0.35f, 0.90f, 0.40f, 1.f), // in logic
  };
  static const ImVec4 kGrey(0.55f, 0.55f, 0.55f, 1.f);
  size_t count = 0;
  const PortApLogic::Check* checks = PortApLogic::Checks(count);
  int totals[4] = {};
  int checked = 0;
  for (size_t i = 0; i < count; ++i) {
    if (state.checked[i]) {
      ++checked;
    } else {
      ++totals[static_cast< int >(state.levels[i])];
    }
  }
  ImGui::TextColored(kColors[3], "%d in logic", totals[3]);
  ImGui::SameLine();
  ImGui::TextColored(kColors[2], "%d sequence break", totals[2]);
  ImGui::SameLine();
  ImGui::TextColored(kColors[1], "%d visible only", totals[1]);
  ImGui::SameLine();
  ImGui::TextColored(kColors[0], "%d out of reach", totals[0]);
  ImGui::SameLine();
  ImGui::TextColored(kGrey, "%d checked", checked);

  // One header per area, holding what can be reached there, best first.
  const char* area = nullptr;
  bool open = false;
  for (size_t i = 0; i < count; ++i) {
    if (area == nullptr || std::strcmp(area, checks[i].area) != 0) {
      area = checks[i].area;
      int inLogic = 0;
      int other = 0;
      for (size_t j = 0; j < count; ++j) {
        if (std::strcmp(checks[j].area, area) != 0 || state.checked[j]) {
          continue;
        }
        if (state.levels[j] == PortApLogic::Level::Normal) {
          ++inLogic;
        } else if (state.levels[j] != PortApLogic::Level::None) {
          ++other;
        }
      }
      char header[160];
      std::snprintf(header, sizeof(header), "%s (%d in logic, %d other)###aplogic%s", area, inLogic,
                    other, area);
      ImGui::SetNextItemOpen(inLogic > 0, ImGuiCond_Once);
      open = ImGui::CollapsingHeader(header);
      if (open) {
        for (int level = 3; level >= 1; --level) {
          for (size_t j = 0; j < count; ++j) {
            if (std::strcmp(checks[j].area, area) != 0 || state.checked[j] ||
                static_cast< int >(state.levels[j]) != level) {
              continue;
            }
            if (checks[j].section[0] != 0) {
              ImGui::TextColored(kColors[level], "  %s - %s", checks[j].room, checks[j].section);
            } else {
              ImGui::TextColored(kColors[level], "  %s", checks[j].room);
            }
          }
        }
        if (inLogic + other == 0) {
          ImGui::TextDisabled("  nothing within reach");
        }
      }
    }
  }

  DrawRemasteredDebug();
}

void DrawTrackerTab() {
  bool reveal = sRevealMap;
  ImGui::BeginDisabled(sOriginalExperience);
  if (ImGui::Checkbox("Reveal map", &reveal)) {
    SetRevealMap(reveal);
  }
  ImGui::EndDisabled();
  ItemHelp("Shows every world's map as if its map station had been used, and lists every world on "
           "the star map. Rooms a map station leaves hidden stay hidden, and rooms you haven't "
           "entered keep the unexplored colour. The save is not changed.");
  bool pickups = sMapPickups;
  if (ImGui::Checkbox("Pickup dots on the map", &pickups)) {
    SetMapPickups(pickups);
  }
  ItemHelp("A white dot marks each item pickup in the rooms the map shows, until you collect it. "
           "Every item gets the same dot, so it doesn't give away what a pickup holds. An "
           "Archipelago game colours them by what its logic lets you reach.");
  if (PortMapPickups::Forced()) {
    SameLineAfterHelp();
    ImGui::TextDisabled("(on in randomized games)");
  }

  DrawTrackerLogic();

  ImGui::SeparatorText("Progress");
  bool progress = sTrackerProgress;
  if (ImGui::Checkbox("Show progress", &progress)) {
    sTrackerProgress = progress;
    MarkDirty();
  }
  ItemHelp("Item collection, scans and rooms visited, with what is still missing. Off by default, "
           "since the counts and lists show how much is left to find.");
  if (!sTrackerProgress) {
    return;
  }

  CStateManager* mgr = sStateManager;
  if (mgr == nullptr || mgr->GetPlayerState() == nullptr) {
    ImGui::TextDisabled("Progress shows once a game is running.");
    return;
  }
  const PortTracker::Summary summary = PortTracker::Collect(*mgr);

  ImGui::SeparatorText("Items");
  ImGui::Text("Item collection %d%%", summary.itemPercent);
  DrawTrackerCount("Energy Tanks", summary.energyTanks);
  ImGui::SameLine(ImGui::GetFontSize() * 12.f);
  DrawTrackerCount("Missile expansions", summary.missileExpansions);
  DrawTrackerCount("Power Bombs", summary.powerBombExpansions);
  ImGui::SameLine(ImGui::GetFontSize() * 12.f);
  DrawTrackerCount("Artifacts", summary.artifacts);
  const PortTracker::Count upgrades = {
      static_cast< int >(summary.upgradesHeld.size()),
      static_cast< int >(summary.upgradesHeld.size() + summary.upgradesMissing.size())};
  DrawTrackerCount("Upgrades", upgrades);
  if (!summary.upgradesMissing.empty()) {
    std::string missing;
    for (const std::string& name : summary.upgradesMissing) {
      missing += (missing.empty() ? "" : ", ") + name;
    }
    ImGui::TextWrapped("Missing: %s", missing.c_str());
  }

  ImGui::SeparatorText("Scans");
  for (int i = 0; i < PortTracker::kScan_Count; ++i) {
    DrawTrackerCount(PortTracker::ScanGroupName(i), summary.scans[i]);
    if (i % 2 == 0) {
      ImGui::SameLine(ImGui::GetFontSize() * 12.f);
    }
  }
  DrawTrackerCount("All scans", summary.scanTotal);

  ImGui::SeparatorText("Rooms visited");
  for (const PortTracker::World& world : summary.worlds) {
    const PortTracker::Count rooms = {world.visited, world.total};
    DrawTrackerCount(world.name.c_str(), rooms);
    if (world.mapStation || world.current) {
      ImGui::SameLine();
      ImGui::TextDisabled("%s%s%s", world.mapStation ? "map station" : "",
                          world.mapStation && world.current ? ", " : "",
                          world.current ? "you are here" : "");
    }
  }

  int unvisited = 0;
  for (const PortTracker::Room& room : summary.rooms) {
    unvisited += room.visited ? 0 : 1;
  }
  char header[160];
  std::snprintf(header, sizeof(header), "Rooms not yet visited in %s (%d)###trackerrooms",
                summary.currentWorld.c_str(), unvisited);
  if (ImGui::CollapsingHeader(header)) {
    if (summary.roomNamesLoading > 0) {
      ImGui::TextDisabled("Loading room names...");
    }
    for (const PortTracker::Room& room : summary.rooms) {
      if (!room.visited) {
        ImGui::BulletText("%s", room.name.c_str());
      }
    }
  }
}

void DrawSaveStatesTab() {
  ImGui::TextDisabled("Save anywhere and load back to the same spot.");
  ItemHelp("A state holds what a memory card save holds (items, health, ammo, map, scans, doors and "
           "puzzles already solved, in-game time) plus where Samus stands and whether she is in "
           "morph ball. Loading rebuilds the room as a memory card load does, so enemies and moving "
           "parts start over. While the game is paused, a save or load waits until you unpause.");
  bool hotkeys = sSaveStateHotkeys;
  if (ImGui::Checkbox("F5 saves, F9 loads the selected slot", &hotkeys)) {
    sSaveStateHotkeys = hotkeys;
    MarkDirty();
  }
  const bool running = sStateManager != nullptr;
  if (!running) {
    ImGui::TextDisabled("Saving and loading need a running game.");
  }

  const int selected = PortSaveState::SelectedSlot();
  if (ImGui::BeginTable("##savestates", 4,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Where");
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    for (int slot = 1; slot <= PortSaveState::kSlotCount; ++slot) {
      const PortSaveState::Info info = PortSaveState::SlotInfo(slot);
      ImGui::PushID(slot);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      char label[16];
      std::snprintf(label, sizeof(label), "%d", slot);
      if (ImGui::RadioButton(label, selected == slot)) {
        PortSaveState::SetSelectedSlot(slot);
      }
      ImGui::TableNextColumn();
      if (info.exists) {
        ImGui::Text("%s - %s%s", info.world.c_str(), info.room.c_str(),
                    info.morphed ? " (ball)" : "");
      } else {
        ImGui::TextDisabled("empty");
      }
      ImGui::TableNextColumn();
      if (info.exists) {
        const int total = static_cast< int >(info.playTime);
        ImGui::Text("%d:%02d:%02d", total / 3600, total / 60 % 60, total % 60);
      }
      ImGui::TableNextColumn();
      ImGui::BeginDisabled(!running);
      if (ImGui::SmallButton("Save")) {
        PortSaveState::SetSelectedSlot(slot);
        PortSaveState::RequestSave(slot);
      }
      ImGui::SameLine();
      ImGui::BeginDisabled(!info.exists);
      if (ImGui::SmallButton("Load")) {
        PortSaveState::SetSelectedSlot(slot);
        PortSaveState::RequestLoad(slot);
      }
      ImGui::EndDisabled();
      ImGui::EndDisabled();
      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  const PortSaveState::Info undo = PortSaveState::SlotInfo(PortSaveState::kUndoSlot);
  ImGui::BeginDisabled(!running || !undo.exists);
  if (ImGui::Button("Undo last load")) {
    PortSaveState::RequestLoad(PortSaveState::kUndoSlot);
  }
  ImGui::EndDisabled();
  if (undo.exists) {
    ImGui::SameLine();
    ImGui::TextDisabled("back to %s - %s", undo.world.c_str(), undo.room.c_str());
  }
  const std::string message = PortSaveState::LastMessage();
  if (!message.empty()) {
    ImGui::TextWrapped("%s", message.c_str());
  }
  ImGui::TextDisabled("Folder: %s", PortSaveState::Folder().c_str());
}

// The game's own volumes, the same as the pause menu's two sliders.
void DrawGameAudio() {
  ImGui::SeparatorText("Audio");
  ImGui::BeginDisabled(gpGameState == nullptr);
  int music = gpGameState != nullptr ? gpGameState->GameOptions().GetMusicVolume() : 0;
  int musicPct = (music * 100 + 63) / 127;
  if (ImGui::SliderInt("Music volume", &musicPct, 0, 100, "%d%%") && gpGameState != nullptr) {
    gpGameState->GameOptions().SetMusicVolume((musicPct * 127 + 50) / 100, true);
  }
  int sfx = gpGameState != nullptr ? gpGameState->GameOptions().GetSfxVolume() : 0;
  int sfxPct = (sfx * 100 + 63) / 127;
  if (ImGui::SliderInt("Sound effects volume", &sfxPct, 0, 100, "%d%%") && gpGameState != nullptr) {
    gpGameState->GameOptions().SetSfxVolume((sfxPct * 127 + 50) / 100, true);
  }
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("The pause menu's volume options. Like them, they are stored in the save\n"
                        "file the next time you save, and loading a save restores its volumes.");
}

void DrawOriginalSection() {
  bool original = sOriginalExperience;
  if (ImGui::Checkbox("Original experience", &original)) {
    SetOriginalExperience(original);
  }
  ItemHelp("Plays the game as on the GameCube: 640x480 at 4:3, 60 Hz with no interpolation, no "
           "mods, the retail HUD, font, button icons, FOV and cutscenes, and none of the gameplay extras (spring ball, "
           "fast morph, charge and lock-on options, turbo fire, unlocks). The other settings keep "
           "their values and come back when it's turned off. Controls, cheats, save states, the "
           "randomizer, Archipelago and the timer still work.");
}

void DrawGameTab() {
  DrawOriginalSection();
  DrawGameSection();
  DrawCutscenesSection();
  DrawUnlocksSection();
  DrawGameAudio();
  DrawLanguageSection();
  DrawSpeedrunSection();
}

void DrawControlsTab() {
  if (!ImGui::BeginTabBar("controlsPages")) {
    return;
  }
  if (SubTab("Controls", "Options")) {
    DrawControlsOptions();
    ImGui::EndTabItem();
  }
  if (SubTab("Controls", "Keyboard & mouse")) {
    DrawControlsKeyboardMouse();
    ImGui::EndTabItem();
  }
  if (SubTab("Controls", "Controller")) {
    DrawControlsController();
    ImGui::EndTabItem();
  }
  if (SubTab("Controls", "Touch & gyro")) {
    DrawControlsTouchGyro();
    ImGui::EndTabItem();
  }
  ImGui::EndTabBar();
}

void DrawVideoTab() {
  if (!ImGui::BeginTabBar("videoPages")) {
    return;
  }
  if (SubTab("Video", "Display")) {
    DrawVideoDisplay();
    ImGui::EndTabItem();
  }
  if (SubTab("Video", "Quality")) {
    DrawVideoQuality();
    ImGui::EndTabItem();
  }
  if (SubTab("Video", "Frame rate")) {
    DrawPerformanceTab();
    ImGui::EndTabItem();
  }
  if (SubTab("Video", "Compatibility")) {
    DrawVideoCompatibility();
    ImGui::EndTabItem();
  }
  ImGui::EndTabBar();
}

void DrawModsTab() {
  DrawMods();
  DrawTexturesSection();
}

void DrawSystemTab() {
  DrawOverlaySection();
  DrawSettingsSection();
  DrawMemoryCard();
#if defined(__ANDROID__)
  PortDataFolder::DrawPanel();
#endif
  DrawLogSection();
  DrawDiscordSection();
  DrawUpdateSection();
  ImGui::SeparatorText("About");
  ImGui::TextDisabled("Version %s, build %s", MP_BUILD_VERSION, MP_BUILD_REVISION);
}

struct DebugPage {
  const char* name;
  void (*draw)();
};

// Trick names are comma-separated in the settings; the box shows them so.
std::string JoinNames(const std::vector< std::string >& names) {
  std::string text;
  for (const std::string& name : names) {
    text += (text.empty() ? "" : ", ") + name;
  }
  return text;
}

std::vector< std::string > SplitNames(const char* text) {
  std::vector< std::string > names;
  std::string item;
  for (const char* c = text;; ++c) {
    if (*c == ',' || *c == '\0') {
      item = Trim(item);
      if (!item.empty()) {
        names.push_back(item);
      }
      item.clear();
      if (*c == '\0') {
        break;
      }
    } else {
      item += *c;
    }
  }
  return names;
}

struct RandoSeedRow {
  std::string name;
  std::string summary;
  long long modified = 0;
};

std::string RandoSummary(const PortRandoGen::Settings& s) {
  static const char* const kBosses[] = {"both bosses", "Ridley", "Prime", "no boss"};
  std::string text = std::to_string(s.requiredArtifacts) + " artifacts, " +
                     kBosses[std::clamp(s.finalBosses, 0, 3)];
  if (s.elevatorRandomization) {
    text += ", elevators";
  }
  if (s.doorColorRandomization != 0) {
    text += s.doorColorRandomization == 1 ? ", doors global" : ", doors regional";
  }
  if (s.startingRoom != 0) {
    text += s.startingRoom == 1 ? ", safe start" : ", buckle-up start";
  }
  if (s.blastShieldRandomization != 0) {
    text += s.blastShieldRandomization == 1 ? ", shields replaced" : ", shields mixed";
  }
  if (s.lockedDoorCount > 0) {
    text += ", " + std::to_string(s.lockedDoorCount) + " locked";
  }
  if (s.trickDifficulty >= 0) {
    static const char* const kTricks[] = {"easy", "medium", "hard"};
    text += std::string(", tricks ") + kTricks[std::min(s.trickDifficulty, 2)];
  }
  return text;
}

std::vector< RandoSeedRow > ScanRandoSeeds() {
  std::vector< RandoSeedRow > rows;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(PortRandoGen::SeedDirectory(), ec)) {
    const std::string file = entry.path().filename().string();
    if (file.size() <= 5 || file.compare(file.size() - 5, 5, ".json") != 0 ||
        (file.size() > 11 && file.compare(file.size() - 11, 11, ".state.json") == 0)) {
      continue;
    }
    RandoSeedRow row;
    row.name = file.substr(0, file.size() - 5);
    PortRandoGen::Seed seed;
    std::string error;
    row.summary = PortRandoGen::Load(entry.path().string(), seed, error) ? RandoSummary(seed.settings)
                                                                          : "unreadable: " + error;
    const auto time = std::filesystem::last_write_time(entry.path(), ec);
    row.modified = ec ? 0 : static_cast< long long >(time.time_since_epoch().count());
    rows.push_back(std::move(row));
  }
  std::sort(rows.begin(), rows.end(),
            [](const RandoSeedRow& a, const RandoSeedRow& b) { return a.modified > b.modified; });
  return rows;
}

// The built-in randomizer: options, seed text, and the seeds made so far.
// Playing goes through the Archipelago client, so it replaces any session.
void DrawRandomizerTab() {
  static bool sScanned = false;
  static std::vector< RandoSeedRow > sRows;
  static char sSeedText[64];
  static char sAllow[256];
  static char sDeny[256];
  static bool sTextLoaded = false;
  static std::string sMessage;
  static bool sMessageError = false;
  static std::string sSpoilerName;
  static std::string sSpoilerText;
  static std::string sSpoilerNote;
  // The list is read again each time the page is opened (a frame passed
  // without it), so seeds made with the console's `rando gen` show up.
  static int sLastFrame = -2;
  if (ImGui::GetFrameCount() != sLastFrame + 1)
    sScanned = false;
  sLastFrame = ImGui::GetFrameCount();
  if (!sScanned) {
    sRows = ScanRandoSeeds();
    sScanned = true;
  }

  PortRandoGen::Settings s;
  {
    std::lock_guard< std::mutex > lock(sRandoMutex);
    s = sRandoSettings;
  }
  if (!sTextLoaded) {
    sTextLoaded = true;
    SDL_strlcpy(sAllow, JoinNames(s.trickAllow).c_str(), sizeof(sAllow));
    SDL_strlcpy(sDeny, JoinNames(s.trickDeny).c_str(), sizeof(sDeny));
  }
  bool changed = false;
  const auto check = [&](const char* label, bool& value, const char* help) {
    changed |= ImGui::Checkbox(label, &value);
    ItemHelp(help);
  };

  ImGui::TextWrapped("Makes a seed from these options and plays it as a one-player Archipelago game "
                     "(slot Samus), with its own save card. The same options and seed text give the "
                     "same seed everywhere.");

  ImGui::SeparatorText("Goal");
  changed |= ImGui::SliderInt("Required artifacts", &s.requiredArtifacts, 1, 12);
  ItemHelp("How many of the 12 Chozo artifacts open the way to the end.");
  changed |= ImGui::Combo("Final bosses", &s.finalBosses, "Ridley and Prime\0Ridley\0Prime\0None\0");
  ItemHelp("Which bosses must be beaten to finish. None finishes at the artifact temple.");
  check("Artifact hints", s.artifactHints, "The Artifact Temple totems say where each artifact is.");

  ImGui::SeparatorText("Start");
  changed |= ImGui::Combo("Starting room", &s.startingRoom, "Normal\0Safe\0Buckle up\0");
  ItemHelp("Normal starts at the Landing Site (Save Station 1 with random elevators). Safe and Buckle up "
           "start in a random room with that room's items; Buckle up's rooms are harder to get out of.");
  check("Random starting beam", s.randomizeStartingBeam,
        "Start with the Wave, Ice or Plasma Beam instead of the Power Beam.");

  ImGui::SeparatorText("Items");
  check("Missile Launcher", s.missileLauncher, "Missiles are useless until the Missile Launcher is found.");
  check("Main Power Bomb", s.mainPowerBomb, "Power bombs need the main Power Bomb item.");
  check("Shuffle Scan Visor", s.shuffleScanVisor, "The Scan Visor is an item to find, not a start item.");
  check("Progressive beams", s.progressiveBeams, "Each beam has upgrades received in order, not as named items.");
  check("Spring Ball", s.springBall, "Morph Ball Bombs also give the Spring Ball (jump in Morph Ball form).");
  changed |= ImGui::Combo("Remove X-Ray requirement", &s.removeXray,
                          "None\0Most\0All but the Omega Pirate\0");
  ItemHelp("Takes the X-Ray Visor out of the logic where it is needed to see hidden things.");
  changed |= ImGui::Combo("Remove Thermal requirement", &s.removeThermal, "None\0Most\0All\0");
  ItemHelp("Takes the Thermal Visor out of the logic where it is needed.");
  check("Remove Hive Mecha", s.removeHiveMecha, "Skips the Hive Mecha fight in Hive Totem.");

  ImGui::SeparatorText("World");
  check("Pre-scanned elevators", s.preScanElevators, "Elevator destinations are known without scanning them.");
  check("Elevator randomization", s.elevatorRandomization, "Elevators lead to other areas.");
  changed |= ImGui::Combo("Door colours", &s.doorColorRandomization, "None\0Global\0Regional\0");
  ItemHelp("Shuffles the coloured door locks, everywhere or within each area.");
  check("Power Beam doors", s.includePowerBeamDoors,
        "Door colours may turn a colour into Power Beam doors (with a random starting beam, the start "
        "beam's doors open to the Power Beam instead).");
  check("Morph Ball Bomb doors", s.includeMorphBallBombDoors,
        "With door colours, one colour of one area other than the start's opens to Morph Ball Bombs.");
  changed |= ImGui::Combo("Blast shields", &s.blastShieldRandomization, "None\0Replace existing\0Mix it up\0");
  ItemHelp("Replace existing gives the disc's missile shields random types. Mix it up puts random shields on "
           "doors all over each area instead.");
  ImGui::BeginDisabled(s.blastShieldRandomization != 2);
  int frequency = s.blastShieldFrequency >= 6 ? 2 : s.blastShieldFrequency >= 4 ? 1 : 0;
  if (ImGui::Combo("Blast shield frequency", &frequency, "Low (10%)\0Medium (40%)\0High (60%)\0")) {
    s.blastShieldFrequency = frequency == 0 ? 1 : frequency == 1 ? 4 : 6;
    changed = true;
  }
  ItemHelp("How many of each area's doors take a shield under Mix it up.");
  ImGui::EndDisabled();
  ImGui::BeginDisabled(s.blastShieldRandomization == 0);
  changed |= ImGui::Combo("Blast shield types", &s.blastShieldAvailableTypes, "No beam combos\0All\0");
  ItemHelp("All adds Flamethrower, Ice Spreader and Wavebuster shields, at most one per area.");
  ImGui::EndDisabled();
  changed |= ImGui::SliderInt("Locked doors", &s.lockedDoorCount, 0, 2);
  ItemHelp("How many areas (not Magmoor) get one door locked for good.");
  check("Backwards Lower Mines", s.backwardsLowerMines, "Phazon Mines' lower levels can be entered from the other end.");
  check("Flaahgra power bombs", s.flaahgraPowerBombs, "Flaahgra can be beaten with power bombs.");

  ImGui::SeparatorText("Logic");
  check("Heat damage without the Varia Suit", s.nonVariaHeatDamage, "Hot rooms hurt without Varia, so other suits or tanks can carry you through.");
  changed |= ImGui::Combo("Staggered suit damage", &s.staggeredSuitDamage, "Default\0Progressive\0Additive\0");
  ItemHelp("How the suits split damage between them.");
  int combat = s.combatLogic + 1;
  if (ImGui::Combo("Combat logic", &combat, "None\0Normal\0Minimal\0")) {
    s.combatLogic = combat - 1;
    changed = true;
  }
  ItemHelp("How strictly the logic expects you to have the gear to win fights.");
  int tricks = s.trickDifficulty + 1;
  if (ImGui::Combo("Trick difficulty", &tricks, "None\0Easy\0Medium\0Hard\0")) {
    s.trickDifficulty = tricks - 1;
    changed = true;
  }
  ItemHelp("Sequence breaks the logic may expect, up to this difficulty.");
  if (ImGui::InputText("Allow tricks", sAllow, sizeof(sAllow))) {
    s.trickAllow = SplitNames(sAllow);
    changed = true;
  }
  ItemHelp("Trick names, separated by commas, always expected whatever the difficulty.");
  if (ImGui::InputText("Deny tricks", sDeny, sizeof(sDeny))) {
    s.trickDeny = SplitNames(sDeny);
    changed = true;
  }
  ItemHelp("Trick names, separated by commas, never expected whatever the difficulty.");

  if (changed) {
    {
      std::lock_guard< std::mutex > lock(sRandoMutex);
      sRandoSettings = s;
    }
    MarkDirty();
  }
  if (ImGui::Button("Reset options")) {
    {
      std::lock_guard< std::mutex > lock(sRandoMutex);
      sRandoSettings = PortRandoGen::Settings();
    }
    sTextLoaded = false;
    MarkDirty();
  }

  ImGui::SeparatorText("New seed");
  ImGui::InputTextWithHint("Seed", "empty picks a random one", sSeedText, sizeof(sSeedText));
  ItemHelp("Any text. The same text with the same options makes the same seed.");
  ImGui::SameLine();
  if (ImGui::Button("Random")) {
    sSeedText[0] = '\0';
  }
  // A loaded game keeps its save card until the title screen, so seeds are
  // only switched there (PortAp::PlaySolo refuses too).
  const bool inGame = PortDebug::StateManager() != nullptr;
  const std::string inPlay = PortAp::SoloSeedInPlay();
  if (ImGui::Button(inGame ? "Generate" : "Generate & Play")) {
    PortRandoGen::Seed seed;
    std::string error;
    sMessageError = true;
    if (!PortRandoGen::Generate(s, sSeedText, seed, error) || !PortRandoGen::Save(seed, error)) {
      sMessage = error;
    } else if (inGame) {
      sMessageError = false;
      sMessage = "Saved seed " + seed.name + ". Quit to the title screen to play it.";
    } else if (!PortAp::PlaySolo(seed.name, error)) {
      sMessage = error;
    } else {
      sMessageError = false;
      sMessage = "Playing seed " + seed.name;
    }
    sScanned = false;
  }
  ItemHelp(inGame ? "Makes the seed and saves it. Quit to the title screen to play it."
                  : "Makes the seed, saves it and starts playing it. If an Archipelago session is running, "
                    "it is replaced.");
  if (!sMessage.empty()) {
    ImGui::TextColored(sMessageError ? ImVec4(1.f, 0.4f, 0.4f, 1.f) : ImVec4(0.5f, 1.f, 0.5f, 1.f), "%s",
                       sMessage.c_str());
  }

  ImGui::SeparatorText("Seeds");
  if (sRows.empty()) {
    ImGui::TextDisabled("No seeds yet.");
  } else if (inGame) {
    ImGui::TextDisabled("Quit to the title screen to switch seeds.");
  }
  bool deleted = false;
  for (const RandoSeedRow& row : sRows) {
    const bool current = !inPlay.empty() && PortAp::SameSoloSeed(inPlay, row.name);
    ImGui::PushID(row.name.c_str());
    if (current) {
      ImGui::Text("%s (playing)", row.name.c_str());
    } else {
      ImGui::TextUnformatted(row.name.c_str());
    }
    ImGui::TextDisabled("%s", row.summary.c_str());
    ImGui::BeginDisabled(inGame && !current);
    if (ImGui::Button("Play")) {
      std::string error;
      sMessageError = !PortAp::PlaySolo(row.name, error);
      sMessage = sMessageError ? error : "Playing seed " + row.name;
    }
    ImGui::EndDisabled();
    ItemHelp("Plays this seed with its own save card. A running Archipelago session is replaced. "
             "Seeds are switched at the title screen.");
    ImGui::SameLine();
    if (ImGui::Button("Spoiler")) {
      PortRandoGen::Seed seed;
      std::string error;
      sSpoilerName = row.name;
      sSpoilerNote.clear();
      sSpoilerText = PortRandoGen::Load(PortRandoGen::SeedPath(row.name), seed, error) ? seed.spoiler
                                                                                       : "Could not read: " + error;
    }
    ImGui::SameLine();
    if (ImGui::Button("Write spoiler file")) {
      PortRandoGen::Seed seed;
      std::string error;
      sSpoilerName = row.name;
      if (!PortRandoGen::Load(PortRandoGen::SeedPath(row.name), seed, error)) {
        sSpoilerNote = "Could not read: " + error;
      } else {
        const std::string path = PortRandoGen::SeedDirectory() + "/" + row.name + ".spoiler.txt";
        std::ofstream out(path, std::ios::trunc);
        out << seed.spoiler;
        sSpoilerNote = out.good() ? "Wrote " + path : "Could not write " + path;
      }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(current);
    if (ImGui::Button("Delete")) {
      ImGui::OpenPopup("Delete seed?");
    }
    ImGui::EndDisabled();
    ItemHelp(current ? "This seed is being played. Play another seed or disconnect first."
                     : "Deletes the seed, its progress and its save card.");
    if (ImGui::BeginPopupModal("Delete seed?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::Text("Delete seed %s, its progress and its save card?", row.name.c_str());
      ImGui::TextUnformatted("This can't be undone.");
      if (ImGui::Button("Delete")) {
        std::string error;
        sMessageError = !PortAp::DeleteSolo(row.name, error);
        sMessage = sMessageError ? error : "Deleted seed " + row.name;
        if (sSpoilerName == row.name) {
          sSpoilerText.clear();
          sSpoilerNote.clear();
        }
        sScanned = false;
        deleted = true;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }
    ImGui::PopID();
    ImGui::Separator();
    if (deleted) {
      break; // sRows is read again next frame
    }
  }
  if (!sSpoilerNote.empty()) {
    ImGui::TextWrapped("%s", sSpoilerNote.c_str());
  }
  if (!sSpoilerText.empty()) {
    ImGui::SeparatorText("Spoiler");
    ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "This shows where everything is in seed %s.",
                       sSpoilerName.c_str());
    if (ImGui::Button("Hide")) {
      sSpoilerText.clear();
    } else if (ImGui::BeginChild("randoSpoiler", ImVec2(0, ImGui::GetFontSize() * 24.f), ImGuiChildFlags_Borders)) {
      ImGui::TextUnformatted(sSpoilerText.c_str());
    }
    if (!sSpoilerText.empty()) {
      ImGui::EndChild();
    }
  }
}

const DebugPage kDebugPages[] = {
    {"Game", DrawGameTab},
    {"Controls", DrawControlsTab},
    {"Video", DrawVideoTab},
    {"Remastered", DrawRemasteredTab},
    {"Mods", DrawModsTab},
    {"Randomizer", DrawRandomizerTab},
    {"Archipelago", DrawArchipelagoTab},
    {"Tracker", DrawTrackerTab},
    {"Save states", DrawSaveStatesTab},
    {"System", DrawSystemTab},
    {"Debug", DrawDebugTab},
};

// MP_DEBUG_TAB=<page>[/<sub-tab>], case-insensitive. The old page names are
// kept: Input, Render, Performance, Extras, States, Session and Chat.
struct StartTarget {
  std::string page;
  std::string sub;
};

const StartTarget& GetStartTarget() {
  static const StartTarget target = [] {
    StartTarget t;
    const char* env = std::getenv("MP_DEBUG_TAB");
    if (env == nullptr || *env == '\0') {
      return t;
    }
    std::string raw = env;
    if (const size_t slash = raw.find('/'); slash != std::string::npos) {
      t.sub = raw.substr(slash + 1);
      raw.resize(slash);
    }
    struct Alias {
      const char* from;
      const char* page;
      const char* sub;
    };
    static const Alias kAliases[] = {
        {"Input", "Controls", ""},         {"Render", "Video", ""},
        {"Performance", "Video", "Frame rate"}, {"Extras", "Game", ""},
        {"States", "Save states", ""},     {"Session", "Archipelago", "Connection"},
        {"Chat", "Archipelago", "Chat"},
    };
    t.page = raw;
    for (const Alias& alias : kAliases) {
      if (SDL_strcasecmp(raw.c_str(), alias.from) == 0) {
        t.page = alias.page;
        if (t.sub.empty()) {
          t.sub = alias.sub;
        }
      }
    }
    return t;
  }();
  return target;
}

// A page's inner tab, opened first when MP_DEBUG_TAB names it. The caller ends
// the item with EndTabItem when this returns true.
bool SubTab(const char* page, const char* name) {
  static bool pending = !GetStartTarget().sub.empty();
  const StartTarget& target = GetStartTarget();
  const bool start = pending && SDL_strcasecmp(target.page.c_str(), page) == 0 &&
                     SDL_strcasecmp(target.sub.c_str(), name) == 0;
  const bool shown = ImGui::BeginTabItem(name, nullptr, start ? ImGuiTabItemFlags_SetSelected : 0);
  if (start) {
    pending = false;
  }
  return shown;
}

// The innermost window under the finger that can actually scroll vertically,
// climbing out of child windows (a table, the page list) that cannot.
ImGuiWindow* ScrollableWindowAt(ImGuiWindow* window) {
  for (; window != nullptr; window = window->ParentWindow) {
    if (window->ScrollMax.y > 0.f && (window->Flags & ImGuiWindowFlags_NoScrollWithMouse) == 0) {
      return window;
    }
    if ((window->Flags & ImGuiWindowFlags_ChildWindow) == 0) {
      break;
    }
  }
  return nullptr;
}

bool IsResizeGrip(ImGuiWindow* window, ImGuiID id) {
  for (int n = 0; n < 4; ++n) {
    if (id == ImGui::GetWindowResizeCornerID(window, n) ||
        id == ImGui::GetWindowResizeBorderID(window, static_cast< ImGuiDir >(n))) {
      return true;
    }
  }
  return false;
}

// ImGui has no touch scrolling: a finger dragged down a page presses whatever
// it landed on and scrolls nothing. A mostly vertical drag is taken away from
// the widget it began on (so a button under it does not fire on release) and
// scrolls the window instead, and the finger's speed carries on as a fling
// after it lifts. A mostly horizontal drag is left alone, so sliders still
// work, and so are the scrollbar, a window being moved or resized, and drags
// that start outside the content area. Runs after NewFrame, before any window.
struct TouchScroll {
  ImGuiWindow* window = nullptr;
  bool decided = false;
  bool dragging = false;
  float velocity = 0.f; // pixels per second, positive scrolls down
};
TouchScroll sTouchScroll;

void UpdateTouchScroll() {
  ImGuiContext& g = *ImGui::GetCurrentContext();
  const ImGuiIO& io = g.IO;
  TouchScroll& scroll = sTouchScroll;
  const bool touch = io.MouseSource == ImGuiMouseSource_TouchScreen;

  if (io.MouseClicked[0]) {
    scroll = TouchScroll{};
    if (touch) {
      ImGuiWindow* window = ScrollableWindowAt(g.HoveredWindow);
      if (window != nullptr && window->InnerClipRect.Contains(io.MouseClickedPos[0])) {
        scroll.window = window;
      }
    }
  }
  if (scroll.window == nullptr) {
    return;
  }

  if (io.MouseDown[0]) {
    if (!scroll.decided) {
      const ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.f);
      // A share of the font size rather than io.MouseDragThreshold's fixed
      // pixels, which on a dense phone screen is a jitter, not a drag.
      const float threshold = g.FontSize * 0.6f;
      if (delta.x * delta.x + delta.y * delta.y < threshold * threshold) {
        return;
      }
      scroll.decided = true;
      ImGuiWindow* activeWindow = g.ActiveIdWindow;
      const bool ownDrag =
          g.MovingWindow != nullptr ||
          (g.ActiveId != 0 && activeWindow != nullptr &&
           (g.ActiveId == ImGui::GetWindowScrollbarID(activeWindow, ImGuiAxis_X) ||
            g.ActiveId == ImGui::GetWindowScrollbarID(activeWindow, ImGuiAxis_Y) ||
            IsResizeGrip(activeWindow, g.ActiveId)));
      if (std::fabs(delta.y) <= std::fabs(delta.x) || ownDrag) {
        scroll.window = nullptr;
        return;
      }
      scroll.dragging = true;
    }
    if (!scroll.dragging) {
      return;
    }
    if (g.ActiveId != 0) {
      ImGui::ClearActiveID();
    }
    ImGui::SetScrollY(scroll.window, scroll.window->Scroll.y - io.MouseDelta.y);
    if (io.DeltaTime > 0.f) {
      // Smoothed, so the last jittery frame before the finger lifts does not
      // decide the whole fling.
      const float instant = -io.MouseDelta.y / io.DeltaTime;
      scroll.velocity += (instant - scroll.velocity) * 0.4f;
    }
    return;
  }

  // Released: keep scrolling at the finger's speed, easing off.
  if (!scroll.dragging) {
    scroll.window = nullptr;
    return;
  }
  const float y = scroll.window->Scroll.y;
  const bool atEdge = (scroll.velocity < 0.f && y <= 0.f) ||
                      (scroll.velocity > 0.f && y >= scroll.window->ScrollMax.y);
  if (atEdge || std::fabs(scroll.velocity) < g.FontSize) {
    scroll = TouchScroll{};
    return;
  }
  ImGui::SetScrollY(scroll.window, y + scroll.velocity * io.DeltaTime);
  scroll.velocity *= std::exp(-4.f * io.DeltaTime);
}

// A finger that lifts leaves ImGui's cursor where it was, so whatever was last
// tapped stays drawn as hovered. Move the cursor off-screen once the release
// has been seen; queued now, it lands on the next frame.
void ClearTouchHover() {
  ImGuiIO& io = ImGui::GetIO();
  if (io.MouseSource == ImGuiMouseSource_TouchScreen && io.MouseReleased[0] &&
      !ImGui::IsAnyMouseDown()) {
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
  }
}

// Full screen inside the display's safe area (clear of the notch and the
// gesture bars), no title bar to drag, a Close button a thumb can hit, and a
// page list down the side in place of a tab strip too narrow to tap.
bool DrawPageWindow() {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImVec2 pos = viewport->WorkPos;
  ImVec2 size = viewport->WorkSize;
  SDL_Window* window = MainWindow();
  SDL_Rect safe;
  int windowWidth = 0, windowHeight = 0;
  if (window != nullptr && SDL_GetWindowSafeArea(window, &safe) &&
      SDL_GetWindowSize(window, &windowWidth, &windowHeight) && windowWidth > 0 &&
      windowHeight > 0 && safe.w > 0 && safe.h > 0) {
    // ImGui's display size need not be in window coordinates.
    const float sx = size.x / static_cast< float >(windowWidth);
    const float sy = size.y / static_cast< float >(windowHeight);
    pos = ImVec2(pos.x + safe.x * sx, pos.y + safe.y * sy);
    size = ImVec2(safe.w * sx, safe.h * sy);
  }
  ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                      ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_NoScrollbar |
                                      ImGuiWindowFlags_NoScrollWithMouse;
  bool open = true;
  if (ImGui::Begin("Metroid Prime Port##touch", nullptr, kFlags)) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const char* const kClose = "Close";
    const float closeWidth = ImGui::CalcTextSize(kClose).x + style.FramePadding.x * 4.f;
    const char* const kTitle = "Metroid Prime Port";
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(kTitle);
    const char* const kBuild = MP_BUILD_REVISION;
    const float buildWidth = ImGui::CalcTextSize(kBuild).x;
    const bool buildFits = ImGui::GetContentRegionAvail().x >
                           ImGui::CalcTextSize(kTitle).x + buildWidth + closeWidth +
                               style.ItemSpacing.x * 5.f;
    if (buildFits) {
      ImGui::SameLine(0.f, style.ItemSpacing.x * 2.f);
      ImGui::TextDisabled("%s", kBuild);
    }
#if !defined(__ANDROID__)
    // Only when it fits beside the title, the build and the Close button.
    const char* const kKeys = "F1: hide   F10: frame limit   F12: screenshot";
    if (ImGui::GetContentRegionAvail().x > ImGui::CalcTextSize(kTitle).x + buildWidth +
                                               ImGui::CalcTextSize(kKeys).x + closeWidth +
                                               style.ItemSpacing.x * 8.f) {
      ImGui::SameLine(0.f, style.ItemSpacing.x * 3.f);
      ImGui::TextDisabled("%s", kKeys);
    }
#endif
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - closeWidth);
    if (ImGui::Button(kClose, ImVec2(closeWidth, 0.f))) {
      open = false;
    }
    ImGui::Separator();

    static int sPage = 0;
    // MP_DEBUG_TAB=<name> opens on that page, for captures of the overlay.
    static bool sStartPending = true;
    if (sStartPending) {
      for (int i = 0; i < static_cast< int >(ARRAY_SIZE(kDebugPages)); ++i) {
        if (SDL_strcasecmp(GetStartTarget().page.c_str(), kDebugPages[i].name) == 0) {
          sPage = i;
        }
      }
      sStartPending = false;
    }
    float listWidth = 0.f;
    for (const DebugPage& page : kDebugPages) {
      listWidth = std::max(listWidth, ImGui::CalcTextSize(page.name).x);
    }
    listWidth += style.FramePadding.x * 2.f + style.WindowPadding.x * 2.f;
    const float rowHeight = ImGui::GetFrameHeight() * 1.2f;
    if (ImGui::BeginChild("##pages", ImVec2(listWidth, 0.f), ImGuiChildFlags_Borders)) {
      for (int i = 0; i < static_cast< int >(ARRAY_SIZE(kDebugPages)); ++i) {
        if (ImGui::Selectable(kDebugPages[i].name, sPage == i, ImGuiSelectableFlags_None,
                              ImVec2(0.f, rowHeight))) {
          sPage = i;
        }
      }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    // Keyed by page, so each page keeps its own scroll position.
    ImGui::PushID(sPage);
    if (ImGui::BeginChild("##page", ImVec2(0.f, 0.f), ImGuiChildFlags_Borders)) {
      kDebugPages[sPage].draw();
    }
    ImGui::EndChild();
    ImGui::PopID();
  }
  ImGui::End();
  return open;
}

bool DrawDesktopWindow() {
  ImGui::SetNextWindowPos(ImVec2(8.f, 8.f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(440.f, 200.f), ImGuiCond_FirstUseEver);
  if (port::EnvString("MP_DEBUG_TAB") != nullptr) // a capture wants to see the tab
    ImGui::SetNextWindowSize(ImVec2(520.f, 620.f), ImGuiCond_Once);
  bool open = true;
  if (ImGui::Begin("Metroid Prime Port", &open, ImGuiWindowFlags_MenuBar)) {
    if (ImGui::BeginMenuBar()) {
      const char* const kKeys = "F1: hide   F10: frame limit   F12: screenshot";
      ImGui::TextUnformatted(kKeys);
      // Only when it fits beside the hotkey hint.
      const ImGuiStyle& style = ImGui::GetStyle();
      if (ImGui::GetContentRegionAvail().x >
          ImGui::CalcTextSize(MP_BUILD_REVISION).x + style.ItemSpacing.x * 3.f) {
        ImGui::SameLine(0.f, style.ItemSpacing.x * 3.f);
        ImGui::TextDisabled("%s", MP_BUILD_REVISION);
      }
      ImGui::EndMenuBar();
    }

    if (ImGui::BeginTabBar("##debug_tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
      // MP_DEBUG_TAB=<name> opens on that tab, for captures of the overlay.
      static bool sStartPending = true;
      for (const DebugPage& page : kDebugPages) {
        const bool start =
            sStartPending && SDL_strcasecmp(GetStartTarget().page.c_str(), page.name) == 0;
        if (ImGui::BeginTabItem(page.name, nullptr, start ? ImGuiTabItemFlags_SetSelected : 0)) {
          if (start)
            sStartPending = false;
          page.draw();
          ImGui::EndTabItem();
        }
      }
      ImGui::EndTabBar();
    }
  }
  ImGui::End();
  return open;
}

void DrawUI() {
  EnsureInitialized();
  if (!sAudioSettingsApplied) {
    // Apply persisted audio mutes once the backends are alive.
    sAudioSettingsApplied = true;
    SetAiAudioEnabled(sAiAudioEnabled);
    SetMusyxAudioEnabled(sMusyxAudioEnabled);
  }
  if (!sPresentationSettingsApplied) {
    // Apply persisted vsync once the swapchain surface exists (first drawn
    // frame), so the present mode is chosen from real surface capabilities.
    sPresentationSettingsApplied = true;
    aurora_enable_vsync(sVsyncEnabled && !sTurbo);
  }
  if (sFullscreenHotkey.exchange(false, std::memory_order_acq_rel)) {
    SetFullscreen(!VIGetWindowFullscreen());
  }
#if !defined(__ANDROID__)
  else if (const int window = sWindowFullscreen.exchange(-1, std::memory_order_acq_rel);
           window >= 0 && (window != 0) != sFullscreen) {
    sFullscreen = window != 0;
    MarkDirty();
  }
#endif
  ProcessCardPicks();
  ProcessGpuDriverPick();
#if !defined(__ANDROID__)
  // Every frame, not only with the panel open: an importer stops when its
  // output pipe fills.
  PortImporters::Poll();
#endif
  FinishRemasteredImport();
  DrawStaleImportToast();
  DrawUpdateToast();
  DrawDiscReadFailedAlert();
  DrawShaderCompilationToast();
  if (sTouchLayoutSavePending.exchange(false, std::memory_order_acq_rel)) {
    MarkDirty();
    SaveSettings();
  }
  if (!sVisible) {
    UpdateMenuSounds(false);
    sTouchScroll = TouchScroll{};
    if (sGalleryOpen) {
      CloseGallery();
    }
    FreeRetiredGalleryTextures(false);
    return;
  }

  UpdateTouchScroll();
  const bool open = PageLayout() ? DrawPageWindow() : DrawDesktopWindow();
  ClearTouchHover();
  DrawGalleryWindow();

  if (!open) {
    sVisible = false;
  }
  UpdateMenuSounds(sVisible);

  // Not while a control is held: a dragged slider changes a setting every frame, and
  // rewriting the settings file each time is a flash write per frame on a phone. It is
  // saved the frame the control is let go.
  if (sSettingsDirty && !ImGui::IsAnyItemActive()) {
    SaveSettings();
  }
}

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

#if defined(__ANDROID__)
// The touch overlay covers the display and consumes every touch before SDL
// sees it. While the debug overlay is open the game is paused and those touches
// belong to ImGui, so the Java side asks this and stops claiming them.
#if defined(__ANDROID__)
// The pad itself - descriptor, mapping, axis conversion, attach and detach -
// lives in platform/touch_pad.cpp so that it can be built and tested on the
// host. It used to be here, inside this #if, which meant it was never compiled
// anywhere except an Android build and no test could ever have caught a wrong
// button mapping. See tests/touch_pad.cpp.
namespace {
PortTouchPad::Pad g_touchPad;

PortTouchPad::Pad& TouchPad() {
  if (!g_touchPad.ok()) {
    g_touchPad = PortTouchPad::Attach();
    __android_log_print(ANDROID_LOG_INFO, "touchpad", "attached id=%d gamepad=%d open=%d",
                        g_touchPad.id, SDL_IsGamepad(g_touchPad.id) ? 1 : 0,
                        g_touchPad.ok() ? 1 : 0);
    if (!g_touchPad.ok())
      __android_log_print(ANDROID_LOG_ERROR, "touchpad", "attach failed: %s", SDL_GetError());
  }
  return g_touchPad;
}
} // namespace

// KNOWN LIMITATION: a short tap can be missed.
//
// The virtual joystick API is state-sampling, not event-queueing. Setting a
// button stores the latest value and marks it changed (SDL_virtualjoystick.c:401);
// the change is only delivered at the next update, which sends whatever the
// value is *then* (:742). So a press and release that both land between two
// updates leave only the release, and the game never sees the press. A quick tap
// on A or Start can therefore do nothing, most visibly while a game frame is
// stalled. Triggers behave the same way.
//
// This is not a data race - the setters hold SDL's joystick mutex, so nothing
// tears - and it is not specific to this port; it is how SDL's virtual joystick
// works. Sustained presses and ordinary releases are unaffected, which is why it
// has not shown up as "controls don't work".
//
// Fixing it properly means latching a press until the game has sampled it, and
// the latch has to be released on an update the port does not control. That is a
// real design problem, not a two-line patch, so it is recorded rather than
// half-solved. A missed tap is recoverable by tapping again; a control that fires
// when it should not is not.

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeVirtualButton(JNIEnv*, jclass, jint button,
                                                                 jboolean down) {
  if (SDL_Joystick* pad = TouchPad().handle) {
    SDL_SetJoystickVirtualButton(pad, static_cast< int >(button), down == JNI_TRUE);
  }
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeVirtualAxis(JNIEnv*, jclass, jint axis,
                                                               jfloat value) {
  if (SDL_Joystick* pad = TouchPad().handle) {
    SDL_SetJoystickVirtualAxis(pad, static_cast< int >(axis), PortTouchPad::AxisValue(value));
  }
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchAim(JNIEnv*, jclass, jfloat dxDp,
                                                            jfloat dyDp) {
  PortDebug::AddTouchAim(dxDp, dyDp);
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchAimDown(JNIEnv*, jclass, jboolean down) {
  PortDebug::SetTouchAimDown(down == JNI_TRUE);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchAimEnabled(JNIEnv*, jclass) {
  return PortDebug::TouchAim() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchClassic(JNIEnv*, jclass) {
  return PortDebug::TouchClassic() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchTwinStick(JNIEnv*, jclass) {
  return PortDebug::TouchTwinStick() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchWheelsEnabled(JNIEnv*, jclass) {
  return PortDebug::TouchWheels() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchVisorTapScan(JNIEnv*, jclass) {
  return PortDebug::TouchVisorTapScan() ? JNI_TRUE : JNI_FALSE;
}

// Bits 0-3: visors owned (Combat, X-Ray, Scan, Thermal); 4-7: beams owned
// (Power, Ice, Wave, Plasma); 8-9 current visor; 10-11 current beam; 12: valid.
extern "C" JNIEXPORT jint JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeWheelOwned(JNIEnv*, jclass) {
  return static_cast< jint >(PortDebug::WheelState());
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeRequestVisor(JNIEnv*, jclass, jint visor) {
  PortDebug::RequestVisor(visor);
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeRequestBeam(JNIEnv*, jclass, jint beam) {
  PortDebug::RequestBeam(beam);
}

// {width, height, ARGB pixels...} of a wheel icon (wheel 0 visor, 1 beam; item in the same order as
// nativeRequestVisor/nativeRequestBeam), or null until the HUD has loaded it.
extern "C" JNIEXPORT jintArray JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeWheelIcon(JNIEnv* env, jclass, jint wheel,
                                                             jint item) {
  if (wheel < 0 || wheel > 1 || item < 0 || item > 3) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(sWheelIconMutex);
  const WheelIcon& icon = sWheelIcons[wheel][item];
  if (icon.argb.empty()) {
    return nullptr;
  }
  const jsize count = static_cast<jsize>(icon.argb.size());
  jintArray out = env->NewIntArray(2 + count);
  if (out == nullptr) {
    return nullptr;
  }
  const jint dims[2] = {icon.w, icon.h};
  env->SetIntArrayRegion(out, 0, 2, dims);
  env->SetIntArrayRegion(out, 2, count, reinterpret_cast<const jint*>(icon.argb.data()));
  return out;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchMapTapEnabled(JNIEnv*, jclass) {
  return PortDebug::TouchMapTap() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeMinimapRect(JNIEnv* env, jclass,
                                                              jfloatArray out) {
  // x0, y0, x1, y1, then 1 when the minimap is drawn there (0: draw a map button).
  float rect[5];
  bool drawn = true;
  if (!PortDebug::TouchMapTap() || !PortDebug::MinimapRect(rect, &drawn) || out == nullptr ||
      env->GetArrayLength(out) < 5) {
    return JNI_FALSE;
  }
  rect[4] = drawn ? 1.f : 0.f;
  env->SetFloatArrayRegion(out, 0, 5, rect);
  return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeMapPan(JNIEnv*, jclass, jfloat dxDp,
                                                         jfloat dyDp, jfloat viewHeightDp) {
  PortDebug::AddMapPan(dxDp, dyDp, viewHeightDp, 250);
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeMapRotate(JNIEnv*, jclass, jfloat radians) {
  PortDebug::AddMapRotate(radians);
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeMapZoom(JNIEnv*, jclass, jfloat ratio) {
  PortDebug::AddMapZoom(ratio);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeMapScreenOpen(JNIEnv*, jclass) {
  return PortDebug::MapScreenOpen() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativePauseScreenOpen(JNIEnv*, jclass) {
  return PortDebug::PauseScreenOpen() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeMapTap(JNIEnv*, jclass) {
  if (PortDebug::TouchMapTap()) {
    PortDebug::RequestMapTap();
  }
}
#endif

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTapUpdateToast(JNIEnv*, jclass, jfloat x, jfloat y) {
  return PortDebug::TapUpdateToast(x, y) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeDebugOverlayVisible(JNIEnv*, jclass) {
  return PortDebug::OverlayVisible() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchBeamShift(JNIEnv*, jclass, jboolean held) {
  sTouchBeamShift.store(held == JNI_TRUE, std::memory_order_release);
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchTurboFire(JNIEnv*, jclass, jboolean held) {
  sTouchTurboFire.store(held == JNI_TRUE, std::memory_order_release);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchTurbo(JNIEnv*, jclass) {
  return PortDebug::TouchTurboFlag() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeSetTouchDevice(JNIEnv*, jclass, jboolean xbox) {
  sTouchActive.store(true, std::memory_order_release);
  PortPrompts::NoteTouchInput(xbox == JNI_TRUE);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchColors(JNIEnv*, jclass) {
  return PortDebug::TouchColorsFlag() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchLabels(JNIEnv*, jclass) {
  return PortDebug::TouchLabelsFlag() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchFloatingStick(JNIEnv*, jclass) {
  return PortDebug::TouchFloatingStickFlag() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jfloat JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchSideMarginDp(JNIEnv*, jclass) {
  return PortDebug::TouchSideMarginDp();
}

extern "C" JNIEXPORT jfloat JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchStickInsetDp(JNIEnv*, jclass) {
  return PortDebug::TouchStickInsetDp();
}

extern "C" JNIEXPORT jfloat JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchButtonInsetDp(JNIEnv*, jclass) {
  return PortDebug::TouchButtonInsetDp();
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchLayout(JNIEnv* env, jclass) {
  return env->NewStringUTF(PortDebug::TouchLayout().c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeSetTouchLayout(JNIEnv* env, jclass,
                                                                  jstring layout) {
  if (layout == nullptr) {
    return;
  }
  const char* chars = env->GetStringUTFChars(layout, nullptr);
  if (chars != nullptr) {
    PortDebug::SetTouchLayout(chars);
    env->ReleaseStringUTFChars(layout, chars);
  }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTouchEditRequested(JNIEnv*, jclass) {
  return PortDebug::TakeTouchEditRequested() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeToggleDebugOverlay(JNIEnv*, jclass) {
  PortDebug::RequestToggle();
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_MetroidPrimeActivity_nativeTexturePackStatus(JNIEnv* env, jclass,
                                                                       jstring status) {
  const char* chars = env->GetStringUTFChars(status, nullptr);
  if (chars != nullptr) {
    PortDebug::SetTexturePackStatus(chars);
    env->ReleaseStringUTFChars(status, chars);
  }
}

// The copy landed in <user root>.new; the next frame swaps it in.
extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_MetroidPrimeActivity_nativeTexturePackReady(JNIEnv*, jclass) {
  PortTextures::RequestUserPackReload();
}

// A file picked for the Remastered import, while this process lived. The
// address is also in RemasteredPickFile, for when it did not.
extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_MetroidPrimeActivity_nativeRemasteredPicked(JNIEnv* env, jclass, jint which,
                                                                      jstring uri) {
  const char* chars = env->GetStringUTFChars(uri, nullptr);
  if (chars != nullptr) {
    std::lock_guard lock(PortDebug::sRemasteredPickMutex);
    PortDebug::sRemasteredPicks.emplace_back(int(which), chars);
    env->ReleaseStringUTFChars(uri, chars);
  }
}

// Whether a real pad, keyboard or mouse was used since the last call.
extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTakePhysicalInput(JNIEnv*, jclass) {
  return sPhysicalInput.exchange(false, std::memory_order_acq_rel) ? JNI_TRUE : JNI_FALSE;
}
#endif
