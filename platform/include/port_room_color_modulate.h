#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace PortRoomGeo {
// 0xb42074 -> 0xfb650c / 0x226fdc: DrawNormal means forced-sorted plus
// opaque depth prepass, not retail's additional unmodulated colour draw.
struct AlphaRenderPlan {
  bool visible, unsorted, sortedOpaque, depthOpaque;
};
inline AlphaRenderPlan PlanAlphaRender(float alpha, bool drawNormal) {
  const bool visible = alpha > 0.f; // AlphaBlended::ShouldRender, 0xb413dc
  const bool forced = drawNormal || alpha < 1.f;
  return {visible, visible && !forced, visible && forced, visible && drawNormal};
}

// Linear, white-RGB Alpha mode of CScriptColorModulateMP1. The importer refuses
// other modes, splines and RGB tints.
struct AlphaModulateConfig {
  float a = 1.f, b = 1.f, timeA2B = 1.f, timeB2A = 1.f;
  // CScriptColorModulateMP1+0xc8: reverse, restore, compare, update, backwards,
  // bit 9 draw-normal and bit 12 start-on-Registered (0xd5ef34/0xd5fdb8).
  // Defaults from SLdrColorModulateMP1 (0xd60d00).
  uint32_t flags = 12;
};
struct AlphaModulateState {
  float time = 0.f, alpha = 1.f;
  bool enabled = false, reversing = false, toA = false, restore = false;

  // AcceptScriptMsg(Registered), 0xd5fdb8: active + state bit 0x1000
  // self-sends Decrement; activation alone does not start the transition.
  bool Registered(const AlphaModulateConfig& c, bool active) {
    return active && (c.flags & 0x1000) != 0 && Message(c, false);
  }

  // 0xd5fdb8: Increment selects B->A; Decrement selects A->B. In particular
  // the interruption formula is NOT the retail CScriptColorModulate formula.
  bool Message(const AlphaModulateConfig& c, bool increment) {
    if (reversing) {
      reversing = false;
      toA = !toA;
      return false;
    }
    const bool writes = !enabled;
    if (!enabled) {
      alpha = increment ? c.b : c.a;
    } else if (!toA) {
      time = 0.f;
    } else if (increment) {
      time = c.timeB2A - c.timeB2A * (time / c.timeA2B);
    } else {
      time = c.timeA2B - c.timeA2B * (time / c.timeB2A);
    }
    restore = false;
    enabled = true;
    toA = increment;
    return writes;
  }
  // Returns true on completion (MaxReached). Writes the endpoint before End,
  // and ends strictly AFTER the duration, matching 0xd5fb8c and 0xd5f858.
  bool Tick(const AlphaModulateConfig& c, float dt) {
    time += dt;
    const float duration = toA ? c.timeB2A : c.timeA2B;
    float t = std::fabs(duration) < 1e-5f ? 1.f : time / duration;
    if (t > 1.f) t = 1.f;
    const float from = toA ? c.b : c.a, to = toA ? c.a : c.b;
    alpha = from + (to - from) * t;
    restore = false;
    if (!(time > duration)) {
      return false;
    }
    time = 0.f;
    if ((c.flags & 1) && !reversing) {
      reversing = true;
      toA = !toA;
      return false;
    }
    enabled = reversing = false;
    restore = (c.flags & 2) != 0;
    return true;
  }
};
} // namespace PortRoomGeo
