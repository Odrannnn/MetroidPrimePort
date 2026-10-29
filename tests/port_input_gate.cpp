#include "port_input_gate.h"

#include <cstdio>

#define REQUIRE(cond)                                                                              \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "%s:%d: REQUIRE failed: %s\n", __FILE__, __LINE__, #cond);              \
      return 1;                                                                                    \
    }                                                                                              \
  } while (0)

namespace {
struct Pad {
  unsigned short button = 0;
  signed char stickX = 0, stickY = 0, substickX = 0, substickY = 0;
  unsigned char triggerLeft = 0, triggerRight = 0, analogA = 0, analogB = 0;
};
} // namespace

int main() {
  PortInputGate::Gate<Pad> g;
  Pad p;
  // Closed menu: untouched.
  p.button = 0x100; p.stickY = -127;
  g.Apply(false, p);
  REQUIRE(p.button == 0x100 && p.stickY == -127);
  // Open: everything released, whatever the keys do.
  p.button = 0x100; p.stickY = -127; p.triggerLeft = 255;
  g.Apply(true, p);
  REQUIRE(p.button == 0 && p.stickY == 0 && p.triggerLeft == 0);
  // Close with the button, the stick and the trigger still held: masked.
  p.button = 0x100; p.stickY = -127; p.triggerLeft = 255; p.stickX = 0;
  g.Apply(false, p);
  REQUIRE(p.button == 0 && p.stickY == 0 && p.triggerLeft == 0);
  // A different button pressed meanwhile gets through; the held one stays masked.
  p = Pad{}; p.button = 0x100 | 0x1; p.stickY = -127;
  g.Apply(false, p);
  REQUIRE(p.button == 0x1 && p.stickY == 0);
  // Released, then pressed again: fires.
  p = Pad{};
  g.Apply(false, p);
  p.button = 0x100; p.stickY = -127;
  g.Apply(false, p);
  REQUIRE(p.button == 0x100 && p.stickY == -127);
  // A held stick that reverses is a new input.
  g.Apply(true, p);
  p = Pad{}; p.stickY = 127;
  g.Apply(false, p);
  REQUIRE(p.stickY == 0);
  p.stickY = -127;
  g.Apply(false, p);
  REQUIRE(p.stickY == -127);
  // Rest reads -1 on the Y stick: that is released, and the next press fires.
  g.Apply(true, p);
  p = Pad{}; p.stickY = -127;
  g.Apply(false, p);
  REQUIRE(p.stickY == 0);
  p.stickY = -1;
  g.Apply(false, p);
  p.stickY = -127;
  g.Apply(false, p);
  REQUIRE(p.stickY == -127);
  // Opening again clears the latch: the next close re-reads what is held.
  g.Apply(true, p);
  p = Pad{};
  g.Apply(false, p);
  p.button = 0x2;
  g.Apply(false, p);
  REQUIRE(p.button == 0x2);
  return 0;
}
