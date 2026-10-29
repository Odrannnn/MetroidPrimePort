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
// While the grapple arm is out (from its wind-up, before the beam) the left arm reaches for the
// grapple point, easing in over kReachInSeconds and out over kReachOutSeconds. The hand stops at
// kReachStraight of the arm's length, the elbow bent down and kReachElbowOut out to the side.
inline constexpr float kReachInSeconds = 0.1f;
inline constexpr float kReachOutSeconds = 0.3f;
inline constexpr float kReachStraight = 0.98f;
inline constexpr float kReachElbowOut = 0.5f;
// Below this speed (game units per second) the body stands.
inline constexpr float kStandSpeed = 0.5f;
// The aim-pose grid's coordinates move at most this fast (grid units, 1 = one aim clip, per
// second) and accelerate at most this hard: CABSAim's limits.
inline constexpr float kAimGridSpeed = 3.f;
inline constexpr float kAimGridAccel = 10.f;

// An airborne body lands with Land only after this long in the air, in seconds (a step off a
// kerb doesn't crouch).
inline constexpr float kLandAirSeconds = 0.5f;

// The body (and its cannon) fades as the camera comes within kFadeDistance of its bounds, to
// kFadeAlpha with the camera inside them.
inline constexpr float kFadeDistance = 0.6f;
inline constexpr float kFadeAlpha = 0.2f;

// A walk stops only once the body has stood kSettleSeconds: a stick held near its dead zone
// dips under kStandSpeed for a tick or two, which would flick the legs to Stop and back.
inline constexpr float kSettleSeconds = 0.1f;

// A walk brakes once its speed has fallen by at least kBrakeDecel (units per second squared)
// each tick for kBrakeSeconds (or until it is under kBrakeHold) and is under kBrakeSpeed: the
// stick let go, the body halts in a few ticks. Braking stops straight away, so the walk's
// stride doesn't play on while she slows, and the collision's last jitter under kBrakeHold
// doesn't start it again.
inline constexpr float kBrakeDecel = 30.f;
inline constexpr float kBrakeSeconds = 0.05f;
inline constexpr float kBrakeSpeed = 6.f;
inline constexpr float kBrakeHold = 1.2f;

// The stance the movement asks for. fwd and side are the speeds along the facing (+ forward)
// and across it, rise the vertical one; landing: the body was in the air kLandAirSeconds;
// settled: it has been under kStandSpeed kSettleSeconds; braking: as kBrakeDecel.
// Stopping from a walk plays Stop, and landing Land, until done (the current stance's clip
// played out), unless the body moves (again: the touchdown tick's own speed, a blip of the
// collision, doesn't count) and isn't braking.
inline Stance ChooseStance(float fwd, float side, bool grounded, float rise, Stance current,
                           bool done, bool landing, bool settled = true, bool braking = false) {
  // The tick that leaves the ground has no rise yet (it comes from the position delta): it
  // keeps the ground stance, so a jump doesn't flick through Fall first.
  if (!grounded && (rise != 0.f || current == Stance::Jump || current == Stance::Fall)) {
    return rise > 0.f ? Stance::Jump : Stance::Fall;
  }
  if ((current == Stance::Jump || current == Stance::Fall) && landing) {
    return Stance::Land;
  }
  const float speed = std::sqrt(fwd * fwd + side * side);
  if (speed < kStandSpeed) {
    if (current == Stance::Loco && !settled && !braking) {
      return Stance::Loco;
    }
    if (current == Stance::Loco || (current == Stance::Stop && !done)) {
      return Stance::Stop;
    }
    if (current == Stance::Land && !done) {
      return Stance::Land;
    }
    return Stance::Idle;
  }
  if (braking && (current == Stance::Loco || (current == Stance::Stop && !done))) {
    return Stance::Stop;
  }
  return Stance::Loco;
}

// Strafing: the legs walk the way the body goes, turned from the facing by the angle LegYaw
// gives (radians, + to the left, at most a quarter turn past the side), the upper body turned
// back to the facing. Backward (back, kept within kLegFlip of the side so the legs don't swap
// over at it) they walk back, the way the body came from. (With BackWalk::Procedural only nearer
// the front than Stepping: past it the procedural gait sidesteps or backpedals instead.)
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

// Where to start the Stop clip so its feet carry on from the walk's. lead: the left ankle's lead
// over the right along the facing, as a share of the walk's widest (-1..1); rising: it grows as
// the stride goes on. stop: the Stop clip's leads at n phases over the whole clip. The clip steps
// on from the walk before it settles; the phase returned (0..1) is the one there whose lead,
// as a share of the clip's widest, is nearest lead and moves the same way, 0 when none does.
inline float StopStart(float lead, bool rising, const float* stop, int n) {
  float amp = 0.f;
  for (int k = 0; k < n; ++k) {
    amp = std::max(amp, std::fabs(stop[k]));
  }
  if (amp <= 0.f) {
    return 0.f;
  }
  // Settled: the lead holds within 2% of its widest from here to the end.
  int moving = 0;
  for (int k = 0; k + 1 < n; ++k) {
    if (std::fabs(stop[k + 1] - stop[k]) > 0.02f * amp) {
      moving = k + 1;
    }
  }
  float best = 0.f;
  float bestMiss = 1e9f;
  for (int k = 0; k < moving; ++k) {
    const float a = stop[k] / amp;
    const float b = stop[k + 1] / amp;
    if ((b > a) != rising || a == b) {
      continue;
    }
    const float t = std::clamp((lead - a) / (b - a), 0.f, 1.f);
    const float miss = std::fabs(a + (b - a) * t - lead);
    if (miss < bestMiss - 1e-4f) {
      bestMiss = miss;
      best = (k + t) / n;
    }
  }
  return best;
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

// The procedural backpedal (BackWalk::Procedural): each foot loops through stance (planted,
// sliding forward under her at ground speed) for duty of the step (kBackDuty walking, less once a
// planted foot can't cover the stride: then both feet leave the ground, a run) and swing (lifted,
// carried back) for the rest. It lands toe first, kBackToeDown, and rolls up onto the toe, kBackToeUp,
// before it lifts, as a step backward does.
inline constexpr float kBackDuty = 0.6f;
inline constexpr float kBackToeDown = 0.35f;
inline constexpr float kBackToeUp = 0.2f;

// One foot at phase 0..1 (0 = touch-down, furthest back): y in half strides (-1 back .. 1 front),
// lift 0..1, pitch in radians (+ toe up, - toe down).
struct BackFoot {
  float y;
  float lift;
  float pitch;
};
inline BackFoot BackpedalFoot(float phase, float duty = kBackDuty) {
  const float p = phase - std::floor(phase);
  const auto ease = [](float t) { return t * t * (3.f - 2.f * t); };
  if (p < duty) {
    const float t = p / duty;
    float pitch = 0.f;
    if (t < 0.2f) {
      pitch = -kBackToeDown * (1.f - ease(t / 0.2f));
    } else if (t > 0.85f) {
      pitch = kBackToeUp * ease((t - 0.85f) / 0.15f);
    }
    return {-1.f + 2.f * t, 0.f, pitch};
  }
  const float t = (p - duty) / (1.f - duty);
  return {1.f - 2.f * ease(t), std::sin(3.14159265f * t), kBackToeUp + (-kBackToeDown - kBackToeUp) * ease(t)};
}

// The pelvis bob at the left foot's phase: 0 at mid-stance of either foot (highest), 1 lowest.
inline float BackpedalBob(float phaseL, float duty = kBackDuty) {
  return 0.5f * (1.f - std::cos(4.f * 3.14159265f * (phaseL - duty * 0.5f)));
}

// The interior angle at the knee (radians) of a leg with thigh a and shin b whose hip and ankle are
// d apart: pi straight.
inline float KneeAngle(float a, float b, float d) {
  return std::acos(std::clamp((a * a + b * b - d * d) / (2.f * a * b), -1.f, 1.f));
}

// With BackWalk::Procedural the procedural gait walks every way but the front: straight back the
// backpedal, to the side a sidestep, the hips facing forward throughout (nearer the front the
// forward walk plays, its legs turned by LegYaw). It takes over past kStepAngle (radians) from the
// facing, kStepFlip either side of it so it doesn't flicker there.
inline constexpr float kStepAngle = 0.96f;
inline constexpr float kStepFlip = 0.09f;
inline bool Stepping(float fwd, float side, bool stepping) {
  const float a = std::atan2(std::fabs(side), fwd);
  return stepping ? a > kStepAngle - kStepFlip : a > kStepAngle + kStepFlip;
}

// How far each foot stands out from the ready pose's place so the feet never come nearer than
// minGap: across, the gait's share across the facing; half, how far a foot moves either way of
// its place; gap, the ready pose's feet apart. Out of step by half a loop, they close by up to
// 2 |across| half.
inline float StepWiden(float across, float half, float gap, float minGap) {
  return std::max(std::fabs(across) * half - 0.5f * (gap - minGap), 0.f);
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
// The cannon's muzzle in the world (the drawn cannon's tip down the barrel, else the body's
// Charge_LCTR) as the body is posed now; false when the
// body is not ready, then the first-person muzzle stands in.
bool CannonPosition(const CPlayer& player, CVector3f& out);
// The muzzle in the world, facing down the barrel, as this frame's Render drew it; false (once
// taken, or when the body was not drawn) leaves out the cannon's charge effect.
bool TakeMuzzle(CTransform4f& out);
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
// Debug (console `thirdperson backwalk`): how Samus walks backward: the procedural backpedal (the
// default; no clip walks backward well), the walk-back clip (Phazon_Struggle_walkback's legs), or
// the forward walks played in reverse.
enum class BackWalk { Struggle, Reverse, Procedural };
void SetBackWalk(BackWalk mode);
BackWalk GetBackWalk();

} // namespace PortThirdPersonBody
