// Internal to the F1 overlay: platform/debug_ui.cpp and the parts in platform/debug_ui/. What more
// than one of those files uses is declared here; anything only one file uses stays static in it.
//
// The settings and runtime switches live in debug_ui.cpp (namespace DebugUi, pulled into the
// global namespace below so the parts read them unqualified), the settings file in settings.cpp,
// and each page or section in its own file inside namespace PortDebug.
#pragma once

#include "port_debug.h"
#include "port_dynamic_res.h"
#include "port_mouse.h"
#include "port_remastered_import.h"
#include "port_strings.h"

#include <SDL3/SDL_events.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class CStateManager;
struct ImGuiWindow;
struct SDL_Window;

namespace DebugUi {

using port::Trim;

// debug_ui.cpp: the settings, runtime switches and the state shared across threads.
extern PortInput::Settings& sInput;
extern bool sInitialized;
extern bool sFastBoot;
extern bool sSkipCutscenes;
extern float sCutsceneSpeed;
extern unsigned sSimRate;
extern bool sSimAdaptive;
extern bool sFrameLimitEnabled;
extern int sFpsCap; // with the 60 FPS cap off; 0 = unlimited
inline constexpr int kFpsCapMin = 30;
inline constexpr int kFpsCapMax = 500;
inline int ClampFpsCap(int fps) { return fps <= 0 ? 0 : std::clamp(fps, kFpsCapMin, kFpsCapMax); }
extern bool sTurbo;
extern unsigned sTurboTicks;
extern bool sTraceTiming;
extern uint64_t sTimingNs;
extern double sActualFps, sActualTps;
extern double sThroughputFps;
extern bool sVsyncEnabled;
extern bool sFullscreen;
extern bool sOverlayWindowed;
extern int sUiTheme;
extern bool sUiSounds;
extern bool sMainLoopRan;
extern float sRenderScale;
extern bool sDynamicRes;
extern int sDynamicResTarget;
extern float sDynamicResMin;
extern int sSharpenMode;
extern int sSharpenAmount;
extern float sDynScale;
extern PortDebug::EAspectMode sAspectMode;
extern bool sHudWide;
extern bool sCinemaBars;
extern bool sSharpScanWindow;
extern bool sGeneratedBallLooks;
extern bool sAutoAim;
extern int sScanSpeed;
extern int sHintDelay;
extern bool sSecondaryCrosshairs;
extern bool sMenuStandardButtons;
extern std::atomic< int > sPromptGlyphs; // index into kPromptGlyphs
inline constexpr const char* kPromptGlyphs[] = {"auto",   "xbox",     "playstation",
                                                "switch", "gamecube", "keyboard"};
extern bool sShowShaderCompilation;
extern bool sShowMenuHint;
extern int sHudScale;
extern int sScanSpread;
extern int sHudOpacity[PortDebug::kHudOpacityPartCount]; // indexed by PortDebug::HudOpacityPart
extern int sCrosshairOpacity;
extern PortDebug::ECrosshairStyle sCrosshairStyle;
extern bool sCrosshairCustomColor;
extern float sCrosshairColor[3];
extern bool sHideHelmet;
extern bool sHideVisorEffects;
extern bool sRevealMap;
extern int sMapPickups;
extern bool sMapLogicColors;
extern bool sMapPickupKinds;
extern bool sMapStationPickups;
extern bool sMapExtraUi;
extern bool sTrackerProgress;
extern int sApSuitDamage;
extern bool sCheats;
extern bool sSkippableCutscenes;
extern bool sBeamDoorsStayUnlocked;
extern bool sStackBeams;
extern int sWalkSpeed;
extern bool sRevision100Compatibility;
extern std::string sTextLanguage;
extern int sElevatorRide;
extern int sBootLogos;
extern bool sSaveStateHotkeys;
extern bool sQuickResume;
extern bool sSaveStateRoomState;
extern std::atomic< bool > sTouchBeamShift;
extern std::atomic< bool > sTouchTurboFire;
extern bool sInvulnerable;
extern bool sPhazonBeamCheat;
extern int sInvulnerableRun;
extern bool sLogFile;
extern std::mutex sRandoMutex;
extern PortRandoGen::Settings sRandoSettings;
extern float sFirstPersonFov;
extern float sGunOffset[3];
extern int sMsaa;
extern int sAnisotropy;
extern bool sPerPixelLighting;
extern bool sSunShadows;
extern bool sProjectedShadows;
extern bool sOpenGles;
extern bool sOpenGlesAtStart;
extern PortDebug::EGraphicsBackend sGraphicsBackend;
extern PortDebug::EGraphicsBackend sGraphicsBackendAtStart;
extern std::string sGpuDriver;
extern std::string sGpuDriverAtStart;
extern bool sUnlockHardMode;
extern int sStorageClamp;
extern int sStorageClampAtStart;
extern bool sStorageClampEnvFromSetting;

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
inline constexpr DriverWorkaround kDriverWorkarounds[] = {
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
inline constexpr size_t kDriverWorkaroundCount = std::size(kDriverWorkarounds);
// `validation` is for one start only: BeginDriverWorkarounds applies it and unticks it.
inline constexpr unsigned kDriverChecksBit = 1u << 3;
static_assert(std::string_view{kDriverWorkarounds[3].key} == "validation");
// Setting `driver_workarounds`: bit i is kDriverWorkarounds[i]; AtStart is what this run's device
// got.
extern unsigned sDriverWorkarounds;
extern unsigned sDriverWorkaroundsAtStart;
extern bool sDriverChecksThisRun;
extern const char* sDriverWorkaroundsEnv;
extern bool sDriverWorkaroundsCleared;
// `driver_workarounds` / MP_DRIVER_WORKAROUNDS: comma-separated names to kDriverWorkarounds bits.
unsigned ParseDriverWorkarounds(std::string_view rest);

extern bool sUnlockFusionSuit;
extern bool sUnlockGalleries;
extern bool sSpeedrunTimer;
extern bool sLiveSplit;
extern std::string sLiveSplitAddress;
extern bool sLiveSplitSplitUpgrades;
extern bool sDiscord;
extern bool sUpdateCheck;
extern bool sModsEnabled;
extern std::string sModsDisabled;
extern bool sOriginalExperience;
extern std::atomic< uint32_t > sWheelMask;
// The touch wheels' icons: ARGB pixels per [wheel][item], filled by the game thread, copied out by
// the Android UI thread.
struct WheelIcon {
  int w = 0;
  int h = 0;
  std::vector< uint32_t > argb;
};
extern std::mutex sWheelIconMutex;
extern WheelIcon sWheelIcons[2][4];
extern std::string sGameOptions;
extern PortMouse::AimState sMouseAimState;
extern PortMouse::ButtonGate sMouseButtonGate;
extern float sMouseFrameX;
extern float sMouseFrameY;
extern float sStickFrameX;
extern float sStickFrameY;
extern bool sSmoothFrames;
extern bool sFrameInterpolation;
extern bool sActorInterpolation;
extern bool sPoseInterpolation;
// Faster on a phone (POCO F8 Ultra: 8-27% more fps in the heaviest rooms), even or mixed on a PC.
inline constexpr bool kRoomGeoResidentDefault = true;
extern bool sRoomGeoResident;
extern bool sParticleInterpolation;
extern float sPresentOverride;
extern bool sAiAudioEnabled;
extern bool sMusyxAudioEnabled;
extern std::atomic< int > sSaveStateHotkey;
extern std::atomic< bool > sFullscreenHotkey;
extern std::atomic< int > sWindowFullscreen;
extern std::atomic< bool > sOverlayVisible;
extern std::atomic< bool > sTouchColorsFlag;
extern std::atomic< bool > sTouchLabelsFlag;
extern std::atomic< bool > sTouchTurboFlag;
extern std::atomic< bool > sTouchFloatingStickFlag;
inline constexpr float kTouchMarginMaxDp = PortInput::kTouchMarginMaxDp;
inline constexpr float kTouchSideMarginDefault = PortInput::kTouchSideMarginDefault;
inline constexpr float kTouchStickInsetDefault = PortInput::kTouchStickInsetDefault;
inline constexpr float kTouchButtonInsetDefault = PortInput::kTouchButtonInsetDefault;
inline constexpr float kTouchOpacityMin = PortInput::kTouchOpacityMin;
inline constexpr float kTouchOpacityMax = PortInput::kTouchOpacityMax;
extern std::atomic< bool > sTouchLayoutSavePending;
extern std::atomic< bool > sTouchEditRequested;
extern std::atomic< bool > sPhysicalInput;
extern std::atomic< bool > sTouchActive;
extern float sSpringFlickLatch;
extern bool sSpringFlickArmed;
extern bool sGyroOverride;
extern float sGyroOverridePitch;
extern float sGyroOverrideYaw;
extern bool sVisible;
extern bool sSettingsDirty;
extern bool sConfigReset;
extern CStateManager* sStateManager;
extern std::string sDiscPath;
extern std::string sRemasteredImagePath;
extern std::string sRemasteredKeysPath;
extern std::string sRemasteredSkip;

std::string SettingsFilePath();
bool ParseBool(const std::string& value);
void MarkDirty();
PortRemastered::ImportParts RemasteredImportParts();
void SetRemasteredImportParts(const PortRemastered::ImportParts& parts);
void ApplyLiveSplit();
void ApplyDiscord();
void ApplyUpdateCheck();
bool SDLCALL debug_event_watch(void*, SDL_Event* event);
// Loads the settings file and the MP_* overrides on first use; every accessor calls it.
void EnsureInitialized();

// The bodies of the plain accessors, for a setting that is one variable and whose accessors do
// nothing else: `bool FastBoot() { return Loaded(sFastBoot); }` reads it once the settings are
// loaded, and `void SetX(bool on) { StoreSetting(sX, on); }` stores it and marks the file for
// saving. Accessors that also clamp, check the Original experience or apply the value are
// written out in full.
template < typename T >
const T& Loaded(const T& value) {
  EnsureInitialized();
  return value;
}
template < typename T, typename V >
void StoreSetting(T& setting, const V& value) {
  EnsureInitialized();
  setting = value;
  MarkDirty();
}

// settings.cpp: port_settings.ini.
void LoadSettings();
void SaveSettings();
std::string SettingsText();
// Records which settings the MP_* overrides changed from fileText (SettingsText
// before they were applied), so SaveSettings keeps the file's values for them.
void NoteEnvOverrides(const std::string& fileText);
std::string LoggedSettingValue(const std::string& key, const std::string& value);
void LogSettingChanges(const std::string& text);
extern std::map< std::string, std::string > sLoggedSettings;
extern bool sSettingsLogged;

} // namespace DebugUi

using namespace DebugUi;

namespace PortDebug {

// Pages and sections, each drawn by the file it is named after.
struct DebugPage {
  const char* name;
  void (*draw)();
};
// search.cpp: the page list, sub-tabs and the settings search.
extern const DebugPage kDebugPages[];
extern const int kDebugPageCount;
// MP_DEBUG_TAB=<page>[/<sub-tab>].
struct StartTarget {
  std::string page;
  std::string sub;
};
const StartTarget& GetStartTarget();
struct SettingsIndexRow {
  const char* page;
  const char* sub;
  const char* section;
  const char* label;
  const char* keywords;
};
extern char sSearch[128];
int TakeJumpPage();
void DrawPageBody(const DebugPage& page);
bool SubTab(const char* page, const char* name);
// A page's section, opened when MP_DEBUG_TAB names it.
bool StartSection(const char* page, const char* name);
void BeginSearchJump();
void EndSearchJump();
void DrawSearchField();
bool Searching();
void DrawSearchResults();
ImGuiWindow* ScrollableWindowAt(ImGuiWindow* window);

// window.cpp
bool TouchScrolling(); // a finger drag is scrolling a page, taken from the widget it began on

// ui_common.cpp
bool PageLayout();
SDL_Window* MainWindow();
extern float sUiAppliedScale;
ImVec4 ThemeWarnColor(const ImVec4& plain = ImVec4(1.f, 0.8f, 0.3f, 1.f));
ImVec4 ThemeGoodColor(const ImVec4& plain = ImVec4(0.4f, 1.f, 0.4f, 1.f));
ImVec4 ThemeBadColor(const ImVec4& plain = ImVec4(1.f, 0.5f, 0.3f, 1.f));
void UpdateMenuSounds(bool visible);
// What a setting does. A tooltip on the desktop; a tap shows none, so Android
// prints it, dimmed, under the setting instead.
#if defined(__ANDROID__)
inline constexpr bool kInlineHelp = true;
#else
inline constexpr bool kInlineHelp = false;
#endif
void ItemHelp(const char* text);
void SameLineAfterHelp();
bool BeginOriginalLocked();
void EndOriginalLocked(bool locked);

// options.cpp
extern PortDynamicRes::Controller sDynController;
extern double sDynGpuMs;
float EffectiveRenderScale();
float DynamicResTop();
// Lines of the frame buffer drawn at `scale` (0 = auto) in the current window.
int DrawnLines(float scale);
int EffectiveMsaa();
void ApplyPresentSharpness();
double DynamicResTargetFps();
void ResetDynamicRes();
enum : unsigned { kGpuTimesDynamicRes = 1, kGpuTimesPanel = 2 };
void WantGpuTimes(unsigned user, bool on);

// runtime.cpp
void SyncGameOptions();
extern bool sDiscReadFailedLastSession;

// file_dialogs.cpp
void ProcessCardPicks();
void DrawMemoryCard();
extern std::mutex sRemasteredPickMutex;
extern std::vector< std::pair< int, std::string > > sRemasteredPicks;
inline constexpr int kPalLanguagesPick = 2;
extern std::string sPalLanguagesPick;
// Which 3 of the same picker: a PAL/JP GameCube or Trilogy image for the full Flaahgra theme.
inline constexpr int kFlaahgraPick = 3;
extern std::string sFlaahgraPick;
// Which 4 of the same picker: the Metroid Dread image for the Dread Samus import (desktop).
inline constexpr int kDreadPick = 4;
// Not a picker: OpenRemasteredPick's descriptor slot for the Remastered key file the Dread import
// opens again, so the Remastered panel's own descriptor stays open (Android).
inline constexpr int kDreadKeysSlot = 5;
extern std::string sDreadPick;
std::string RemasteredPickFile(int which);
void TakeRemasteredPickFiles();
void OpenRemasteredDialog(int which);
std::string OpenRemasteredPick(int which, const std::string& uri);
std::string RemasteredPickName(const std::string& uri);
void DrawModReloadMessage();
extern bool sImportUnloadedMods;
void FinishRemasteredImport();

// gallery.cpp
extern bool sGalleryOpen;
extern std::vector< std::string > sGalleryPaths;
extern int sGalleryIndex;
void ReleaseGalleryTexture();
void FreeRetiredGalleryTextures(bool all);
void CloseGallery();
void DrawGalleryWindow();

// overlays.cpp
void PhotoTouchEvent(const SDL_Event& event);
void DrawStaleImportToast();
void BeginTopToastStack();
void DrawPhotoUI();
void DrawResumePrompt();
void DrawUpdateToast();
#if defined(PORT_TOUCH_OVERLAY)
// touch_overlay.cpp: the SDL event filter that feeds it, and its draw
void InstallTouchOverlay();
void DrawTouchOverlay();
#endif
void HoldBackgroundShadersOnFrontEnd();
void DrawShaderCompilationToast();
std::string HotkeyHint();
void DrawMenuHintToast();
void DrawDiscReadFailedAlert();

// mods.cpp
void DrawImportKeys();
void DrawRemasteredImport();
void DrawDreadSamusImport();
void DrawMods();
// remastered.cpp: the Mods page's built-in mods (Remastered's models, Dread's Samus).
void DrawBuiltinMods();

// game_sections.cpp
void DrawCutscenesSection();
void DrawUnlocksSection();
void DrawSpeedrunSection();
void DrawDiscordSection();
void DrawUpdateSection();
std::string TexturePackStatus();
void SetTexturePackStatus(std::string status);
void PickTexturePack();
void DrawOverlaySection();

// video.cpp
void DrawVideoDisplay();
void ProcessGpuDriverPick();
void DrawVideoQuality();
void DrawVideoCompatibility();
void DrawTexturesSection();

// controls.cpp
void DrawControlsOptions();
void DrawControlsKeyboardMouse();
void DrawControlsController();
void DrawControlsTouchGyro();

// The remaining pages and sections.
void DrawArchipelagoTab();
void DrawAudio();
void DrawFlaahgraTheme();
void DrawVoices();
void DrawCheats();
void DrawFreeCam();
void DrawRendering();
void DrawLogSection();
void DrawDiscCheckSection();
void DrawDebugTab();
void DrawPerformanceTab();
void DrawLanguageSection();
void DrawRemasteredDebug();
void DrawSaveStatesTab();
void DrawTrackerTab();
void DrawRandomizerTab();
void DrawGameTab();
void DrawControlsTab();
void DrawVideoTab();
void DrawModsTab();
void DrawSystemTab();

} // namespace PortDebug
