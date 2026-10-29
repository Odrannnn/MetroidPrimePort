// In-game debug overlay. The port draws it with Aurora's ImGui backend, which
// is already initialized and rendered every presented frame, so this only has
// to build the windows between aurora_begin_frame and aurora_end_frame.
//
// The overlay is split across files: this one holds the settings and runtime
// switches the game reads and the state shared across threads; the settings
// file, each page and each section live in platform/debug_ui/<section>.cpp.
// What more than one of them uses is declared in debug_ui/debug_ui_internal.h.

#include "debug_ui/debug_ui_internal.h"

#include "port_build_info.h"
#include "port_discord.h"
#include "port_env.h"
#include "port_gyro_motion.h"
#include "port_input_devices.h"
#include "port_livesplit.h"
#include "port_log.h"
#include "port_paths.h"
#include "port_pointer.h"
#include "port_update_check.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace DebugUi {

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
#if defined(__ANDROID__) || defined(SDL_PLATFORM_IOS)
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
float sDynamicResMin = 1.f; // the lowest scale it drops to, kDynamicResMinLowest to 4
// Sharpening of a frame stretched up to the window: 0 off, 1 while dynamic resolution has
// lowered the scale, 2 whenever the EFB is smaller than the window. Strength 0-100.
int sSharpenMode = 1;
int sSharpenAmount = 50;
float sDynScale = 0.f;      // the scale in use; 0 = sRenderScale
PortDebug::EAspectMode sAspectMode = PortDebug::kAspect_Window;
bool sHudWide = true;
bool sCinemaBars = false;
bool sPhazonShell = true;
bool sSharpScanWindow = false;
bool sGeneratedBallLooks = true;
bool sAutoAim = true;
int sScanSpeed = 100;
int sHintDelay = 0;
bool sSecondaryCrosshairs = true;
// A remapped pad drives menus and the map with its standard buttons (issue #57).
bool sMenuStandardButtons = true;
// Index into kPromptGlyphs; 0 = follow the input in use. Atomic: PortPrompts reads it off the UI thread.
std::atomic< int > sPromptGlyphs{0};
bool sShowShaderCompilation = true;
bool sShowMenuHint = true;
int sHudScale = PortDebug::kHudScaleMax;
#if defined(PORT_TOUCH_CONTROLS)
int sScanSpread = PortDebug::kScanSpreadMin; // touch controls cover the screen edges
#else
int sScanSpread = PortDebug::kScanSpreadMax;
#endif
// Indexed by PortDebug::HudOpacityPart.
int sHudOpacity[PortDebug::kHudOpacityPartCount] = {PortDebug::kHudOpacityVisor, PortDebug::kHudOpacityVisor,
                                                    PortDebug::kHudOpacityVisor, PortDebug::kHudOpacityMax};
int sCrosshairOpacity = PortDebug::kCrosshairOpacityMax;
PortDebug::ECrosshairStyle sCrosshairStyle = PortDebug::kCrosshairStyle_Classic;
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
// Off by default: a beam door that was opened stays blue (PortBeamDoors).
bool sBeamDoorsStayUnlocked = false;
bool sStackBeams = false;
int sWalkSpeed = PortDebug::kWalkSpeedDefault;
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
bool sPhazonBeamCheat = false;
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
// Setting `graphics_backend` (Windows and desktop Linux, issue #135); sGraphicsBackendAtStart
// is what this run was started with, so the Video page can say when a change needs a restart
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
// The parts of the installed import the mods load without (remastered_hide), comma separated:
// PortMods::kImportParts.
std::string sRemasteredHide;

std::string SettingsFilePath() {
  const std::string& dir = PortPaths::UserFolder();
  return (dir.empty() ? std::string("./") : dir) + "port_settings.ini";
}

bool ParseBool(const std::string& value) {
  return value == "1" || value == "true" || value == "on" || value == "yes";
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

} // namespace DebugUi

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
} // namespace

namespace DebugUi {

bool SDLCALL debug_event_watch(void*, SDL_Event* event) {
  PortDebug::PhotoTouchEvent(*event);
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
  const std::string fileSettings = SettingsText();

  // Environment variables are explicit per-run overrides and win over the file.
  if (port::EnvFlag("MP_TRACE_TIMING")) {
    sTraceTiming = true;
  }
  if (port::EnvFlag("MP_FAST_BOOT")) {
    sFastBoot = true;
  }
  if (const char* language = port::EnvString("MP_LANGUAGE")) {
    sTextLanguage = language;
  }
  if (port::EnvFlag("MP_TURBO")) {
    sTurbo = true;
    const int ticks = port::EnvInt("MP_TURBO", 0);
    if (ticks >= 1 && ticks <= 16) {
      sTurboTicks = static_cast< unsigned >(ticks);
    }
  }
  if (const char* present = port::EnvString("MP_PRESENT_T")) {
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
  if (port::EnvString("MP_GODMODE") != nullptr) {
    sInvulnerableRun = port::EnvFlag("MP_GODMODE") ? 1 : 0;
  }
  if (port::EnvFlag("MP_SHOW_DEBUG_UI")) {
    sVisible = true;
  }
  if (const char* aspect = port::EnvString("MP_ASPECT")) {
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
  sGeneratedBallLooks = port::EnvFlag("MP_POWER_SPIDER_BALL", sGeneratedBallLooks);
  sAutoAim = port::EnvFlag("MP_AUTO_AIM", sAutoAim);
  sScanSpeed = std::clamp(port::EnvInt("MP_SCAN_SPEED", sScanSpeed), PortDebug::kScanSpeedMin,
                          PortDebug::kScanSpeedMax);
  sHintDelay = std::clamp(port::EnvInt("MP_HINT_DELAY", sHintDelay), PortDebug::kHintDelayMin,
                          PortDebug::kHintDelayMax);
  sSecondaryCrosshairs = port::EnvFlag("MP_SECONDARY_CROSSHAIRS", sSecondaryCrosshairs);
  if (const char* style = std::getenv("MP_CROSSHAIR_STYLE")) {
    if (std::strcmp(style, "classic") == 0) {
      sCrosshairStyle = PortDebug::kCrosshairStyle_Classic;
    } else if (std::strcmp(style, "remastered") == 0) {
      sCrosshairStyle = PortDebug::kCrosshairStyle_Remastered;
    } else if (std::strcmp(style, "gamecube") == 0) {
      sCrosshairStyle = PortDebug::kCrosshairStyle_GameCube;
    }
  }
  sMenuStandardButtons = port::EnvFlag("MP_MENU_STANDARD_BUTTONS", sMenuStandardButtons);
  PortPointer::SetEnabled(port::EnvFlag("MP_POINTER_MENUS", PortPointer::Enabled()));
  PortPointer::SetMapSwapFingers(port::EnvFlag("MP_MAP_SWAP_FINGERS", PortPointer::MapSwapFingers()));
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
  if (port::EnvString("MP_FPS_CAP") != nullptr) {
    sFpsCap = ClampFpsCap(port::EnvInt("MP_FPS_CAP", 0));
  }
  NoteEnvOverrides(fileSettings);

  std::atexit(SaveSettings);
  ApplyLiveSplit();
  ApplyDiscord();
  ApplyUpdateCheck();
  PortUpdateCheck::CheckNow(); // once per launch
}
} // namespace DebugUi

namespace PortDebug {

void MarkSettingsDirty() { MarkDirty(); }

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
    PortLog::Write("metroid_prime_port: saved disc image %s\n", sDiscPath.c_str());
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
