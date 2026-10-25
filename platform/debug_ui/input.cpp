// F1 overlay: Gyro, mouse aim, touch aim and gestures, the visor/beam wheels and the map gestures.
// State and helpers shared with the other parts are in debug_ui_internal.h.

#include "debug_ui_internal.h"

#include "port_env.h"
#include "port_debug.h"
#include "port_gyro_motion.h"
#include "port_photo.h"
#include "port_pointer.h"
#include "port_log.h"
#include "port_paths.h"
#include "port_config_reset.h"
#include "port_input_devices.h"
#include "port_savestate.h"
#include "port_mouse.h"

#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "Kyoto/Graphics/CModel.hpp"
#include "GuiSys/CGuiModel.hpp"

#include <aurora/gfx.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_timer.h>

#if defined(PORT_TOUCH_CONTROLS)
#include "touch_pad.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <utility>
#include <vector>

namespace PortDebug {

// State only this part uses.
namespace {
std::atomic<uint64_t> sWheelStampNs{0};
// Whether the pause menu (inventory, logbook) is up; the touch overlay shows R there.
std::atomic<bool> sPauseScreenOpen{false};
std::atomic<bool> sPausedDialogOpen{false};
std::atomic<uint64_t> sPauseScreenStampNs{0};
std::atomic<int> sVisorRequest{-1};
std::atomic<uint64_t> sVisorRequestUntilNs{0};
std::atomic<int> sBeamRequest{-1};
std::atomic<uint64_t> sBeamRequestUntilNs{0};
std::atomic<uint64_t> sSpringRequestUntilNs{0};
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
// Pointer aim's pointer, and whether it steered this tick (free look).
PortMouse::PointerAim sPointerAim;
bool sPointerAimFree = false;
// The last update left the pointer free under a lock (lock-on free aim).
bool sPointerAimLocked = false;
// The last update held the pointer still under a lock (UpdateMouseAim's pointerHold).
bool sPointerAimHeld = false;
// A locked update this tick moved the pointer (TickAim's job for the pointer under a lock).
bool sLockPointerRecorded = false;
PortMouse::ButtonGate sMouseMenuGate;
PortMouse::HeldButtons sMouseHeldButtons;
float sMousePendingX = 0.f;
float sMousePendingY = 0.f;
// Gyro travel since the last tick. Kept apart from the mouse's pending delta,
// which is dropped whenever the mouse is not captured (twin stick, phones).
float sGyroPendingX = 0.f;
float sGyroPendingY = 0.f;
// With pointer aim and "Gyro also turns the camera": the gyro's camera turn, in
// the mouse's pixels, kept apart since the pointer takes the rest of the travel.
float sGyroCameraPendingX = 0.f;
float sGyroCameraPendingY = 0.f;
float sGyroCameraFrameX = 0.f;
float sGyroCameraFrameY = 0.f;
// The "Gyro aim" binding, set each frame by the input engine before PollGyro.
PortGyro::Activation sGyroActivation;
// The last tick applied look input. A paused game or a cinematic skips the
// player update, and would then drop what the frames between ticks showed.
bool sAimAppliedLastTick = false;
// The tick's first applied aim update: it keeps the twin stick turn it
// recorded and makes the tick's only pointer edge turn (issue #140).
PortMouse::TickAim sAimTick;
// Gyro state: the phone's sensor is looked up once, so the sensor list is not
// walked on every tick.
bool sPhoneGyroSearched = false;
SDL_Sensor* sPhoneGyro = nullptr;
SDL_Sensor* sPhoneAccel = nullptr;
const char* sGyroStatus = "off";
// Calibration and gravity for the current source, reset when it changes (a
// different pad, or the phone), and whether that source has an accelerometer.
PortGyro::Motion sGyroMotion;
const void* sGyroMotionSource = nullptr;
bool sGyroHasAccel = false;
PortGyro::Filter sGyroFilter;
} // namespace

void SetGyroMode(int mode) {
  EnsureInitialized();
  if (mode < 0 || mode > 4) {
    return;
  }
  sInput.gyroMode = mode;
  MarkDirty();
}

void SetGyroSource(int source) {
  EnsureInitialized();
  if (source < 0 || source > 2) {
    return;
  }
  sInput.gyroSource = source;
  MarkDirty();
}

void SetGyroSensitivity(float scale) {
  EnsureInitialized();
  if (std::isfinite(scale)) {
    sInput.gyroSensitivity = std::clamp(scale, PortInput::kGyroSensitivityMin, PortInput::kGyroSensitivityMax);
    MarkDirty();
  }
}

void SetGyroSensitivityY(float scale) {
  EnsureInitialized();
  if (std::isfinite(scale)) {
    sInput.gyroSensitivityY = std::clamp(scale, PortInput::kGyroSensitivityMin, PortInput::kGyroSensitivityMax);
    MarkDirty();
  }
}

void SetGyroSpace(int space) {
  EnsureInitialized();
  if (space < 0 || space >= PortGyro::kSpaceCount) {
    return;
  }
  sInput.gyroSpace = space;
  MarkDirty();
}

void SetGyroAutoCalibrate(int mode) {
  if (mode >= 0 && mode <= 2) {
    StoreSetting(sInput.gyroAutoCalibrate, mode);
  }
}

namespace {
constexpr double kRecalibrateSeconds = 5.0;
// How long a manual calibration averages the still pad.
constexpr double kManualCalibrationSeconds = 3.0;
// SDL_GetTicksNS() the pending recalibration starts sampling at, or 0.
uint64_t sRecalibrateAtNs = 0;
// SDL_GetTicksNS() the running manual calibration ends at, or 0.
uint64_t sManualCalibrationEndNs = 0;
// Whether auto calibration ran on the last gyro sample.
bool sGyroCalibrating = false;
// Set by the "Reset aim" action, taken by the next aim tick.
std::atomic<bool> sResetAimRequested{false};
// Whether the gyro is feeding the aim (mode on, a source found). Under pointer
// aim it moves the pointer, so the sticks turn the camera.
bool sGyroDrivesAim = false;

uint64_t NsFromSeconds(double seconds) { return static_cast<uint64_t>(seconds * 1e9); }

// Countdown over: average the still pad for a while (auto calibration waits),
// then keep that as the drift, whatever auto calibrate is set to.
void FireRecalibration() {
  const uint64_t now = SDL_GetTicksNS();
  if (sRecalibrateAtNs != 0 && now >= sRecalibrateAtNs) {
    sRecalibrateAtNs = 0;
    sGyroMotion.BeginManualCalibration();
    sManualCalibrationEndNs = now + NsFromSeconds(kManualCalibrationSeconds);
  }
  if (sManualCalibrationEndNs != 0 && now >= sManualCalibrationEndNs) {
    sManualCalibrationEndNs = 0;
    sGyroMotion.EndManualCalibration();
  }
}
} // namespace

void StartGyroRecalibration() {
  sGyroMotion.EndManualCalibration();
  sManualCalibrationEndNs = 0;
  sRecalibrateAtNs = SDL_GetTicksNS() + NsFromSeconds(kRecalibrateSeconds);
}

void CancelGyroRecalibration() { sRecalibrateAtNs = 0; }

float GyroRecalibrationCountdown() {
  FireRecalibration();
  if (sRecalibrateAtNs == 0) {
    return -1.f;
  }
  return static_cast<float>(double(sRecalibrateAtNs - SDL_GetTicksNS()) * 1e-9);
}

float GyroManualCalibrationProgress() {
  FireRecalibration();
  if (sManualCalibrationEndNs == 0 || !sGyroMotion.ManualCalibrating()) {
    return -1.f;
  }
  const double left = double(sManualCalibrationEndNs - SDL_GetTicksNS()) * 1e-9;
  return std::clamp(static_cast<float>(1.0 - left / kManualCalibrationSeconds), 0.f, 1.f);
}

bool GyroCalibrating() { return sGyroCalibrating || sGyroMotion.ManualCalibrating(); }

void ResetGyroCalibration() { sGyroMotion.Reset(); }

void RequestResetAim() { sResetAimRequested.store(true, std::memory_order_release); }

float GyroCalibrationConfidence() {
  return sGyroMotionSource != nullptr && sGyroHasAccel ? sGyroMotion.Confidence() : -1.f;
}

void SetGyroButton(bool held) { sGyroActivation.Update(sInput.gyroMode, held); }

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
// rates in rad/s from the chosen source, after calibration and gyro space;
// false when there is none. dt is the time since the last call.
bool ReadGyroRates(float& pitch, float& yaw, float dt) {
  if (sGyroOverride) {
    pitch = sGyroOverridePitch;
    yaw = sGyroOverrideYaw;
    sGyroStatus = "console";
    return true;
  }
  const bool wantController = sInput.gyroSource == 0 || sInput.gyroSource == 1;
  const bool wantPhone = sInput.gyroSource == 0 || sInput.gyroSource == 2;
  // SDL's sensor axes (x right, y up, z toward the player), rad/s and m/s^2.
  float gyro[3];
  float accel[3];
  bool haveAccel = false;
  const void* source = nullptr;

  if (wantController) {
    // The player's pad, not the first one SDL lists: that can be another pad
    // or a virtual one (Steam Input's, the Android touch pad) with no gyro.
    const s32 padIndex = PADGetIndexForPort(PAD_CHAN0);
    if (SDL_Gamepad* pad = PADGetSDLGamepadForIndex(padIndex >= 0 ? static_cast< u32 >(padIndex) : 0)) {
      if (SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO)) {
        // Asked per pad, so a reconnected or swapped pad gets its sensors on too.
        if (!SDL_GamepadSensorEnabled(pad, SDL_SENSOR_GYRO)) {
          SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
        }
        if (SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, gyro, 3)) {
          source = pad;
          sGyroStatus = "controller";
          if (SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL)) {
            if (!SDL_GamepadSensorEnabled(pad, SDL_SENSOR_ACCEL)) {
              SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
            }
            haveAccel = SDL_GetGamepadSensorData(pad, SDL_SENSOR_ACCEL, accel, 3);
          }
        }
      }
    }
  }

  if (source == nullptr && wantPhone) {
    if (sPhoneGyro == nullptr && !sPhoneGyroSearched) {
      sPhoneGyroSearched = true;
      int count = 0;
      if (SDL_SensorID* ids = SDL_GetSensors(&count)) {
        for (int i = 0; i < count; ++i) {
          const SDL_SensorType type = SDL_GetSensorTypeForID(ids[i]);
          if (type == SDL_SENSOR_GYRO && sPhoneGyro == nullptr) {
            sPhoneGyro = SDL_OpenSensor(ids[i]);
          } else if (type == SDL_SENSOR_ACCEL && sPhoneAccel == nullptr) {
            sPhoneAccel = SDL_OpenSensor(ids[i]);
          }
        }
        SDL_free(ids);
      }
    }
    if (sPhoneGyro != nullptr && SDL_GetSensorData(sPhoneGyro, gyro, 3)) {
      source = sPhoneGyro;
      sGyroStatus = "phone";
      haveAccel = sPhoneAccel != nullptr && SDL_GetSensorData(sPhoneAccel, accel, 3);
      // The phone reports its own axes (x right, y up in its natural
      // orientation), so turn them to the screen's, as SDL does for its
      // accelerometer: landscape would otherwise swap pitch and yaw. Then it
      // is held like a pad.
      const SDL_DisplayID display = SDL_GetPrimaryDisplay();
      const int quarters = (OrientationQuarters(SDL_GetCurrentDisplayOrientation(display)) -
                            OrientationQuarters(SDL_GetNaturalDisplayOrientation(display)) + 4) %
                           4;
      const auto turn = [quarters](float v[3]) {
        const float x = v[0];
        const float y = v[1];
        switch (quarters) {
        case 1:
          v[0] = -y;
          v[1] = x;
          break;
        case 2:
          v[0] = -x;
          v[1] = -y;
          break;
        case 3:
          v[0] = y;
          v[1] = -x;
          break;
        default:
          break;
        }
      };
      turn(gyro);
      if (haveAccel) {
        turn(accel);
      }
    }
  }

  if (source == nullptr) {
    return false;
  }
  if (source != sGyroMotionSource || haveAccel != sGyroHasAccel) {
    sGyroMotion.Reset();
    sGyroFilter.Reset();
    sGyroMotionSource = source;
    sGyroHasAccel = haveAccel;
  }
  // 2 = only in menus: paused, a frontend screen (no free aim running) or F1
  // open. Outside them the learned drift is kept, only learning stops.
  const bool inMenus = Visible() || PausedMenuOpen() || !sMouseGameplayActive;
  const bool calibrate = sInput.gyroAutoCalibrate == 1 || (sInput.gyroAutoCalibrate == 2 && inMenus);
  sGyroCalibrating = haveAccel && calibrate;
  sGyroMotion.Process(gyro, haveAccel ? accel : nullptr, dt, sInput.gyroSpace, calibrate, pitch, yaw);
  sGyroFilter.Apply(pitch, yaw, dt, sInput.gyroSmoothing, sInput.gyroTightening);
  return true;
}
} // namespace

static void PointerAimScale(float& sx, float& sy);

void PollGyro() {
  EnsureInitialized();
  // Called once per presented frame, which is not once per tick when the
  // frame limiter is off, so the rates integrate over the real frame time.
  static uint64_t sLastNs = 0;
  const uint64_t nowNs = SDL_GetTicksNS();
  const float dt = sLastNs == 0 ? 0.f : std::min(float(double(nowNs - sLastNs) * 1e-9), 0.1f);
  sLastNs = nowNs;
  FireRecalibration();
  sGyroDrivesAim = false;
  if (sSpringFlickLatch > 0.f) {
    sSpringFlickLatch -= dt;
  }
  // Gyro feeds the same aim state the mouse, twin stick and the touch layout's
  // drag aim use, so aiming only has an effect where that is driving the
  // camera. Flicks need no aim. The raw sInput.twinStick: TwinStick() reads false
  // while touch is in use, yet touch keeps the direct aim path.
  const bool aim = sInput.gyroMode != 0 && (sInput.mouseAim || sInput.twinStick || TouchDirectAim());
  if (!aim && !sInput.springFlick) {
    sGyroStatus = sInput.gyroMode == 0 ? "off" : "needs mouse aim, twin stick or touch controls";
    return;
  }

  float yaw = 0.f;
  float pitch = 0.f;
  if (!ReadGyroRates(pitch, yaw, dt)) {
    sGyroStatus = "no gyro found";
    sSpringFlickArmed = true;
    return;
  }
  if (sGyroMotion.ManualCalibrating()) {
    // The pad is meant to lie still: nothing it reads now aims or flicks.
    sGyroStatus = "calibrating, keep the pad still";
    sSpringFlickArmed = true;
    return;
  }

  if (sInput.springFlick) {
    // One flick per upward swing: it re-arms once the pitch speed has dropped
    // to half the threshold. The latch outlives the tick so a flick just before
    // the ball lands still springs.
    if (sSpringFlickArmed && pitch > sInput.springFlickRate) {
      sSpringFlickLatch = 0.2f;
      sSpringFlickArmed = false;
    } else if (pitch < sInput.springFlickRate * 0.5f) {
      sSpringFlickArmed = true;
    }
  }
  sGyroDrivesAim = aim;
  if (!aim) {
    if (sInput.gyroMode != 0) {
      sGyroStatus = "flicks only (aim needs mouse aim, twin stick or touch controls)";
    }
    return;
  }

  // The "Gyro aim" binding (R3 / left ctrl by default) turns gyro on while held
  // in hold to aim, off in hold to pause, or flips aim on each fresh press.
  const bool active = sGyroActivation.Active();
  if (!active) {
    sGyroStatus = sInput.gyroMode == 3 ? "paused by the Gyro aim button" : "waiting for the Gyro aim button";
    return;
  }

  if (!std::isfinite(dt) || dt <= 0.f) {
    return;
  }
  // Natural scale: the pending pixels become mouse_sensitivity rad each in
  // direct aim (with the mouse inversion applied), so divide that back out and
  // pre-undo the inversion: gyro turns gyro_sensitivity times the pad's angle
  // whatever the mouse is set to. x right / y up; the aim state expects right/down.
  const float perPixel = MouseSensitivity();
  if (!(perPixel > 0.f)) {
    return;
  }
  const float sx = MouseInvertX() ? -1.f : 1.f;
  const float sy = MouseInvertY() ? -1.f : 1.f;
  const float scale = sInput.gyroSensitivity * dt / perPixel;
  const float ySense = sInput.gyroSplit ? sInput.gyroSensitivityY : sInput.gyroSensitivity;
  const float scaleY = ySense * dt / perPixel;
  // A lock-on free aim pointer is a pointer too, with or without pointer aim (issue #188).
  if (!PointerAim() && !sPointerAimLocked) {
    sGyroPendingX += sx * yaw * scale;
    sGyroPendingY -= sy * pitch * scaleY;
    return;
  }
  // Pointer aim: gyro_cursor_angle degrees of pad turn sweep the pointer across
  // the screen (2 pointer units), at the same pixels per degree both ways, so
  // divide out the window and mouse scaling the pointer applies. No inversion.
  float px = 0.f, py = 0.f;
  PointerAimScale(px, py);
  const float sweep = sInput.gyroCursorAngle * (PortMouse::kPi / 180.f);
  if (px > 0.f && sweep > 0.f) {
    const float cursor = 2.f / sweep * dt / px;
    sGyroPendingX += yaw * cursor;
    sGyroPendingY -= pitch * cursor * (ySense / sInput.gyroSensitivity);
  }
  if (sInput.gyroCursorCamera) {
    sGyroCameraPendingX += sx * yaw * scale;
    sGyroCameraPendingY -= sy * pitch * scaleY;
  }
}

void ResetMouseAim() {
  sMouseAimState.Reset();
  sPointerAim.Center();
  sPointerAimFree = false;
  sPointerAimLocked = false;
  sPointerAimHeld = false;
  sMouseGameplayActive = false;
  sMouseButtonGate.Reset();
  sMousePendingX = sMousePendingY = sMouseFrameX = sMouseFrameY = 0.f;
  sTouchLookFrameX = sTouchLookFrameY = 0.f;
  sGyroPendingX = sGyroPendingY = sStickFrameX = sStickFrameY = 0.f;
  sGyroCameraPendingX = sGyroCameraPendingY = sGyroCameraFrameX = sGyroCameraFrameY = 0.f;
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
bool MouseInvertX() { EnsureInitialized(); return sInput.mouseInvertX; }
bool MouseInvertY() { EnsureInitialized(); return sInput.mouseInvertY; }
bool MouseButtons() { EnsureInitialized(); return sInput.mouseButtons; }
bool MouseCrosshair() { EnsureInitialized(); return sInput.mouseCrosshair; }
// Window travel (pixels) to pointer units (-1..1 of the half view). At the default
// sensitivity the pointer follows the cursor's speed; the sensitivity slider scales it.
static void PointerAimScale(float& sx, float& sy) {
  constexpr float kReferenceSensitivity = 0.0035f;
  int width = 0, height = 0;
  if (!WindowSize(width, height)) {
    width = 640;
    height = 480;
  }
  const float speed = MouseSensitivity() / kReferenceSensitivity;
  sx = 2.f / static_cast<float>(width) * speed;
  sy = 2.f / static_cast<float>(height) * speed;
}

bool AimCrosshair() { return MouseCrosshair() || PointerAim(); }
// Any direct aim moves the pointer: the mouse, twin stick's right stick, gyro and touch drags.
bool PointerAim() {
  EnsureInitialized();
  return sInput.pointerAim && DirectAim() && !sOriginalExperience;
}
void SetPointerAim(bool on) {
  EnsureInitialized();
  sInput.pointerAim = on;
  sPointerAim.Center();
  MarkDirty();
}
int PointerAimBox() { EnsureInitialized(); return sInput.pointerAimBox; }
int PointerAimTurn() { EnsureInitialized(); return sInput.pointerAimTurn; }
bool LockOnFreeAim() {
  EnsureInitialized();
  return sInput.lockOnFreeAim && DirectAim() && !sOriginalExperience;
}
void SetPointerAimPosition(float x, float y) {
  sPointerAim.Center();
  sPointerAim.Move(x, y);
}
bool PointerAimPosition(float& x, float& y) {
  x = sPointerAim.x;
  y = sPointerAim.y;
  return sPointerAimFree;
}
bool PresentedPointerAimPosition(float fraction, float& x, float& y) {
  if (!PointerAimPosition(x, y)) return false;
  // Held, the pointer stays put: travel since the tick does not move it.
  if (sPointerAimHeld) return true;
  // The travel since the tick, so the drawn pointer moves every frame, and the
  // last tick's stick travel drawn after its tick (PresentedAimDelta).
  float touchX = 0.f, touchY = 0.f;
  {
    std::lock_guard lock(sTouchAimMutex);
    touchX = sTouchAimPendingX;
    touchY = sTouchAimPendingY;
  }
  const bool interpolating = FrameInterpolation() && std::isfinite(fraction) && fraction >= 0.f;
  const float lag = interpolating ? 1.f - std::min(fraction, 1.f) : 0.f;
  float sx = 0.f, sy = 0.f;
  PointerAimScale(sx, sy);
  sPointerAim.Presented(lag, (sMousePendingX + sGyroPendingX + touchX) * sx,
                        (sMousePendingY + sGyroPendingY + touchY) * sy, x, y);
  return true;
}
int CrosshairSize() { EnsureInitialized(); return sInput.crosshairSize; }
void SetCrosshairSize(int percent) {
  EnsureInitialized();
  sInput.crosshairSize = std::clamp(percent, kCrosshairSizeMin, kCrosshairSizeMax);
  MarkDirty();
}
unsigned MouseWeaponButtons(unsigned held) {
  return sMouseButtonGate.Poll(MouseAim() && MouseButtons() && MouseGameplayActive() &&
                               MouseCaptured() && !Visible(), held);
}
unsigned MouseMenuButtons(unsigned held, bool focused) {
  // The resume prompt's buttons take the click; the game must not also read it as A.
  PortSaveState::ResumeInfo promptInfo;
  bool promptYes = true;
  const bool resumePrompt = PortSaveState::ResumePromptShown(promptInfo, promptYes);
  // A pointer-driven screen reads the clicks itself (hover, then click = A).
  const bool pointerScreen = PortPointer::Enabled() && PortPointer::ScreenActive();
  return sMouseMenuGate.Poll(MouseAim() && MouseButtons() && !MouseGameplayActive() &&
                                 !Visible() && !resumePrompt && !pointerScreen && focused,
                             held);
}
void NoteMouseButton(bool synthetic, unsigned mask, bool down) {
  sMouseHeldButtons.Note(synthetic, mask, down);
}
void ClearMouseButtons() { sMouseHeldButtons.Clear(); }
unsigned MouseHeldButtons() { return sMouseHeldButtons.Held(); }

bool UpdateMouseAim(bool active, bool locked, float x, float y, float z, bool pointerLock,
                    bool pointerHold) {
  SetMouseGameplayActive(active);
  const PortMouse::AimState lastAim = sMouseAimState;
  const PortMouse::PointerAim lastPointer = sPointerAim;
  // Lock-on free aim: the lock owns the view and a pointer moves over it, pointer aim's
  // or, without pointer aim, one for the lock only. It starts and ends a lock at the
  // centre, where the target is.
  const bool lockFree = active && locked && pointerLock && LockOnFreeAim();
  const bool pointer = PointerAim() || lockFree;
  // A held pointer stays where the lock found it, and neither moves nor turns the view.
  const bool held = active && locked && !lockFree && pointerHold && PointerAim();
  sPointerAimHeld = held;
  if (lockFree != sPointerAimLocked) {
    sPointerAim.Center();
    sPointerAimLocked = lockFree;
  }
  // Reset aim: under pointer aim recentre the pointer, otherwise look level
  // (pitch 0; yaw stays). reset_aim_both does both either way.
  if (sResetAimRequested.exchange(false, std::memory_order_acq_rel)) {
    if (pointer || sInput.resetAimBoth) {
      sPointerAim.Center();
    }
    if ((!pointer || sInput.resetAimBoth) && sMouseAimState.initialized) {
      sMouseAimState.pitch = 0.f;
      sMouseAimState.lagPitch = 0.f;
    }
  }
  // Under pointer aim the stick moves the pointer, unless the gyro does: then the
  // stick (and the keyboard look keys) turn the camera as they do without
  // pointer aim. A lock leaves the view nothing to turn, so they move the pointer too.
  const bool stickTurns = pointer && sGyroDrivesAim && !lockFree;
  float dx = sMouseFrameX;
  float dy = sMouseFrameY;
  if (pointer) {
    // The travel moves the pointer, not the view. A lock (or anything that stops free
    // look) re-centres it: lock-on (without lock-on free aim), scan and grapple still aim
    // through the centre.
    if (active && (!locked || lockFree)) {
      float sx = 0.f, sy = 0.f;
      PointerAimScale(sx, sy);
      if (stickTurns) {
        sPointerAim.MoveLagged((dx - sStickFrameX) * sx, (dy - sStickFrameY) * sy, 0.f, 0.f);
      } else {
        sPointerAim.MoveLagged(dx * sx, dy * sy, sStickFrameX * sx, sStickFrameY * sy);
      }
    } else if (held) {
      sPointerAim.lagX = sPointerAim.lagY = 0.f;
    } else {
      sPointerAim.Center();
    }
    // Only the gyro's own camera turn (if on) moves the view directly.
    dx = sGyroCameraFrameX + (stickTurns ? sStickFrameX : 0.f);
    dy = sGyroCameraFrameY + (stickTurns ? sStickFrameY : 0.f);
  }
  const bool stickLag = !pointer || stickTurns;
  const bool applied = sMouseAimState.Update(active, locked, x, y, z, dx, dy, MouseSensitivity(),
                                            MouseInvertX(), MouseInvertY(), stickLag ? sStickFrameX : 0.f,
                                            stickLag ? sStickFrameY : 0.f);
  if (applied && pointer && sAimTick.First()) {
    float dyaw = 0.f, dpitch = 0.f;
    PortMouse::PointerAim::TurnAt(sPointerAim.x, sPointerAim.y, PointerAimBox() / 100.f,
                                  PointerAimTurn() * (PortMouse::kPi / 180.f), TickPeriod(), dyaw,
                                  dpitch);
    sMouseAimState.Turn(dyaw, dpitch);
  }
  sPointerAimFree = pointer && (applied || lockFree || held);
  if (!sPointerAimFree) sPointerAim.Center();
  if (applied) {
    sAimTick.Applied(sMouseAimState, lastAim, sPointerAim, lastPointer, sPointerAimFree);
  } else if (lockFree) {
    // As TickAim keeps the pointer's lag from the tick's first update, but apart from it:
    // a lock that ends between the two updates leaves the second its lag and edge turn.
    if (sLockPointerRecorded) {
      sPointerAim.lagX = lastPointer.lagX;
      sPointerAim.lagY = lastPointer.lagY;
    }
    sLockPointerRecorded = true;
  }
  // The second update of a tick must not move the pointer by the same travel again.
  if (applied || lockFree) {
    sMouseFrameX = sMouseFrameY = sStickFrameX = sStickFrameY = 0.f;
    sGyroCameraFrameX = sGyroCameraFrameY = 0.f;
  }
  sAimAppliedLastTick = applied;
  return applied;
}
void SynchronizeMouseAim(float x, float y, float z) { sMouseAimState.Synchronize(x, y, z); }
void TurnMouseAim(float dyaw) { sMouseAimState.Turn(dyaw, 0.f); }

float MouseSensitivity() { return Loaded(sInput.mouseSensitivity); }

void SetMouseSensitivity(float radiansPerPixel) {
  EnsureInitialized();
  if (std::isfinite(radiansPerPixel) && radiansPerPixel > 0.f) {
    sInput.mouseSensitivity = radiansPerPixel;
  }
}

void SetMouseSensitivityY(float radiansPerPixel) {
  EnsureInitialized();
  if (std::isfinite(radiansPerPixel) && radiansPerPixel > 0.f) {
    sInput.mouseSensitivityY = radiansPerPixel;
  }
}

void AddMouseDelta(float dx, float dy) {
  if (!sMouseCaptured || !(MouseAim() || PortPhoto::Active()) || Visible() || !std::isfinite(dx) ||
      !std::isfinite(dy)) {
    return;
  }
  if (sInput.mouseSensitivitySplit) {
    // Vertical counts become the horizontal-equivalent counts that turn at the Y sensitivity.
    const float ratio = sInput.mouseSensitivityY / sInput.mouseSensitivity;
    if (std::isfinite(ratio) && ratio > 0.f) {
      dy *= ratio;
    }
  }
  sMousePendingX += dx;
  sMousePendingY += dy;
}

bool TouchAim() { return Loaded(sInput.touchAim); }
void SetTouchAim(bool on) { StoreSetting(sInput.touchAim, on); }

bool TouchMapTap() { return Loaded(sInput.touchMapTap); }
void SetTouchMapTap(bool on) { StoreSetting(sInput.touchMapTap, on); }

bool TouchClassic() { return Loaded(sInput.touchClassic); }

void SetTouchClassic(bool on) {
  EnsureInitialized();
  sInput.touchClassic = on;
  if (on) {
    sInput.touchTwinStick = false;
  }
  MarkDirty();
}

bool TouchTwinStick() { return Loaded(sInput.touchTwinStick); }

void SetTouchTwinStick(bool on) {
  EnsureInitialized();
  sInput.touchTwinStick = on;
  if (on) {
    sInput.touchClassic = false;
  }
  MarkDirty();
}

bool TouchWheels() { return Loaded(sInput.touchWheels); }
void SetTouchWheels(bool on) { StoreSetting(sInput.touchWheels, on); }

bool TouchVisorTapScan() { return Loaded(sInput.touchVisorTapScan); }
void SetTouchVisorTapScan(bool on) { StoreSetting(sInput.touchVisorTapScan, on); }

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
void SetPauseScreenOpen(bool open, bool dialog) {
  sPauseScreenOpen.store(open);
  sPausedDialogOpen.store(dialog);
  sPauseScreenStampNs.store(SDL_GetTicksNS());
}

// Not refreshed for a while (left the game) counts as closed.
bool PauseScreenOpen() {
  return sPauseScreenOpen.load() && SDL_GetTicksNS() - sPauseScreenStampNs.load() < 300'000'000;
}

bool PausedMenuOpen() {
  return (sPauseScreenOpen.load() || sPausedDialogOpen.load()) &&
         SDL_GetTicksNS() - sPauseScreenStampNs.load() < 300'000'000;
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

void RequestSpringBall() { sSpringRequestUntilNs.store(SDL_GetTicksNS() + kWheelRequestNs); }

bool SpringBallRequested() { return SDL_GetTicksNS() < sSpringRequestUntilNs.load(); }

void RequestSaveStateHotkey(int which) {
  if (which == 1 || which == 2) {
    sSaveStateHotkey.store(which, std::memory_order_release);
  }
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

void SetTouchAimSpeed(float pixelsPerDp) {
  EnsureInitialized();
  if (std::isfinite(pixelsPerDp)) {
    sInput.touchAimSpeed = std::clamp(pixelsPerDp, 0.25f, 10.f);
    MarkDirty();
  }
}

void SetTouchAimSpeedY(float pixelsPerDp) {
  EnsureInitialized();
  if (std::isfinite(pixelsPerDp)) {
    sInput.touchAimSpeedY = std::clamp(pixelsPerDp, 0.25f, 10.f);
    MarkDirty();
  }
}

// Called from the Android UI thread; the game thread drains it in
// BeginFrameMouse.
void AddTouchAim(float dxDp, float dyDp) {
  if ((!sInput.touchAim && sInput.touchClassic) || Visible() || !std::isfinite(dxDp) ||
      !std::isfinite(dyDp)) {
    return;
  }
  std::lock_guard lock(sTouchAimMutex);
  sTouchAimPendingX += dxDp * sInput.touchAimSpeed;
  sTouchAimPendingY += dyDp * (sInput.touchAimSplit ? sInput.touchAimSpeedY : sInput.touchAimSpeed);
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
  sGyroCameraFrameX = sGyroCameraPendingX;
  sGyroCameraFrameY = sGyroCameraPendingY;
  sGyroCameraPendingX = sGyroCameraPendingY = 0.f;
  // AddStickAim adds this tick's travel during its input update.
  sStickFrameX = sStickFrameY = 0.f;
  sAimAppliedLastTick = false;
  sAimTick.BeginTick();
  sLockPointerRecorded = false;
}

void SetSpringBallSwipe(bool enabled) { StoreSetting(sInput.springSwipe, enabled); }

// CMorphBall springs on the request only on the ground in the ball with the
// Spring Ball unlocked, so a swipe anywhere else just expires.
void TouchSpringSwipe() {
  if (sInput.springSwipe && !sOriginalExperience && !Visible()) {
    RequestSpringBall();
  }
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
  return sInput.touchAim && !Visible();
}

bool PresentedAimDelta(float fraction, float& dyaw, float& dpitch) {
  dyaw = dpitch = 0.f;
  if (!FrameInterpolation() || !sMouseGameplayActive || !sAimAppliedLastTick ||
      !std::isfinite(fraction) || fraction < 0.f) {
    return false;
  }
  // What the next tick will consume: the mouse, gyro and touch travel so far,
  // and the edge turn a pointer dead box makes over the part of the tick
  // already shown.
  const float ahead = std::min(fraction, 1.f) * TickPeriod();
  // The stick cannot be measured, only predicted, so its turn is shown over the
  // period that follows its tick, like every other interpolated state: the last
  // tick's stick turn (after the pitch or pointer edge clamp, so a clamped
  // stick draws no motion) with the un-elapsed share `lag` taken back. Drawing a
  // predicted turn before the tick instead put the stick a tick ahead of the
  // world, so releasing it retracted turn already drawn (issue #128).
  const float lag = 1.f - std::min(fraction, 1.f);
  float touchX = 0.f;
  float touchY = 0.f;
  {
    std::lock_guard lock(sTouchAimMutex);
    touchX = sTouchAimPendingX;
    touchY = sTouchAimPendingY;
  }
  const float dx = sMousePendingX + sGyroPendingX + touchX;
  const float dy = sMousePendingY + sGyroPendingY + touchY;
  if (sPointerAimFree) {
    // The edge turn the next tick will make, over the part of it already shown.
    float sx = 0.f, sy = 0.f;
    PointerAimScale(sx, sy);
    float px = 0.f, py = 0.f;
    sPointerAim.Presented(lag, dx * sx, dy * sy, px, py);
    PortMouse::PointerAim::TurnAt(px, py, PointerAimBox() / 100.f,
                                  PointerAimTurn() * (PortMouse::kPi / 180.f), ahead, dyaw, dpitch);
    // Plus the gyro's own camera turn so far.
    float gyroYaw = 0.f, gyroPitch = 0.f;
    if (sMouseAimState.Preview(sGyroCameraPendingX, sGyroCameraPendingY, MouseSensitivity(),
                               MouseInvertX(), MouseInvertY(), gyroYaw, gyroPitch, lag)) {
      dyaw += static_cast<float>(std::remainder(double(gyroYaw) - sMouseAimState.yaw, 2.0 * PortMouse::kPi));
      dpitch += gyroPitch - sMouseAimState.pitch;
    }
    return dyaw != 0.f || dpitch != 0.f;
  }
  float yaw = 0.f;
  float pitch = 0.f;
  if (!sMouseAimState.Preview(dx, dy, MouseSensitivity(), MouseInvertX(), MouseInvertY(), yaw,
                              pitch, lag)) {
    return false;
  }
  dyaw = static_cast<float>(std::remainder(double(yaw) - sMouseAimState.yaw, 2.0 * PortMouse::kPi));
  dpitch = pitch - sMouseAimState.pitch;
  return dyaw != 0.f || dpitch != 0.f;
}

} // namespace PortDebug
