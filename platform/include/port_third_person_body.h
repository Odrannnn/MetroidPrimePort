#pragma once

#include <algorithm>
#include <cmath>

class CStateManager;
class CActorLights;
class CTransform4f;
class CPlayer;
class CVector3f;

// The third-person gameplay camera's animated Samus (port_third_person_body.cpp). In gameplay the
// player's body is BallTransitionsANCS, which only has morph clips, so it stands frozen. Prime
// keeps the walking clips in the cutscene Samus ANCS (CINF 19C53902); this draws that body
// instead and picks its clip from the player's movement.
namespace PortThirdPersonBody {

enum class Clip { Idle, SlowWalk, Walk, Run, Backward, Hold };

struct Choice {
  Clip clip = Clip::Idle;
  float rate = 1.f; // playback rate scale, 1 = the clip's own speed
};

// Speeds in game units per second, along the facing (fwd, + is forward) and across it.
inline constexpr float kStandSpeed = 0.5f;
inline constexpr float kWalkSpeed = 4.f;
inline constexpr float kRunSpeed = 7.f;

// Which clip fits the movement. An airborne body, or one moving more sideways than along its
// facing (strafing: there is no strafe clip), holds the ready pose. Walking backward plays the
// walk clip run in reverse (the animation tree only advances, so the .cpp steps its phase backward).
inline Choice ChooseClip(float fwd, float side, bool grounded) {
  Choice c;
  const float speed = std::sqrt(fwd * fwd + side * side);
  if (!grounded) {
    c.clip = Clip::Hold;
    return c;
  }
  if (speed < kStandSpeed) {
    c.clip = Clip::Idle;
    return c;
  }
  if (std::fabs(side) > std::fabs(fwd)) {
    c.clip = Clip::Hold;
    return c;
  }
  if (fwd < 0.f) {
    c.clip = Clip::Backward;
    c.rate = std::clamp(speed / kWalkSpeed, 0.5f, 1.5f);
  } else if (speed < kWalkSpeed) {
    c.clip = Clip::SlowWalk;
    c.rate = std::clamp(speed / 2.5f, 0.5f, 1.5f);
  } else if (speed < kRunSpeed) {
    c.clip = Clip::Walk;
    c.rate = std::clamp(speed / 5.5f, 0.6f, 1.5f);
  } else {
    c.clip = Clip::Run;
    c.rate = std::clamp(speed / 9.f, 0.7f, 1.6f);
  }
  return c;
}

// Once per game tick while the third-person camera is active (PortThirdPerson::Update).
void Update(CStateManager& mgr, float dt);
// The body is built and animated, and stands in for the frozen one.
bool Ready();
// Before drawing the frame (CPlayer::PreRender).
void PreRender();
// Draws the body (and its cannon) at the player's transform; false if it is not ready.
bool Render(const CStateManager& mgr, const CTransform4f& xf, const CActorLights* lights);
// The cannon's muzzle in the world (the body's Charge_LCTR) as the body is posed now; false when the
// body is not ready, then the first-person muzzle stands in.
bool CannonPosition(const CPlayer& player, CVector3f& out);
// Drops the body (third person off, a new world).
void Release();

} // namespace PortThirdPersonBody
