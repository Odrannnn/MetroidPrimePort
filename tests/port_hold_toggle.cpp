#include "port_hold_toggle.h"
#include "port_test.h"

#include <cstdio>
#include <cstdlib>

namespace {

using PortHoldToggle::Output;
bool Is(Output out, bool held, bool pressed) { return out.held == held && out.pressed == pressed; }

constexpr float kDt = 1.f / 60.f;
} // namespace

int main() {
  // Toggle: a press right after the one that ended the latch is trigger bounce
  // and stays hidden; a press after the debounce window locks again.
  {
    PortHoldToggle::Toggle t;
    REQUIRE(Is(t.Update(true, true, true, false, kDt), true, true));
    REQUIRE(Is(t.Update(true, true, false, true, kDt), true, false));
    REQUIRE(Is(t.Update(true, false, false, true, kDt), true, false));
    REQUIRE(Is(t.Update(true, true, true, true, kDt), false, false));  // exit press
    REQUIRE(Is(t.Update(true, false, false, false, kDt), false, false)); // bounce: release
    REQUIRE(Is(t.Update(true, true, true, false, kDt), false, false));   // bounce: re-press
    REQUIRE(Is(t.Update(true, true, false, false, kDt), false, false));
    REQUIRE(Is(t.Update(true, false, false, false, kDt), false, false));
    for (int i = 0; i < 20; ++i) {
      REQUIRE(Is(t.Update(true, false, false, false, kDt), false, false));
    }
    REQUIRE(Is(t.Update(true, true, true, false, kDt), true, true)); // a real press
  }
  // Toggle: a press that locks on latches, release keeps it, the next press lets
  // go and is hidden until the button is released.
  {
    PortHoldToggle::Toggle t;
    REQUIRE(Is(t.Update(true, false, false, false), false, false));
    REQUIRE(Is(t.Update(true, true, true, false), true, true));
    REQUIRE(Is(t.Update(true, true, false, true), true, false));
    REQUIRE(t.Latched());
    REQUIRE(Is(t.Update(true, false, false, true), true, false));
    REQUIRE(Is(t.Update(true, false, false, true), true, false));
    REQUIRE(Is(t.Update(true, true, true, true), false, false));
    REQUIRE(Is(t.Update(true, true, false, true), false, false));
    REQUIRE(Is(t.Update(true, false, false, false), false, false));
    REQUIRE(!t.Latched());
    // And it can latch again.
    REQUIRE(Is(t.Update(true, true, true, false), true, true));
    REQUIRE(Is(t.Update(true, true, false, true), true, false));
    REQUIRE(t.Latched());
  }
  // Toggle: a press with nothing to lock on is a plain hold (strafe), and a lock
  // that comes later in the same hold latches.
  {
    PortHoldToggle::Toggle t;
    REQUIRE(Is(t.Update(true, true, true, false), true, true));
    REQUIRE(Is(t.Update(true, true, false, false), true, false));
    REQUIRE(!t.Latched());
    REQUIRE(Is(t.Update(true, false, false, false), false, false));
    REQUIRE(Is(t.Update(true, false, false, true), false, false));
    REQUIRE(!t.Latched());
    t.Update(true, true, true, false);
    t.Update(true, true, false, false);
    REQUIRE(Is(t.Update(true, true, false, true), true, false));
    REQUIRE(Is(t.Update(true, false, false, true), true, false));
    REQUIRE(t.Latched());
  }
  // Toggle: losing the lock releases the latch.
  {
    PortHoldToggle::Toggle t;
    t.Update(true, true, true, false);
    t.Update(true, true, false, true);
    REQUIRE(Is(t.Update(true, false, false, true), true, false));
    REQUIRE(Is(t.Update(true, false, false, false), false, false));
    REQUIRE(!t.Latched());
  }
  // Toggle: lock lost while the finger is still down does not re-latch on that
  // same hold.
  {
    PortHoldToggle::Toggle t;
    t.Update(true, true, true, false);
    t.Update(true, true, false, true);
    REQUIRE(Is(t.Update(true, true, false, false), false, false));
    REQUIRE(Is(t.Update(true, true, false, true), false, false));
    REQUIRE(Is(t.Update(true, false, false, false), false, false));
    REQUIRE(Is(t.Update(true, true, true, false), true, true));
  }
  // Toggle: inactive passes the button through and clears the latch.
  {
    PortHoldToggle::Toggle t;
    t.Update(true, true, true, false);
    t.Update(true, true, false, true);
    REQUIRE(Is(t.Update(false, true, false, false), true, false));
    REQUIRE(!t.Latched());
    REQUIRE(Is(t.Update(true, false, false, false), false, false));
  }

  // Sticky: a tap is a tap.
  {
    PortHoldToggle::Sticky s;
    REQUIRE(Is(s.Update(true, true, true, kDt), true, true));
    REQUIRE(Is(s.Update(true, true, false, kDt), true, false));
    REQUIRE(Is(s.Update(true, false, false, kDt), false, false));
    REQUIRE(!s.Latched());
  }
  // Sticky: a long hold stays held after release; the next press releases it
  // (firing the charge) and is hidden until the button comes up.
  {
    PortHoldToggle::Sticky s;
    s.Update(true, true, true, kDt);
    for (int i = 0; i < 30; ++i) {
      REQUIRE(Is(s.Update(true, true, false, kDt), true, false));
    }
    REQUIRE(Is(s.Update(true, false, false, kDt), true, false));
    REQUIRE(s.Latched());
    REQUIRE(Is(s.Update(true, false, false, kDt), true, false));
    REQUIRE(Is(s.Update(true, true, true, kDt), false, false));
    REQUIRE(Is(s.Update(true, true, false, kDt), false, false));
    REQUIRE(Is(s.Update(true, false, false, kDt), false, false));
    REQUIRE(!s.Latched());
    REQUIRE(Is(s.Update(true, true, true, kDt), true, true));
  }
  // Sticky: just under the threshold does not stick.
  {
    PortHoldToggle::Sticky s;
    s.Update(true, true, true, kDt);
    for (int i = 0; i < 19; ++i) {
      s.Update(true, true, false, kDt);
    }
    REQUIRE(Is(s.Update(true, false, false, kDt), false, false));
  }
  // Sticky: inactive (morphed, cutscene) drops the latch.
  {
    PortHoldToggle::Sticky s;
    s.Update(true, true, true, kDt);
    for (int i = 0; i < 30; ++i) {
      s.Update(true, true, false, kDt);
    }
    s.Update(true, false, false, kDt);
    REQUIRE(Is(s.Update(false, false, false, kDt), false, false));
    REQUIRE(Is(s.Update(true, false, false, kDt), false, false));
  }
  // Grace: a drop shorter than kGrace is bridged, a longer one ends it.
  {
    PortHoldToggle::Grace g;
    REQUIRE(!g.Update(false, kDt));
    REQUIRE(g.Update(true, kDt));
    for (int i = 0; i < 5; ++i) {
      REQUIRE(g.Update(false, kDt)); // 5 ticks < 0.15 s
    }
    REQUIRE(g.Update(true, kDt));
    int ticks = 0;
    while (g.Update(false, kDt) && ticks < 100) {
      ++ticks;
    }
    REQUIRE(ticks >= 8 && ticks <= 9);
  }
  // GrappleKey (pressed, jump, atPoint, grappling): a press with no grapple point
  // does nothing, even if one comes into reach afterwards.
  {
    PortHoldToggle::GrappleKey k;
    REQUIRE(!k.Update(true, true, false, false, false, kDt));
    REQUIRE(!k.Update(true, false, false, true, false, kDt));
  }
  // GrappleKey: a press at a point holds L through the grapple, the key released,
  // until the next press.
  {
    PortHoldToggle::GrappleKey k;
    REQUIRE(k.Update(true, true, false, true, false, kDt));
    REQUIRE(k.Update(true, false, false, true, false, kDt));
    for (int i = 0; i < 60; ++i) {
      REQUIRE(k.Update(true, false, false, false, true, kDt)); // grappling
    }
    REQUIRE(!k.Update(true, true, false, false, true, kDt));
    REQUIRE(!k.Taken());
    REQUIRE(!k.Update(true, false, false, false, true, kDt));
  }
  // GrappleKey: a jump press lets go too; a jump with nothing taken does nothing.
  {
    PortHoldToggle::GrappleKey k;
    REQUIRE(!k.Update(true, false, true, true, false, kDt));
    REQUIRE(k.Update(true, true, false, true, false, kDt));
    REQUIRE(k.Update(true, false, false, false, true, kDt));
    REQUIRE(!k.Update(true, false, true, false, true, kDt));
    REQUIRE(!k.Update(true, false, false, false, true, kDt));
  }
  // GrappleKey: a take that loses its point lets go after the grace and needs a
  // new press; inactive (morphed, Original Experience) drops it at once.
  {
    PortHoldToggle::GrappleKey k;
    REQUIRE(k.Update(true, true, false, true, false, kDt));
    for (int i = 0; i < 5; ++i) {
      REQUIRE(k.Update(true, false, false, false, false, kDt));
    }
    for (int i = 0; i < 10; ++i) {
      k.Update(true, false, false, false, false, kDt);
    }
    REQUIRE(!k.Taken());
    REQUIRE(!k.Update(true, false, false, true, false, kDt));
    REQUIRE(k.Update(true, true, false, true, false, kDt));
    REQUIRE(!k.Update(false, false, false, true, false, kDt));
  }
  // Toggle as the spider ball uses it: R pressed away from a track is a hold,
  // attaching during it latches, leaving the track lets go.
  {
    PortHoldToggle::Toggle t;
    REQUIRE(Is(t.Update(true, true, true, false, kDt), true, true));
    REQUIRE(Is(t.Update(true, true, false, false, kDt), true, false));
    REQUIRE(Is(t.Update(true, true, false, true, kDt), true, false)); // attached
    REQUIRE(t.Latched());
    REQUIRE(Is(t.Update(true, false, false, true, kDt), true, false));
    REQUIRE(Is(t.Update(true, false, false, false, kDt), false, false)); // track ended
    REQUIRE(!t.Latched());
  }
  std::puts("hold/toggle ok");
  return 0;
}
