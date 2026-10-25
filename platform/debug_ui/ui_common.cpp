// F1 overlay: Shared overlay plumbing: page layout, themes, menu sounds, UI scale, the GPU self-
// test, controller navigation, the toggle and help markers. State and helpers shared with the other
// parts are in debug_ui_internal.h.

#include "debug_ui_internal.h"

#include "port_env.h"
#include "port_debug.h"
#include "port_graphics_backend.h"
#include "port_gyro_motion.h"
#include "port_pointer.h"
#include "port_log.h"
#include "port_paths.h"
#include "port_input_devices.h"
#include "port_input_remap_ui.h"
#include "port_mods.h"
#include "port_discord.h"
#include "port_savestate.h"

#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "Kyoto/Graphics/CGX.hpp"
#include "Kyoto/Audio/CSfxManager.hpp"
#include "MetroidPrime/SFX/UI.h"

#include <aurora/imgui.h>
#include <imgui_internal.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_events.h>

#if defined(PORT_TOUCH_CONTROLS)
#include "touch_pad.h"
#endif

#include <algorithm>
#include <atomic>
#include <cmath>

namespace PortDebug {

// State only this part uses.
namespace {
std::atomic< bool > sToggleRequested{false};
} // namespace

// The overlay is a full-screen panel with a page list instead of tabs, which
// fits a touchscreen and reads better on the desktop too. The desktop can go
// back to the old floating tabbed window (System > Overlay as a floating
// window); MP_TOUCH_UI forces the page layout regardless.
bool PageLayout() {
#if defined(PORT_TOUCH_CONTROLS)
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
ImVec4 ThemeWarnColor(const ImVec4& plain) {
  switch (sThemeResolved) {
  case kThemePrime: return ImVec4(1.f, 0.88f, 0.32f, 1.f); // yellow, apart from the amber accent
  case kThemeRemastered: return ImVec4(1.f, 0.82f, 0.38f, 1.f);
  default: return plain;
  }
}

ImVec4 ThemeGoodColor(const ImVec4& plain) {
  switch (sThemeResolved) {
  case kThemePrime: return ImVec4(0.45f, 0.95f, 0.55f, 1.f);
  case kThemeRemastered: return ImVec4(0.45f, 1.f, 0.80f, 1.f);
  default: return plain;
  }
}

ImVec4 ThemeBadColor(const ImVec4& plain) {
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

// The test leaves its own GX state behind: drop the game's cached copy of it, and put back the copy
// clear colour its efb-clear-copy case sets (the game sets it rarely, so it isn't re-sent every frame).
void RestoreGxAfterGpuSelfTest() {
  CGX::ResetGXStatesFull();
  CGraphics::SetViewMatrix();
  CGraphics::PortResendCopyClear();
}

void RunGpuSelfTestIfRequested() {
  // Pipelines compile asynchronously and a draw is skipped until its pipeline is ready, so a request first
  // runs a silent warm-up pass that waits for the test's own pipelines (selftest.cpp), then the logged pass.
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
    // The warm-up's lookups waited for every case's pipeline, so the logged pass cannot lose a draw:
    // a couple of frames between the passes is all that is left to do.
    if (++waited < 2) {
      return;
    }
    stage = Stage::Idle;
    runReal = true;
  } else if (sGpuSelfTestRequested.exchange(false, std::memory_order_acq_rel)) {
    if (aurora_gpu_selftest_run(true)) {
      stage = Stage::WarmingUp;
      waited = 0;
      RestoreGxAfterGpuSelfTest();
    }
    return;
  }
  if (runReal && aurora_gpu_selftest_run(false)) {
    RestoreGxAfterGpuSelfTest();
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
#if defined(PORT_TOUCH_OVERLAY)
    InstallTouchOverlay();
#endif
  }
  UpdateUiScale();
  ApplyThemeIfChanged();
  PortInputDevices::PollPortActions();
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
  sTouchColorsFlag.store(sInput.touchColors, std::memory_order_release);
  sTouchLabelsFlag.store(sInput.touchLabels, std::memory_order_release);
  sTouchTurboFlag.store(sInput.touchTurbo && !sOriginalExperience, std::memory_order_release);
  sTouchFloatingStickFlag.store(sInput.touchFloatingStick, std::memory_order_release);

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
  const s32 padIndex = PADGetIndexForPort(PAD_CHAN0);
  SDL_Gamepad* pad = PADGetSDLGamepadForIndex(padIndex >= 0 ? static_cast< u32 >(padIndex) : 0);
  // While the Controls tab captures a pad input, the pad binds instead of
  // navigating (releasing everything here also ends any nav press in progress).
  // A pad that went away releases everything the same way.
  const bool capturing = pad == nullptr || PortInputRemap::Capturing();
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
  // ImGui activates with FaceDown and cancels with FaceRight. On a Nintendo
  // pad (A on the right, as the game's default mapping there) confirm on A.
  const PADControllerType padType = pad != nullptr ? PADGetControllerTypeForIndex(0) : PAD_TYPE_UNKNOWN;
  const bool nintendo = padType == PAD_TYPE_SWITCH_PROCON || padType == PAD_TYPE_JOYCON_PAIR;
  io.AddKeyEvent(ImGuiKey_GamepadFaceDown, held(nintendo ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH));
  io.AddKeyEvent(ImGuiKey_GamepadFaceRight, held(nintendo ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST));
  io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, held(nintendo ? SDL_GAMEPAD_BUTTON_NORTH : SDL_GAMEPAD_BUTTON_WEST));
  io.AddKeyEvent(ImGuiKey_GamepadFaceUp, held(nintendo ? SDL_GAMEPAD_BUTTON_WEST : SDL_GAMEPAD_BUTTON_NORTH));
  io.AddKeyEvent(ImGuiKey_GamepadL1, held(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
  io.AddKeyEvent(ImGuiKey_GamepadR1, held(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER));
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

// What a setting does (kInlineHelp: printed under it on Android, a tooltip elsewhere).
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

} // namespace PortDebug
