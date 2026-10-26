#pragma once

#include <cmath>
#include <cstdint>

namespace PortMouse {
constexpr float kMaxPitch = 1.52f; // approximately 87 degrees, short of the pole
constexpr float kPi = 3.14159265358979323846f;

// Quake/Source/Apex sensitivity scale: one count turns 0.022 degrees per unit
// (m_yaw), so the same number gives the same turn per count as those games.
// The setting itself stays in radians per count.
constexpr double kSourceRadiansPerUnit = 0.022 * 3.14159265358979323846 / 180.0;
inline float SourceFromRadians(float radiansPerCount) {
  return float(radiansPerCount / kSourceRadiansPerUnit);
}
inline float RadiansFromSource(float source) { return float(source * kSourceRadiansPerUnit); }
// The default: 3.0 on that scale, as in Quake, Source and Apex (issue #210).
constexpr float kDefaultSensitivity = float(3.0 * kSourceRadiansPerUnit);
// What one pixel of stick and touch aim travel turns, and the sensitivity at which
// the pointer follows the cursor's speed. Fixed (the old mouse default), so stick and
// touch speeds do not move with the mouse sensitivity (issue #209).
constexpr float kAimPixelRadians = 0.0035f;
// Hand travel for a full turn at a mouse's counts per inch.
inline float CentimetresPer360(float radiansPerCount, float dpi) {
  if (!(radiansPerCount > 0.f) || !(dpi > 0.f)) return 0.f;
  return float(2.0 * 3.14159265358979323846 / radiansPerCount / dpi * 2.54);
}

struct Planar {
  float right;
  float forward;
};
inline Planar ClampPlanar(Planar value, float maximum) {
  const float length = std::hypot(value.right, value.forward);
  if (!std::isfinite(length) || maximum <= 0.f) return {0.f, 0.f};
  if (length > maximum) {
    const float scale = maximum / length;
    value.right *= scale;
    value.forward *= scale;
  }
  return value;
}

// The retail force law is a 60 Hz discrete map: the friction f is subtracted
// each tick, then the force closes k = A*dt/(m*vmax) of the gap to the desired
// speed, so both the top speed (vmax - f(1-k)/k) and the ramp up to it depend
// on dt. Off 60 Hz (tickScale = dt*60 != 1, friction already scaled to f*tickScale),
// this gives the 60 Hz result over tickScale ticks: the force is scaled to
// close 1-(1-k60)^tickScale of the gap, and the desired speed is shifted so the
// post-friction speed (the one the position integrates) keeps its 60 Hz
// equilibrium (frictionApplied: the player subtracts the friction this tick).
struct ForceLaw {
  float frictionSpeed; // replaces f*m*vmax/(dt*A)
  float shift;         // added to the desired speed, signed like the input
  float gain;          // scales the force
};
inline ForceLaw ForceLawAtRate(float tickScale, float friction, float mass, float maxSpeed,
                               float dt, float acceleration, bool frictionApplied) {
  const float k = acceleration * dt / (mass * maxSpeed);
  const float kRef = acceleration / (60.f * mass * maxSpeed);
  if (!(kRef > 0.f && kRef < 1.f && k > 0.f)) return {friction / k, 0.f, 1.f};
  const float gain = 1.f - std::pow(1.f - kRef, tickScale);
  // The position integrates the post-friction speed v1 = v2 - f*tickScale, so the
  // equilibrium to keep is v1 = vmax - f/kRef, not the 60 Hz post-force speed.
  const float shift = frictionApplied ? friction * tickScale / gain - friction / kRef : 0.f;
  return {friction / kRef, shift, gain / k};
}

// The retail forward-force law applied to either horizontal axis. Damping and
// collision integration remain in the player; this is not teleport movement.
// direction is this axis's component of the unit stick direction (0: the input's
// sign). Pass it while no friction is applied: the offset then only raises the
// target, and in full on each axis it would raise a diagonal's past retail's.
inline float AxisForce(float input, float velocity, float maxSpeed, float friction,
                       float mass, float dt, float acceleration, float tickScale = 1.f,
                       bool frictionApplied = true, float direction = 0.f) {
  if (input == 0.f || maxSpeed <= 0.f || dt <= 0.f || acceleration <= 0.f) return 0.f;
  const float sign = direction != 0.f ? direction : input > 0.f ? 1.f : -1.f;
  float frictionSpeed = friction * mass * maxSpeed / (dt * acceleration);
  ForceLaw law{frictionSpeed, 0.f, 1.f};
  if (tickScale != 1.f) {
    law = ForceLawAtRate(tickScale, friction, mass, maxSpeed, dt, acceleration, frictionApplied);
    frictionSpeed = law.frictionSpeed;
  }
  const float desired = input * (maxSpeed - frictionSpeed) + sign * (frictionSpeed + law.shift);
  const float fraction = (desired - velocity) / maxSpeed;
  return (fraction < -1.f ? -1.f : fraction > 1.f ? 1.f : fraction) * acceleration * law.gain;
}

struct AimState {
  float yaw = 0.f;
  float pitch = 0.f;
  bool initialized = false;
  // The turn the last Update made from its lagged travel (lagDx, lagDy), after
  // the pitch clamp. Frames between ticks draw that part of the turn after its
  // tick rather than before it (Preview's lag), so a clamp that ate the travel
  // draws no motion.
  float lagYaw = 0.f;
  float lagPitch = 0.f;

  void Reset() {
    initialized = false;
    lagYaw = lagPitch = 0.f;
  }

  void Synchronize(float x, float y, float z) {
    const float length = std::sqrt(x * x + y * y + z * z);
    if (!std::isfinite(length) || length < 0.00001f) {
      Reset();
      return;
    }
    // A target directly overhead has no horizontal heading. Retain the last
    // heading rather than snapping to atan2(0, 0) when that lock is released.
    if (x * x + y * y > length * length * 0.00000001f) yaw = std::atan2(-x, y);
    const float up = z / length;
    pitch = ClampPitch(std::asin(up < -1.f ? -1.f : up > 1.f ? 1.f : up));
    initialized = true;
  }

  // A locked camera owns orientation. Track its effective direction, but never
  // consume hidden mouse movement or rotate the player's body underneath it.
  // (lagDx, lagDy) is the part of (dx, dy) whose turn is drawn after the tick
  // (the twin stick's), recorded in lagYaw/lagPitch.
  bool Update(bool active, bool locked, float x, float y, float z,
              float dx, float dy, float sensitivity, bool invertX, bool invertY,
              float lagDx = 0.f, float lagDy = 0.f) {
    lagYaw = lagPitch = 0.f;
    if (!active) {
      Reset();
      return false;
    }
    if (!initialized || locked) Synchronize(x, y, z);
    if (!initialized || locked) return false;
    float restYaw = yaw;
    float restPitch = pitch;
    Apply(dx - lagDx, dy - lagDy, sensitivity, invertX, invertY, restYaw, restPitch);
    Apply(dx, dy, sensitivity, invertX, invertY, yaw, pitch);
    lagYaw = static_cast<float>(std::remainder(double(yaw) - restYaw, 2.0 * kPi));
    lagPitch = pitch - restPitch;
    return true;
  }

  // The angles Update would reach with (dx, dy), leaving the state alone.
  // Frames drawn between ticks show look input the next tick will apply, on top
  // of the last tick's lagged turn with the share lag (0..1) of it not yet shown
  // taken back.
  bool Preview(float dx, float dy, float sensitivity, bool invertX, bool invertY,
               float& outYaw, float& outPitch, float lag = 0.f) const {
    outYaw = yaw;
    outPitch = pitch;
    if (!initialized) return false;
    if (std::isfinite(lag) && lag > 0.f) {
      lag = lag > 1.f ? 1.f : lag;
      outYaw = static_cast<float>(std::remainder(double(yaw) - double(lag) * lagYaw, 2.0 * kPi));
      outPitch = pitch - lag * lagPitch;
    }
    Apply(dx, dy, sensitivity, invertX, invertY, outYaw, outPitch);
    return true;
  }

  // Pointer aim's edge turn: radians, world +pitch up.
  void Turn(float dyaw, float dpitch) {
    if (!initialized || !std::isfinite(dyaw) || !std::isfinite(dpitch)) return;
    yaw = static_cast<float>(std::remainder(double(yaw) + dyaw, 2.0 * kPi));
    pitch = ClampPitch(double(pitch) + dpitch);
  }

private:
  static void Apply(float dx, float dy, float sensitivity, bool invertX, bool invertY,
                    float& outYaw, float& outPitch) {
    if (!std::isfinite(dx) || !std::isfinite(dy) ||
        !std::isfinite(sensitivity) || sensitivity <= 0.f) return;
    // SDL motion is right/down positive; world +pitch looks upward.
    const double nextYaw = outYaw + double(dx) * sensitivity * (invertX ? 1.0 : -1.0);
    const double nextPitch = outPitch + double(dy) * sensitivity * (invertY ? 1.0 : -1.0);
    outYaw = static_cast<float>(std::remainder(nextYaw, 2.0 * kPi));
    outPitch = ClampPitch(nextPitch);
  }

  static float ClampPitch(double angle) {
    return static_cast<float>(angle < -kMaxPitch ? -kMaxPitch : angle > kMaxPitch ? kMaxPitch : angle);
  }
};

// Pointer aim (the Trilogy/Prime 3 scheme): the mouse moves a pointer over the
// screen, x and y in -1..1 of the view's half width/height (right/down
// positive). Inside the dead box the view holds still; past it the view turns
// toward that edge, faster the further out the pointer is.
struct PointerAim {
  float x = 0.f;
  float y = 0.f;
  // How far the last MoveLagged's lagged travel moved the pointer, after the
  // edge clamp (see AimState::lagYaw).
  float lagX = 0.f;
  float lagY = 0.f;

  void Center() { x = y = lagX = lagY = 0.f; }

  // dx, dy: window-space travel already scaled to the -1..1 range.
  static void Move(float dx, float dy, float& outX, float& outY) {
    if (!std::isfinite(dx) || !std::isfinite(dy)) return;
    outX = Clamp1(outX + dx);
    outY = Clamp1(outY + dy);
  }
  void Move(float dx, float dy) { MoveLagged(dx, dy, 0.f, 0.f); }
  // (lagDx, lagDy) is the part of (dx, dy) drawn after the tick.
  void MoveLagged(float dx, float dy, float lagDx, float lagDy) {
    float restX = x;
    float restY = y;
    Move(dx - lagDx, dy - lagDy, restX, restY);
    Move(dx, dy, x, y);
    lagX = x - restX;
    lagY = y - restY;
  }
  // Where a frame between ticks draws the pointer: the travel (dx, dy) since the
  // tick on top of the last move, with the share lag (0..1) of its lagged travel
  // not yet shown taken back.
  void Presented(float lag, float dx, float dy, float& outX, float& outY) const {
    outX = x;
    outY = y;
    if (std::isfinite(lag) && lag > 0.f) {
      lag = lag > 1.f ? 1.f : lag;
      outX -= lag * lagX;
      outY -= lag * lagY;
    }
    Move(dx, dy, outX, outY);
  }

  // How far past the dead box (0..1) the pointer is on one axis, signed.
  static float Overshoot(float value, float box) {
    if (!(box >= 0.f && box < 1.f)) return 0.f;
    const float past = (std::fabs(value) - box) / (1.f - box);
    if (!(past > 0.f)) return 0.f;
    return value < 0.f ? -std::fmin(past, 1.f) : std::fmin(past, 1.f);
  }

  // The view turn for dt seconds at (px, py): radians, world yaw (+left) and
  // pitch (+up). maxRate is the turn speed in radians/second at the screen edge.
  static void TurnAt(float px, float py, float box, float maxRate, float dt, float& dyaw,
                     float& dpitch) {
    dyaw = dpitch = 0.f;
    if (!std::isfinite(maxRate) || !std::isfinite(dt) || maxRate <= 0.f || dt <= 0.f) return;
    dyaw = -Overshoot(px, box) * maxRate * dt;
    dpitch = -Overshoot(py, box) * maxRate * dt;
  }

  // The view ray through (px, py) in camera space as yaw (+right) and pitch (+up)
  // angles applied in that order, for a vertical fov (degrees) and aspect ratio.
  static void RayAngles(float px, float py, float fovYDeg, float aspect, float& yaw,
                        float& pitch) {
    yaw = pitch = 0.f;
    const float tanY = std::tan(0.5f * fovYDeg * (kPi / 180.f));
    if (!std::isfinite(tanY) || !(tanY > 0.f) || !(aspect > 0.f)) return;
    const float right = px * tanY * aspect;
    const float up = -py * tanY;
    yaw = std::atan(right);
    pitch = std::atan2(up, std::sqrt(1.f + right * right));
  }

  // Scales (yaw, pitch) so the direction stays within maxAngle of straight ahead.
  // The angle is not linear in the scale, so a few passes settle it onto maxAngle.
  static void ClampAngles(float& yaw, float& pitch, float maxAngle) {
    for (int pass = 0; pass < 4; ++pass) {
      const float angle = std::acos(std::cos(yaw) * std::cos(pitch));
      if (!std::isfinite(angle) || !(angle > maxAngle) || !(angle > 0.f)) return;
      const float scale = maxAngle / angle;
      yaw *= scale;
      pitch *= scale;
    }
  }

private:
  static float Clamp1(float v) { return v < -1.f ? -1.f : v > 1.f ? 1.f : v; }
};

// The game applies the aim twice a tick: CPlayer::ProcessInput with the tick's
// travel, then CPlayer::Update to revalidate it with none left. Only the first
// applied update of a tick records the turn drawn after it (AimState::lagYaw,
// PointerAim::lagX); the second would clear it, and the twin stick would turn
// the view once a tick between smooth frames (issue #140). Only the first makes
// the tick's pointer edge turn too: a second turned the view twice the set
// speed, and the frames between ticks, which draw one tick's turn, jumped by
// the other half at each tick.
class TickAim {
public:
  void BeginTick() { mRecorded = false; }
  // No update has been applied yet this tick.
  bool First() const { return !mRecorded; }
  // After an applied update; `aim` and `pointer` were `aimBefore` and
  // `pointerBefore` ahead of it. pointerFree: the update left pointer aim free.
  void Applied(AimState& aim, const AimState& aimBefore, PointerAim& pointer,
               const PointerAim& pointerBefore, bool pointerFree) {
    if (mRecorded) {
      aim.lagYaw = aimBefore.lagYaw;
      aim.lagPitch = aimBefore.lagPitch;
      if (pointerFree) {
        pointer.lagX = pointerBefore.lagX;
        pointer.lagY = pointerBefore.lagY;
      }
    }
    mRecorded = true;
  }
private:
  bool mRecorded = false;
};

class ButtonGate {
public:
  void Reset() { mReady = false; }
  uint32_t Poll(bool enabled, uint32_t held) {
    if (!enabled) {
      Reset();
      return 0;
    }
    // A UI click or a button held while acquiring capture is not a new shot.
    // Once neutral has been observed, preserve held states for charge/release.
    if (!mReady) {
      mReady = held == 0;
      return 0;
    }
    return held;
  }
private:
  bool mReady = false;
};

// Buttons held on real mice. SDL_GetMouseState also reports the mouse SDL
// synthesises from touches and pens, which Android turns on for ImGui, so a
// finger on the screen read as a held left button (fire). Fed from button
// events, skipping the synthetic ones.
class HeldButtons {
public:
  void Note(bool synthetic, uint32_t mask, bool down) {
    if (synthetic) return;
    mHeld = down ? (mHeld | mask) : (mHeld & ~mask);
  }
  // Releases can go missing when focus or the device goes away.
  void Clear() { mHeld = 0; }
  uint32_t Held() const { return mHeld; }
private:
  uint32_t mHeld = 0;
};
} // namespace PortMouse
