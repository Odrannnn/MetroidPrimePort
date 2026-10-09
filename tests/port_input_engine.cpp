#include "port_input_engine.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

using namespace PortInput;

namespace {
int sLine = 0;
void Check(bool condition) {
  if (!condition) {
    std::fprintf(stderr, "input engine regression failed at line %d\n", sLine);
    std::abort();
  }
}
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    sLine = __LINE__;                                                                              \
    Check(cond);                                                                                   \
  } while (0)

bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// SDL_GamepadButton / SDL_GamepadAxis numbering.
constexpr uint16_t kA = 0, kB = 1, kX = 2, kY = 3, kLB = 9, kRB = 10;
constexpr uint16_t kLeftX = 0, kRightX = 2;

Input Btn(uint16_t code) { return {Device::PadButton, 0, 50, code}; }
Input Axis(uint16_t code, int8_t dir) { return {Device::PadAxis, dir, 50, code}; }

Binding Bind(Action a, std::initializer_list<Input> inputs, Trigger trigger = Trigger::Press) {
  Binding b;
  b.action = a;
  for (const Input& in : inputs) {
    b.inputs[b.count++] = in;
  }
  b.trigger = trigger;
  return b;
}

struct Rig {
  Profile profile;
  Runtime runtime;
  RawState raw;
  uint16_t contexts = kCtxGameplay;
  double now = 1000;

  void Start() { runtime.SetProfile(&profile); }
  void Set(uint16_t button, bool down) {
    if (down) {
      raw.padButtons |= 1u << button;
    } else {
      raw.padButtons &= ~(1u << button);
    }
  }
  const Output& Step(double ms = 16) {
    now += ms;
    return runtime.Poll(raw, contexts, now);
  }
};
} // namespace

int main() {
  // A plain press: held while down, pressed/released on the edges.
  {
    Rig r;
    r.profile.bindings = {Bind(Action::PadA, {Btn(kA)})};
    r.Start();
    CHECK(!r.Step().Held(Action::PadA));
    r.Set(kA, true);
    const Output& o = r.Step();
    CHECK(o.Held(Action::PadA) && o.Pressed(Action::PadA) && Near(o.Value(Action::PadA), 1.f));
    CHECK(r.Step().Held(Action::PadA) && !r.runtime.Last().Pressed(Action::PadA));
    r.Set(kA, false);
    CHECK(!r.Step().Held(Action::PadA) && r.runtime.Last().Released(Action::PadA));
  }

  // Sticks pass through below the press threshold, one half per action.
  {
    Rig r;
    r.profile.bindings = {Bind(Action::MainRight, {Axis(kLeftX, 1)}),
                          Bind(Action::MainLeft, {Axis(kLeftX, -1)})};
    r.Start();
    r.raw.padAxes[kLeftX] = 0.2f;
    const Output& o = r.Step();
    CHECK(Near(o.Value(Action::MainRight), 0.2f) && Near(o.Value(Action::MainLeft), 0.f));
    CHECK(Near(o.Axis(Action::MainLeft, Action::MainRight), 0.2f));
    r.raw.padAxes[kLeftX] = -0.7f;
    CHECK(Near(r.Step().Axis(Action::MainLeft, Action::MainRight), -0.7f));
  }

  // Ordered chord LB + A: wins over A, takes LB from its own binding, and keeps
  // LB consumed until it is released. LB + B shares the held modifier.
  {
    Rig r;
    r.profile.bindings = {Bind(Action::BeamShift, {Btn(kLB)}), Bind(Action::PadA, {Btn(kA)}),
                          Bind(Action::VisorScan, {Btn(kLB), Btn(kA)}),
                          Bind(Action::VisorThermal, {Btn(kLB), Btn(kB)}),
                          Bind(Action::PadB, {Btn(kB)})};
    r.Start();
    r.Set(kLB, true);
    CHECK(!r.Step().Held(Action::BeamShift)); // waits the chord window
    CHECK(!r.Step(16).Held(Action::BeamShift));
    CHECK(r.Step(24).Held(Action::BeamShift)); // 40 ms after the press: fires
    r.Set(kA, true);
    const Output& o = r.Step();
    CHECK(o.Held(Action::VisorScan) && !o.Held(Action::PadA) && !o.Held(Action::BeamShift));
    r.Set(kA, false);
    CHECK(!r.Step().Held(Action::VisorScan) && !r.runtime.Last().Held(Action::BeamShift));
    r.Set(kB, true);
    CHECK(r.Step().Held(Action::VisorThermal) && !r.runtime.Last().Held(Action::PadB));
    r.Set(kB, false);
    r.Set(kLB, false);
    r.Step();
    // A alone still works; LB pressed and A inside the window: LB never fires.
    r.Set(kA, true);
    CHECK(r.Step().Held(Action::PadA));
    r.Set(kA, false);
    r.Step();
    r.Set(kLB, true);
    CHECK(!r.Step().Held(Action::BeamShift));
    r.Set(kA, true);
    CHECK(r.Step().Held(Action::VisorScan));
    for (int i = 0; i < 5; ++i) {
      CHECK(!r.Step().Held(Action::BeamShift) && r.runtime.Last().Held(Action::VisorScan));
    }
    r.Set(kA, false);
    r.Set(kLB, false);
    r.Step();
    // LB tapped inside the window fires for one poll on release.
    r.Set(kLB, true);
    CHECK(!r.Step().Held(Action::BeamShift));
    r.Set(kLB, false);
    CHECK(r.Step().Pressed(Action::BeamShift));
    CHECK(r.Step().Released(Action::BeamShift));
  }

  // Any-order chord X + Y: within the window it wins over both; outside it the
  // singles fire.
  {
    Rig r;
    Binding chord = Bind(Action::PadZ, {Btn(kX), Btn(kY)});
    chord.anyOrder = true;
    r.profile.bindings = {Bind(Action::PadX, {Btn(kX)}), Bind(Action::PadY, {Btn(kY)}), chord};
    r.Start();
    r.Set(kY, true); // the "trigger" first: order doesn't matter
    r.Step();
    r.Set(kX, true);
    const Output& o = r.Step();
    CHECK(o.Held(Action::PadZ) && !o.Held(Action::PadX) && !o.Held(Action::PadY));
    for (int i = 0; i < 4; ++i) {
      CHECK(r.Step().Held(Action::PadZ) && !r.runtime.Last().Held(Action::PadY));
    }
    r.Set(kX, false);
    r.Set(kY, false);
    r.Step();
    r.Set(kX, true);
    r.Step();
    CHECK(r.Step(100).Held(Action::PadX));
    r.Set(kY, true);
    CHECK(!r.Step().Held(Action::PadZ));
    CHECK(r.Step(48).Held(Action::PadY) && !r.runtime.Last().Held(Action::PadZ));
  }

  // Triggers: tap, hold, double tap, toggle.
  {
    Rig r;
    r.profile.bindings = {Bind(Action::PadB, {Btn(kB)}, Trigger::Tap),
                          Bind(Action::PadX, {Btn(kB)}, Trigger::Hold),
                          Bind(Action::PadY, {Btn(kB)}, Trigger::DoubleTap),
                          Bind(Action::SpringBall, {Btn(kA)}, Trigger::Toggle)};
    r.Start();
    r.Set(kB, true);
    CHECK(!r.Step().Held(Action::PadB));
    r.Set(kB, false);
    CHECK(r.Step(100).Pressed(Action::PadB)); // short press: tap
    CHECK(!r.Step().Held(Action::PadB));
    r.Set(kB, true);
    CHECK(r.Step(50).Held(Action::PadY)); // second press soon after a tap
    CHECK(!r.runtime.Last().Held(Action::PadX));
    CHECK(r.Step(300).Held(Action::PadX)); // held 300 ms
    r.Set(kB, false);
    CHECK(!r.Step().Held(Action::PadB)); // long press: no tap
    r.Set(kB, true);
    CHECK(!r.Step(50).Held(Action::PadY)); // the last press wasn't a tap
    r.Set(kB, false);
    r.Step();

    r.Set(kA, true);
    CHECK(r.Step().Pressed(Action::SpringBall));
    r.Set(kA, false);
    CHECK(r.Step().Held(Action::SpringBall));
    r.Set(kA, true);
    CHECK(r.Step().Released(Action::SpringBall));
    r.Set(kA, false);
    CHECK(!r.Step().Held(Action::SpringBall));
  }

  // A chord taking a toggle's press undoes the flip.
  {
    Rig r;
    r.profile.chordWindowMs = 0; // no deferral: LB toggles at once
    r.profile.bindings = {Bind(Action::SpringBall, {Btn(kLB)}, Trigger::Toggle),
                          Bind(Action::VisorScan, {Btn(kLB), Btn(kA)})};
    r.Start();
    r.Set(kLB, true);
    CHECK(r.Step().Held(Action::SpringBall));
    r.Set(kA, true);
    CHECK(r.Step().Held(Action::VisorScan) && !r.runtime.Last().Held(Action::SpringBall));
    r.Set(kA, false);
    r.Set(kLB, false);
    CHECK(!r.Step().Held(Action::SpringBall));
  }

  // Turbo: 10 Hz = 50 ms on, 50 ms off from the first press.
  {
    Rig r;
    Binding b = Bind(Action::PadA, {Btn(kA)});
    b.turboHz = 10;
    r.profile.bindings = {b};
    r.Start();
    r.Set(kA, true);
    CHECK(r.Step().Held(Action::PadA));    // 0 ms
    CHECK(r.Step(48).Held(Action::PadA));  // 48
    CHECK(!r.Step(16).Held(Action::PadA)); // 64
    CHECK(r.Step(48).Pressed(Action::PadA)); // 112
  }

  // Context switch: the A that changed context doesn't act in the new one, a
  // held stick keeps moving, and a context binding outranks a general one.
  {
    Rig r;
    Binding map = Bind(Action::PadB, {Btn(kA)});
    map.contexts = kCtxMap;
    r.profile.bindings = {Bind(Action::PadA, {Btn(kA)}), map, Bind(Action::MainRight, {Axis(kLeftX, 1)})};
    r.Start();
    r.Set(kA, true);
    r.raw.padAxes[kLeftX] = 0.8f;
    CHECK(r.Step().Held(Action::PadA) && !r.runtime.Last().Held(Action::PadB));
    r.contexts = kCtxMap;
    const Output& o = r.Step();
    CHECK(!o.Held(Action::PadA) && !o.Held(Action::PadB) && Near(o.Value(Action::MainRight), 0.8f));
    r.Set(kA, false);
    r.Step();
    r.Set(kA, true);
    CHECK(r.Step().Held(Action::PadB) && !r.runtime.Last().Held(Action::PadA));
  }

  // Layers: holding RB switches A to its layer binding in the same poll;
  // letting RB go with A held leaves A dead until released.
  {
    Rig r;
    Binding xray = Bind(Action::VisorXray, {Btn(kA)});
    xray.contexts = kCtxLayer1;
    r.profile.bindings = {Bind(Action::PadA, {Btn(kA)}), xray, Bind(Action::Layer1, {Btn(kRB)})};
    r.Start();
    r.Set(kRB, true);
    r.Set(kA, true);
    const Output& o = r.Step();
    CHECK(o.Held(Action::VisorXray) && !o.Held(Action::PadA) && (r.runtime.ActiveContexts() & kCtxLayer1));
    r.Set(kRB, false);
    CHECK(!r.Step().Held(Action::PadA) && !r.runtime.Last().Held(Action::VisorXray));
    r.Set(kA, false);
    r.Step();
    r.Set(kA, true);
    CHECK(r.Step().Held(Action::PadA));
  }

  // Delta actions sum over bindings.
  {
    Rig r;
    Binding stick = Bind(Action::LookX, {Axis(kRightX, 0)});
    stick.scale = 10.f;
    r.profile.bindings = {Bind(Action::LookX, {{Device::MouseMotion, 0, 50, 0}}), stick};
    r.Start();
    r.raw.mouseDelta[0] = 3.f;
    r.raw.padAxes[kRightX] = -0.5f;
    CHECK(Near(r.Step().Value(Action::LookX), -2.f));
  }

  // Reset: inputs held through it stay dead until released.
  {
    Rig r;
    r.profile.bindings = {Bind(Action::PadA, {Btn(kA)}), Bind(Action::SpringBall, {Btn(kB)}, Trigger::Toggle)};
    r.Start();
    r.Set(kA, true);
    r.Set(kB, true);
    CHECK(r.Step().Held(Action::PadA) && r.runtime.Last().Held(Action::SpringBall));
    r.runtime.Reset();
    CHECK(!r.Step().Held(Action::PadA) && !r.runtime.Last().Held(Action::SpringBall));
    r.Set(kA, false);
    r.Step();
    r.Set(kA, true);
    CHECK(r.Step().Held(Action::PadA));
  }

  // Action keys round-trip.
  CHECK(ActionFromKey("visor_scan") == Action::VisorScan);
  CHECK(ActionFromKey("nope") == Action::None);
  CHECK(Info(Action::LookX).kind == ActionKind::Delta);

  std::puts("port_input_engine: all checks passed");
  return 0;
}
