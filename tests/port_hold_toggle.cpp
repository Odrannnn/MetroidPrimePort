#include "port_hold_toggle.h"

#include <cstdio>
#include <cstdlib>

namespace {
int sLine = 0;
void Check(bool condition) {
  if (!condition) {
    std::fprintf(stderr, "hold/toggle regression failed at line %d\n", sLine);
    std::abort();
  }
}
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    sLine = __LINE__;                                                                              \
    Check(cond);                                                                                   \
  } while (0)

using PortHoldToggle::Output;
bool Is(Output out, bool held, bool pressed) { return out.held == held && out.pressed == pressed; }

constexpr float kDt = 1.f / 60.f;
} // namespace

int main() {
  // Toggle: a press that locks on latches, release keeps it, the next press lets
  // go and is hidden until the button is released.
  {
    PortHoldToggle::Toggle t;
    CHECK(Is(t.Update(true, false, false, false), false, false));
    CHECK(Is(t.Update(true, true, true, false), true, true));
    CHECK(Is(t.Update(true, true, false, true), true, false));
    CHECK(t.Latched());
    CHECK(Is(t.Update(true, false, false, true), true, false));
    CHECK(Is(t.Update(true, false, false, true), true, false));
    CHECK(Is(t.Update(true, true, true, true), false, false));
    CHECK(Is(t.Update(true, true, false, true), false, false));
    CHECK(Is(t.Update(true, false, false, false), false, false));
    CHECK(!t.Latched());
    // And it can latch again.
    CHECK(Is(t.Update(true, true, true, false), true, true));
    CHECK(Is(t.Update(true, true, false, true), true, false));
    CHECK(t.Latched());
  }
  // Toggle: a press with nothing to lock on is a plain hold (strafe), and a lock
  // that comes later in the same hold latches.
  {
    PortHoldToggle::Toggle t;
    CHECK(Is(t.Update(true, true, true, false), true, true));
    CHECK(Is(t.Update(true, true, false, false), true, false));
    CHECK(!t.Latched());
    CHECK(Is(t.Update(true, false, false, false), false, false));
    CHECK(Is(t.Update(true, false, false, true), false, false));
    CHECK(!t.Latched());
    t.Update(true, true, true, false);
    t.Update(true, true, false, false);
    CHECK(Is(t.Update(true, true, false, true), true, false));
    CHECK(Is(t.Update(true, false, false, true), true, false));
    CHECK(t.Latched());
  }
  // Toggle: losing the lock releases the latch.
  {
    PortHoldToggle::Toggle t;
    t.Update(true, true, true, false);
    t.Update(true, true, false, true);
    CHECK(Is(t.Update(true, false, false, true), true, false));
    CHECK(Is(t.Update(true, false, false, false), false, false));
    CHECK(!t.Latched());
  }
  // Toggle: lock lost while the finger is still down does not re-latch on that
  // same hold.
  {
    PortHoldToggle::Toggle t;
    t.Update(true, true, true, false);
    t.Update(true, true, false, true);
    CHECK(Is(t.Update(true, true, false, false), false, false));
    CHECK(Is(t.Update(true, true, false, true), false, false));
    CHECK(Is(t.Update(true, false, false, false), false, false));
    CHECK(Is(t.Update(true, true, true, false), true, true));
  }
  // Toggle: inactive passes the button through and clears the latch.
  {
    PortHoldToggle::Toggle t;
    t.Update(true, true, true, false);
    t.Update(true, true, false, true);
    CHECK(Is(t.Update(false, true, false, false), true, false));
    CHECK(!t.Latched());
    CHECK(Is(t.Update(true, false, false, false), false, false));
  }

  // Sticky: a tap is a tap.
  {
    PortHoldToggle::Sticky s;
    CHECK(Is(s.Update(true, true, true, kDt), true, true));
    CHECK(Is(s.Update(true, true, false, kDt), true, false));
    CHECK(Is(s.Update(true, false, false, kDt), false, false));
    CHECK(!s.Latched());
  }
  // Sticky: a long hold stays held after release; the next press releases it
  // (firing the charge) and is hidden until the button comes up.
  {
    PortHoldToggle::Sticky s;
    s.Update(true, true, true, kDt);
    for (int i = 0; i < 30; ++i) {
      CHECK(Is(s.Update(true, true, false, kDt), true, false));
    }
    CHECK(Is(s.Update(true, false, false, kDt), true, false));
    CHECK(s.Latched());
    CHECK(Is(s.Update(true, false, false, kDt), true, false));
    CHECK(Is(s.Update(true, true, true, kDt), false, false));
    CHECK(Is(s.Update(true, true, false, kDt), false, false));
    CHECK(Is(s.Update(true, false, false, kDt), false, false));
    CHECK(!s.Latched());
    CHECK(Is(s.Update(true, true, true, kDt), true, true));
  }
  // Sticky: just under the threshold does not stick.
  {
    PortHoldToggle::Sticky s;
    s.Update(true, true, true, kDt);
    for (int i = 0; i < 19; ++i) {
      s.Update(true, true, false, kDt);
    }
    CHECK(Is(s.Update(true, false, false, kDt), false, false));
  }
  // Sticky: inactive (morphed, cutscene) drops the latch.
  {
    PortHoldToggle::Sticky s;
    s.Update(true, true, true, kDt);
    for (int i = 0; i < 30; ++i) {
      s.Update(true, true, false, kDt);
    }
    s.Update(true, false, false, kDt);
    CHECK(Is(s.Update(false, false, false, kDt), false, false));
    CHECK(Is(s.Update(true, false, false, kDt), false, false));
  }
  std::puts("hold/toggle ok");
  return 0;
}
