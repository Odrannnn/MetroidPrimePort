#include "Kyoto/Input/CDolphinController.hpp"
#include "port_console.h"
#include "port_debug.h"
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#ifdef MP_ENABLE_SMOKE_DRIVER
#include "port_smoke.h"
#endif

#include <Kyoto/Alloc/CMemory.hpp>

#include <dolphin/gba.h>
#include <dolphin/os/OSSerial.h>

#ifdef TARGET_PC
#include "port_controls.h"
#include "port_input_devices.h"
#include "port_input_map.h"
#endif

#include <string.h>

namespace {
static_assert(PortInputMap::kPadA == PAD_BUTTON_A && PortInputMap::kPadL == PAD_TRIGGER_L &&
                  PortInputMap::kPadR == PAD_TRIGGER_R && PortInputMap::kPadUp == PAD_BUTTON_UP &&
                  PortInputMap::kPadStart == PAD_BUTTON_START && PortInputMap::kPadZ == PAD_TRIGGER_Z,
              "PortInputMap's pad bits are dolphin/pad.h's");

// The D-pad picks beams while the shift is held (PortInputMap::ShiftDPadToCStick).
void ApplyBeamShift(PADStatus& status) {
  unsigned buttons = status.button;
  int x = status.substickX;
  int y = status.substickY;
  PortInputMap::ShiftDPadToCStick(buttons, x, y);
  status.button = static_cast< u16 >(buttons);
  status.substickX = static_cast< s8 >(x);
  status.substickY = static_cast< s8 >(y);
}

bool KeyHeld(const bool* keys, int numKeys, s32 scancode) {
  return keys != nullptr && scancode > PAD_KEY_INVALID && scancode < numKeys && keys[scancode];
}

// Port 0's keyboard keys held on a C-stick direction (the keyboard presets pick
// beams that way) and on L.
struct SKeyboardPadHeld {
  bool cStick = false;
  bool triggerL = false;
};

SKeyboardPadHeld KeyboardPadHeld() {
  SKeyboardPadHeld held;
  int numKeys = 0;
  const bool* keys = SDL_GetKeyboardState(&numKeys);
  for (u32 slot = 0; slot < PAD_KEY_SLOT_COUNT; ++slot) {
    u32 count = 0;
    if (const PADKeyAxisBinding* axes = PADGetKeyAxisBindingsSlot(PAD_CHAN0, slot, &count)) {
      for (u32 i = 0; i < count; ++i) {
        if (!KeyHeld(keys, numKeys, axes[i].scancode)) continue;
        if (axes[i].padAxis >= PAD_AXIS_RIGHT_X_POS && axes[i].padAxis <= PAD_AXIS_RIGHT_Y_NEG) {
          held.cStick = true;
        } else if (axes[i].padAxis == PAD_AXIS_TRIGGER_L) {
          held.triggerL = true;
        }
      }
    }
    if (const PADKeyButtonBinding* buttons = PADGetKeyButtonBindingsSlot(PAD_CHAN0, slot, &count)) {
      for (u32 i = 0; i < count; ++i) {
        if (buttons[i].padButton == PAD_TRIGGER_L && KeyHeld(keys, numKeys, buttons[i].scancode)) {
          held.triggerL = true;
        }
      }
    }
  }
  return held;
}
} // namespace

CDolphinController::CDolphinController()
: x1c4_validControllers(PAD_CHAN0_BIT | PAD_CHAN1_BIT | PAD_CHAN2_BIT | PAD_CHAN3_BIT)
, x1c8_invalidControllers(0)
, x1cc_(0) {
  static bool sIsInitialized = false;
  if (!sIsInitialized) {
    PADSetSpec(PAD_SPEC_5);
    PADInit();
#ifdef TARGET_PC
    u32 bindingCount = 0;
    if (PADGetKeyButtonBindings(PAD_CHAN0, &bindingCount) == nullptr) {
      PortControls::ApplyDefaultKeyBindings(PAD_CHAN0);
    }
#endif
    sIsInitialized = true;
  }
}

CDolphinController::~CDolphinController() {}

bool CDolphinController::Initialize() {
  GBAInit();
  memset(x4_status, 0, sizeof(PADStatus) * 4);
  for (uint i = 0; i < 4; ++i) {
    x34_gamepadStates[i].SetDeviceIsPresent(false);
    x194_motorStates[i] = kMS_StopHard;
    x1b4_controllerTypePollTime[i] = 0;
    x1a4_controllerTypes[i] = skTypeUnknown;
  }

  PADControlAllMotors((const u32*)x194_motorStates);
  Poll();
  return true;
}

void CDolphinController::Poll() {
  ReadDevices();
  ProcessInputData();
}

void CDolphinController::ReadDevices() {
  // Not SDL_GetMouseState: that counts touches as left clicks.
  unsigned held = PortDebug::MouseHeldButtons() & PortInputMap::kMouseButtonMask;
  bool inputFocused = SDL_GetKeyboardFocus() != nullptr;
#ifdef MP_ENABLE_SMOKE_DRIVER
  held = PortSmokeMouseButtons(held);
  inputFocused = inputFocused || PortSmokeMouseEnabled() || PortSmokeScriptedInput();
#endif
  // The console's pad commands work in a window without focus.
  inputFocused = inputFocused || PortConsoleEnabled();
  // Every binding (pad, keyboard, mouse, alt buttons, turbo, touch extras) goes
  // through the input engine; Aurora's PADRead then adds the virtual pads and
  // its suppression to port 0's result.
  const PortInputDevices::SPoll devices = PortInputDevices::Poll(held, inputFocused);
  PADSetPortOverride(0, &devices.status);
  PADStatus status[4]{};
  PADRead(status);
  PADClamp(status);
  const bool mouseL = devices.mouseL;
  const bool shiftHeld = devices.beamShift;
  for (int i = 0; i < 4; ++i) {
    // One disconnected port must not prevent the other ports updating. Clear
    // stale held buttons on disconnect and keep UI interaction out of gameplay.
    if (status[i].err != PAD_ERR_NONE || PortDebug::Visible() || !inputFocused) {
      const auto error = status[i].err;
      status[i] = {};
      status[i].err = error;
    }
  }
  memcpy(x4_status, status, sizeof(status));
  PortDebug::SetBeamShiftHeld(shiftHeld && inputFocused && !PortDebug::Visible());

  // Twin-stick: feed the right stick into the first-person aim and consume it,
  // so it does not also drive the game's own free-look. The map screen keeps
  // the C-stick: it pans the map there.
  if (PortDebug::TwinStick() && !PortDebug::MapScreenOpen() && x4_status[0].err == PAD_ERR_NONE) {
    // Under mouse aim the mouse aims, so a keyboard key on the C-stick picks a
    // beam, as without twin-stick (the mouse-and-keyboard preset's 1-4), and is
    // left to the game. Without it the keys aim (the classic preset's IJKL).
    const SKeyboardPadHeld keyboard =
        PortDebug::MouseAim() ? KeyboardPadHeld() : SKeyboardPadHeld{};
    if (keyboard.cStick) {
      PortDebug::SetTwinStickRightY(0.f);
    } else {
      const float sx = static_cast< float >(x4_status[0].substickX) / 127.f;
      const float sy = static_cast< float >(x4_status[0].substickY) / 127.f;
      PortDebug::AddStickAim(sx, sy, PortDebug::TickPeriod());
      PortDebug::SetTwinStickRightY(sy);
      x4_status[0].substickX = 0;
      x4_status[0].substickY = 0;
    }

    // Beams are selected from the C-stick, which twin-stick just consumed, so
    // under twin-stick left shift (and the touch overlay's held Beam button) is a
    // beam shift too, and so are the L trigger and LB unless a pad button is
    // bound as the shift (Remastered's layout locks on with L and jumps with LB).
    // LB bound to another button (e.g. a visor's D-pad direction) is that button
    // only: held as the shift, it would turn its own D-pad press into a beam.
    // Touch has its own shift button, so there L and LB stay lock-on and jump.
    // Under mouse aim an L from a key or mouse button is lock-on only: the
    // keyboard has its own beam keys, and the arrows stay the D-pad while locked on.
    const bool* keys = SDL_GetKeyboardState(nullptr);
    SDL_Gamepad* pad = PADGetSDLGamepadForIndex(0);
    const bool padShiftBound = PortDebug::ShiftBinding(2) >= 0 || PortDebug::TouchActive();
    const bool lFromPad =
        !keyboard.triggerL && !mouseL && (x4_status[0].button & PAD_TRIGGER_L) != 0;
    const bool lbShift = pad != nullptr && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) &&
                         !PortControls::PadButtonBoundBesidesL(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    const bool beamModifier = shiftHeld || (keys != nullptr && keys[SDL_SCANCODE_LSHIFT] != 0) ||
                              (!padShiftBound && (lFromPad || lbShift));
    if (beamModifier) {
      ApplyBeamShift(x4_status[0]);
    }
  } else {
    PortDebug::SetTwinStickRightY(0.f);
    // Without twin-stick the C-stick still picks beams; the shift gives the
    // D-pad the same job, for a keyboard or a pad whose C-stick is awkward.
    if (shiftHeld && x4_status[0].err == PAD_ERR_NONE) {
      ApplyBeamShift(x4_status[0]);
    }
  }

  // Start+Back is the debug overlay chord; do not also pause the game with it.
  // Either alone may be bound to Start (the Remastered preset pauses on Back).
  if (SDL_Gamepad* pad = PADGetSDLGamepadForIndex(0)) {
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK) &&
        SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START)) {
      x4_status[0].button &= ~PAD_BUTTON_START;
    }
  }

  for (int i = 0; i < 4; ++i) {
    uint controller = (PAD_CHAN0_BIT >> i);
    if (x4_status[i].err != PAD_ERR_NOT_READY) {
      if (x4_status[i].err == PAD_ERR_NONE) {
        x34_gamepadStates[i].SetDeviceIsPresent(true);
      } else if (x4_status[i].err == PAD_ERR_NO_CONTROLLER) {
        x1c8_invalidControllers |= controller;
        x34_gamepadStates[i].SetDeviceIsPresent(false);
      }
    }

    if (x1b4_controllerTypePollTime[i] != 0) {
      --x1b4_controllerTypePollTime[i];
    } else {
      const uint type = SIProbe(i);
      if ((type & (SI_ERROR_NO_RESPONSE | SI_ERROR_UNKNOWN | SI_ERROR_BUSY)) != 0) {
        if (x1b4_controllerTypePollTime[i] == 0) {
          x1a4_controllerTypes[i] = skTypeUnknown;
        }
      } else {
        x1b4_controllerTypePollTime[i] = 60;
        if (type == SI_GC_WAVEBIRD) {
          x1a4_controllerTypes[i] = skTypeWavebird;
        } else if (type == SI_GBA) {
          x1a4_controllerTypes[i] = skTypeGBA;
        } else if (type == SI_GC_CONTROLLER) {
          x1a4_controllerTypes[i] = skTypeStandard;
        }
      }
    }
  }

  if (x1c8_invalidControllers != 0 && PADReset(x1c8_invalidControllers)) {
    x1c8_invalidControllers = 0;
  }
}

void CDolphinController::ProcessInputData() {
  for (int i = 0; i < 4; ++i) {
    ProcessAxis(i, kJA_LeftX);
    ProcessAxis(i, kJA_LeftY);
    ProcessAxis(i, kJA_RightX);
    ProcessAxis(i, kJA_RightY);
    ProcessButtons(i);
  }
}

void CDolphinController::ProcessAxis(int controller, EJoyAxis axis) {
  const float maxAxisValue = 1.f / GetAnalogStickMaxValue(axis);
  CControllerAxis& data = x34_gamepadStates[controller].GetAxis(axis);

  float axisValue = 0.f;
  switch (axis) {
  case kJA_LeftX:
    axisValue = x4_status[controller].stickX;
    break;
  case kJA_LeftY:
    axisValue = x4_status[controller].stickY;
    break;
  case kJA_RightX:
    axisValue = x4_status[controller].substickX;
    break;
  case kJA_RightY:
    axisValue = x4_status[controller].substickY;
    break;
  default:
    break;
  }

  float absolute = axisValue * maxAxisValue;
  if (absolute < kAbsoluteMinimum) {
    absolute = kAbsoluteMinimum;
  } else if (absolute > kAbsoluteMaximum) {
    absolute = kAbsoluteMaximum;
  }

  float relativeValue = absolute - data.GetAbsoluteValue();
  if (relativeValue < kRelativeMinimum) {
    relativeValue = kRelativeMinimum;
  } else if (relativeValue > kRelativeMaximum) {
    relativeValue = kRelativeMaximum;
  }
  data.SetRelativeValue(relativeValue);
  data.SetAbsoluteValue(absolute);
}

static ushort mButtonMapping[size_t(kBU_MAX)] = {
    PAD_BUTTON_A,     PAD_BUTTON_B,    PAD_BUTTON_X,  PAD_BUTTON_Y,
    PAD_BUTTON_START, PAD_TRIGGER_Z,   PAD_BUTTON_UP, PAD_BUTTON_RIGHT,
    PAD_BUTTON_DOWN,  PAD_BUTTON_LEFT, PAD_TRIGGER_L, PAD_TRIGGER_R,
};

void CDolphinController::ProcessButtons(int controller) {
  for (int i = 0; i < int(kBU_MAX); ++i) {
    ProcessDigitalButton(controller, x34_gamepadStates[controller].GetButton(EButton(i)),
                         mButtonMapping[i]);
  }

  ProcessAnalogButton(x4_status[controller].triggerL,
                      x34_gamepadStates[controller].GetAnalogButton(kBA_Left));
  ProcessAnalogButton(x4_status[controller].triggerR,
                      x34_gamepadStates[controller].GetAnalogButton(kBA_Right));
}

void CDolphinController::ProcessDigitalButton(int controller, CControllerButton& button,
                                              ushort mapping) {
  bool btnPressed = (x4_status[controller].button & mapping);
  button.SetPressEvent(PADButtonDown(button.GetIsPressed(), btnPressed));
  button.SetReleaseEvent(PADButtonUp(button.GetIsPressed(), btnPressed));
  button.SetIsPressed(btnPressed);
}

void CDolphinController::ProcessAnalogButton(float value, CControllerAxis& axis) {
  value *= 1.f / 150.f;
  if (value > kAbsoluteMaximum) {
    value = kAbsoluteMaximum;
  }

  float relative = value - axis.GetAbsoluteValue();
  if (relative > kRelativeMaximum) {
    relative = kRelativeMaximum;
  }

  axis.SetRelativeValue(relative);
  axis.SetAbsoluteValue(value);
}

uint CDolphinController::GetDeviceCount() const { return 4; }

CControllerGamepadData& CDolphinController::GetGamepadData(int controller) {
  return x34_gamepadStates[controller];
}

uint CDolphinController::GetControllerType(int controller) const {
  return x1a4_controllerTypes[controller];
}

void CDolphinController::SetMotorState(EIOPort port, EMotorState state) {
  x194_motorStates[port] = state;
  PADControlAllMotors((const u32*)x194_motorStates);
}

float CDolphinController::GetAnalogStickMaxValue(EJoyAxis axis) const {
  switch (axis) {
  case kJA_LeftX:
  case kJA_LeftY:
    return 72.0f;

  case kJA_RightX:
  case kJA_RightY:
    return 59.0f;

  default:
    return 0.0f;
  }
}

const uint CDolphinController::skTypeUnknown = 'UNKN';
const uint CDolphinController::skTypeStandard = 'STND';
const uint CDolphinController::skTypeGBA = 'GBA_';
const uint CDolphinController::skTypeWavebird = 'WAVE';
