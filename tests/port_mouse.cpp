#include "port_mouse.h"
#include "port_test.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>

namespace {
bool Near(float a, float b) { return std::fabs(a - b) < 0.00001f; }
}

int main() {
  // Source scale: 1.0 is 0.022 degrees a count, and the conversion round-trips.
  REQUIRE(std::fabs(PortMouse::RadiansFromSource(1.f) - 0.022f * PortMouse::kPi / 180.f) < 1e-9f);
  REQUIRE(std::fabs(PortMouse::SourceFromRadians(0.0035f) - 9.115f) < 0.001f);
  REQUIRE(Near(PortMouse::SourceFromRadians(PortMouse::RadiansFromSource(2.5f)), 2.5f));
  // Sensitivity 1 at 800 DPI is the familiar ~51.95 cm per turn.
  REQUIRE(std::fabs(PortMouse::CentimetresPer360(PortMouse::RadiansFromSource(1.f), 800.f) - 51.95f) < 0.01f);
  REQUIRE(PortMouse::CentimetresPer360(0.f, 800.f) == 0.f);
  const auto diagonal = PortMouse::ClampPlanar({1.f, 1.f}, 1.f);
  REQUIRE(std::fabs(std::hypot(diagonal.right, diagonal.forward) - 1.f) < 0.00001f);
  REQUIRE(PortMouse::AxisForce(1.f, 0.f, 12.f, 0.1f, 90.f, 1.f / 60.f, 1000.f) > 0.f);
  REQUIRE(PortMouse::AxisForce(-1.f, 0.f, 12.f, 0.1f, 90.f, 1.f / 60.f, 1000.f) < 0.f);
  REQUIRE(PortMouse::AxisForce(0.f, 5.f, 12.f, 0.1f, 90.f, 1.f / 60.f, 1000.f) == 0.f);
  // The player's map per tick (friction f*s, then the force over dt) moves at
  // the 60 Hz top speed and ramp at any tick rate.
  const auto run = [](float rate, float seconds) {
    const float dt = 1.f / rate;
    const float tickScale = dt * 60.f;
    float v = 0.f;
    float moved = 0.f; // the post-friction speed, which the position integrates
    for (int tick = 0; tick < int(seconds * rate + 0.5f); ++tick) {
      v = std::fmax(0.f, v - 0.1f * tickScale);
      moved = v;
      v += PortMouse::AxisForce(1.f, v, 12.f, 0.1f, 90.f, dt, 1000.f, tickScale) / 90.f * dt;
    }
    return moved;
  };
  for (const float rate : {30.f, 90.f, 120.f, 240.f}) {
    REQUIRE(std::fabs(run(rate, 20.f) - run(60.f, 20.f)) < 0.01f);
    REQUIRE(std::fabs(run(rate, 1.f) - run(60.f, 1.f)) < 0.05f);
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
  REQUIRE(std::fabs(planar(0.7071068f, 0.7071068f) - planar(0.f, 1.f)) < 0.01f);
  {
    // Direct aim moves along the unit stick direction by the retail forward law,
    // with friction taken in the stick's axes (CPlayer::GetDampedClampedVelocityWR).
    // Constants (plausible, not the retail tweak): vmax 12, friction 0.1, mass 90,
    // acceleration 5000, 60 Hz.
    constexpr float vmax = 12.f, fr = 0.1f, mass = 90.f, accel = 5000.f, dt = 1.f / 60.f;
    // (a) The single force equals the inline retail forward formula bit for bit.
    float vRetail = 0.f, vAxis = 0.f;
    for (int tick = 0; tick < 300; ++tick) {
      const float fs = fr * mass * vmax / (dt * accel);
      const float desired = 1.f * (vmax - fs) + fs * 1.f;
      float fraction = (desired - vRetail) / vmax;
      fraction = fraction < -1.f ? -1.f : fraction > 1.f ? 1.f : fraction;
      vRetail += fraction * accel / mass * dt;
      vRetail = std::fmax(0.f, vRetail - fr);
      vAxis += PortMouse::AxisForce(1.f, vAxis, vmax, fr, mass, dt, accel) / mass * dt;
      vAxis = std::fmax(0.f, vAxis - fr);
      REQUIRE(vRetail == vAxis);
    }
    // (b) A diagonal unit stick, velocity kept in world (right, forward) and
    // friction taken per axis of the stick frame, tracks the straight run.
    const auto step = [&](float sr, float sf, float& vr, float& vf, float input) {
      const float along = vr * sr + vf * sf;
      const float force = PortMouse::AxisForce(input, along, vmax, fr, mass, dt, accel);
      vr += force * sr / mass * dt;
      vf += force * sf / mass * dt;
      float a = vr * sr + vf * sf;
      float c = vr * sf - vf * sr;
      a = a > 0.f ? std::fmax(0.f, a - fr) : std::fmin(0.f, a + fr);
      c = c > 0.f ? std::fmax(0.f, c - fr) : std::fmin(0.f, c + fr);
      vr = a * sr + c * sf;
      vf = a * sf - c * sr;
    };
    const float root = 0.7071067811865476f;
    float sr = 0.f, sf = 0.f, dr = 0.f, df = 0.f;
    float diagonalTop = 0.f;
    for (int tick = 0; tick < 300; ++tick) {
      step(0.f, 1.f, sr, sf, 1.f);
      step(root, root, dr, df, 1.f);
      REQUIRE(std::fabs(std::hypot(dr, df) - std::hypot(sr, sf)) < 1e-4f);
      diagonalTop = std::hypot(dr, df);
    }
    REQUIRE(diagonalTop > 0.5f * vmax);
    // (c) After release it slows to a halt along the stick, never sideways.
    for (int tick = 0; tick < 600; ++tick) {
      step(root, root, dr, df, 0.f);
      REQUIRE(std::fabs(dr * root - df * root) < 1e-4f);
    }
    REQUIRE(std::hypot(dr, df) < 1e-4f);
  }
  const auto speed = PortMouse::ClampPlanar({20.f, 20.f}, 12.f);
  REQUIRE(std::hypot(speed.right, speed.forward) <= 12.0001f);
  PortMouse::AimState aim;
  constexpr float sensitivity = 0.0035f;
  // Right/up mouse motion must turn right/up in world coordinates, immediately.
  REQUIRE(aim.Update(true, false, 0.f, 1.f, 0.f, 100.f, -100.f, sensitivity, false, false));
  REQUIRE(Near(aim.yaw, -0.35f) && Near(aim.pitch, 0.35f));
  aim.Reset();
  REQUIRE(aim.Update(true, false, 0.f, 1.f, 0.f, 100.f, 100.f, sensitivity, true, true));
  REQUIRE(Near(aim.yaw, 0.35f) && Near(aim.pitch, 0.35f));

  aim.Update(true, false, 0.f, 1.f, 0.f, 0.f, -2000.f, sensitivity, false, false);
  REQUIRE(Near(aim.pitch, PortMouse::kMaxPitch));
  aim.Update(true, false, 0.f, 1.f, 0.f, 0.f, 1.f, sensitivity, false, false);
  REQUIRE(Near(aim.pitch, PortMouse::kMaxPitch - sensitivity));
  aim.Update(true, false, 0.f, 1.f, 0.f, 100000.f, 4000.f, sensitivity, false, false);
  REQUIRE(std::fabs(aim.yaw) <= PortMouse::kPi && Near(aim.pitch, -PortMouse::kMaxPitch));

  // Target lock updates effective aim but discards hidden motion. Releasing it
  // keeps the camera's last direction, even if the body points elsewhere.
  REQUIRE(!aim.Update(true, true, 1.f, 0.f, 0.5f, 500.f, 500.f, sensitivity, false, false));
  REQUIRE(Near(aim.yaw, -PortMouse::kPi / 2.f) && Near(aim.pitch, std::atan(0.5f)));
  const float lockedPitch = aim.pitch;
  aim.Update(true, false, 0.f, 1.f, 0.f, 0.f, 0.f, sensitivity, false, false);
  REQUIRE(Near(aim.pitch, lockedPitch) && Near(aim.yaw, -PortMouse::kPi / 2.f));
  const float beforePole = aim.yaw;
  aim.Synchronize(0.f, 0.f, 1.f);
  REQUIRE(Near(aim.yaw, beforePole) && Near(aim.pitch, PortMouse::kMaxPitch));

  // Cinematic/morph/menu handoff must rebase instead of reusing stale angles.
  REQUIRE(!aim.Update(false, false, 0.f, 1.f, 0.f, 500.f, 500.f, sensitivity, false, false));
  REQUIRE(!aim.initialized);
  aim.Update(true, false, -1.f, 0.f, -0.5f, 0.f, 0.f, sensitivity, false, false);
  REQUIRE(Near(aim.yaw, PortMouse::kPi / 2.f) && Near(aim.pitch, -lockedPitch));
  const float before = aim.pitch;
  aim.Update(true, false, 0.f, 1.f, 0.f, 0.f,
             std::numeric_limits<float>::infinity(), sensitivity, false, false);
  REQUIRE(Near(before, aim.pitch));
  // Preview reaches Update's angles without touching the state.
  float previewYaw = 0.f;
  float previewPitch = 0.f;
  const float stateYaw = aim.yaw;
  const float statePitch = aim.pitch;
  REQUIRE(aim.Preview(900.f, -40.f, sensitivity, true, false, previewYaw, previewPitch));
  REQUIRE(aim.yaw == stateYaw && aim.pitch == statePitch);
  aim.Update(true, false, 0.f, 1.f, 0.f, 900.f, -40.f, sensitivity, true, false);
  REQUIRE(aim.yaw == previewYaw && aim.pitch == previewPitch);
  REQUIRE(aim.Preview(0.f, 1.e6f, sensitivity, false, false, previewYaw, previewPitch));
  REQUIRE(Near(previewPitch, -PortMouse::kMaxPitch));
  // The twin stick's turn is drawn after its tick (issue #128): a frame takes
  // back the un-elapsed share of the turn the tick made, after the clamp.
  aim.Synchronize(0.f, 1.f, 0.f);
  const float startYaw = aim.yaw;
  aim.Update(true, false, 0.f, 1.f, 0.f, 100.f, 0.f, sensitivity, false, false, 100.f, 0.f);
  REQUIRE(Near(aim.lagYaw, aim.yaw - startYaw));
  REQUIRE(aim.Preview(0.f, 0.f, sensitivity, false, false, previewYaw, previewPitch, 1.f));
  REQUIRE(Near(previewYaw, startYaw)); // just after the tick: where the last frame was
  REQUIRE(aim.Preview(0.f, 0.f, sensitivity, false, false, previewYaw, previewPitch, 0.5f));
  REQUIRE(Near(previewYaw, startYaw + 0.5f * aim.lagYaw));
  // Mouse travel in the same tick is not lagged.
  const float mixYaw = aim.yaw;
  aim.Update(true, false, 0.f, 1.f, 0.f, 150.f, 0.f, sensitivity, false, false, 100.f, 0.f);
  REQUIRE(Near(aim.lagYaw, aim.yaw - mixYaw - 50.f * sensitivity * -1.f));
  // Released: the next tick turns nothing and the drawn view stays put.
  const float releasedYaw = aim.yaw;
  aim.Update(true, false, 0.f, 1.f, 0.f, 0.f, 0.f, sensitivity, false, false);
  REQUIRE(aim.lagYaw == 0.f && aim.lagPitch == 0.f);
  REQUIRE(aim.Preview(0.f, 0.f, sensitivity, false, false, previewYaw, previewPitch, 1.f));
  REQUIRE(previewYaw == releasedYaw);
  // Held into the pitch limit: the clamp eats the travel, so no frame moves.
  aim.Update(true, false, 0.f, 1.f, 0.f, 0.f, -1.e6f, sensitivity, false, false, 0.f, -1.e6f);
  REQUIRE(Near(aim.pitch, PortMouse::kMaxPitch));
  aim.Update(true, false, 0.f, 1.f, 0.f, 0.f, -100.f, sensitivity, false, false, 0.f, -100.f);
  REQUIRE(aim.lagPitch == 0.f);
  for (const float lag : {0.f, 0.3f, 1.f}) {
    REQUIRE(aim.Preview(0.f, 0.f, sensitivity, false, false, previewYaw, previewPitch, lag));
    REQUIRE(previewPitch == aim.pitch);
  }
  // A lock drops the lagged turn.
  aim.Update(true, true, 0.f, 1.f, 0.f, 0.f, -100.f, sensitivity, false, false, 0.f, -100.f);
  REQUIRE(aim.lagYaw == 0.f && aim.lagPitch == 0.f);
  // The game applies the aim twice a tick, the second time with no travel left
  // (issue #140): the turn the stick made is still drawn after the tick, and the
  // next tick, with the stick released, draws none.
  {
    PortMouse::TickAim tickLag;
    PortMouse::PointerAim noPointer;
    const auto apply = [&](float dx, float lagDx) {
      const PortMouse::AimState before = aim;
      const PortMouse::PointerAim pointerBefore = noPointer;
      REQUIRE(aim.Update(true, false, 0.f, 1.f, 0.f, dx, 0.f, sensitivity, false, false, lagDx, 0.f));
      tickLag.Applied(aim, before, noPointer, pointerBefore, false);
    };
    aim.Synchronize(0.f, 1.f, 0.f);
    const float tickStart = aim.yaw;
    tickLag.BeginTick();
    apply(100.f, 100.f);
    apply(0.f, 0.f);
    REQUIRE(Near(aim.lagYaw, aim.yaw - tickStart));
    REQUIRE(aim.Preview(0.f, 0.f, sensitivity, false, false, previewYaw, previewPitch, 0.5f));
    REQUIRE(Near(previewYaw, tickStart + 0.5f * aim.lagYaw));
    tickLag.BeginTick();
    apply(0.f, 0.f);
    apply(0.f, 0.f);
    REQUIRE(aim.lagYaw == 0.f && aim.lagPitch == 0.f);
  }
  aim.Reset();
  REQUIRE(!aim.Preview(1.f, 1.f, sensitivity, false, false, previewYaw, previewPitch));
  REQUIRE(!aim.Update(true, false, 0.f, 0.f, 0.f, 1.f, 1.f, sensitivity, false, false));

  PortMouse::ButtonGate buttons;
  REQUIRE(buttons.Poll(true, 1) == 0); // click used to enter capture/UI is not fire
  REQUIRE(buttons.Poll(true, 1) == 0);
  REQUIRE(buttons.Poll(true, 0) == 0);
  for (int tick = 0; tick < 120; ++tick) REQUIRE(buttons.Poll(true, 1) == 1);
  REQUIRE(buttons.Poll(true, 0) == 0); // charge-release edge remains visible
  REQUIRE(buttons.Poll(true, 6) == 6); // simultaneous lock-on/missile buttons
  REQUIRE(buttons.Poll(false, 1) == 0);
  REQUIRE(buttons.Poll(true, 1) == 0); // do not resume a held click on refocus
  buttons.Poll(true, 0);
  REQUIRE(buttons.Poll(true, 1) == 1);

  PortMouse::HeldButtons held;
  held.Note(true, 1, true); // a touch-synthesised click is not a mouse button
  REQUIRE(held.Held() == 0);
  held.Note(false, 1, true);
  held.Note(false, 4, true);
  held.Note(true, 1, false); // nor does a synthetic release clear a real press
  REQUIRE(held.Held() == 5);
  held.Note(false, 1, false);
  REQUIRE(held.Held() == 4);
  held.Clear();
  REQUIRE(held.Held() == 0);

  // Pointer aim: the pointer stops at the edges, holds still inside the dead box and
  // turns toward the edge past it, full speed at the edge.
  PortMouse::PointerAim pointer;
  pointer.Move(0.5f, -3.f);
  REQUIRE(Near(pointer.x, 0.5f) && Near(pointer.y, -1.f));
  pointer.Move(std::numeric_limits< float >::quiet_NaN(), 0.f);
  REQUIRE(Near(pointer.x, 0.5f));
  // The stick's pointer travel is drawn after its tick, after the edge clamp: a
  // stick holding the pointer at the edge draws it still (issue #128).
  float drawnX = 0.f, drawnY = 0.f;
  pointer.MoveLagged(0.3f, 0.f, 0.2f, 0.f);
  REQUIRE(Near(pointer.x, 0.8f) && Near(pointer.lagX, 0.2f));
  pointer.Presented(1.f, 0.f, 0.f, drawnX, drawnY);
  REQUIRE(Near(drawnX, 0.6f) && Near(drawnY, -1.f));
  pointer.Presented(0.5f, 0.05f, 0.f, drawnX, drawnY);
  REQUIRE(Near(drawnX, 0.75f));
  pointer.MoveLagged(0.4f, 0.f, 0.4f, 0.f);
  pointer.MoveLagged(0.4f, 0.f, 0.4f, 0.f);
  REQUIRE(pointer.x == 1.f && pointer.lagX == 0.f);
  for (const float lag : {0.f, 0.4f, 1.f}) {
    pointer.Presented(lag, 0.f, 0.f, drawnX, drawnY);
    REQUIRE(drawnX == 1.f);
  }
  pointer.Center();
  REQUIRE(pointer.lagX == 0.f && pointer.lagY == 0.f);
  // Pointer aim keeps the stick's pointer travel over the tick's second aim
  // update too (issue #140).
  {
    PortMouse::TickAim tickLag;
    PortMouse::AimState view;
    view.Synchronize(0.f, 1.f, 0.f);
    const auto apply = [&](float dx) {
      const PortMouse::AimState before = view;
      const PortMouse::PointerAim pointerBefore = pointer;
      pointer.MoveLagged(dx, 0.f, dx, 0.f);
      REQUIRE(view.Update(true, false, 0.f, 1.f, 0.f, 0.f, 0.f, 0.001f, false, false));
      tickLag.Applied(view, before, pointer, pointerBefore, true);
    };
    tickLag.BeginTick();
    REQUIRE(tickLag.First());
    apply(0.25f);
    REQUIRE(!tickLag.First()); // the second update makes no edge turn
    apply(0.f);
    REQUIRE(Near(pointer.x, 0.25f) && Near(pointer.lagX, 0.25f));
    tickLag.BeginTick();
    apply(0.f);
    REQUIRE(pointer.lagX == 0.f);
    pointer.Center();
  }
  pointer.Move(0.5f, -3.f);
  float turnYaw = 1.f, turnPitch = 1.f;
  PortMouse::PointerAim::TurnAt(0.6f, -0.69f, 0.7f, 3.f, 0.5f, turnYaw, turnPitch);
  REQUIRE(turnYaw == 0.f && turnPitch == 0.f);
  PortMouse::PointerAim::TurnAt(1.f, -0.85f, 0.7f, 3.f, 0.5f, turnYaw, turnPitch);
  REQUIRE(Near(turnYaw, -1.5f) && Near(turnPitch, 0.75f)); // right turns -yaw, up turns +pitch
  pointer.Center();
  REQUIRE(pointer.x == 0.f && pointer.y == 0.f);
  // The ray through a corner of a 90-degree, 2:1 view; the clamp keeps its direction.
  float rayYaw = 0.f, rayPitch = 0.f;
  PortMouse::PointerAim::RayAngles(1.f, -1.f, 90.f, 2.f, rayYaw, rayPitch);
  REQUIRE(Near(rayYaw, std::atan(2.f)) && Near(rayPitch, std::atan2(1.f, std::sqrt(5.f))));
  PortMouse::PointerAim::RayAngles(0.f, 0.f, 90.f, 2.f, rayYaw, rayPitch);
  REQUIRE(rayYaw == 0.f && rayPitch == 0.f);
  float clampYaw = 0.8f, clampPitch = 0.4f;
  PortMouse::PointerAim::ClampAngles(clampYaw, clampPitch, 0.3f);
  REQUIRE(std::fabs(std::acos(std::cos(clampYaw) * std::cos(clampPitch)) - 0.3f) < 0.001f);
  REQUIRE(Near(clampYaw / clampPitch, 2.f));
  PortMouse::AimState turned;
  turned.Synchronize(0.f, 1.f, 0.f);
  turned.Turn(0.25f, 10.f);
  REQUIRE(Near(turned.yaw, 0.25f) && Near(turned.pitch, PortMouse::kMaxPitch));
  // Stick and touch travel turn the view, and move the pointer, the same at any mouse
  // sensitivity (issue #209); mouse counts scale with it.
  for (const float source : {0.3f, 1.f, 3.f, 9.115f, 30.f}) {
    const float sensitivity = PortMouse::RadiansFromSource(source);
    const float counts = PortMouse::AimPixelsAsCounts(100.f, sensitivity);
    PortMouse::AimState stick;
    stick.Synchronize(0.f, 1.f, 0.f);
    REQUIRE(stick.Update(true, false, 0.f, 1.f, 0.f, counts, -counts, sensitivity, false, false));
    REQUIRE(Near(stick.yaw, -100.f * PortMouse::kAimPixelRadians));
    REQUIRE(Near(stick.pitch, 100.f * PortMouse::kAimPixelRadians));
    PortMouse::PointerAim stickPointer;
    stickPointer.Move(counts * PortMouse::PointerUnitsPerCount(1000.f, sensitivity), 0.f);
    REQUIRE(Near(stickPointer.x, 0.2f));
    PortMouse::AimState mouse;
    mouse.Synchronize(0.f, 1.f, 0.f);
    REQUIRE(mouse.Update(true, false, 0.f, 1.f, 0.f, 10.f, 0.f, sensitivity, false, false));
    REQUIRE(Near(mouse.yaw, -10.f * sensitivity));
  }
  // At kAimPixelRadians a count is a pixel: the pointer follows the cursor.
  REQUIRE(Near(PortMouse::PointerUnitsPerCount(1000.f, PortMouse::kAimPixelRadians) * 500.f, 1.f));
  std::puts("mouse axes, limits, handoff, held-button and pointer-aim regressions passed");
}
