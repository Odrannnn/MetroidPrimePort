// Photo mode (pause menu, F8, F1 > Game, console `photo`); see port_photo.h.
#include "port_photo.h"

#include "port_debug.h"
#include "port_freecam.h"
#include "Kyoto/Input/CFinalInput.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Cameras/CGameCamera.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include <dolphin/gx/GXAurora.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_timer.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#if defined(__ANDROID__)
#include <SDL3/SDL_system.h>
#include <filesystem>
#include <jni.h>
#endif

namespace aurora {
void request_screenshot_png(const char* path) noexcept;
int screenshot_png_state(uint32_t* width, uint32_t* height) noexcept;
bool frame_buffer_size_for_scale(float scale, uint32_t* width, uint32_t* height) noexcept;
uint32_t max_texture_dimension() noexcept;
} // namespace aurora

namespace PortPhoto {
namespace {

constexpr float kRollRate = 60.f;  // degrees per second on the d-pad
constexpr float kFovRate = 40.f;   // degrees per second with X held
constexpr float kFovMin = 20.f;
constexpr float kFovMax = 120.f;
constexpr uint64_t kMaxCapturePixels = 48'000'000;  // a 7680x4320 frame is 33 M
constexpr uint64_t kMaxCaptureSamples = 96'000'000; // pixels x MSAA samples; 5124x2880 at 4x is 59 M
constexpr int kSettleFrames = 8;        // frames for the EFB to resize and draw at the new size
constexpr int kHideFrames = 2;          // frames with the panel and toasts gone before the shot
constexpr double kSaveTimeoutSeconds = 20.0;
constexpr double kPendingTimeoutSeconds = 0.4;
constexpr double kAfterPauseTimeoutSeconds = 10.0;

enum class Phase { Idle, Hiding, Settling, Saving };

bool sActive = false;
bool sPanelShown = true;
bool sPanelFocus = false;
bool sRequestEnter = false;
bool sRequestExit = false;
bool sAfterPause = false;
double sRequestAt = 0.0;
double sAfterPauseAt = 0.0;
int sSupersample = 1;
float sSpeed = 10.f;
const void* sWorld = nullptr;
std::string sMessage;

// What the free camera was doing when photo mode took it over.
struct Prior {
  bool active = false;
  bool frozen = false;
  bool showPlayer = true;
  float speed = 10.f;
  float fov = 0.f;
  PortFreeCam::Pose pose{};
};
Prior sPrior;

Phase sPhase = Phase::Idle;
int sPhaseFrames = 0;
double sPhaseAt = 0.0;
int sCaptureFactor = 1;
CaptureResult sLast;

double Now() { return static_cast<double>(SDL_GetTicks()) * 0.001; }

void Say(const std::string& text) {
  sMessage = text;
  std::fprintf(stderr, "[photo] %s\n", text.c_str());
}

bool CanEnter(CStateManager& mgr, std::string& why) {
  const CPlayer* player = mgr.GetPlayer();
  const CWorld* world = mgr.GetWorld();
  if (player == nullptr || world == nullptr || gpGameState == nullptr) {
    why = "no game running";
    return false;
  }
  if (mgr.GetWantsToQuit()) {
    why = "the game is quitting";
    return false;
  }
  if (player->IsPlayerDeadEnough() || player->GetDeathTime() > 0.f ||
      mgr.GetPlayerState()->GetHealthInfo().GetHP() <= 0.f) {
    why = "Samus is dead";
    return false;
  }
  if (mgr.GetDeferredStateTransition() != kSMT_InGame) {
    why = "the game is changing screens";
    return false;
  }
  return true;
}

// The smallest render scale whose frame buffer is at least `factor` x the current one in both
// directions. Exactly N x is out of reach: the buffer is the scaled window height with the width
// rounded from the aspect, then fitted to the render mode's 854:480, so each rounding lands on
// whole pixels (1280x719 at 1x, 5124x2880 at 4x). Returns 0 when it can't be had.
float ScaleForFactor(int factor, float base, uint32_t w1, uint32_t h1, uint32_t maxDim) {
  // Scales a few frame-buffer pixels apart give the same size, so step finely from the nominal.
  const float step = base / static_cast<float>(std::max<uint32_t>(h1, 1) * 2);
  float scale = base * static_cast<float>(factor);
  for (int i = 0; i < 64; ++i, scale += step) {
    uint32_t w = 0;
    uint32_t h = 0;
    if (!aurora::frame_buffer_size_for_scale(scale, &w, &h)) {
      return 0.f;
    }
    if (w < w1 * factor || h < h1 * factor) {
      continue;
    }
    // Over the device's texture limit, or past what the GPU can hold (the EFB, its MSAA copy and
    // the screenshot readback all scale with it).
    const uint64_t pixels = static_cast<uint64_t>(w) * h;
    if (w > maxDim || h > maxDim || pixels > kMaxCapturePixels ||
        pixels * static_cast<uint64_t>(std::max(PortDebug::Msaa(), 1)) > kMaxCaptureSamples) {
      return 0.f;
    }
    return scale;
  }
  return 0.f;
}

// Picks the scale for the pending capture, stepping the factor down (4 -> 2 -> 1) when the larger
// size won't fit, and says so.
float PickCaptureScale() {
  const float base = PortDebug::BaseRenderScale() > 0.f ? PortDebug::BaseRenderScale() : 1.f;
  uint32_t w1 = 0;
  uint32_t h1 = 0;
  AuroraGetRenderSize(&w1, &h1);
  const uint32_t limit = aurora::max_texture_dimension();
  const uint32_t maxDim = limit > 0 ? limit : 8192u;
  const int wanted = sCaptureFactor;
  float scale = 0.f;
  for (; sCaptureFactor > 1; sCaptureFactor /= 2) {
    scale = ScaleForFactor(sCaptureFactor, base, w1, h1, maxDim);
    if (scale > 0.f) {
      break;
    }
  }
  sLast.clamped = sCaptureFactor != wanted;
  if (sLast.clamped) {
    Say("A " + std::to_string(wanted) + "x capture is too large for this GPU, taking " +
        std::to_string(sCaptureFactor) + "x");
  }
  return sCaptureFactor > 1 ? scale : 0.f;
}

#if defined(__ANDROID__)
// The photo is written under the app's private folder; MetroidPrimeActivity.exportPhoto moves it
// to the shared Pictures/Metroid Prime, where the gallery finds it. Returns that place, or "" when
// it stays where it was.
std::string ExportToPictures(const std::string& path) {
  std::error_code ec;
  const std::string absolute = std::filesystem::absolute(path, ec).string();
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (ec || env == nullptr || activity == nullptr) {
    return {};
  }
  std::string result;
  jclass cls = env->GetObjectClass(activity);
  if (jmethodID method = env->GetMethodID(cls, "exportPhoto", "(Ljava/lang/String;)Ljava/lang/String;")) {
    jstring jpath = env->NewStringUTF(absolute.c_str());
    auto shared = static_cast<jstring>(env->CallObjectMethod(activity, method, jpath));
    if (!env->ExceptionCheck() && shared != nullptr) {
      const char* chars = env->GetStringUTFChars(shared, nullptr);
      result = chars != nullptr ? chars : "";
      env->ReleaseStringUTFChars(shared, chars);
    }
    if (shared != nullptr) {
      env->DeleteLocalRef(shared);
    }
    env->DeleteLocalRef(jpath);
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    result.clear();
  }
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(activity);
  return result;
}
#endif

} // namespace

bool Active() { return sActive; }

bool Enter(CStateManager& mgr, std::string& why) {
  if (sActive) {
    return true;
  }
  if (!CanEnter(mgr, why)) {
    return false;
  }
  sPrior.active = PortFreeCam::Active();
  sPrior.frozen = PortFreeCam::Frozen();
  sPrior.showPlayer = PortFreeCam::ShowPlayer();
  sPrior.speed = PortFreeCam::Speed();
  sPrior.fov = PortFreeCam::FovOverride();
  sPrior.pose = PortFreeCam::GetPose();
  if (!sPrior.active) {
    PortFreeCam::SetActive(true, &mgr);
  }
  // The photo's own start: where the game camera is, not where a free camera was left.
  if (sPrior.active) {
    PortFreeCam::SetActive(false, &mgr);
    PortFreeCam::SetActive(true, &mgr);
  }
  PortFreeCam::SetFrozen(true);
  PortFreeCam::SetShowPlayer(true);
  PortFreeCam::SetSpeed(sSpeed);
  PortFreeCam::Pose pose = PortFreeCam::GetPose();
  pose.roll = 0.f;
  PortFreeCam::SetPose(pose);
  // The game camera's field of view, before the widening to the window's aspect.
  const float fov = mgr.GetCameraManager()->GetCurrentCamera(mgr).GetUnwidenedFov();
  PortFreeCam::SetFovOverride(std::clamp(std::isfinite(fov) ? fov : 55.f, kFovMin, kFovMax));
  sWorld = mgr.GetWorld();
  sPanelShown = true;
  sPanelFocus = false;
  sActive = true;
  return true;
}

void Exit() {
  if (!sActive) {
    return;
  }
  sActive = false;
  sPanelFocus = false;
  sPhase = Phase::Idle;
  PortDebug::SetCaptureRenderScale(0.f);
  PortFreeCam::SetFovOverride(0.f);
  PortFreeCam::SetFrozen(sPrior.frozen);
  PortFreeCam::SetShowPlayer(sPrior.showPlayer);
  PortFreeCam::SetSpeed(sPrior.speed);
  if (sPrior.active) {
    PortFreeCam::SetPose(sPrior.pose);
    PortFreeCam::SetFovOverride(sPrior.fov);
  } else {
    PortFreeCam::SetActive(false, nullptr);
  }
  sWorld = nullptr;
}

void RequestEnter() {
  if (!sActive) {
    sRequestEnter = true;
    sRequestExit = false;
    sRequestAt = Now();
  }
}

void RequestExit() {
  if (sActive) {
    sRequestExit = true;
    sRequestEnter = false;
  }
}

void RequestToggle() {
  if (sActive) {
    RequestExit();
  } else {
    RequestEnter();
  }
}

void RequestEnterAfterPause() {
  sAfterPause = true;
  sAfterPauseAt = Now();
}

bool EnterPending() { return sAfterPause; }

void Tick(CStateManager& mgr, bool inPlay) {
  if (sActive) {
    std::string why;
    if (sRequestExit || !inPlay || mgr.GetWorld() != sWorld || mgr.GetWantsToQuit() ||
        mgr.GetPlayer() == nullptr) {
      Exit();
    }
    sRequestExit = false;
    sRequestEnter = false;
    sAfterPause = false;
    return;
  }
  sRequestExit = false;
  if (!inPlay) {
    return;
  }
  if (sRequestEnter || sAfterPause) {
    sRequestEnter = false;
    sAfterPause = false;
    std::string why;
    if (!Enter(mgr, why)) {
      Say("Can't start photo mode: " + why);
    }
  }
}

void Frame() {
  const double now = Now();
  // Tick only runs while the game plays: a request made over a menu or a load has no taker.
  if (sRequestEnter && now - sRequestAt > kPendingTimeoutSeconds) {
    sRequestEnter = false;
    Say("Can't start photo mode: the game isn't in play");
  }
  if (sAfterPause && now - sAfterPauseAt > kAfterPauseTimeoutSeconds) {
    sAfterPause = false;
  }
  if (sActive && PortDebug::StateManager() == nullptr) {
    Exit();
  }
  if (!sActive) {
    if (sPhase != Phase::Idle) {
      sPhase = Phase::Idle;
      PortDebug::SetCaptureRenderScale(0.f);
    }
    return;
  }
  switch (sPhase) {
  case Phase::Idle:
    break;
  case Phase::Hiding:
    if (++sPhaseFrames >= kHideFrames) {
      const float scale = PickCaptureScale();
      sLast.scale = scale;
      PortDebug::SetCaptureRenderScale(sCaptureFactor > 1 ? scale : 0.f);
      sPhase = Phase::Settling;
      sPhaseFrames = 0;
    }
    break;
  case Phase::Settling:
    if (++sPhaseFrames >= (sCaptureFactor > 1 ? kSettleFrames : 2)) {
      const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
      sLast.path = "screenshots/metroid-prime-photo-" + std::to_string(stamp) + ".png";
#if defined(__ANDROID__)
      // SDL opens a relative path as an APK asset there, which can't be written.
      if (const char* root = SDL_GetAndroidInternalStoragePath()) {
        sLast.path = std::string(root) + "/" + sLast.path;
      }
#endif
      aurora::request_screenshot_png(sLast.path.c_str());
      sPhase = Phase::Saving;
      sPhaseAt = now;
    }
    break;
  case Phase::Saving: {
    unsigned w = 0;
    unsigned h = 0;
    const int state = aurora::screenshot_png_state(&w, &h);
    if (state == 0 && now - sPhaseAt < kSaveTimeoutSeconds) {
      break;
    }
    PortDebug::SetCaptureRenderScale(0.f);
    sPhase = Phase::Idle;
    sLast.state = state > 0 ? 1 : -1;
    sLast.width = w;
    sLast.height = h;
    if (state > 0) {
#if defined(__ANDROID__)
      if (std::string shared = ExportToPictures(sLast.path); !shared.empty()) {
        sLast.path = std::move(shared);
      }
#endif
      Say("Saved " + sLast.path + " (" + std::to_string(w) + "x" + std::to_string(h) + ")" +
          (sLast.clamped ? ", smaller than asked: too large for this GPU" : ""));
    } else {
      Say("Couldn't save the photo");
    }
    break;
  }
  }
}

bool Input(const CFinalInput& input) {
  if (!sActive) {
    return PortFreeCam::Input(input);
  }
  if (!PortDebug::OverlayVisible() && sPhase == Phase::Idle && sPanelFocus) {
    // The panel has the pad: ImGui reads it, so only the way out is handled here.
    if (input.PStart()) {
      RequestExit();
    } else if (input.PY() || input.PB()) {
      sPanelFocus = false;
    }
    return true;
  }
  if (!PortDebug::OverlayVisible() && sPhase == Phase::Idle) {
    const float dt = std::clamp(input.Time(), 0.f, 0.1f);
    if (input.PA()) {
      RequestCapture();
    }
    if (input.PB() || input.PStart()) {
      RequestExit();
    }
    if (input.PY()) {
      if (sPanelShown) {
        sPanelFocus = true;
      } else {
        SetPanelShown(true);
      }
    }
    const float side = (input.DDPRight() ? 1.f : 0.f) - (input.DDPLeft() ? 1.f : 0.f);
    if (side != 0.f) {
      if (input.DX()) {
        SetFov(Fov() + side * kFovRate * dt);
      } else {
        SetRoll(Roll() + side * kRollRate * dt);
      }
    }
  }
  // Frozen while a shot is taken, so nothing moves under it.
  if (sPhase != Phase::Idle) {
    return true;
  }
  return PortFreeCam::Input(input);
}

float Fov() {
  const float fov = PortFreeCam::FovOverride();
  return fov > 0.f ? fov : 55.f;
}
void SetFov(float degrees) {
  if (std::isfinite(degrees)) {
    PortFreeCam::SetFovOverride(std::clamp(degrees, kFovMin, kFovMax));
  }
}
float Roll() { return PortFreeCam::GetPose().roll; }
void SetRoll(float degrees) {
  if (std::isfinite(degrees)) {
    PortFreeCam::Pose pose = PortFreeCam::GetPose();
    pose.roll = std::clamp(degrees, -180.f, 180.f);
    PortFreeCam::SetPose(pose);
  }
}
bool ShowSamus() { return PortFreeCam::ShowPlayer(); }
void SetShowSamus(bool show) { PortFreeCam::SetShowPlayer(show); }

float Speed() { return sActive ? PortFreeCam::Speed() : sSpeed; }
void SetSpeed(float speed) {
  if (std::isfinite(speed)) {
    sSpeed = std::clamp(speed, 0.1f, 500.f);
    if (sActive) {
      PortFreeCam::SetSpeed(sSpeed);
    }
  }
}

int Supersample() { return sSupersample; }
void SetSupersample(int factor) { sSupersample = factor >= 4 ? 4 : factor >= 2 ? 2 : 1; }

bool PanelShown() { return sPanelShown; }
void SetPanelShown(bool shown) {
  sPanelShown = shown;
  if (!shown) {
    sPanelFocus = false;
  }
}
// Held right mouse button: the mouse looks around even with the panel up.
static bool RightMouseHeld() {
#ifdef __ANDROID__
  return false;
#else
  return (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_RMASK) != 0;
#endif
}
bool MouseLook() {
#ifdef __ANDROID__
  return false;
#else
  return sActive && sPhase == Phase::Idle && (!sPanelShown || RightMouseHeld());
#endif
}
bool PanelWantsCursor() { return sActive && sPanelShown && !RightMouseHeld(); }
bool PanelFocused() { return sActive && sPanelShown && sPanelFocus; }

bool RequestCapture(int factor) {
  if (!sActive || sPhase != Phase::Idle) {
    return false;
  }
  sCaptureFactor = factor > 0 ? (factor >= 4 ? 4 : factor >= 2 ? 2 : 1) : sSupersample;
  sLast = CaptureResult{};
  sPanelFocus = false;
  sPhase = Phase::Hiding;
  sPhaseFrames = 0;
  return true;
}

bool Capturing() { return sPhase != Phase::Idle; }
CaptureResult LastCapture() { return sLast; }

std::string TakeMessage() {
  std::string text;
  text.swap(sMessage);
  return text;
}

} // namespace PortPhoto
