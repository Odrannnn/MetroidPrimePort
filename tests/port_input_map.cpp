#include "port_input_map.h"

#include <cstdio>
#include <cstdlib>

namespace {
void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "input map regression failed: %s\n", what);
    std::abort();
  }
}
} // namespace

int main() {
  using namespace PortInputMap;

  // Names round-trip, and an unknown one is rejected.
  for (int i = 0; i < kMA_Count; ++i) {
    Check(MouseActionFromName(MouseActionInfo(i).name) == i, "action name round trip");
  }
  Check(MouseActionFromName("fire") == -1, "unknown action name");
  Check(MouseActionInfo(99).padButton == 0 && MouseActionInfo(-1).padButton == 0, "action range");

  // The defaults are the old fixed buttons: left fire, middle missile, right lock-on.
  int actions[kMouseButtonCount];
  for (int i = 0; i < kMouseButtonCount; ++i) {
    actions[i] = DefaultMouseAction(i);
  }
  Check(MouseActions(actions, 1u << 0).buttons == kPadA, "left is A");
  Check(MouseActions(actions, 1u << 1).buttons == kPadY, "middle is Y");
  Check(MouseActions(actions, 1u << 2).buttons == kPadL, "right is L");
  Check(MouseActions(actions, (1u << 3) | (1u << 4)).buttons == 0, "side buttons unbound");
  Check(MouseActions(actions, 0x7).buttons == (kPadA | kPadY | kPadL), "all three");

  // Rebound, with a shift; the menu path lets only its allowed buttons through.
  actions[3] = kMA_B;
  actions[4] = kMA_Shift;
  const SMouseResult both = MouseActions(actions, (1u << 3) | (1u << 4));
  Check(both.buttons == kPadB && both.shift, "side buttons rebound");
  const SMouseResult menu = MouseActions(actions, 0x1f, kPadA | kPadB);
  Check(menu.buttons == (kPadA | kPadB) && !menu.shift, "menu buttons");
  Check(MouseButtonsFor(actions, kPadA | kPadB) == ((1u << 0) | (1u << 3)), "buttons for A and B");
  Check(MouseButtonsFor(actions, 0) == 0, "buttons for nothing");

  std::puts("input map tests passed");
  return 0;
}
