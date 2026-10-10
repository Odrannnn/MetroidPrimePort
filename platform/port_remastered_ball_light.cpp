#include "port_env.h"
#include "port_remastered_ball_light.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace PortRemasteredBallLight {
namespace {
int sEnabled = -1;
float sScale = -1.f;

// Remastered's intensity table, per glow index: { dry, fully in water } normally and while
// boosting.
constexpr float kIntensity[5][2] = {{30.f, 60.f}, {30.f, 60.f}, {30.f, 60.f}, {30.f, 60.f}, {30.f, 90.f}};
constexpr float kBoostIntensity = 450.f;
constexpr float kPi = 3.14159265f;

float SrgbToLinear(float c) {
  c = std::clamp(c, 0.f, 1.f);
  return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
} // namespace

bool Enabled() {
  if (sEnabled < 0) {
    sEnabled = port::EnvFlag("MP_REMASTERED_BALL_LIGHT", true) ? 1 : 0;
  }
  return sEnabled != 0;
}

void SetEnabled(bool on) { sEnabled = on ? 1 : 0; }

float Scale() {
  if (sScale < 0.f) {
    const float value = port::EnvFloat("MP_REMASTERED_BALL_LIGHT_SCALE", 1.f);
    sScale = value >= 0.f ? value : 1.f;
  }
  return sScale;
}

void SetScale(float scale) { sScale = std::max(scale, 0.f); }

void Update(const Inputs& in, float outLinearColor[3]) {
  const float water = std::clamp(in.water, 0.f, 1.f);
  const int g = std::clamp(in.glowIndex, 0, 4);
  const float boost = std::clamp(in.boost, 0.f, 1.f);
  const float dry = kIntensity[g][0] + (kBoostIntensity - kIntensity[g][0]) * boost;
  const float wet = kIntensity[g][1] + (kBoostIntensity - kIntensity[g][1]) * boost;
  const float intensity = (dry + (wet - dry) * water) / kPi * std::clamp(in.fade, 0.f, 1.f) * Scale();
  for (int i = 0; i < 3; ++i) {
    outLinearColor[i] = SrgbToLinear(in.srgb[i]) * intensity;
  }
}

} // namespace PortRemasteredBallLight
