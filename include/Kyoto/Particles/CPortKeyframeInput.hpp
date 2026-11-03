#pragma once

// Port-only PKYF (Remastered's KEYF, CREKeyframeInput ctor 0x296490, GetValue 0x29661c; the
// vector CVEKeyframeInput ctor 0x2a18fc, GetValue 0x2a19b8 runs the same steps on 12-byte keys):
// keys read at an input element's value instead of the age, wrapped from the loop start once past
// the last key when looping, and blended linearly between neighbours. D is 1 / R, divided once.
// Header only, so a test can link it alone.

#include "Kyoto/Math/CVector3f.hpp"
#include "Kyoto/Particles/CRealElement.hpp"

#include <utility>
#include <vector>

// The key timing both share: the key k for the input t, and the weight f of key k + 1 when it
// blends (false: key k alone).
struct CPortKeyframeTiming {
  bool x4_loop;
  int x8_count;
  float xc_loopStart;
  float x10_start;
  float x14_rate;
  float x18_step;

  CPortKeyframeTiming(bool loop, int count, float loopStart, float start, float rate)
  : x4_loop(loop)
  , x8_count(count)
  , xc_loopStart(loopStart)
  , x10_start(start)
  , x14_rate(rate)
  , x18_step(1.f / rate) {}

  // Each product and sum is its own statement: the original does not fuse them.
  bool Locate(float t, int& k, float& f) const {
    const int last = x8_count - 1;
    if (x4_loop) {
      const float lastStep = x18_step * static_cast< float >(last);
      const float end = x10_start + lastStep;
      if (t >= end) {
        const float span = end - xc_loopStart;
        t = t - end;
        // An empty loop span would never leave the loop; the original hangs there.
        while (t >= span && span > 0.f) {
          t = t - span;
        }
        t = xc_loopStart + t;
      }
    }
    const float u = t - x10_start;
    const float scaled = x14_rate * u;
    // fcvtzs: truncation that saturates, NaN is 0.
    int index = 0;
    if (scaled >= 2147483648.f) {
      index = 0x7fffffff;
    } else if (scaled > -2147483648.f) {
      index = static_cast< int >(scaled);
    } else if (scaled == scaled) {
      index = static_cast< int >(0x80000000u);
    }
    k = x8_count > index ? index : last;
    if (index < 0) {
      k = 0;
    }
    if (k < 0 || last <= k) {
      return false;
    }
    const float stepK = x18_step * static_cast< float >(k);
    const float rest = u - stepK;
    f = x14_rate * rest;
    if (0.f - f >= 0.f) {
      f = 0.f;
    }
    if (f + -1.f >= 0.f) {
      f = 1.f;
    }
    return true;
  }
};

class CREPortKeyframeInput : public CRealElement {
  CPortKeyframeTiming x4_timing;
  std::vector< float > x1c_keys;
  CRealElement* x20_input;

public:
  CREPortKeyframeInput(bool loop, int count, float loopStart, float start, float rate,
                       std::vector< float > keys, CRealElement* input)
  : x4_timing(loop, count, loopStart, start, rate), x1c_keys(std::move(keys)), x20_input(input) {}
  ~CREPortKeyframeInput() override { delete x20_input; }

  bool GetValue(int frame, float& valOut) const override {
    float t = 0.f;
    if (x20_input) {
      x20_input->GetValue(frame, t);
    }
    valOut = Evaluate(t);
    return false;
  }

  // The value for the input t.
  float Evaluate(float t) const {
    if (x4_timing.x8_count <= 0 || x1c_keys.size() < static_cast< size_t >(x4_timing.x8_count)) {
      return 0.f;
    }
    int k;
    float f;
    if (!x4_timing.Locate(t, k, f)) {
      return x1c_keys[k];
    }
    const float below = x1c_keys[k] * (1.f - f);
    const float above = x1c_keys[k + 1] * f;
    return above + below;
  }
};

class CVEPortKeyframeInput : public CVectorElement {
  CPortKeyframeTiming x4_timing;
  std::vector< CVector3f > x1c_keys;
  CRealElement* x20_input;

public:
  CVEPortKeyframeInput(bool loop, int count, float loopStart, float start, float rate,
                       std::vector< CVector3f > keys, CRealElement* input)
  : x4_timing(loop, count, loopStart, start, rate), x1c_keys(std::move(keys)), x20_input(input) {}
  ~CVEPortKeyframeInput() override { delete x20_input; }

  bool GetValue(int frame, CVector3f& valOut) const override {
    float t = 0.f;
    if (x20_input) {
      x20_input->GetValue(frame, t);
    }
    valOut = Evaluate(t);
    return false;
  }

  // The value for the input t, each component blended as the real one.
  CVector3f Evaluate(float t) const {
    if (x4_timing.x8_count <= 0 || x1c_keys.size() < static_cast< size_t >(x4_timing.x8_count)) {
      return CVector3f(0.f, 0.f, 0.f);
    }
    int k;
    float f;
    if (!x4_timing.Locate(t, k, f)) {
      return x1c_keys[k];
    }
    const float g = 1.f - f;
    const CVector3f& a = x1c_keys[k];
    const CVector3f& b = x1c_keys[k + 1];
    const float bx = b.GetX() * f;
    const float by = b.GetY() * f;
    const float bz = b.GetZ() * f;
    const float ax = a.GetX() * g;
    const float ay = a.GetY() * g;
    const float az = a.GetZ() * g;
    return CVector3f(bx + ax, by + ay, bz + az);
  }
};
