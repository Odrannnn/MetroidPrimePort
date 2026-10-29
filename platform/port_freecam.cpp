// The debug free camera (F1 > Debug > Camera, console `freecam`); see port_freecam.h.
#include "port_freecam.h"

#include "port_debug.h"
#include "port_third_person.h"
#include "Kyoto/Input/CFinalInput.hpp"
#include "Kyoto/Math/CRelAngle.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "MetroidPrime/CActor.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include <algorithm>
#include <cmath>

namespace PortFreeCam {
namespace {

constexpr float kDegrees = 57.29578f;
constexpr float kTurnRate = 120.f; // degrees per second at a full stick
constexpr float kPitchLimit = 89.f;

bool sActive = false;
bool sFrozen = false;
float sSpeed = 10.f;
bool sShowPlayer = true;
bool sDrive = false;
Pose sPose{0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
float sFov = 0.f;
float sTouchMoveX = 0.f;
float sTouchMoveY = 0.f;
float sTouchYaw = 0.f;
float sTouchPitch = 0.f;
// The pose and field of view the current tick started from. The pose only moves on
// ticks, so frames presented between them blend from here (as actors do) instead of
// holding still and jumping at a frame rate above the tick rate.
Pose sPrevPose = sPose;
float sPrevFov = 0.f;
uint sPrevGeneration = 0;

CTransform4f Rotation(const Pose& pose) {
  return CTransform4f::RotateZ(CRelAngle::FromDegrees(pose.yaw)) *
         CTransform4f::RotateX(CRelAngle::FromDegrees(pose.pitch));
}

CTransform4f ViewRotation(const Pose& pose) {
  return Rotation(pose) * CTransform4f::RotateY(CRelAngle::FromDegrees(pose.roll));
}

// Once per tick, before the first change: what this tick's frames blend from.
void MarkTick() {
  if (sPrevGeneration != CActor::PortTickGeneration()) {
    sPrevPose = sPose;
    sPrevFov = sFov;
    sPrevGeneration = CActor::PortTickGeneration();
  }
}

// A jump (a new start, a typed pose, a slider): no blend from where it was.
void Snap() {
  sPrevPose = sPose;
  sPrevFov = sFov;
}

// Where the frame being drawn sits between the tick's start and its end, or -1 to draw
// the end: no tick moved the camera since (paused, frozen for a shot), or no blending.
float Blend() {
  const float t = CCameraManager::GetPresentationInterpolation();
  if (t < 0.f || t >= 1.f || sPrevGeneration != CActor::PortTickGeneration()) {
    return -1.f;
  }
  return t;
}

Pose PresentedPose() {
  const float t = Blend();
  if (t < 0.f) {
    return sPose;
  }
  const auto lerp = [t](float a, float b) { return a + (b - a) * t; };
  float yaw = sPose.yaw - sPrevPose.yaw;
  yaw -= 360.f * std::round(yaw / 360.f);
  return Pose{lerp(sPrevPose.x, sPose.x),         lerp(sPrevPose.y, sPose.y),
              lerp(sPrevPose.z, sPose.z),         sPrevPose.yaw + yaw * t,
              lerp(sPrevPose.pitch, sPose.pitch), lerp(sPrevPose.roll, sPose.roll)};
}

} // namespace

bool Active() { return sActive; }

void SetActive(bool on, const CStateManager* mgr) {
  if (on && !sActive && mgr != nullptr) {
    const CTransform4f xf = mgr->GetCameraManager()->GetCurrentCameraTransform(*mgr);
    const CVector3f fwd = xf.GetForward();
    const CVector3f pos = xf.GetTranslation();
    sPose.x = pos.GetX();
    sPose.y = pos.GetY();
    sPose.z = pos.GetZ();
    sPose.yaw = std::atan2(-fwd.GetX(), fwd.GetY()) * kDegrees;
    sPose.pitch = std::asin(std::clamp(fwd.GetZ(), -1.f, 1.f)) * kDegrees;
    sPose.roll = 0.f;
  }
  sActive = on;
  Snap();
  SetTouchMove(0.f, 0.f);
  sTouchYaw = 0.f;
  sTouchPitch = 0.f;
}

bool Frozen() { return sActive && sFrozen; }
void SetFrozen(bool frozen) { sFrozen = frozen; }

float Speed() { return sSpeed; }
void SetSpeed(float speed) {
  if (std::isfinite(speed)) {
    sSpeed = std::clamp(speed, 0.1f, 500.f);
  }
}

bool ShowPlayer() { return sActive && sShowPlayer; }
void SetShowPlayer(bool show) { sShowPlayer = show; }
bool Drive() { return sActive && sDrive; }
void SetDrive(bool drive) { sDrive = drive; }

Pose GetPose() { return sPose; }

void SetPose(const Pose& pose, bool snap) {
  if (!snap) {
    MarkTick();
  }
  sPose = pose;
  sPose.pitch = std::clamp(sPose.pitch, -kPitchLimit, kPitchLimit);
  sPose.roll = std::isfinite(sPose.roll) ? std::clamp(sPose.roll, -180.f, 180.f) : 0.f;
  if (snap) {
    Snap();
  }
}

float FovOverride() { return sActive ? sFov : 0.f; }
float PresentedFovOverride() {
  const float t = Blend();
  if (!sActive || sFov <= 0.f || sPrevFov <= 0.f || t < 0.f) {
    return FovOverride();
  }
  return sPrevFov + (sFov - sPrevFov) * t;
}
void SetTouchMove(float x, float y) {
  sTouchMoveX = std::isfinite(x) ? std::clamp(x, -1.f, 1.f) : 0.f;
  sTouchMoveY = std::isfinite(y) ? std::clamp(y, -1.f, 1.f) : 0.f;
}

void AddTouchTurn(float yaw, float pitch) {
  if (std::isfinite(yaw) && std::isfinite(pitch)) {
    sTouchYaw += yaw;
    sTouchPitch += pitch;
  }
}

void SetFovOverride(float degrees, bool snap) {
  if (!snap) {
    MarkTick();
  }
  sFov = std::isfinite(degrees) && degrees > 0.f ? std::clamp(degrees, 1.f, 170.f) : 0.f;
  if (snap) {
    sPrevFov = sFov;
  }
}

bool Input(const CFinalInput& input) {
  if (!sActive) {
    return false;
  }
  MarkTick();
  if (PortDebug::OverlayVisible()) {
    return true; // the overlay has the pad
  }
  if (sDrive) {
    return false; // the player has the pad
  }
  const float dt = std::clamp(input.Time(), 0.f, 0.1f);
  sPose.yaw -= (input.ARARight() - input.ARALeft()) * kTurnRate * dt;
  sPose.pitch += (input.ARAUp() - input.ARADown()) * kTurnRate * dt;
  if (PortDebug::MouseCaptured()) {
    float dx = 0.f;
    float dy = 0.f;
    PortDebug::GetFrameMouseDelta(dx, dy);
    sPose.yaw -= dx * PortDebug::MouseSensitivity() * kDegrees;
    sPose.pitch -= dy * PortDebug::MouseSensitivity() * kDegrees;
  }
  sPose.yaw += sTouchYaw;
  sPose.pitch += sTouchPitch;
  sTouchYaw = 0.f;
  sTouchPitch = 0.f;
  sPose.yaw = std::fmod(sPose.yaw, 360.f);
  sPose.pitch = std::clamp(sPose.pitch, -kPitchLimit, kPitchLimit);

  const float step = sSpeed * (input.DRTrigger() || input.DR() ? 4.f : 1.f) * dt;
  const float rise = (input.DZ() || input.DDPUp() ? 1.f : 0.f) -
                     (input.DLTrigger() || input.DL() || input.DDPDown() ? 1.f : 0.f);
  const CTransform4f rot = Rotation(sPose);
  const float ahead = std::clamp(input.ALeftY() + sTouchMoveY, -1.f, 1.f);
  const float side = std::clamp(input.ALeftX() + sTouchMoveX, -1.f, 1.f);
  const CVector3f move = rot.GetForward() * ahead + rot.GetRight() * side;
  sPose.x += move.GetX() * step;
  sPose.y += move.GetY() * step;
  sPose.z += move.GetZ() * step + rise * step;
  return true;
}

CTransform4f View(const CTransform4f& game) {
  if (!sActive) {
    return PortThirdPerson::View(game);
  }
  const Pose pose = PresentedPose();
  CTransform4f xf = ViewRotation(pose);
  xf.SetTranslation(CVector3f(pose.x, pose.y, pose.z));
  return xf;
}

} // namespace PortFreeCam
