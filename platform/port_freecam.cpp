// The debug free camera (F1 > Debug > Camera, console `freecam`); see port_freecam.h.
#include "port_freecam.h"

#include "port_debug.h"
#include "Kyoto/Input/CFinalInput.hpp"
#include "Kyoto/Math/CRelAngle.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Math/CVector3f.hpp"
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
Pose sPose{0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
float sFov = 0.f;
float sTouchMoveX = 0.f;
float sTouchMoveY = 0.f;
float sTouchYaw = 0.f;
float sTouchPitch = 0.f;

CTransform4f Rotation() {
  return CTransform4f::RotateZ(CRelAngle::FromDegrees(sPose.yaw)) *
         CTransform4f::RotateX(CRelAngle::FromDegrees(sPose.pitch));
}

CTransform4f ViewRotation() {
  return Rotation() * CTransform4f::RotateY(CRelAngle::FromDegrees(sPose.roll));
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

Pose GetPose() { return sPose; }

void SetPose(const Pose& pose) {
  sPose = pose;
  sPose.pitch = std::clamp(sPose.pitch, -kPitchLimit, kPitchLimit);
  sPose.roll = std::isfinite(sPose.roll) ? std::clamp(sPose.roll, -180.f, 180.f) : 0.f;
}

float FovOverride() { return sActive ? sFov : 0.f; }
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

void SetFovOverride(float degrees) { sFov = std::isfinite(degrees) && degrees > 0.f ? std::clamp(degrees, 1.f, 170.f) : 0.f; }

bool Input(const CFinalInput& input) {
  if (!sActive) {
    return false;
  }
  if (PortDebug::OverlayVisible()) {
    return true; // the overlay has the pad
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
  const CTransform4f rot = Rotation();
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
    return game;
  }
  CTransform4f xf = ViewRotation();
  xf.SetTranslation(CVector3f(sPose.x, sPose.y, sPose.z));
  return xf;
}

} // namespace PortFreeCam
