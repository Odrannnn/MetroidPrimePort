// The third-person body's blend logic (platform/include/port_third_person_body.h).
#include "port_test.h"
#include "port_third_person_body.h"

#include <algorithm>
#include <cmath>

using namespace PortThirdPersonBody;

namespace {
bool Near(float a, float b) { return std::fabs(a - b) < 1e-3f; }
} // namespace

int main() {
  // Stances: standing, walking any way.
  CHECK(ChooseStance(0.f, 0.f, true, 0.f, Stance::Idle, true, false) == Stance::Idle);
  CHECK(ChooseStance(0.2f, 0.1f, true, 0.f, Stance::Idle, true, false) == Stance::Idle);
  CHECK(ChooseStance(5.f, 0.f, true, 0.f, Stance::Idle, true, false) == Stance::Loco);
  CHECK(ChooseStance(-5.f, 0.f, true, 0.f, Stance::Idle, true, false) == Stance::Loco);
  CHECK(ChooseStance(0.4f, 5.f, true, 0.f, Stance::Idle, true, false) == Stance::Loco); // strafing

  // Strafing legs: forward they turn toward the way the body goes (+ left), backward toward
  // the way it came from; at the side they keep walking the way they were, swapping only
  // kLegFlip past it.
  constexpr float kQuarter = 1.5707963f;
  CHECK(Near(LegYaw(5.f, 0.f, false), 0.f));
  CHECK(Near(LegYaw(0.f, 5.f, false), -kQuarter)); // right
  CHECK(Near(LegYaw(0.f, -5.f, false), kQuarter)); // left
  CHECK(Near(LegYaw(-5.f, 0.f, true), 0.f));
  CHECK(Near(LegYaw(-5.f, 5.f, true), kQuarter / 2.f)); // back right: legs to the front left
  CHECK(!LegsBackward(0.f, 5.f, false));
  CHECK(LegsBackward(0.f, 5.f, true));
  CHECK(!LegsBackward(-0.5f, 5.f, false));
  CHECK(LegsBackward(-2.f, 5.f, false));
  CHECK(!LegsBackward(2.f, 5.f, true));
  CHECK(LegsBackward(-5.f, 0.f, false));
  // Stopping from a walk plays the stop clip to its end, unless the body moves again.
  CHECK(ChooseStance(0.f, 0.f, true, 0.f, Stance::Loco, true, false) == Stance::Stop);
  CHECK(ChooseStance(0.f, 0.f, true, 0.f, Stance::Stop, false, false) == Stance::Stop);
  CHECK(ChooseStance(0.f, 0.f, true, 0.f, Stance::Stop, true, false) == Stance::Idle);
  CHECK(ChooseStance(5.f, 0.f, true, 0.f, Stance::Stop, false, false) == Stance::Loco);
  // In the air: rising jumps, dropping falls, at any speed.
  CHECK(ChooseStance(9.f, 0.f, false, 3.f, Stance::Loco, true, false) == Stance::Jump);
  CHECK(ChooseStance(9.f, 0.f, false, -3.f, Stance::Jump, true, false) == Stance::Fall);
  CHECK(ChooseStance(0.f, 0.f, false, -1.f, Stance::Idle, true, false) == Stance::Fall);
  // Landing: a long fall plays Land (even at the touchdown's speed blip), to its end standing
  // still; a short hop doesn't, and moving leaves it.
  CHECK(ChooseStance(0.f, 0.f, true, 0.f, Stance::Fall, true, true) == Stance::Land);
  CHECK(ChooseStance(0.f, 0.f, true, 0.f, Stance::Jump, true, true) == Stance::Land);
  CHECK(ChooseStance(0.f, 0.f, true, 0.f, Stance::Fall, true, false) == Stance::Idle);
  CHECK(ChooseStance(5.f, 0.f, true, 0.f, Stance::Fall, true, true) == Stance::Land);
  CHECK(ChooseStance(5.f, 0.f, true, 0.f, Stance::Fall, true, false) == Stance::Loco);
  CHECK(ChooseStance(0.f, 0.f, true, 0.f, Stance::Land, false, false) == Stance::Land);
  CHECK(ChooseStance(0.f, 0.f, true, 0.f, Stance::Land, true, false) == Stance::Idle);
  CHECK(ChooseStance(5.f, 0.f, true, 0.f, Stance::Land, false, false) == Stance::Loco);

  // The walk clips by speed: one alone outside their range, the bracketing two linearly inside,
  // continuous where the pairs meet.
  constexpr int kLocoClips = 3;
  const float speeds[kLocoClips] = {1.5f, 2.f, 9.f};
  float w[kLocoClips];
  LocoWeights(1.f, speeds, w);
  CHECK(w[0] == 1.f && w[1] == 0.f && w[2] == 0.f);
  LocoWeights(1.75f, speeds, w);
  CHECK(Near(w[0], 0.5f) && Near(w[1], 0.5f) && w[2] == 0.f);
  LocoWeights(2.f, speeds, w);
  CHECK(Near(w[1], 1.f) && Near(w[0] + w[2], 0.f));
  LocoWeights(5.5f, speeds, w);
  CHECK(w[0] == 0.f && Near(w[1], 0.5f) && Near(w[2], 0.5f));
  LocoWeights(12.f, speeds, w);
  CHECK(w[2] == 1.f && w[0] == 0.f && w[1] == 0.f);
  const float same[kLocoClips] = {2.f, 2.f, 9.f}; // a missing slow walk falls back to the walk
  LocoWeights(2.f, same, w);
  CHECK(Near(w[0] + w[1] + w[2], 1.f));

  // A foot curve's peak, from its first harmonic.
  float curve[16];
  for (int k = 0; k < 16; ++k) {
    curve[k] = std::cos(6.2831853f * (k / 16.f - 0.25f));
  }
  CHECK(Near(CurvePeak(curve, 16), 0.25f));
  for (int k = 0; k < 16; ++k) {
    curve[k] = std::cos(6.2831853f * (k / 16.f - 0.9f)) + 0.3f;
  }
  CHECK(Near(CurvePeak(curve, 16), 0.9f));

  // Fades.
  CHECK(Approach(0.f, 1.f, 0.3f) == 0.3f && Approach(0.9f, 1.f, 0.3f) == 1.f);
  CHECK(Approach(1.f, 0.f, 0.3f) == 0.7f && Approach(0.1f, 0.f, 0.3f) == 0.f);

  // The aim grid's smoothing: speed and acceleration limited, lands on the target.
  float x = 0.f;
  float v = 0.f;
  float maxSpeed = 0.f;
  float lastSpeed = 0.f;
  bool accelOk = true;
  for (int i = 0; i < 240; ++i) {
    SmoothToward(x, v, 1.f, 1.f / 60.f);
    maxSpeed = std::max(maxSpeed, std::fabs(v));
    accelOk = accelOk && (v == 0.f || std::fabs(v - lastSpeed) <= kAimGridAccel / 60.f + 1e-4f);
    lastSpeed = v;
    CHECK(x <= 1.f);
  }
  CHECK(x == 1.f && v == 0.f && maxSpeed <= kAimGridSpeed && accelOk);

  // Bilinear grid weights: sum to one, corners and centre exact.
  float g[9];
  AimGridWeights(0.f, 0.f, g);
  CHECK(Near(g[4], 1.f));
  AimGridWeights(1.f, 1.f, g);
  CHECK(Near(g[8], 1.f));
  AimGridWeights(-1.f, -1.f, g);
  CHECK(Near(g[0], 1.f));
  AimGridWeights(-3.f, 0.5f, g); // clamped to the left edge
  CHECK(Near(g[3], 0.5f) && Near(g[6], 0.5f));
  AimGridWeights(0.3f, -0.6f, g);
  float sum = 0.f;
  for (float e : g) {
    sum += e;
  }
  CHECK(Near(sum, 1.f) && Near(g[1], 0.7f * 0.6f) && Near(g[2], 0.3f * 0.6f));
  return port_test::Failures() == 0 ? 0 : 1;
}
