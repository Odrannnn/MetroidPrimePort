#pragma once

// Port-only: Remastered's explosion-lit visor face reflection (CVisorEffectManagerMP1 and
// CSamusFaceReflectionMP1, Switch 1.0.0). An explosion whose PART has FRMD 1..3 is registered
// while it is near the first-person camera; the face reflection's colour is the sum of each
// registered explosion's generator colour weighted by its mode's spline over the explosion's age.
// Pure, so it can be tested alone; the caller supplies each explosion's data.
// AddExplosionEffect 0xd10144, RemoveExplosionEffect 0xd10454, AddBillboardEffect 0xd0fc34,
// RemoveBillboardEffect 0xd0fffc, Update 0xd0ef2c, GetFaceReflectionAlphaModifier 0xd10e74,
// ctor 0xd0e970, CSamusFaceReflectionMP1::PreRender 0xe7c044, Draw 0xe7c318,
// SLdrSamusFaceReflectionLooseData::Load 0xd15b34 (x/PreloadMP1/RenderLooseDataMP1.LDTA).
#ifdef TARGET_PC

#include <algorithm>
#include <vector>

#include "MetroidPrime/TGameTypes.hpp"

namespace PortFaceReflection {

// The loose data, per mode 1..3 (data+0x00/+0x40/+0x80): a linear spline (min 0, max 1, pre and
// post 0, clamped) with three keys, its scale (+0x38/+0x78/+0xb8) and the registration distance
// (+0x3c/+0x7c/+0xbc).
struct Mode {
  float keyTime[3];
  float keyValue[3];
  float scale;
  float distance;
};

inline const Mode& ModeData(int mode) {
  static const Mode kModes[3] = {
      {{0.f, 0.1f, 0.902f}, {0.f, 1.f, 0.f}, 0.6f, 8.f},
      {{0.f, 0.1f, 0.8f}, {0.f, 0.99991f, 0.f}, 0.3f, 12.f},
      {{0.f, 0.1f, 1.f}, {0.f, 1.f, 0.f}, 1.f, 16.f},
  };
  return kModes[mode - 1];
}

// The mode's spline at t, times its scale. Outside the keys the spline is 0.
inline float Weight(int mode, float t) {
  const Mode& m = ModeData(mode);
  float v = 0.f;
  if (t >= m.keyTime[0] && t <= m.keyTime[2]) {
    const int i = t < m.keyTime[1] ? 0 : 1;
    const float span = m.keyTime[i + 1] - m.keyTime[i];
    const float f = span > 0.f ? (t - m.keyTime[i]) / span : 0.f;
    v = m.keyValue[i] + (m.keyValue[i + 1] - m.keyValue[i]) * f;
  }
  return std::min(1.f, std::max(0.f, v)) * m.scale;
}

struct State {
  std::vector< TUniqueId > explosions;  // +0xe0/+0xe8
  int billboards = 0;                   // +0xd0
  float t = 0.f;                        // +0xf0
  int fade = 0;                         // +0xf4: 0 none, 1 fading in, 2 fading out
};

// AddExplosionEffect: none while a HUD billboard exists, and only within the mode's distance of
// the first-person camera (the caller checks the camera and the generator).
inline void AddExplosion(State& s, TUniqueId id, int mode, float distToCamera) {
  if (s.billboards != 0 || mode < 1 || mode > 3 || !(distToCamera < ModeData(mode).distance)) {
    return;
  }
  s.explosions.push_back(id);
}

// RemoveExplosionEffect: the last entry takes the removed one's slot.
inline void RemoveExplosion(State& s, TUniqueId id) {
  std::vector< TUniqueId >::iterator it = std::find(s.explosions.begin(), s.explosions.end(), id);
  if (it != s.explosions.end()) {
    *it = s.explosions.back();
    s.explosions.pop_back();
  }
}

// The first billboard fades the reflection out, the last one going fades it back in, over 0.5 s.
inline void AddBillboard(State& s) {
  if (++s.billboards == 1) {
    s.t = 0.5f;
    s.fade = 2;
  }
}

inline void RemoveBillboard(State& s) {
  if (s.billboards > 0 && --s.billboards == 0) {
    s.t = 0.5f;
    s.fade = 1;
  }
}

// Update: skipped while soft-paused (the caller checks).
inline void Update(State& s, float dt) {
  s.t -= dt;
  if (0.f - s.t >= 0.f) {
    s.t = 0.f;
  }
  if (s.t == 0.f) {
    s.fade = 0;
  }
}

inline float AlphaModifier(const State& s) {
  switch (s.fade) {
  case 1:
    return 1.f - 2.f * s.t;
  case 2:
    return 2.f * s.t;
  default:
    return s.billboards == 0 ? 1.f : 0.f;
  }
}

} // namespace PortFaceReflection

#endif
