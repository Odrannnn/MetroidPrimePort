#include "port_input_devices.h"

#include "port_debug.h"
#include "port_input_map.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>

#include <algorithm>
#include <cmath>

namespace PortInputDevices {
namespace {

using PortInput::Action;
using PortInput::Binding;
using PortInput::Device;
using PortInput::Input;
using PortInput::Profile;

constexpr u32 kPort = 0;

Action PadBitAction(unsigned padButton) {
  switch (padButton) {
  case PAD_BUTTON_A: return Action::PadA;
  case PAD_BUTTON_B: return Action::PadB;
  case PAD_BUTTON_X: return Action::PadX;
  case PAD_BUTTON_Y: return Action::PadY;
  case PAD_TRIGGER_Z: return Action::PadZ;
  case PAD_BUTTON_START: return Action::PadStart;
  case PAD_TRIGGER_L: return Action::PadL;
  case PAD_TRIGGER_R: return Action::PadR;
  case PAD_BUTTON_UP: return Action::PadUp;
  case PAD_BUTTON_DOWN: return Action::PadDown;
  case PAD_BUTTON_LEFT: return Action::PadLeft;
  case PAD_BUTTON_RIGHT: return Action::PadRight;
  default: return Action::None;
  }
}

// Aurora's PAD axes, as the port's actions (PAD_AXIS_LEFT_Y_POS is up).
Action PadAxisAction(PADAxis axis) {
  switch (axis) {
  case PAD_AXIS_LEFT_X_POS: return Action::MainRight;
  case PAD_AXIS_LEFT_X_NEG: return Action::MainLeft;
  case PAD_AXIS_LEFT_Y_POS: return Action::MainUp;
  case PAD_AXIS_LEFT_Y_NEG: return Action::MainDown;
  case PAD_AXIS_RIGHT_X_POS: return Action::CRight;
  case PAD_AXIS_RIGHT_X_NEG: return Action::CLeft;
  case PAD_AXIS_RIGHT_Y_POS: return Action::CUp;
  case PAD_AXIS_RIGHT_Y_NEG: return Action::CDown;
  case PAD_AXIS_TRIGGER_L: return Action::LAnalog;
  case PAD_AXIS_TRIGGER_R: return Action::RAnalog;
  default: return Action::None;
  }
}

// A native button code (SDL_GamepadButton or a trigger's half pull), as the raw
// state's pad button bit; -1 for none.
int NativeButtonCode(u32 native) {
  if (native == PAD_NATIVE_BUTTON_TRIGGER_LEFT) return PAD_RAW_BUTTON_TRIGGER_LEFT;
  if (native == PAD_NATIVE_BUTTON_TRIGGER_RIGHT) return PAD_RAW_BUTTON_TRIGGER_RIGHT;
  if (native < SDL_GAMEPAD_BUTTON_COUNT) return static_cast< int >(native);
  return -1;
}

Input Make(Device device, int code, int dir = 0) {
  Input in;
  in.device = device;
  in.code = static_cast< uint16_t >(code);
  in.dir = static_cast< int8_t >(dir);
  return in;
}

void Bind(Profile& p, Action action, const Input& in, uint16_t turboHz = 0) {
  if (action == Action::None || !in.Valid()) return;
  Binding b;
  b.action = action;
  b.inputs[0] = in;
  b.count = 1;
  b.turboHz = turboHz;
  p.bindings.push_back(b);
}

// A key slot's scancode: a key, or a mouse button read the way `mouseBase` says.
Input KeySlotInput(s32 scancode, int mouseBase) {
  if (scancode >= 0 && scancode < PortInput::kKeyCount) return Make(Device::Key, scancode);
  if (scancode <= PAD_KEY_MOUSE_LEFT && scancode >= PAD_KEY_MOUSE_X2) {
    return Make(Device::MouseButton, mouseBase + (PAD_KEY_MOUSE_LEFT - scancode));
  }
  return {};
}

Input NativeInput(s32 native) {
  const int code = native < 0 ? -1 : NativeButtonCode(static_cast< u32 >(native));
  return code < 0 ? Input{} : Make(Device::PadButton, code);
}

// trigger_axis_taken in Aurora's pad.cpp: the L/R analog's input also presses
// another PAD button, so it drives only that button.
bool TriggerAxisTaken(const PADAxisMapping* axes, u32 axisCount, const PADButtonMapping* buttons,
                      u32 buttonCount, PADAxis axis) {
  const PADAxisMapping* m = nullptr;
  for (u32 i = 0; i < axisCount; ++i) {
    if (axes[i].padAxis == axis) {
      m = &axes[i];
      break;
    }
  }
  if (m == nullptr) return false;
  u32 native = static_cast< u32 >(m->nativeButton);
  if (m->nativeAxis.nativeAxis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) {
    native = PAD_NATIVE_BUTTON_TRIGGER_LEFT;
  } else if (m->nativeAxis.nativeAxis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
    native = PAD_NATIVE_BUTTON_TRIGGER_RIGHT;
  } else if (m->nativeAxis.nativeAxis != -1) {
    return false;
  }
  if (native == PAD_NATIVE_BUTTON_INVALID) return false;
  bool other = false;
  for (u32 i = 0; i < buttonCount; ++i) {
    if (buttons[i].nativeButton != native) continue;
    if (buttons[i].padButton == PAD_TRIGGER_L || buttons[i].padButton == PAD_TRIGGER_R) return false;
    other = true;
  }
  return other;
}

void BindController(Profile& p) {
  u32 buttonCount = 0;
  u32 axisCount = 0;
  const PADButtonMapping* buttons = PADGetButtonMappings(kPort, &buttonCount);
  const PADAxisMapping* axes = PADGetAxisMappings(kPort, &axisCount);
  const PADDeadZones* dz = PADGetDeadZones(kPort);
  if (buttons == nullptr) buttonCount = 0;
  if (axes == nullptr) axisCount = 0;

  bool lSet = false;
  bool rSet = false;
  for (u32 i = 0; i < buttonCount; ++i) {
    if (buttons[i].nativeButton == PAD_NATIVE_BUTTON_INVALID) continue;
    if (buttons[i].padButton == PAD_TRIGGER_L) lSet = true;
    if (buttons[i].padButton == PAD_TRIGGER_R) rSet = true;
    Bind(p, PadBitAction(buttons[i].padButton), NativeInput(static_cast< s32 >(buttons[i].nativeButton)));
  }

  for (u32 i = 0; i < axisCount; ++i) {
    const PADAxisMapping& m = axes[i];
    const Action action = PadAxisAction(m.padAxis);
    const bool trigger = m.padAxis == PAD_AXIS_TRIGGER_L || m.padAxis == PAD_AXIS_TRIGGER_R;
    if (trigger && TriggerAxisTaken(axes, axisCount, buttons, buttonCount, m.padAxis)) continue;
    if (m.nativeAxis.nativeAxis >= 0 && m.nativeAxis.nativeAxis < PAD_RAW_AXIS_COUNT) {
      Bind(p, action, Make(Device::PadAxis, m.nativeAxis.nativeAxis, m.nativeAxis.sign < 0 ? -1 : 1));
    } else if (m.nativeAxis.nativeAxis == -1) {
      Bind(p, action, NativeInput(m.nativeButton));
    }
    // Aurora's emulated trigger click: a pulled L/R analog presses L/R when no
    // button is mapped to them.
    if (trigger && dz != nullptr && dz->emulateTriggers) {
      const bool left = m.padAxis == PAD_AXIS_TRIGGER_L;
      if (!(left ? lSet : rSet)) {
        Bind(p, left ? Action::PadL : Action::PadR,
             Make(Device::PadButton,
                  left ? PAD_RAW_BUTTON_TRIGGER_LEFT_CLICK : PAD_RAW_BUTTON_TRIGGER_RIGHT_CLICK));
      }
    }
  }
}

void BindKeyboard(Profile& p) {
  for (u32 slot = 0; slot < PAD_KEY_SLOT_COUNT; ++slot) {
    u32 count = 0;
    if (const PADKeyButtonBinding* keys = PADGetKeyButtonBindingsSlot(kPort, slot, &count)) {
      for (u32 i = 0; i < count; ++i) {
        Bind(p, PadBitAction(keys[i].padButton), KeySlotInput(keys[i].scancode, kMouseKeySlot));
      }
    }
    if (const PADKeyAxisBinding* axes = PADGetKeyAxisBindingsSlot(kPort, slot, &count)) {
      for (u32 i = 0; i < count; ++i) {
        Bind(p, PadAxisAction(axes[i].padAxis), KeySlotInput(axes[i].scancode, kMouseKeySlot));
      }
    }
  }
}

void BindPortControls(Profile& p) {
  for (int bit = 0; bit < PortDebug::kPadAltCount; ++bit) {
    Bind(p, PadBitAction(1u << bit), NativeInput(PortDebug::PadAltButton(bit)));
  }

  for (int slot = 0; slot < PAD_KEY_SLOT_COUNT; ++slot) {
    Bind(p, Action::BeamShift, KeySlotInput(PortDebug::ShiftBinding(slot), kMouseHeld));
  }
  Bind(p, Action::BeamShift, NativeInput(PortDebug::ShiftBinding(PAD_KEY_SLOT_COUNT)));
  Bind(p, Action::BeamShift, Make(Device::Touch, kTouchBeamShift));

  // Turbo: A on one poll and off the next, so the gun sees a press edge every
  // other tick. Retail has no turbo; the touch button has its own setting.
  const float tick = PortDebug::TickPeriod();
  const auto turboHz = static_cast< uint16_t >(std::lround(tick > 0.f ? 0.5f / tick : 30.f));
  if (!PortDebug::OriginalExperience()) {
    for (int slot = 0; slot < PAD_KEY_SLOT_COUNT; ++slot) {
      Bind(p, Action::PadA, KeySlotInput(PortDebug::TurboBinding(slot), kMouseHeld), turboHz);
    }
    Bind(p, Action::PadA, NativeInput(PortDebug::TurboBinding(PAD_KEY_SLOT_COUNT)), turboHz);
  }
  Bind(p, Action::PadA, Make(Device::Touch, kTouchTurbo), turboHz);
  Bind(p, Action::PadZ, Make(Device::Touch, kTouchMapTap));

  // Mouse buttons: under mouse aim each does its action (or the beam shift);
  // out of it the ones on A and B still press them (bombs, boosts, text boxes).
  for (int i = 0; i < PortInputMap::kMouseButtonCount; ++i) {
    const int action = PortDebug::MouseAction(i);
    if (action == PortInputMap::kMA_Shift) {
      Bind(p, Action::BeamShift, Make(Device::MouseButton, kMouseGameplay + i));
      continue;
    }
    const unsigned pad = PortInputMap::MouseActionInfo(action).padButton;
    const Action a = PadBitAction(pad);
    Bind(p, a, Make(Device::MouseButton, kMouseGameplay + i));
    if (pad == PAD_BUTTON_A || pad == PAD_BUTTON_B) {
      Bind(p, a, Make(Device::MouseButton, kMouseMenu + i));
    }
  }
}

float StickValue(s16 raw, u16 deadZone, bool useDeadZones) {
  if (useDeadZones && std::abs(static_cast< int >(raw)) <= deadZone) return 0.f;
  return std::clamp(static_cast< float >(raw) / 32767.f, -1.f, 1.f);
}

void FillRawState(PortInput::RawState& raw, unsigned mouseHeld, bool focused) {
  int numKeys = 0;
  if (const bool* keys = SDL_GetKeyboardState(&numKeys)) {
    for (int i = 0; i < numKeys && i < PortInput::kKeyCount; ++i) {
      if (keys[i]) raw.keys.set(static_cast< size_t >(i));
    }
  }

  PADRawState pad{};
  if (PADGetRawState(kPort, &pad)) {
    raw.padButtons = pad.buttons;
    const PADDeadZones* dz = PADGetDeadZones(kPort);
    const bool useDz = dz != nullptr && dz->useDeadzones;
    for (int a = 0; a < PAD_RAW_AXIS_COUNT; ++a) {
      if (a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
        raw.padAxes[a] = std::max(0.f, static_cast< float >(pad.axes[a]) / 32767.f);
      } else {
        const bool left = a == SDL_GAMEPAD_AXIS_LEFTX || a == SDL_GAMEPAD_AXIS_LEFTY;
        raw.padAxes[a] = StickValue(pad.axes[a], dz == nullptr ? 0 : left ? dz->stickDeadZone : dz->substickDeadZone,
                                    useDz);
      }
    }
  }

  uint32_t mouse = 0;
  // Aurora reads a key slot's mouse button with SDL_GetMouseState.
  mouse |= (SDL_GetMouseState(nullptr, nullptr) & PortInputMap::kMouseButtonMask) << kMouseKeySlot;
  mouse |= (PortDebug::MouseHeldButtons() & PortInputMap::kMouseButtonMask) << kMouseHeld;
  int actions[PortInputMap::kMouseButtonCount];
  for (int i = 0; i < PortInputMap::kMouseButtonCount; ++i) {
    actions[i] = PortDebug::MouseAction(i);
  }
  // Its gate also waits for a release, so a held charge carried into morph ball
  // doesn't drop a bomb; only the A/B buttons feed it, so a held side button
  // with nothing to do there doesn't hold it shut.
  const unsigned menuMask = PortInputMap::MouseButtonsFor(actions, PAD_BUTTON_A | PAD_BUTTON_B);
  mouse |= (PortDebug::MouseMenuButtons(mouseHeld & menuMask, focused) & PortInputMap::kMouseButtonMask)
           << kMouseMenu;
  if (PortDebug::MouseGameplayActive() && PortDebug::MouseCaptured() && PortDebug::MouseButtons()) {
    mouse |= (PortDebug::MouseWeaponButtons(mouseHeld) & PortInputMap::kMouseButtonMask) << kMouseGameplay;
  }
  raw.mouseButtons = mouse;

  if (PortDebug::TouchBeamShift()) raw.touch |= uint64_t{1} << kTouchBeamShift;
  if (PortDebug::TouchTurboFire()) raw.touch |= uint64_t{1} << kTouchTurbo;
  if (PortDebug::ConsumeMapTapZ()) raw.touch |= uint64_t{1} << kTouchMapTap;
}

// Aurora's stick conversion: x/256 truncated, y negated (SDL's y points down).
s8 StickX(float right, float left) {
  const long v = std::clamp(std::lround((right - left) * 32767.f), -32768L, 32767L);
  return static_cast< s8 >(std::clamp(static_cast< int >(v / 256), -127, 127));
}

s8 StickY(float down, float up) {
  const auto yl = static_cast< Sint16 >(std::clamp(std::lround((down - up) * 32767.f), -32768L, 32767L));
  const auto y = static_cast< Sint16 >(-(yl + 1u) / 256u);
  return static_cast< s8 >(std::clamp(static_cast< int >(y), -127, 127));
}

u8 Trigger(float analog, bool click) {
  if (click) return 180;
  return static_cast< u8 >(std::clamp(std::lround(analog * 32767.f) / 128L, 0L, 255L));
}

} // namespace

void BuildProfile(Profile& out) {
  out = Profile{};
  BindController(out);
  BindKeyboard(out);
  BindPortControls(out);
}

PADStatus Synthesize(const PortInput::Output& out) {
  PADStatus s{};
  static constexpr unsigned kButtons[] = {PAD_BUTTON_A,    PAD_BUTTON_B,     PAD_BUTTON_X,    PAD_BUTTON_Y,
                                          PAD_TRIGGER_Z,   PAD_BUTTON_START, PAD_TRIGGER_L,   PAD_TRIGGER_R,
                                          PAD_BUTTON_UP,   PAD_BUTTON_DOWN,  PAD_BUTTON_LEFT, PAD_BUTTON_RIGHT};
  for (unsigned b : kButtons) {
    if (out.Held(PadBitAction(b))) s.button |= static_cast< u16 >(b);
  }
  s.stickX = StickX(out.Value(Action::MainRight), out.Value(Action::MainLeft));
  s.stickY = StickY(out.Value(Action::MainDown), out.Value(Action::MainUp));
  s.substickX = StickX(out.Value(Action::CRight), out.Value(Action::CLeft));
  s.substickY = StickY(out.Value(Action::CDown), out.Value(Action::CUp));
  s.triggerLeft = Trigger(out.Value(Action::LAnalog), (s.button & PAD_TRIGGER_L) != 0);
  s.triggerRight = Trigger(out.Value(Action::RAnalog), (s.button & PAD_TRIGGER_R) != 0);
  const bool any = s.button != 0 || s.stickX != 0 || s.stickY != 0 || s.substickX != 0 || s.substickY != 0 ||
                   s.triggerLeft != 0 || s.triggerRight != 0;
  // Idle: Aurora still reports the port present when a pad or the keyboard is.
  s.err = any ? PAD_ERR_NONE : PAD_ERR_NO_CONTROLLER;
  return s;
}

SPoll Poll(unsigned mouseHeld, bool focused) {
  static Profile sProfile;
  static PortInput::Runtime sRuntime;
  static bool sInit = false;
  static double sClockMs = 0.0;

  Profile next;
  BuildProfile(next);
  if (!sInit || next != sProfile) {
    sProfile = std::move(next);
    sRuntime.SetProfile(&sProfile);
    sInit = true;
  }

  PortInput::RawState raw;
  FillRawState(raw, mouseHeld, focused);
  sClockMs += static_cast< double >(PortDebug::TickPeriod()) * 1000.0;
  const PortInput::Output& out = sRuntime.Poll(raw, PortInput::kCtxGameplay, sClockMs);

  SPoll r;
  r.status = Synthesize(out);
  r.beamShift = out.Held(Action::BeamShift);
  for (int i = 0; i < PortInputMap::kMouseButtonCount; ++i) {
    if ((raw.mouseButtons >> (kMouseGameplay + i) & 1u) != 0 &&
        PortDebug::MouseAction(i) != PortInputMap::kMA_Shift &&
        (PortInputMap::MouseActionInfo(PortDebug::MouseAction(i)).padButton & PAD_TRIGGER_L) != 0) {
      r.mouseL = true;
    }
  }
  return r;
}

} // namespace PortInputDevices
