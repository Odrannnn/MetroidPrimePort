#pragma once

// Input rules the port adds on top of the pad: what each mouse button does
// under mouse aim, the beam shift (D-pad picks beams while it is held), and
// Spring Ball on the jump button. Plain data and arithmetic, so it is tested
// without SDL or the game (tests/port_input_map.cpp).

#include <cstring>

namespace PortInputMap {

// GameCube pad button bits (PAD_BUTTON_* in dolphin/pad.h).
constexpr unsigned kPadLeft = 0x0001;
constexpr unsigned kPadRight = 0x0002;
constexpr unsigned kPadDown = 0x0004;
constexpr unsigned kPadUp = 0x0008;
constexpr unsigned kPadZ = 0x0010;
constexpr unsigned kPadR = 0x0020;
constexpr unsigned kPadL = 0x0040;
constexpr unsigned kPadA = 0x0100;
constexpr unsigned kPadB = 0x0200;
constexpr unsigned kPadX = 0x0400;
constexpr unsigned kPadY = 0x0800;
constexpr unsigned kPadStart = 0x1000;
constexpr unsigned kPadDirections = kPadLeft | kPadRight | kPadDown | kPadUp;

// What a mouse button does. The order is the settings combo's; the names are
// what port_settings.ini stores.
enum EMouseAction {
  kMA_None,
  kMA_A,
  kMA_B,
  kMA_X,
  kMA_Y,
  kMA_L,
  kMA_R,
  kMA_Z,
  kMA_Start,
  kMA_Up,
  kMA_Down,
  kMA_Left,
  kMA_Right,
  kMA_Shift,
  kMA_Count
};

struct SMouseActionInfo {
  const char* name;
  unsigned padButton;
};
inline const SMouseActionInfo& MouseActionInfo(int action) {
  static const SMouseActionInfo kInfo[kMA_Count] = {
      {"none", 0},        {"a", kPadA},         {"b", kPadB},       {"x", kPadX},
      {"y", kPadY},       {"l", kPadL},         {"r", kPadR},       {"z", kPadZ},
      {"start", kPadStart}, {"up", kPadUp},     {"down", kPadDown}, {"left", kPadLeft},
      {"right", kPadRight}, {"shift", 0},
  };
  return kInfo[action >= 0 && action < kMA_Count ? action : kMA_None];
}
// -1 for a name no action has.
inline int MouseActionFromName(const char* name) {
  for (int i = 0; i < kMA_Count; ++i) {
    if (std::strcmp(name, MouseActionInfo(i).name) == 0) {
      return i;
    }
  }
  return -1;
}

// SDL's button order: left, middle, right, X1, X2 (SDL_BUTTON_MASK(n) is bit n-1).
constexpr int kMouseButtonCount = 5;
constexpr unsigned kMouseButtonMask = (1u << kMouseButtonCount) - 1;
inline const char* MouseButtonKey(int button) {
  static const char* const kKeys[kMouseButtonCount] = {"mouse_left", "mouse_middle", "mouse_right",
                                                       "mouse_x1", "mouse_x2"};
  return kKeys[button];
}
// Fire, missile, lock-on: what the buttons did before they could be changed.
inline int DefaultMouseAction(int button) {
  return button == 0 ? kMA_A : button == 1 ? kMA_Y : button == 2 ? kMA_L : kMA_None;
}

struct SMouseResult {
  unsigned buttons = 0; // pad button bits
  bool shift = false;
};
// The pad buttons the held mouse buttons add. `allowed` limits them (0 = all).
inline SMouseResult MouseActions(const int* actions, unsigned held, unsigned allowed = 0) {
  SMouseResult result;
  for (int i = 0; i < kMouseButtonCount; ++i) {
    if ((held & (1u << i)) == 0) {
      continue;
    }
    if (actions[i] == kMA_Shift) {
      result.shift = allowed == 0;
    } else {
      result.buttons |= MouseActionInfo(actions[i]).padButton;
    }
  }
  if (allowed != 0) {
    result.buttons &= allowed;
  }
  return result;
}
// The mouse buttons whose action presses one of `pad`'s buttons.
inline unsigned MouseButtonsFor(const int* actions, unsigned pad) {
  unsigned mask = 0;
  for (int i = 0; i < kMouseButtonCount; ++i) {
    if (actions[i] != kMA_Shift && (MouseActionInfo(actions[i]).padButton & pad) != 0) {
      mask |= 1u << i;
    }
  }
  return mask;
}

} // namespace PortInputMap
