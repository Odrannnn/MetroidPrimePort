// F1 overlay: Windows drawn over the game outside the overlay pages: toasts, the photo mode bar,
// the resume prompt and alerts. State and helpers shared with the other parts are in
// debug_ui_internal.h.

#include "debug_ui_internal.h"

#include "port_env.h"
#include "port_debug.h"
#include "port_graphics_backend.h"
#include "port_gyro_motion.h"
#include "port_freecam.h"
#include "port_photo.h"
#include "port_pointer.h"
#include "port_input_devices.h"
#include "port_input_remap_ui.h"
#include "port_mods.h"
#include "port_discord.h"
#include "port_update_check.h"
#include "port_savestate.h"
#include "port_build_info.h"

#include <aurora/gfx.h>
#include <aurora/imgui.h>
#include <imgui_internal.h>
#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_mouse.h>

#if defined(PORT_TOUCH_CONTROLS)
#include "touch_pad.h"
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace PortDebug {

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
    ImGui::TextWrapped("%s was imported by an older version. Re-import Remastered (F1 > Mods > "
                       "Built-in mods) to get the latest fixes.",
                       name);
  }
  ImGui::End();
}

namespace {
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

// Top-centre toasts stack downwards in draw order instead of overlapping: each takes the
// next free y and, once drawn, pushes it below itself. Reset at the start of every frame.
float sTopToastNextY = 0.f;

} // namespace

void BeginTopToastStack() { sTopToastNextY = 16.f; }


namespace {
void PlaceTopToast(const ImGuiViewport* viewport) {
  ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + sTopToastNextY),
                          ImGuiCond_Always, ImVec2(0.5f, 0.f));
}

// Call between Begin and End of a toast placed with PlaceTopToast.
void TopToastPlaced(const ImGuiViewport* viewport) {
  sTopToastNextY = ImGui::GetWindowPos().y - viewport->Pos.y + ImGui::GetWindowSize().y + 8.f;
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
} // namespace

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

namespace {
// Once per frame on the game thread: hands the touches to the free camera, and records where the
// photo windows are (`windows`) and whether fingers may fly the camera now.
void PhotoTouchFrame(bool enabled, std::vector<ImVec4> windows) {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  float moveX = 0.f, moveY = 0.f, yaw = 0.f, pitch = 0.f;
  {
    std::lock_guard lock(sPhotoTouchMutex);
    PhotoTouch& t = sPhotoTouch;
    t.enabled = enabled;
    t.windows = std::move(windows);
    if (viewport->Size.y > 0.f) {
      t.aspect = viewport->Size.x / viewport->Size.y;
    }
    if (!enabled) {
      t.stickDown = t.lookDown = false;
      t.moveX = t.moveY = 0.f;
    }
    moveX = t.moveX;
    moveY = t.moveY;
    yaw = t.yaw;
    pitch = t.pitch;
    t.yaw = t.pitch = 0.f;
  }
  if (PortPhoto::Active()) {
    PortFreeCam::SetTouchMove(moveX, moveY);
    if (enabled) {
      PortFreeCam::AddTouchTurn(yaw, pitch);
    }
  }
}

// The window just drawn, as fractions of the main viewport.
ImVec4 PhotoWindowRect(const ImGuiViewport* viewport) {
  const ImVec2 pos = ImGui::GetWindowPos();
  const ImVec2 size = ImGui::GetWindowSize();
  if (viewport->Size.x <= 0.f || viewport->Size.y <= 0.f) {
    return ImVec4();
  }
  return ImVec4((pos.x - viewport->Pos.x) / viewport->Size.x, (pos.y - viewport->Pos.y) / viewport->Size.y,
                (pos.x + size.x - viewport->Pos.x) / viewport->Size.x,
                (pos.y + size.y - viewport->Pos.y) / viewport->Size.y);
}
} // namespace

// The photo mode panel (top right, compact) and its messages (a toast under the others).
void DrawPhotoUI() {
  std::vector<ImVec4> touchWindows;
  const bool touchEnabled = PortPhoto::Active() && !PortPhoto::Capturing() && !OverlayVisible();
  struct TouchFrameAtExit {
    const bool& enabled;
    std::vector<ImVec4>& windows;
    ~TouchFrameAtExit() { PhotoTouchFrame(enabled, std::move(windows)); }
  } touchFrame{touchEnabled, touchWindows};
  PortPhoto::Frame();
  static std::string sMessage;
  static double sMessageAt = 0.0;
  const double now = ImGui::GetTime();
  if (std::string text = PortPhoto::TakeMessage(); !text.empty()) {
    sMessage = std::move(text);
    sMessageAt = now;
  }
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  if (!sMessage.empty() && now - sMessageAt < 4.0 && !PortPhoto::Capturing()) {
    PlaceTopToast(viewport);
    ImGui::SetNextWindowSize(ImVec2(std::min(viewport->Size.x - 32.f, 560.f), 0.f));
    ImGui::SetNextWindowBgAlpha(0.8f);
    if (ImGui::Begin("##photo-toast", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoInputs)) {
      ImGui::TextWrapped("%s", sMessage.c_str());
      TopToastPlaced(viewport);
    }
    ImGui::End();
  }
  if (!PortPhoto::Active() || PortPhoto::Capturing()) {
    return;
  }
  // Keyboard and mouse: H panel, P or a left click while looking captures, Esc exits, the wheel
  // zooms. A captured mouse is the camera's, so the click can't be meant for a widget.
  static bool sWasLeftDown = false;
  const bool leftDown = (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) != 0;
  const bool leftClicked = leftDown && !sWasLeftDown;
  sWasLeftDown = leftDown;
  if (!TouchActive() && !OverlayVisible() && !ImGui::GetIO().WantTextInput) {
    if (ImGui::IsKeyPressed(ImGuiKey_H, false)) {
      PortPhoto::SetPanelShown(!PortPhoto::PanelShown());
    }
    if (ImGui::IsKeyPressed(ImGuiKey_P, false) || (leftClicked && MouseCaptured())) {
      PortPhoto::RequestCapture();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      PortPhoto::RequestExit();
    }
    const float wheel = ImGui::GetIO().MouseWheel;
    if (wheel != 0.f && (MouseCaptured() || !ImGui::GetIO().WantCaptureMouse)) {
      PortPhoto::SetFov(PortPhoto::Fov() - wheel * 5.f);
    }
  }
  if (!PortPhoto::PanelShown()) {
    // The touch controls are gone in photo mode, so a touchscreen gets a small bar for it.
    if (TouchActive()) {
      const float margin = 12.f * sUiAppliedScale;
      ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x - margin, viewport->Pos.y + margin),
                              ImGuiCond_Always, ImVec2(1.f, 0.f));
      ImGui::SetNextWindowBgAlpha(0.6f);
      if (ImGui::Begin("##photo-bar", nullptr,
                       ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::Button("Panel")) {
          PortPhoto::SetPanelShown(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Capture")) {
          PortPhoto::RequestCapture();
        }
        ImGui::SameLine();
        if (ImGui::Button("Exit")) {
          PortPhoto::RequestExit();
        }
        touchWindows.push_back(PhotoWindowRect(viewport));
      }
      ImGui::End();
    }
    return;
  }
  // Sized in scaled units so it fits a phone (density ~3). The touch controls hide in photo mode.
  const float margin = 12.f * sUiAppliedScale;
  const float width = std::min(viewport->Size.x - 2.f * margin, 320.f * sUiAppliedScale);
  ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x - margin, viewport->Pos.y + margin),
                          ImGuiCond_Always, ImVec2(1.f, 0.f));
  // Height 0 fits the content each frame; the width stays fixed so wide content wraps, not overflows.
  ImGui::SetNextWindowSize(ImVec2(width, 0.f));
  ImGui::SetNextWindowBgAlpha(0.85f);
  // The pad's d-pad and A also fly, roll and capture, so the panel only navigates while it has the
  // pad (Y); the nav cursor starts on the first widget when it takes it.
  const bool padFocus = PortPhoto::PanelFocused();
  static bool sWasPadFocus = false;
  if (padFocus && !sWasPadFocus) {
    ImGui::SetNextWindowFocus();
    ImGui::SetNavCursorVisible(true);
  }
  sWasPadFocus = padFocus;
  if (ImGui::Begin("Photo mode##photo", nullptr,
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                       ImGuiWindowFlags_NoSavedSettings | (padFocus ? 0 : ImGuiWindowFlags_NoNav))) {
    float fov = PortPhoto::Fov();
    if (ImGui::SliderFloat("FOV", &fov, 20.f, 120.f, "%.0f")) {
      PortPhoto::SetFov(fov);
    }
    float roll = PortPhoto::Roll();
    if (ImGui::SliderFloat("Roll", &roll, -180.f, 180.f, "%.0f")) {
      PortPhoto::SetRoll(roll);
    }
    bool samus = PortPhoto::ShowSamus();
    if (ImGui::Checkbox("Show Samus", &samus)) {
      PortPhoto::SetShowSamus(samus);
    }
    float speed = PortPhoto::Speed();
    if (ImGui::SliderFloat("Speed", &speed, 1.f, 100.f, "%.0f m/s")) {
      PortPhoto::SetSpeed(speed);
      MarkDirty();
    }
    int factor = PortPhoto::Supersample();
    ImGui::TextUnformatted("Size");
    for (const int f : {1, 2, 4}) {
      ImGui::SameLine();
      char label[8];
      std::snprintf(label, sizeof(label), "%dx##ss", f);
      if (ImGui::RadioButton(label, factor == f)) {
        PortPhoto::SetSupersample(f);
        MarkDirty();
      }
    }
    if (ImGui::Button("Capture")) {
      PortPhoto::RequestCapture();
    }
    ImGui::SameLine();
    if (ImGui::Button("Hide panel")) {
      PortPhoto::SetPanelShown(false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Exit")) {
      PortPhoto::RequestExit();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    if (TouchActive()) {
      ImGui::TextWrapped("Touch: left half moves, right half looks around");
    } else if (padFocus) {
      ImGui::TextWrapped("Pad: d-pad move, A change, Y or B back to the camera");
    } else {
      ImGui::TextWrapped("Pad: A capture, B exit, Y panel, d-pad roll, X+d-pad FOV");
      ImGui::TextWrapped("Mouse: hold right button to look, click captures, wheel FOV; P capture, H panel, "
                         "Esc exit");
    }
    ImGui::PopStyleColor();
    touchWindows.push_back(PhotoWindowRect(viewport));
  }
  ImGui::End();
}

// Quick resume's question (CFrontEndUI keeps the decision; the pad and keyboard answer on the game
// side). A window in the middle of the screen that draws with the F1 overlay hidden and takes touch
// and mouse; ImGui navigation stays off so the pad is not read twice.
void DrawResumePrompt() {
  PortSaveState::ResumeInfo info;
  bool yesSelected = true;
  if (!PortSaveState::ResumePromptShown(info, yesSelected) || OverlayVisible()) {
    return;
  }
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  const float scale = sUiAppliedScale;
  ImGui::GetBackgroundDrawList(const_cast<ImGuiViewport*>(viewport))
      ->AddRectFilled(viewport->Pos, ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
                      IM_COL32(0, 0, 0, 150));
  const float width = std::min(viewport->Size.x - 24.f * scale, 440.f * scale);
  ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(width, 0.f));
  ImGui::SetNextWindowBgAlpha(0.92f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.f * scale, 16.f * scale));
  if (ImGui::Begin("##resume-prompt", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
    const char* title = "Resume where you left off?";
    if (info.crashedAgain) {
      title = "The game closed unexpectedly again after resuming here.\nResume anyway?";
    } else if (info.afterCrash) {
      title = "The game closed unexpectedly.\nResume where you left off?";
    }
    ImGui::SetWindowFontScale(1.3f);
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextUnformatted(title);
    ImGui::PopTextWrapPos();
    ImGui::SetWindowFontScale(1.f);
    ImGui::Spacing();
    std::string where = info.world;
    if (!where.empty() && !info.room.empty()) {
      where += " - ";
    }
    where += info.room;
    ImGui::TextWrapped("%s", where.c_str());
    char when[32] = "";
    const std::time_t at = static_cast<std::time_t>(info.savedAt);
    if (const std::tm* tm = std::localtime(&at)) {
      std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", tm);
    }
    ImGui::TextDisabled("%s", when);
    ImGui::Spacing();
    ImGui::Spacing();
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const ImVec2 button((ImGui::GetContentRegionAvail().x - gap) * 0.5f, 56.f * scale);
    for (int i = 0; i < 2; ++i) {
      const bool yes = i == 0;
      const bool highlighted = yes == yesSelected;
      if (i > 0) {
        ImGui::SameLine();
      }
      if (highlighted) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
      }
      ImGui::SetWindowFontScale(1.2f);
      if (ImGui::Button(yes ? "Yes" : "No", button)) {
        PortSaveState::AnswerResumePrompt(yes);
      }
      ImGui::SetWindowFontScale(1.f);
      if (highlighted) {
        ImGui::PopStyleColor();
      }
    }
  }
  ImGui::End();
  ImGui::PopStyleVar();
}

namespace {
// ShowNote's text, set from the input poll and timed from the first frame that draws it.
std::mutex sNoteMutex;
std::string sNote;
bool sNoteNew = false;
} // namespace

void ShowNote(const std::string& text) {
  std::lock_guard<std::mutex> lock(sNoteMutex);
  sNote = text;
  sNoteNew = true;
}

void DrawNoteToast() {
  static std::string sText;
  static double sShownAt = -1.0;
  const double now = ImGui::GetTime();
  {
    std::lock_guard<std::mutex> lock(sNoteMutex);
    if (sNoteNew) {
      sText = sNote;
      sNoteNew = false;
      sShownAt = now;
    }
  }
  if (sText.empty() || now - sShownAt > 2.5) {
    return;
  }
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  PlaceTopToast(viewport);
  ImGui::SetNextWindowBgAlpha(0.8f);
  if (ImGui::Begin("##note-toast", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted(sText.c_str());
    TopToastPlaced(viewport);
  }
  ImGui::End();
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
  PlaceTopToast(viewport);
  ImGui::SetNextWindowSize(ImVec2(std::min(viewport->Size.x - 32.f, 560.f), 0.f));
  ImGui::SetNextWindowBgAlpha(0.8f);
  if (ImGui::Begin("##update-toast", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::TextWrapped("Version %s is out (this is %s). Click or tap here to open its release page.",
                       PortUpdateCheck::Latest().version.c_str(), MP_BUILD_VERSION);
    TopToastPlaced(viewport);
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

// Under D3D11 (old GPUs: D3D12 and Vulkan failed), the seed's shader builds wait while no
// game is loaded. Those drivers run every object creation under one lock, so the front end's
// loads waited behind each build: a crawling title screen and file select for minutes
// (issue #54: GTX 550 Ti, 0.2-1.7 s per shader). In game it loads little and runs smoothly.
// Aurora starts them held under D3D11.
namespace {
bool sBackgroundShadersHeld = false;
} // namespace

void HoldBackgroundShadersOnFrontEnd() {
  // MP_HOLD_FRONTEND_SHADERS=1/0 forces it on/off on any backend.
  static const bool sHoldHere = port::EnvFlag("MP_HOLD_FRONTEND_SHADERS", aurora_get_backend() == BACKEND_D3D11);
  static bool sSynced = false;
  const bool hold = sHoldHere && sStateManager == nullptr;
  if (!sSynced || hold != sBackgroundShadersHeld) {
    sSynced = true;
    sBackgroundShadersHeld = hold;
    aurora_set_background_pipelines_paused(hold);
  }
}

// While the startup compile runs (the pipeline cache and shipped seed, queued before the first
// frame): after half a second, a count and a bar since the toast opened. Once that queue has
// drained, shaders met in game compile without it.
void DrawShaderCompilationToast() {
  static double sQueuedSince = -1.0;
  static uint32_t sBase = 0; // createdPipelines when the toast opened
  static bool sStartupDone = false;
  const AuroraStats* stats = aurora_get_stats();
  if (stats != nullptr && stats->queuedPipelines == 0) {
    sStartupDone = true;
  }
  if (!sShowShaderCompilation || stats == nullptr || sStartupDone || sBackgroundShadersHeld) {
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
  PlaceTopToast(viewport);
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
    TopToastPlaced(viewport);
  }
  ImGui::End();
}

// The panel's hotkey hint, from the live Port menu/Screenshot bindings.
std::string HotkeyHint() {
  std::string hint;
  const std::string menu = PortInputRemap::BindingsText(PortInput::Action::PortMenu, " / ");
  if (!menu.empty()) hint += menu + ": hide   ";
  hint += "F10: frame limit";
  const std::string shot = PortInputRemap::BindingsText(PortInput::Action::Screenshot, " / ");
  if (!shot.empty()) hint += "   " + shot + ": screenshot";
  return hint;
}

// Once per launch, for 8 s, until the panel is first opened: how to open it. Top left,
// clear of the other toasts (top and bottom centre) and Android's START/MENU buttons.
void DrawMenuHintToast() {
  static double sShownAt = -1.0;
  static bool sDone = false;
  if (sDone || !sShowMenuHint) {
    return;
  }
  if (sVisible) {
    sDone = true;
    return;
  }
  const double now = ImGui::GetTime();
  if (sShownAt < 0.0) {
    sShownAt = now;
  }
  const double age = now - sShownAt;
  if (age > 8.0) {
    sDone = true;
    return;
  }
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + 16.f, viewport->Pos.y + 16.f), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.7f);
  // Fades out over the last second.
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, static_cast<float>(std::clamp(8.0 - age, 0.0, 1.0)));
  if (ImGui::Begin("##menu-hint-toast", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_AlwaysAutoResize)) {
    const std::string keys = PortInputRemap::BindingsText(PortInput::Action::PortMenu);
#if defined(PORT_TOUCH_CONTROLS)
    if (keys.empty()) {
      ImGui::TextUnformatted("Tap the cog button for settings");
    } else {
      ImGui::Text("Tap the cog button (or %s) for settings", keys.c_str());
    }
#else
    if (keys.empty()) {
      ImGui::TextUnformatted("Open settings from the Port menu binding in Remap");
    } else {
      ImGui::Text("Press %s for settings", keys.c_str());
    }
#endif
  }
  ImGui::End();
  ImGui::PopStyleVar();
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
  PlaceTopToast(viewport);
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
    TopToastPlaced(viewport);
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(3);
}

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

} // namespace PortDebug
