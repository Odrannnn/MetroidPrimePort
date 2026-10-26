#pragma once

// Trilogy's orbit/scan lock geometry and lock fade, as pure functions (tests/port_tri_orbit.cpp).
// @ fn_80254104 (zone half width), fn_80254158 (zone half height), fn_8019BDFC (lock fade timer).

namespace PortTriOrbit {

// The zone extents are truncated in the port's 640x448 coordinate basis (see CPlayerOrbit.cpp).
const float kExtentScale = 0.65f; // lbl_805C6EE0

// Trilogy divides the width by the widescreen factor: 1 at 4:3, 1.246 at 16:9.
// Port extension: other aspects take a clamped linear interpolation between the two.
inline float AspectK(float aspect) {
  const float a43 = 4.f / 3.f;
  const float a169 = 16.f / 9.f;
  const float kWide = 1.246f;
  if (!(aspect > a43)) {
    return 1.f;
  }
  if (aspect >= a169) {
    return kWide;
  }
  return 1.f + (kWide - 1.f) * (aspect - a43) / (a169 - a43);
}

// Both truncate to int (fctiwz) in the 640x448 space, before the live viewport scale.
inline int HalfWidth(float raw, float k) { return static_cast< int >(kExtentScale * raw / k); }
inline int HalfHeight(float raw) { return static_cast< int >(kExtentScale * raw); }

// The pointer's zone centre on one axis: int((ndc * 0.5 + 0.5) * viewport), ndc in [-1, 1].
inline int PointerCentre(float ndc, float viewport) {
  return static_cast< int >((ndc * 0.5f + 0.5f) * viewport);
}

// fn_8019BDFC: ramps while orbiting (clamped to the tweak duration), decays to 0 otherwise.
// A duration <= 0 is treated as an instant lock: the timer stays 0 and Ratio is 1 when orbiting.
inline float FadeTimerStep(float timer, bool orbiting, float dt, float duration) {
  if (!(duration > 0.f)) {
    return 0.f;
  }
  if (!orbiting) {
    return timer > 0.f ? (timer - dt > 0.f ? timer - dt : 0.f) : 0.f;
  }
  timer += dt;
  return timer > duration ? duration : timer;
}

// player +0x11A4 = clamp(timer / duration, 0, 1); the ring and brackets scale by 1 - this.
inline float FadeRatio(float timer, bool orbiting, float duration) {
  if (!(duration > 0.f)) {
    return orbiting ? 1.f : 0.f;
  }
  const float r = timer / duration;
  return r < 0.f ? 0.f : (r > 1.f ? 1.f : r);
}

} // namespace PortTriOrbit
