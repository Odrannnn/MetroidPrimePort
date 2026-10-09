#include "port_mouse.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>

namespace {
void Check(bool condition) {
  if (!condition) {
    std::fputs("mouse aim regression failed\n", stderr);
    std::abort();
  }
}
bool Near(float a, float b) { return std::fabs(a - b) < 0.00001f; }
}

int main() {
  const auto diagonal = PortMouse::ClampPlanar({1.f, 1.f}, 1.f);
  Check(std::fabs(std::hypot(diagonal.right, diagonal.forward) - 1.f) < 0.00001f);
  Check(PortMouse::AxisForce(1.f, 0.f, 12.f, 0.1f, 90.f, 1.f / 60.f, 1000.f) > 0.f);
  Check(PortMouse::AxisForce(-1.f, 0.f, 12.f, 0.1f, 90.f, 1.f / 60.f, 1000.f) < 0.f);
  Check(PortMouse::AxisForce(0.f, 5.f, 12.f, 0.1f, 90.f, 1.f / 60.f, 1000.f) == 0.f);
  // The player's map per tick (friction f*s, then the force over dt) reaches
  // the 60 Hz top speed and ramp at any tick rate.
  const auto run = [](float rate, float seconds) {
    const float dt = 1.f / rate;
    const float tickScale = dt * 60.f;
    float v = 0.f;
    for (int tick = 0; tick < int(seconds * rate + 0.5f); ++tick) {
      v = std::fmax(0.f, v - 0.1f * tickScale);
      v += PortMouse::AxisForce(1.f, v, 12.f, 0.1f, 90.f, dt, 1000.f, tickScale) / 90.f * dt;
    }
    return v;
  };
  for (const float rate : {30.f, 90.f, 120.f, 240.f}) {
    Check(std::fabs(run(rate, 20.f) - run(60.f, 20.f)) < 0.01f);
    Check(std::fabs(run(rate, 1.f) - run(60.f, 1.f)) < 0.05f);
  }
  // Without friction (a jump's rise), a diagonal stick heads for the same top
  // speed as a straight one, not faster.
  const auto planar = [](float right, float forward) {
    const float dt = 1.f / 60.f;
    const float length = std::hypot(right, forward);
    float vr = 0.f, vf = 0.f;
    for (int tick = 0; tick < 1200; ++tick) {
      const auto force = PortMouse::ClampPlanar(
          {PortMouse::AxisForce(right, vr, 12.f, 0.1f, 90.f, dt, 1000.f, 1.f, false, right / length),
           PortMouse::AxisForce(forward, vf, 12.f, 0.1f, 90.f, dt, 1000.f, 1.f, false, forward / length)},
          1000.f);
      vr += force.right / 90.f * dt;
      vf += force.forward / 90.f * dt;
    }
    return std::hypot(vr, vf);
  };
  Check(std::fabs(planar(0.7071068f, 0.7071068f) - planar(0.f, 1.f)) < 0.01f);
  const auto speed = PortMouse::ClampPlanar({20.f, 20.f}, 12.f);
  Check(std::hypot(speed.right, speed.forward) <= 12.0001f);
  PortMouse::AimState aim;
  constexpr float sensitivity = 0.0035f;
  // Right/up mouse motion must turn right/up in world coordinates, immediately.
  Check(aim.Update(true, false, 0.f, 1.f, 0.f, 100.f, -100.f, sensitivity, false, false));
  Check(Near(aim.yaw, -0.35f) && Near(aim.pitch, 0.35f));
  aim.Reset();
  Check(aim.Update(true, false, 0.f, 1.f, 0.f, 100.f, 100.f, sensitivity, true, true));
  Check(Near(aim.yaw, 0.35f) && Near(aim.pitch, 0.35f));

  aim.Update(true, false, 0.f, 1.f, 0.f, 0.f, -2000.f, sensitivity, false, false);
  Check(Near(aim.pitch, PortMouse::kMaxPitch));
  aim.Update(true, false, 0.f, 1.f, 0.f, 0.f, 1.f, sensitivity, false, false);
  Check(Near(aim.pitch, PortMouse::kMaxPitch - sensitivity));
  aim.Update(true, false, 0.f, 1.f, 0.f, 100000.f, 4000.f, sensitivity, false, false);
  Check(std::fabs(aim.yaw) <= PortMouse::kPi && Near(aim.pitch, -PortMouse::kMaxPitch));

  // Target lock updates effective aim but discards hidden motion. Releasing it
  // keeps the camera's last direction, even if the body points elsewhere.
  Check(!aim.Update(true, true, 1.f, 0.f, 0.5f, 500.f, 500.f, sensitivity, false, false));
  Check(Near(aim.yaw, -PortMouse::kPi / 2.f) && Near(aim.pitch, std::atan(0.5f)));
  const float lockedPitch = aim.pitch;
  aim.Update(true, false, 0.f, 1.f, 0.f, 0.f, 0.f, sensitivity, false, false);
  Check(Near(aim.pitch, lockedPitch) && Near(aim.yaw, -PortMouse::kPi / 2.f));
  const float beforePole = aim.yaw;
  aim.Synchronize(0.f, 0.f, 1.f);
  Check(Near(aim.yaw, beforePole) && Near(aim.pitch, PortMouse::kMaxPitch));

  // Cinematic/morph/menu handoff must rebase instead of reusing stale angles.
  Check(!aim.Update(false, false, 0.f, 1.f, 0.f, 500.f, 500.f, sensitivity, false, false));
  Check(!aim.initialized);
  aim.Update(true, false, -1.f, 0.f, -0.5f, 0.f, 0.f, sensitivity, false, false);
  Check(Near(aim.yaw, PortMouse::kPi / 2.f) && Near(aim.pitch, -lockedPitch));
  const float before = aim.pitch;
  aim.Update(true, false, 0.f, 1.f, 0.f, 0.f,
             std::numeric_limits<float>::infinity(), sensitivity, false, false);
  Check(Near(before, aim.pitch));
  // Preview reaches Update's angles without touching the state.
  float previewYaw = 0.f;
  float previewPitch = 0.f;
  const float stateYaw = aim.yaw;
  const float statePitch = aim.pitch;
  Check(aim.Preview(900.f, -40.f, sensitivity, true, false, previewYaw, previewPitch));
  Check(aim.yaw == stateYaw && aim.pitch == statePitch);
  aim.Update(true, false, 0.f, 1.f, 0.f, 900.f, -40.f, sensitivity, true, false);
  Check(aim.yaw == previewYaw && aim.pitch == previewPitch);
  Check(aim.Preview(0.f, 1.e6f, sensitivity, false, false, previewYaw, previewPitch));
  Check(Near(previewPitch, -PortMouse::kMaxPitch));
  aim.Reset();
  Check(!aim.Preview(1.f, 1.f, sensitivity, false, false, previewYaw, previewPitch));
  Check(!aim.Update(true, false, 0.f, 0.f, 0.f, 1.f, 1.f, sensitivity, false, false));

  PortMouse::ButtonGate buttons;
  Check(buttons.Poll(true, 1) == 0); // click used to enter capture/UI is not fire
  Check(buttons.Poll(true, 1) == 0);
  Check(buttons.Poll(true, 0) == 0);
  for (int tick = 0; tick < 120; ++tick) Check(buttons.Poll(true, 1) == 1);
  Check(buttons.Poll(true, 0) == 0); // charge-release edge remains visible
  Check(buttons.Poll(true, 6) == 6); // simultaneous lock-on/missile buttons
  Check(buttons.Poll(false, 1) == 0);
  Check(buttons.Poll(true, 1) == 0); // do not resume a held click on refocus
  buttons.Poll(true, 0);
  Check(buttons.Poll(true, 1) == 1);

  PortMouse::HeldButtons held;
  held.Note(true, 1, true); // a touch-synthesised click is not a mouse button
  Check(held.Held() == 0);
  held.Note(false, 1, true);
  held.Note(false, 4, true);
  held.Note(true, 1, false); // nor does a synthetic release clear a real press
  Check(held.Held() == 5);
  held.Note(false, 1, false);
  Check(held.Held() == 4);
  held.Clear();
  Check(held.Held() == 0);
  std::puts("mouse axes, limits, handoff and held-button regressions passed");
}
