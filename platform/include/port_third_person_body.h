#pragma once

#include <algorithm>
#include <cmath>
#include <string>

class CStateManager;
class CActorLights;
class CTransform4f;
class CPlayer;
class CVector3f;

// The third-person gameplay camera's animated Samus (port_third_person_body.cpp). In gameplay the
// player's body is BallTransitionsANCS, which only has morph clips, so it stands frozen. Prime
// keeps the walking clips in the cutscene Samus ANCS (CINF 19C53902); this draws that body
// instead and poses it from the player's movement: the walk clips blended by speed with their
// feet in step, the aim clips over them when the cannon is raised, every change cross-faded.
// The Phazon Suit gets CScriptPlayerActor's indirect-texture pass.
namespace PortThirdPersonBody {

// The body is posed by blending the clips itself (port_third_person_body.cpp): each clip is
// sampled at its own phase and the samples are slerped together, as CAnimTreeBlend blends two.
// Stance: what the legs do. Idle stands in the ready pose (also strafing: Prime has no strafe
// clip); Stop plays the walk's settle into it once; Loco walks or runs, the walk clips blended by
// speed; Jump plays a jump's push-off and holds its tuck while rising, Fall a fall's pose while
// dropping; Land plays a landing's crouch and rise once, after a fall long enough to want one.
enum class Stance { Idle, Stop, Loco, Jump, Fall, Land, Count };

// How long the cannon stays raised after a shot, in seconds.
inline constexpr float kGunUpSeconds = 1.f;
// Every change of stance cross-fades over this, in seconds.
inline constexpr float kBlendSeconds = 0.25f;
// While the cannon is raised the arm turns at the shoulder to point it at the shot's aim point:
// at most kAimMaxRadians off the clip's pose, easing in over kAimInSeconds and out over
// kAimOutSeconds.
inline constexpr float kAimMaxRadians = 1.4f;
inline constexpr float kAimInSeconds = 0.08f;
inline constexpr float kAimOutSeconds = 0.3f;
// While the grapple beam is out the left arm reaches for the grapple point, easing in over
// kReachInSeconds and out over kReachOutSeconds.
inline constexpr float kReachInSeconds = 0.1f;
inline constexpr float kReachOutSeconds = 0.3f;
// Below this speed (game units per second) the body stands.
inline constexpr float kStandSpeed = 0.5f;
// The aim-pose grid's coordinates move at most this fast (grid units, 1 = one aim clip, per
// second) and accelerate at most this hard: CABSAim's limits.
inline constexpr float kAimGridSpeed = 3.f;
inline constexpr float kAimGridAccel = 10.f;

// An airborne body lands with Land only after this long in the air, in seconds (a step off a
// kerb doesn't crouch).
inline constexpr float kLandAirSeconds = 0.5f;

// The stance the movement asks for. fwd and side are the speeds along the facing (+ forward)
// and across it, rise the vertical one; landing: the body was in the air kLandAirSeconds.
// Stopping from a walk plays Stop, and landing Land, until done (the current stance's clip
// played out), unless the body moves (again: the touchdown tick's own speed, a blip of the
// collision, doesn't count).
inline Stance ChooseStance(float fwd, float side, bool grounded, float rise, Stance current,
                           bool done, bool landing) {
  if (!grounded) {
    return rise > 0.f ? Stance::Jump : Stance::Fall;
  }
  if ((current == Stance::Jump || current == Stance::Fall) && landing) {
    return Stance::Land;
  }
  const float speed = std::sqrt(fwd * fwd + side * side);
  if (speed < kStandSpeed) {
    if (current == Stance::Loco || (current == Stance::Stop && !done)) {
      return Stance::Stop;
    }
    if (current == Stance::Land && !done) {
      return Stance::Land;
    }
    return Stance::Idle;
  }
  return Stance::Loco;
}

// Strafing: the legs walk the way the body goes, turned from the facing by the angle LegYaw
// gives (radians, + to the left, at most a quarter turn past the side), the upper body turned
// back to the facing. Backward (back, kept within kLegFlip of the side so the legs don't swap
// over at it) they walk back, the way the body came from.
inline constexpr float kLegFlip = 0.2f;
inline bool LegsBackward(float fwd, float side, bool backward) {
  const float speed = std::sqrt(fwd * fwd + side * side);
  return backward ? fwd < kLegFlip * speed : fwd < -kLegFlip * speed;
}
inline float LegYaw(float fwd, float side, bool backward) {
  return backward ? std::atan2(side, -fwd) : std::atan2(-side, fwd);
}

// The walk clips' weights at a ground speed: the two whose own speeds (speeds[], ascending)
// bracket it, linearly; below the slowest or above the fastest that one alone.
inline void LocoWeights(float speed, const float* speeds, int N, float* w) {
  for (int i = 0; i < N; ++i) {
    w[i] = 0.f;
  }
  if (N <= 0) {
    return;
  }
  if (speed <= speeds[0]) {
    w[0] = 1.f;
    return;
  }
  for (int i = 0; i + 1 < N; ++i) {
    if (speed <= speeds[i + 1]) {
      const float span = speeds[i + 1] - speeds[i];
      const float t = span > 1e-4f ? (speed - speeds[i]) / span : 1.f;
      w[i] = 1.f - t;
      w[i + 1] = t;
      return;
    }
  }
  w[N - 1] = 1.f;
}

template <int N>
inline void LocoWeights(float speed, const float (&speeds)[N], float (&w)[N]) {
  LocoWeights(speed, speeds, N, w);
}

// Where in a loop a foot curve (n samples over one loop, e.g. left minus right ankle forward)
// peaks, 0..1, from its first harmonic: what the walk clips are shifted by so their feet agree.
inline float CurvePeak(const float* d, int n) {
  constexpr float kTwoPi = 6.2831853f;
  float c = 0.f;
  float sn = 0.f;
  for (int k = 0; k < n; ++k) {
    c += d[k] * std::cos(kTwoPi * k / n);
    sn += d[k] * std::sin(kTwoPi * k / n);
  }
  const float p = std::atan2(sn, c) / kTwoPi;
  return p - std::floor(p);
}

// Moves value toward target by at most step.
inline float Approach(float value, float target, float step) {
  return value < target ? std::min(value + step, target) : std::max(value - step, target);
}

// Moves x toward target with its speed v limited to kAimGridSpeed and its change to
// kAimGridAccel, braking in time to stop on the target rather than past it.
inline void SmoothToward(float& x, float& v, float target, float dt) {
  const float d = target - x;
  const float brake = std::sqrt(2.f * kAimGridAccel * std::fabs(d));
  const float want = std::copysign(std::min(brake, kAimGridSpeed), d);
  v = Approach(v, want, kAimGridAccel * dt);
  const float next = x + v * dt;
  if ((next - target) * (x - target) <= 0.f) {
    x = target;
    v = 0.f;
  } else {
    x = next;
  }
}

// Bilinear weights over the 3x3 aim grid (row-major from down-left: index row*3+col, row 0 down,
// 1 level, 2 up; col 0 left, 1 centre, 2 right) at u (left -1 .. right 1), v (down -1 .. up 1).
inline void AimGridWeights(float u, float v, float (&w)[9]) {
  for (float& x : w) {
    x = 0.f;
  }
  u = std::clamp(u, -1.f, 1.f) + 1.f;
  v = std::clamp(v, -1.f, 1.f) + 1.f;
  const int c = std::min(int(u), 1);
  const int r = std::min(int(v), 1);
  const float fu = u - c;
  const float fv = v - r;
  w[r * 3 + c] += (1.f - fu) * (1.f - fv);
  w[r * 3 + c + 1] += fu * (1.f - fv);
  w[(r + 1) * 3 + c] += (1.f - fu) * fv;
  w[(r + 1) * 3 + c + 1] += fu * fv;
}

// A shot left the cannon (CPlayerGun): the body raises it for kGunUpSeconds.
void NoteShot();
// Once per game tick while the third-person camera is active (PortThirdPerson::Update).
void Update(CStateManager& mgr, float dt);
// The body is built and animated, and stands in for the frozen one.
bool Ready();
// Before drawing the frame (CPlayer::PreRender).
void PreRender(const CStateManager& mgr);
// Draws the body (and its cannon) at the player's transform; false if it is not ready.
bool Render(const CStateManager& mgr, const CTransform4f& xf, const CActorLights* lights);
// The cannon's muzzle in the world (the body's Charge_LCTR) as the body is posed now; false when the
// body is not ready, then the first-person muzzle stands in.
bool CannonPosition(const CPlayer& player, CVector3f& out);
// The grapple hand in the world (the body's L_middle_1) as the body is posed now; false when the
// body is not ready or the third-person camera is off, then the first-person arm's nozzle stands in.
bool GrappleHand(const CPlayer& player, CVector3f& out);
// Drops the body (third person off, a new world).
void Release();

// Debug (console `thirdperson clips`): the body's Samus clips, one line each: index, name,
// duration, mean root speed.
std::string DescribeClips();
// Debug (console `thirdperson feet`): what Build measured: each walk clip's speed, stride and
// foot phase, each aim clip's cannon yaw and pitch.
std::string DescribeFeet();
// Holds the named clip at phase 0..1 instead of the movement's clip, the body turned by yaw degrees;
// nullptr lets go. False if the body has no such clip (or is not built).
bool PinClip(const char* name, float phase, float yaw = 0.f);

} // namespace PortThirdPersonBody
