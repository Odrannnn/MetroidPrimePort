#pragma once

#include <cmath>
#include <cstdint>

namespace PortMouse {
constexpr float kMaxPitch = 1.52f; // approximately 87 degrees, short of the pole
constexpr float kPi = 3.14159265358979323846f;

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
// equilibrium stays the 60 Hz one (frictionApplied: the player subtracts the
// friction this tick).
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
  const float shift =
      frictionApplied ? friction * tickScale * (1.f - gain) / gain - friction * (1.f - kRef) / kRef : 0.f;
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

  void Reset() { initialized = false; }

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
  bool Update(bool active, bool locked, float x, float y, float z,
              float dx, float dy, float sensitivity, bool invertX, bool invertY) {
    if (!active) {
      Reset();
      return false;
    }
    if (!initialized || locked) Synchronize(x, y, z);
    if (!initialized || locked) return false;
    Apply(dx, dy, sensitivity, invertX, invertY, yaw, pitch);
    return true;
  }

  // The angles Update would reach with (dx, dy), leaving the state alone.
  // Frames drawn between ticks show look input the next tick will apply.
  bool Preview(float dx, float dy, float sensitivity, bool invertX, bool invertY,
               float& outYaw, float& outPitch) const {
    outYaw = yaw;
    outPitch = pitch;
    if (!initialized) return false;
    Apply(dx, dy, sensitivity, invertX, invertY, outYaw, outPitch);
    return true;
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
