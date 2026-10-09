// Controls tab: rebinds keyboard/mouse and controller inputs to the emulated
// pad. The binding backend (matching, persistence, name helpers) is Aurora's.

#include "port_controls.h"
#include "port_debug.h"
#include "port_input_map.h"

#include "MetroidPrime/CControlMapper.hpp"
#include "MetroidPrime/Tweaks/CTweakPlayerControl.hpp"
#include "MetroidPrime/Tweaks/CTweaks.hpp"

#include <dolphin/pad.h>
#include <imgui.h>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>

namespace {

constexpr u32 kControlPort = PAD_CHAN0;
// A capture that sees no input for this long gives up, so a stray Bind click
// never leaves the tab listening (or a held input leaves it disabled).
constexpr Uint64 kCaptureTimeoutMs = 5000;
// Half travel, as PADGetNativeAxisPulled uses.
constexpr Sint16 kAxisPullThreshold = 16384;

typedef ControlMapper::EFunctionList EFunctionList;

struct SControlPadButton {
  PADButton button;
  const char* label;
  EFunctionList function;
};
const SControlPadButton kControlPadButtons[] = {
    {PAD_BUTTON_A, "A", ControlMapper::kFL_AButton},
    {PAD_BUTTON_B, "B", ControlMapper::kFL_BButton},
    {PAD_BUTTON_X, "X", ControlMapper::kFL_XButton},
    {PAD_BUTTON_Y, "Y", ControlMapper::kFL_YButton},
    {PAD_TRIGGER_L, "L", ControlMapper::kFL_LeftTriggerPress},
    {PAD_TRIGGER_R, "R", ControlMapper::kFL_RightTriggerPress},
    {PAD_TRIGGER_Z, "Z", ControlMapper::kFL_ZButton},
    {PAD_BUTTON_START, "Start", ControlMapper::kFL_Start},
    {PAD_BUTTON_UP, "D-pad Up", ControlMapper::kFL_DPadUp},
    {PAD_BUTTON_DOWN, "D-pad Down", ControlMapper::kFL_DPadDown},
    {PAD_BUTTON_LEFT, "D-pad Left", ControlMapper::kFL_DPadLeft},
    {PAD_BUTTON_RIGHT, "D-pad Right", ControlMapper::kFL_DPadRight},
};

// Indexed by PADAxis.
struct SControlPadAxis {
  const char* label;
  EFunctionList function;
};
const SControlPadAxis kControlPadAxes[PAD_AXIS_COUNT] = {
    {"Stick Right", ControlMapper::kFL_LeftStickRight},
    {"Stick Left", ControlMapper::kFL_LeftStickLeft},
    {"Stick Up", ControlMapper::kFL_LeftStickUp},
    {"Stick Down", ControlMapper::kFL_LeftStickDown},
    {"C-Stick Right", ControlMapper::kFL_RightStickRight},
    {"C-Stick Left", ControlMapper::kFL_RightStickLeft},
    {"C-Stick Up", ControlMapper::kFL_RightStickUp},
    {"C-Stick Down", ControlMapper::kFL_RightStickDown},
    {"L Analog", ControlMapper::kFL_LeftTrigger},
    {"R Analog", ControlMapper::kFL_RightTrigger},
};

// Game commands under the names players know, headline actions first: a row
// is labelled with the first one the game maps its pad input to (the mapping
// is the disc's CTweakPlayerControl, so the labels follow it).
struct SCommandName {
  ControlMapper::ECommands command;
  const char* name;
};
const SCommandName kCommandNames[] = {
    {ControlMapper::kC_FireOrBomb, "Fire / Bomb"},
    {ControlMapper::kC_JumpOrBoost, "Jump / Boost"},
    {ControlMapper::kC_MissileOrPowerBomb, "Missile / Power Bomb"},
    {ControlMapper::kC_Morph, "Morph Ball"},
    {ControlMapper::kC_OrbitObject, "Lock On"},
    {ControlMapper::kC_LookHold1, "Free Look"},
    {ControlMapper::kC_ScanItem, "Scan"},
    {ControlMapper::kC_SpiderBall, "Spider Ball"},
    {ControlMapper::kC_NoVisor, "Combat Visor"},
    {ControlMapper::kC_EnviroVisor, "Scan Visor"},
    {ControlMapper::kC_ThermoVisor, "Thermal Visor"},
    {ControlMapper::kC_XrayVisor, "X-Ray Visor"},
    {ControlMapper::kC_PowerBeam, "Power Beam"},
    {ControlMapper::kC_IceBeam, "Ice Beam"},
    {ControlMapper::kC_WaveBeam, "Wave Beam"},
    {ControlMapper::kC_PlasmaBeam, "Plasma Beam"},
    {ControlMapper::kC_Forward, "Forward"},
    {ControlMapper::kC_Backward, "Back"},
    {ControlMapper::kC_TurnLeft, "Turn Left"},
    {ControlMapper::kC_TurnRight, "Turn Right"},
    {ControlMapper::kC_StrafeLeft, "Strafe Left"},
    {ControlMapper::kC_StrafeRight, "Strafe Right"},
    {ControlMapper::kC_LookUp, "Look Up"},
    {ControlMapper::kC_LookDown, "Look Down"},
    {ControlMapper::kC_LookLeft, "Look Left"},
    {ControlMapper::kC_LookRight, "Look Right"},
};

// "Fire / Bomb (A)", or just the pad label for an input no command uses (or
// before the tweaks load).
std::string ActionLabel(EFunctionList function, const char* padLabel) {
  if (gpTweakPlayerControlCurrent != nullptr) {
    for (const SCommandName& entry : kCommandNames) {
      if (gpTweakPlayerControlCurrent->GetMapping(entry.command) == function) {
        return std::string(entry.name) + " (" + padLabel + ")";
      }
    }
  }
  // The game reads these directly rather than through the command mapping
  // (CMFGame: Z opens the map, Start pauses).
  switch (function) {
  case ControlMapper::kFL_ZButton:
    return std::string("Map (") + padLabel + ")";
  case ControlMapper::kFL_Start:
    return std::string("Pause (") + padLabel + ")";
  default:
    return padLabel;
  }
}

struct SMouseCode {
  Uint32 button;
  s32 code;
};
const SMouseCode kMouseCodes[] = {
    {SDL_BUTTON_LEFT, PAD_KEY_MOUSE_LEFT}, {SDL_BUTTON_MIDDLE, PAD_KEY_MOUSE_MIDDLE},
    {SDL_BUTTON_RIGHT, PAD_KEY_MOUSE_RIGHT}, {SDL_BUTTON_X1, PAD_KEY_MOUSE_X1},
    {SDL_BUTTON_X2, PAD_KEY_MOUSE_X2},
};

// kShiftKey / kShiftPad are the beam shift's key slots and pad input, kept in
// port_settings.ini rather than Aurora's mappings (their index is unused).
enum class ECapture { kNone, kKeyButton, kKeyAxis, kPadButton, kPadAxis, kShiftKey, kShiftPad, kTurboKey, kTurboPad };

// The beam shift's pad slot in PortDebug::ShiftBinding.
constexpr int kShiftPadSlot = 2;
// The turbo fire's, in PortDebug::TurboBinding (kTurboKey / kTurboPad).
constexpr int kTurboPadSlot = 2;

// One physical input: a key or mouse button (the negative PAD_KEY_MOUSE_*
// codes), a controller button, or one direction of a controller axis. An
// analog trigger is always the axis form, whether a button or an axis row
// uses it, so the two compare equal.
struct SInput {
  enum EKind { kKey, kPadButton, kPadAxis };
  EKind kind = kKey;
  s32 code = PAD_KEY_INVALID;
  PADAxisSign sign = AXIS_SIGN_POSITIVE;
};

struct SCapture {
  ECapture target = ECapture::kNone;
  int index = 0;
  // Which of a keyboard row's PAD_KEY_SLOT_COUNT keys; always 0 for the pad.
  int slot = 0;
  Uint64 startMs = 0;
  // Inputs held when the capture started, the Bind click or pad press among
  // them. Each only counts once it has been released and pressed again.
  bool heldKeys[SDL_SCANCODE_COUNT] = {};
  Uint32 heldMouse = 0;
  bool heldPadButtons[SDL_GAMEPAD_BUTTON_COUNT] = {};
  bool heldPadAxes[SDL_GAMEPAD_AXIS_COUNT][2] = {};
  // After binding, the rows stay disabled until the bound input is released,
  // so the same press can't also activate the widget under the cursor or the
  // nav focus (Enter, or the pad's A) and start another capture.
  bool settling = false;
  SInput bound;
  // The captured input already drives another row: the tab asks whether to
  // swap, bind it to both or cancel. Its buttons wait for the input's release.
  bool conflict = false;
  bool conflictReleased = false;
  ECapture otherKind = ECapture::kNone;
  int otherIndex = 0;
  int otherSlot = 0;
  // Screen rect of the listening row's Press... button, from the last frame.
  ImVec2 pressMin{0.f, 0.f};
  ImVec2 pressMax{0.f, 0.f};
};
SCapture sCapture;
// ImGui frame the tab was last drawn on.
int sLastDrawFrame = -1;

SDL_Gamepad* PortGamepad() {
  const s32 index = PADGetIndexForPort(kControlPort);
  return index >= 0 ? PADGetSDLGamepadForIndex(static_cast< u32 >(index)) : nullptr;
}

// Only a real mouse: touches and pens also arrive as mouse buttons, and a tap on
// the overlay shouldn't bind "Mouse Left".
Uint32 MouseButtons() { return PortDebug::MouseHeldButtons(); }

bool IsTrigger(int axis) { return axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER; }

bool AxisPulled(SDL_Gamepad* pad, int axis, PADAxisSign sign) {
  const Sint16 value = SDL_GetGamepadAxis(pad, static_cast< SDL_GamepadAxis >(axis));
  if (sign == AXIS_SIGN_POSITIVE) {
    return value >= kAxisPullThreshold;
  }
  // Triggers only pull one way.
  if (IsTrigger(axis)) {
    return false;
  }
  return value <= -kAxisPullThreshold;
}

bool InputHeld(const SInput& input) {
  switch (input.kind) {
  case SInput::kKey:
    if (input.code >= 0) {
      int count = 0;
      const bool* keys = SDL_GetKeyboardState(&count);
      return input.code < count && keys[input.code];
    }
    for (const SMouseCode& mouse : kMouseCodes) {
      if (mouse.code == input.code) {
        return (MouseButtons() & SDL_BUTTON_MASK(mouse.button)) != 0;
      }
    }
    return false;
  case SInput::kPadButton: {
    SDL_Gamepad* pad = PortGamepad();
    return pad != nullptr && SDL_GetGamepadButton(pad, static_cast< SDL_GamepadButton >(input.code));
  }
  case SInput::kPadAxis: {
    SDL_Gamepad* pad = PortGamepad();
    return pad != nullptr && AxisPulled(pad, input.code, input.sign);
  }
  }
  return false;
}

void StartCapture(ECapture target, int index, int slot) {
  sCapture = SCapture{};
  sCapture.target = target;
  sCapture.index = index;
  sCapture.slot = slot;
  sCapture.startMs = SDL_GetTicks();
  int count = 0;
  const bool* keys = SDL_GetKeyboardState(&count);
  for (int i = 0; i < count && i < SDL_SCANCODE_COUNT; ++i) {
    sCapture.heldKeys[i] = keys[i];
  }
  sCapture.heldMouse = MouseButtons();
  if (SDL_Gamepad* pad = PortGamepad()) {
    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i) {
      sCapture.heldPadButtons[i] = SDL_GetGamepadButton(pad, static_cast< SDL_GamepadButton >(i));
    }
    for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; ++i) {
      sCapture.heldPadAxes[i][0] = AxisPulled(pad, i, AXIS_SIGN_NEGATIVE);
      sCapture.heldPadAxes[i][1] = AxisPulled(pad, i, AXIS_SIGN_POSITIVE);
    }
  }
}

void CancelCapture() { sCapture = SCapture{}; }

// Returns the first input of the capture's kind that is held now but wasn't
// when the capture started; clears the start-of-capture marks as inputs are
// released.
bool NewInput(SInput& out) {
  int count = 0;
  const bool* keys = SDL_GetKeyboardState(&count);
  bool found = false;
  const bool wantKeys = sCapture.target == ECapture::kKeyButton || sCapture.target == ECapture::kKeyAxis ||
                        sCapture.target == ECapture::kShiftKey || sCapture.target == ECapture::kTurboKey;
  const bool wantPadButtons = sCapture.target == ECapture::kPadButton || sCapture.target == ECapture::kShiftPad ||
                              sCapture.target == ECapture::kTurboPad;
  for (int i = 0; i < count && i < SDL_SCANCODE_COUNT; ++i) {
    sCapture.heldKeys[i] = sCapture.heldKeys[i] && keys[i];
    // Esc is reported for every capture, since it cancels.
    if ((wantKeys || i == SDL_SCANCODE_ESCAPE) && !found && keys[i] && !sCapture.heldKeys[i]) {
      out = {SInput::kKey, i, AXIS_SIGN_POSITIVE};
      found = true;
    }
  }
  const Uint32 mouse = MouseButtons();
  sCapture.heldMouse &= mouse;
  for (const SMouseCode& code : kMouseCodes) {
    const Uint32 mask = SDL_BUTTON_MASK(code.button);
    if (wantKeys && !found && (mouse & mask) != 0 && (sCapture.heldMouse & mask) == 0) {
      out = {SInput::kKey, code.code, AXIS_SIGN_POSITIVE};
      found = true;
    }
  }

  SDL_Gamepad* pad = PortGamepad();
  if (pad == nullptr) {
    return found;
  }
  for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i) {
    const bool held = SDL_GetGamepadButton(pad, static_cast< SDL_GamepadButton >(i));
    sCapture.heldPadButtons[i] = sCapture.heldPadButtons[i] && held;
    // An axis row can be driven by a button too.
    if ((wantPadButtons || sCapture.target == ECapture::kPadAxis) && !found && held &&
        !sCapture.heldPadButtons[i]) {
      out = {SInput::kPadButton, i, AXIS_SIGN_POSITIVE};
      found = true;
    }
  }
  for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; ++i) {
    for (int s = 0; s < 2; ++s) {
      const PADAxisSign sign = s == 0 ? AXIS_SIGN_NEGATIVE : AXIS_SIGN_POSITIVE;
      const bool pulled = AxisPulled(pad, i, sign);
      sCapture.heldPadAxes[i][s] = sCapture.heldPadAxes[i][s] && pulled;
      // A button row takes an analog trigger, but not a stick.
      const bool wanted = sCapture.target == ECapture::kPadAxis || (wantPadButtons && IsTrigger(i));
      if (wanted && !found && pulled && !sCapture.heldPadAxes[i][s]) {
        out = {SInput::kPadAxis, i, sign};
        found = true;
      }
    }
  }
  return found;
}

// The PAD bit's position, PortDebug::PadAltButton's index.
int PadBit(PADButton button) {
  int bit = 0;
  while (bit < PortDebug::kPadAltCount && (static_cast< u32 >(button) >> bit) != 1u) {
    ++bit;
  }
  return bit;
}

// The beam shift's pad slot and the alt buttons store a controller input as
// Aurora's native button code, a trigger as its click code.
s32 NativeCode(const SInput& input) {
  if (input.kind == SInput::kPadButton && input.code >= 0) {
    return input.code;
  }
  if (input.kind == SInput::kPadAxis && IsTrigger(input.code)) {
    return static_cast< s32 >(input.code == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? PAD_NATIVE_BUTTON_TRIGGER_LEFT
                                                                         : PAD_NATIVE_BUTTON_TRIGGER_RIGHT);
  }
  return -1;
}

SInput NativeCodeInput(s32 code) {
  if (code == static_cast< s32 >(PAD_NATIVE_BUTTON_TRIGGER_LEFT) ||
      code == static_cast< s32 >(PAD_NATIVE_BUTTON_TRIGGER_RIGHT)) {
    return {SInput::kPadAxis,
            code == static_cast< s32 >(PAD_NATIVE_BUTTON_TRIGGER_LEFT) ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER
                                                                      : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
            AXIS_SIGN_POSITIVE};
  }
  return {SInput::kPadButton, code < 0 ? -1 : code, AXIS_SIGN_POSITIVE};
}

SInput ShiftPadInput() { return NativeCodeInput(PortDebug::ShiftBinding(kShiftPadSlot)); }

SInput TurboPadInput() { return NativeCodeInput(PortDebug::TurboBinding(kTurboPadSlot)); }

// Points one row's slot at an input (code -1 unbinds a key or button row).
// Doesn't save.
void BindRow(ECapture kind, int index, int slot, const SInput& input) {
  switch (kind) {
  case ECapture::kKeyButton: {
    PADKeyButtonBinding binding{};
    binding.scancode = input.code;
    binding.padButton = kControlPadButtons[index].button;
    PADSetKeyButtonBindingSlot(kControlPort, static_cast< u32 >(slot), binding);
    // A binding on a switched-off keyboard would never fire.
    PADSetKeyboardActive(kControlPort, TRUE);
    break;
  }
  case ECapture::kKeyAxis: {
    PADKeyAxisBinding binding{};
    binding.scancode = input.code;
    binding.padAxis = static_cast< PADAxis >(index);
    binding.influence = 1;
    PADSetKeyAxisBindingSlot(kControlPort, static_cast< u32 >(slot), binding);
    PADSetKeyboardActive(kControlPort, TRUE);
    break;
  }
  case ECapture::kPadButton: {
    // Slot 1 is the port's own alt button; Aurora maps one per PAD button.
    if (slot != 0) {
      PortDebug::SetPadAltButton(PadBit(kControlPadButtons[index].button), NativeCode(input));
      break;
    }
    PADButtonMapping mapping{};
    mapping.nativeButton = PAD_NATIVE_BUTTON_INVALID;
    if (input.kind == SInput::kPadButton && input.code >= 0) {
      mapping.nativeButton = static_cast< u32 >(input.code);
    } else if (input.kind == SInput::kPadAxis && IsTrigger(input.code)) {
      mapping.nativeButton = input.code == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? PAD_NATIVE_BUTTON_TRIGGER_LEFT
                                                                         : PAD_NATIVE_BUTTON_TRIGGER_RIGHT;
    }
    mapping.padButton = kControlPadButtons[index].button;
    PADSetButtonMapping(kControlPort, mapping);
    break;
  }
  case ECapture::kPadAxis: {
    // A button drives the axis fully while held; code -1 leaves it inert.
    PADAxisMapping mapping{};
    mapping.nativeAxis = {-1, AXIS_SIGN_POSITIVE};
    mapping.nativeButton = static_cast< s32 >(PAD_NATIVE_BUTTON_INVALID);
    if (input.kind == SInput::kPadAxis) {
      mapping.nativeAxis = {input.code, input.sign};
    } else if (input.kind == SInput::kPadButton) {
      mapping.nativeButton = input.code;
    }
    mapping.padAxis = static_cast< PADAxis >(index);
    PADSetAxisMapping(kControlPort, mapping);
    break;
  }
  case ECapture::kShiftKey:
    PortDebug::SetShiftBinding(slot, input.kind == SInput::kKey ? input.code : PAD_KEY_INVALID);
    break;
  case ECapture::kShiftPad:
    PortDebug::SetShiftBinding(kShiftPadSlot, NativeCode(input));
    break;
  case ECapture::kTurboKey:
    PortDebug::SetTurboBinding(slot, input.kind == SInput::kKey ? input.code : PAD_KEY_INVALID);
    break;
  case ECapture::kTurboPad:
    PortDebug::SetTurboBinding(kTurboPadSlot, NativeCode(input));
    break;
  case ECapture::kNone:
    break;
  }
}

s32 KeyForPadButton(const PADKeyButtonBinding* list, u32 count, PADButton button);
s32 KeyForPadAxis(const PADKeyAxisBinding* list, u32 count, PADAxis axis);
u32 NativeButtonForPadButton(const PADButtonMapping* list, u32 count, PADButton button);

// What a row's slot is bound to now; code -1 when nothing.
SInput RowInput(ECapture kind, int index, int slot) {
  u32 count = 0;
  switch (kind) {
  case ECapture::kKeyButton: {
    const PADKeyButtonBinding* list = PADGetKeyButtonBindingsSlot(kControlPort, static_cast< u32 >(slot), &count);
    return {SInput::kKey, KeyForPadButton(list, count, kControlPadButtons[index].button), AXIS_SIGN_POSITIVE};
  }
  case ECapture::kKeyAxis: {
    const PADKeyAxisBinding* list = PADGetKeyAxisBindingsSlot(kControlPort, static_cast< u32 >(slot), &count);
    return {SInput::kKey, KeyForPadAxis(list, count, static_cast< PADAxis >(index)), AXIS_SIGN_POSITIVE};
  }
  case ECapture::kPadButton: {
    if (slot != 0) {
      return NativeCodeInput(PortDebug::PadAltButton(PadBit(kControlPadButtons[index].button)));
    }
    const PADButtonMapping* list = PADGetButtonMappings(kControlPort, &count);
    const u32 native = NativeButtonForPadButton(list, count, kControlPadButtons[index].button);
    if (native == PAD_NATIVE_BUTTON_TRIGGER_LEFT || native == PAD_NATIVE_BUTTON_TRIGGER_RIGHT) {
      return {SInput::kPadAxis,
              native == PAD_NATIVE_BUTTON_TRIGGER_LEFT ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
              AXIS_SIGN_POSITIVE};
    }
    return {SInput::kPadButton, static_cast< s32 >(native), AXIS_SIGN_POSITIVE};
  }
  case ECapture::kPadAxis: {
    const PADAxisMapping* list = PADGetAxisMappings(kControlPort, &count);
    for (u32 i = 0; list != nullptr && i < count; ++i) {
      if (list[i].padAxis != static_cast< PADAxis >(index)) {
        continue;
      }
      if (list[i].nativeAxis.nativeAxis < 0) {
        return {SInput::kPadButton, list[i].nativeButton, AXIS_SIGN_POSITIVE};
      }
      return {SInput::kPadAxis, list[i].nativeAxis.nativeAxis, list[i].nativeAxis.sign};
    }
    return {SInput::kPadAxis, -1, AXIS_SIGN_POSITIVE};
  }
  case ECapture::kShiftKey:
    return {SInput::kKey, PortDebug::ShiftBinding(slot), AXIS_SIGN_POSITIVE};
  case ECapture::kShiftPad:
    return ShiftPadInput();
  case ECapture::kTurboKey:
    return {SInput::kKey, PortDebug::TurboBinding(slot), AXIS_SIGN_POSITIVE};
  case ECapture::kTurboPad:
    return TurboPadInput();
  case ECapture::kNone:
    break;
  }
  return {};
}

bool SameInput(const SInput& a, const SInput& b) {
  return a.kind == b.kind && a.code == b.code && a.code != -1 &&
         (a.kind != SInput::kPadAxis || a.sign == b.sign);
}

// L and R are there twice, as the click and the analog trigger, and a binding
// may put both on one input (the keyboard defaults, a pad preset): that pair
// isn't a conflict, and binding one half moves the other along while they
// still share an input.
bool PairedRow(ECapture kind, int index, ECapture& pairKind, int& pairIndex) {
  if (kind == ECapture::kKeyButton || kind == ECapture::kPadButton) {
    const PADButton button = kControlPadButtons[index].button;
    pairKind = kind == ECapture::kKeyButton ? ECapture::kKeyAxis : ECapture::kPadAxis;
    pairIndex = button == PAD_TRIGGER_L ? PAD_AXIS_TRIGGER_L : button == PAD_TRIGGER_R ? PAD_AXIS_TRIGGER_R : -1;
    return pairIndex >= 0;
  }
  if ((kind == ECapture::kKeyAxis || kind == ECapture::kPadAxis) &&
      (index == PAD_AXIS_TRIGGER_L || index == PAD_AXIS_TRIGGER_R)) {
    const PADButton button = index == PAD_AXIS_TRIGGER_L ? PAD_TRIGGER_L : PAD_TRIGGER_R;
    for (int i = 0; i < static_cast< int >(std::size(kControlPadButtons)); ++i) {
      if (kControlPadButtons[i].button == button) {
        pairKind = kind == ECapture::kKeyAxis ? ECapture::kKeyButton : ECapture::kPadButton;
        pairIndex = i;
        return true;
      }
    }
  }
  return false;
}

// Binds a row's slot, taking its L/R partner's same slot along if the two
// shared the old key. A controller axis row has no alt slot to pair with.
void BindWithPair(ECapture kind, int index, int slot, const SInput& input) {
  ECapture pairKind = ECapture::kNone;
  int pairIndex = -1;
  const bool paired = (slot == 0 || kind == ECapture::kKeyButton || kind == ECapture::kKeyAxis) &&
                      PairedRow(kind, index, pairKind, pairIndex) &&
                      SameInput(RowInput(kind, index, slot), RowInput(pairKind, pairIndex, slot));
  BindRow(kind, index, slot, input);
  if (paired) {
    BindRow(pairKind, pairIndex, slot, input);
  }
}

// Another row slot already driven by `input`, other than the row itself (either
// slot) and its L/R partner.
bool FindConflict(ECapture kind, int index, const SInput& input, ECapture& otherKind, int& otherIndex,
                  int& otherSlot) {
  ECapture pairKind = ECapture::kNone;
  int pairIndex = -1;
  PairedRow(kind, index, pairKind, pairIndex);
  const auto check = [&](ECapture rowKind, int rowCount, int slots) {
    for (int i = 0; i < rowCount; ++i) {
      if ((rowKind == kind && i == index) || (rowKind == pairKind && i == pairIndex)) {
        continue;
      }
      for (int s = 0; s < slots; ++s) {
        if (SameInput(RowInput(rowKind, i, s), input)) {
          otherKind = rowKind;
          otherIndex = i;
          otherSlot = s;
          return true;
        }
      }
    }
    return false;
  };
  const int buttonRows = static_cast< int >(std::size(kControlPadButtons));
  switch (input.kind) {
  case SInput::kKey:
    return check(ECapture::kKeyButton, buttonRows, PAD_KEY_SLOT_COUNT) ||
           check(ECapture::kKeyAxis, PAD_AXIS_COUNT, PAD_KEY_SLOT_COUNT);
  case SInput::kPadButton:
  case SInput::kPadAxis:
    return check(ECapture::kPadButton, buttonRows, 2) || check(ECapture::kPadAxis, PAD_AXIS_COUNT, 1);
  }
  return false;
}

void PollCapture() {
  if (sCapture.target == ECapture::kNone) {
    return;
  }
  if (sCapture.conflict) {
    if (!sCapture.conflictReleased) {
      sCapture.conflictReleased = !InputHeld(sCapture.bound);
    } else if (SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_ESCAPE]) {
      CancelCapture();
    }
    return;
  }
  if (SDL_GetTicks() - sCapture.startMs > kCaptureTimeoutMs) {
    CancelCapture();
    return;
  }
  if (sCapture.settling) {
    if (!InputHeld(sCapture.bound)) {
      CancelCapture();
    }
    return;
  }

  SInput input;
  if (!NewInput(input)) {
    return;
  }
  // Esc cancels, so it can't be bound itself; it has no default binding.
  if (input.kind == SInput::kKey && input.code == SDL_SCANCODE_ESCAPE) {
    CancelCapture();
    return;
  }
  // The overlay covers most of the screen, so a mouse button binds when clicked
  // on the Press... button (or over the game); a click anywhere else on the
  // overlay, such as a tab, is aimed at the UI and cancels.
  if (input.kind == SInput::kKey && input.code < 0) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool onPress = mouse.x >= sCapture.pressMin.x && mouse.x < sCapture.pressMax.x &&
                         mouse.y >= sCapture.pressMin.y && mouse.y < sCapture.pressMax.y;
    if (!onPress && ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow |
                                           ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
      CancelCapture();
      return;
    }
  }
  sCapture.bound = input;
  // The beam shift may share an input with a pad button on purpose (L under
  // twin-stick, say), so it never asks; its row says what else the input does.
  // The turbo fire works the same way (it may sit on the fire input itself).
  const bool shift = sCapture.target == ECapture::kShiftKey || sCapture.target == ECapture::kShiftPad ||
                     sCapture.target == ECapture::kTurboKey || sCapture.target == ECapture::kTurboPad;
  if (!shift && FindConflict(sCapture.target, sCapture.index, input, sCapture.otherKind, sCapture.otherIndex,
                             sCapture.otherSlot)) {
    sCapture.conflict = true;
    return;
  }
  BindWithPair(sCapture.target, sCapture.index, sCapture.slot, input);
  PADSerializeMappings();
  sCapture.settling = true;
  sCapture.startMs = SDL_GetTicks();
}

bool Listening(ECapture target, int index, int slot) {
  return sCapture.target == target && sCapture.index == index && sCapture.slot == slot && !sCapture.settling &&
         !sCapture.conflict;
}

std::string ScancodeName(s32 scancode) {
  switch (scancode) {
  case PAD_KEY_INVALID:
    return "(unbound)";
  case PAD_KEY_MOUSE_LEFT:
    return "Mouse Left";
  case PAD_KEY_MOUSE_MIDDLE:
    return "Mouse Middle";
  case PAD_KEY_MOUSE_RIGHT:
    return "Mouse Right";
  case PAD_KEY_MOUSE_X1:
    return "Mouse X1";
  case PAD_KEY_MOUSE_X2:
    return "Mouse X2";
  default:
    break;
  }
  const char* name = SDL_GetScancodeName(static_cast< SDL_Scancode >(scancode));
  return name != nullptr && name[0] != '\0' ? name : "(unknown)";
}

s32 KeyForPadButton(const PADKeyButtonBinding* list, u32 count, PADButton button) {
  for (u32 i = 0; list != nullptr && i < count; ++i) {
    if (list[i].padButton == button) {
      return list[i].scancode;
    }
  }
  return PAD_KEY_INVALID;
}

s32 KeyForPadAxis(const PADKeyAxisBinding* list, u32 count, PADAxis axis) {
  for (u32 i = 0; list != nullptr && i < count; ++i) {
    if (list[i].padAxis == axis) {
      return list[i].scancode;
    }
  }
  return PAD_KEY_INVALID;
}

u32 NativeButtonForPadButton(const PADButtonMapping* list, u32 count, PADButton button) {
  for (u32 i = 0; list != nullptr && i < count; ++i) {
    if (list[i].padButton == button) {
      return list[i].nativeButton;
    }
  }
  return PAD_NATIVE_BUTTON_INVALID;
}

// PADGetNativeAxisName leaves out the direction, which matters here: each
// stick axis is bound one half at a time.
std::string NativeAxisName(const PADSignedNativeAxis& axis) {
  if (axis.nativeAxis < 0) {
    return "(unbound)";
  }
  const char* name = PADGetNativeAxisName(axis);
  std::string result = name != nullptr ? name : "(axis)";
  if (axis.nativeAxis != SDL_GAMEPAD_AXIS_LEFT_TRIGGER && axis.nativeAxis != SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
    result += axis.sign == AXIS_SIGN_NEGATIVE ? " -" : " +";
  }
  return result;
}

std::string InputName(const SInput& input) {
  switch (input.kind) {
  case SInput::kKey:
    return ScancodeName(input.code);
  case SInput::kPadButton: {
    if (input.code == -1) {
      return "(unbound)";
    }
    const char* name = PADGetNativeButtonName(static_cast< u32 >(input.code));
    return name != nullptr ? name : "(unknown)";
  }
  case SInput::kPadAxis:
    return NativeAxisName({input.code, input.sign});
  }
  return "(unknown)";
}

// "Fire / Bomb (A)", or "the alt key of Fire / Bomb (A)" for a row's second
// slot ("alt button" on the controller).
std::string RowLabel(ECapture kind, int index, int slot) {
  std::string label = kind == ECapture::kKeyAxis || kind == ECapture::kPadAxis
                          ? ActionLabel(kControlPadAxes[index].function, kControlPadAxes[index].label)
                          : ActionLabel(kControlPadButtons[index].function, kControlPadButtons[index].label);
  if (slot == 0) {
    return label;
  }
  return (kind == ECapture::kPadButton ? "the alt button of " : "the alt key of ") + label;
}

// The swap / bind both / cancel prompt for a captured input another row uses.
void DrawConflict() {
  const SInput old = RowInput(sCapture.target, sCapture.index, sCapture.slot);
  const std::string other = RowLabel(sCapture.otherKind, sCapture.otherIndex, sCapture.otherSlot);
  ImGui::Text("%s is already bound to %s.", InputName(sCapture.bound).c_str(), other.c_str());
  ImGui::BeginDisabled(!sCapture.conflictReleased);
  // A button row can't take a stick direction, so that swap is a move.
  const bool handOver = old.code != -1 && !(sCapture.otherKind == ECapture::kPadButton &&
                                            old.kind == SInput::kPadAxis && !IsTrigger(old.code));
  const std::string swapLabel =
      (handOver ? "Swap: " + other + " gets " + InputName(old) : "Move: " + other + " loses it") + "###swap";
  if (ImGui::Button(swapLabel.c_str())) {
    BindWithPair(sCapture.target, sCapture.index, sCapture.slot, sCapture.bound);
    BindWithPair(sCapture.otherKind, sCapture.otherIndex, sCapture.otherSlot, handOver ? old : SInput{});
    PADSerializeMappings();
    CancelCapture();
  }
  ImGui::SameLine();
  if (ImGui::Button("Bind both")) {
    BindWithPair(sCapture.target, sCapture.index, sCapture.slot, sCapture.bound);
    PADSerializeMappings();
    CancelCapture();
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) {
    CancelCapture();
  }
  ImGui::EndDisabled();
}

// A row slot's binding, as a button that starts that slot's capture.
void BindingButton(ECapture target, int index, int slot, const std::string& name, float width) {
  const bool listening = Listening(target, index, slot);
  const std::string label = (listening ? std::string("Press...") : name) + "###bind" + std::to_string(slot);
  if (ImGui::Button(label.c_str(), ImVec2(width, 0.f))) {
    StartCapture(target, index, slot);
  }
  if (listening) {
    sCapture.pressMin = ImGui::GetItemRectMin();
    sCapture.pressMax = ImGui::GetItemRectMax();
  }
}

// A keyboard row: the action, then each slot's key with a button to clear it.
void KeyRow(ECapture kind, int index, const std::string& label, const float* slotX, float keyWidth) {
  const float rowX = ImGui::GetCursorPosX();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label.c_str());
  for (int slot = 0; slot < PAD_KEY_SLOT_COUNT; ++slot) {
    ImGui::PushID(slot);
    const SInput input = RowInput(kind, index, slot);
    ImGui::SameLine(rowX + slotX[slot]);
    BindingButton(kind, index, slot, input.code == PAD_KEY_INVALID ? std::string("-") : InputName(input), keyWidth);
    ImGui::SameLine(0.f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::BeginDisabled(input.code == PAD_KEY_INVALID);
    if (ImGui::Button("x")) {
      BindWithPair(kind, index, slot, SInput{});
      PADSerializeMappings();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
      ImGui::SetTooltip("Clear");
    }
    ImGui::EndDisabled();
    ImGui::PopID();
  }
}

// Controller layouts. GameCube is Aurora's default for the pad type; the others
// start from it. The twin-stick beam modifier is L or LB unless a pad beam shift
// is bound, so only Remastered (which binds one) uses LB.
enum class EPadPreset { kGameCube, kRemastered, kModern, kSouthpaw };

s32 OtherStick(s32 axis) {
  switch (axis) {
  case SDL_GAMEPAD_AXIS_LEFTX:
    return SDL_GAMEPAD_AXIS_RIGHTX;
  case SDL_GAMEPAD_AXIS_LEFTY:
    return SDL_GAMEPAD_AXIS_RIGHTY;
  case SDL_GAMEPAD_AXIS_RIGHTX:
    return SDL_GAMEPAD_AXIS_LEFTX;
  case SDL_GAMEPAD_AXIS_RIGHTY:
    return SDL_GAMEPAD_AXIS_LEFTY;
  default:
    return axis;
  }
}

void ApplyPadPreset(EPadPreset preset) {
  PADRestoreDefaultMapping(kControlPort);
  // The GameCube layouts use the C-stick; the dual-stick ones turn twin-stick
  // back on. Only Remastered has a pad button for the beam shift.
  PortDebug::SetTwinStick(false);
  PortDebug::SetShiftBinding(2, -1);
  PortDebug::SetSwapScanXray(false);
  for (int bit = 0; bit < PortDebug::kPadAltCount; ++bit) {
    PortDebug::SetPadAltButton(bit, -1);
  }
  switch (preset) {
  case EPadPreset::kGameCube:
    break;
  case EPadPreset::kRemastered: {
    // Remastered's Dual Sticks scheme: fire on RT and the right face button,
    // lock on with LT (the default L), missile on RB, jump on the bottom face
    // button and LB, morph on the left one, map on Start, pause on Back, and
    // the top face button held with the D-pad picks beams. Free look (no
    // Remastered equivalent) goes on the right stick click.
    const PADButtonMapping buttons[] = {
        {PAD_NATIVE_BUTTON_TRIGGER_RIGHT, PAD_BUTTON_A},
        {SDL_GAMEPAD_BUTTON_SOUTH, PAD_BUTTON_B},
        {SDL_GAMEPAD_BUTTON_WEST, PAD_BUTTON_X},
        {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_BUTTON_Y},
        {SDL_GAMEPAD_BUTTON_START, PAD_TRIGGER_Z},
        {SDL_GAMEPAD_BUTTON_BACK, PAD_BUTTON_START},
        {SDL_GAMEPAD_BUTTON_RIGHT_STICK, PAD_TRIGGER_R},
    };
    for (const PADButtonMapping& mapping : buttons) {
      PADSetButtonMapping(kControlPort, mapping);
    }
    PADSetAxisMapping(kControlPort, {{-1, AXIS_SIGN_POSITIVE}, SDL_GAMEPAD_BUTTON_RIGHT_STICK, PAD_AXIS_TRIGGER_R});
    PortDebug::SetPadAltButton(PadBit(PAD_BUTTON_A), SDL_GAMEPAD_BUTTON_EAST);
    PortDebug::SetPadAltButton(PadBit(PAD_BUTTON_B), SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    PortDebug::SetShiftBinding(2, SDL_GAMEPAD_BUTTON_NORTH);
    PortDebug::SetSwapScanXray(true);
    PortDebug::SetTwinStick(true);
    break;
  }
  case EPadPreset::kModern: {
    // Fire on RT and lock on with LT (the default L), jump and morph on the
    // face buttons, free look on the right stick click, which also drives the
    // R analog so RT doesn't press R as well.
    const PADButtonMapping buttons[] = {
        {PAD_NATIVE_BUTTON_TRIGGER_RIGHT, PAD_BUTTON_A},
        {SDL_GAMEPAD_BUTTON_SOUTH, PAD_BUTTON_B},
        {SDL_GAMEPAD_BUTTON_EAST, PAD_BUTTON_X},
        {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_BUTTON_Y},
        {SDL_GAMEPAD_BUTTON_NORTH, PAD_TRIGGER_Z},
        {SDL_GAMEPAD_BUTTON_RIGHT_STICK, PAD_TRIGGER_R},
    };
    for (const PADButtonMapping& mapping : buttons) {
      PADSetButtonMapping(kControlPort, mapping);
    }
    PADSetAxisMapping(kControlPort, {{-1, AXIS_SIGN_POSITIVE}, SDL_GAMEPAD_BUTTON_RIGHT_STICK, PAD_AXIS_TRIGGER_R});
    // The right stick aims; without twin-stick it would be the C-stick.
    PortDebug::SetTwinStick(true);
    break;
  }
  case EPadPreset::kSouthpaw: {
    u32 count = 0;
    PADAxisMapping* axes = PADGetAxisMappings(kControlPort, &count);
    for (u32 i = 0; axes != nullptr && i < count; ++i) {
      axes[i].nativeAxis.nativeAxis = OtherStick(axes[i].nativeAxis.nativeAxis);
    }
    break;
  }
  }
  PADSerializeMappings();
}

// A deadzone as a percentage of full travel; Aurora keeps raw SDL axis units.
// Returns true once an edit is finished, to save then rather than every frame.
bool ZoneSlider(const char* label, u16& zone, int minPercent, int maxPercent, const char* tooltip) {
  int percent = static_cast< int >(std::lround(zone * 100.0 / SDL_JOYSTICK_AXIS_MAX));
  if (ImGui::SliderInt(label, &percent, minPercent, maxPercent, "%d%%", ImGuiSliderFlags_AlwaysClamp)) {
    zone = static_cast< u16 >(std::lround(percent * SDL_JOYSTICK_AXIS_MAX / 100.0));
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
    ImGui::SetTooltip("%s", tooltip);
  }
  return ImGui::IsItemDeactivatedAfterEdit();
}

void DrawDeadZones(PADDeadZones& zones) {
  ImGui::SeparatorText("Sticks and triggers");
  bool save = false;
  const u16 stick = zones.stickDeadZone;
  const u16 substick = zones.substickDeadZone;
  save |= ZoneSlider("Stick deadzone", zones.stickDeadZone, 0, 50,
                     "How far the control stick moves before it registers.");
  save |= ZoneSlider("C-stick deadzone", zones.substickDeadZone, 0, 50,
                     "How far the right stick moves before it registers, for\n"
                     "twin-stick aim as well as the C-stick.");
  // A file written with deadzones off would make these sliders do nothing.
  if (zones.stickDeadZone != stick || zones.substickDeadZone != substick) {
    zones.useDeadzones = true;
  }
  save |= ZoneSlider("L click point", zones.leftTriggerActivationZone, 10, 95,
                     "How far the left trigger pulls before it also clicks L\n"
                     "(lock on). Lower it for triggers that don't reach the end.");
  save |= ZoneSlider("R click point", zones.rightTriggerActivationZone, 10, 95,
                     "How far the right trigger pulls before it also clicks R.");
  // Aurora's defaults, from GameController in lib/input.hpp.
  if (ImGui::Button("Reset sticks and triggers")) {
    zones.useDeadzones = true;
    zones.stickDeadZone = 8000;
    zones.substickDeadZone = 8000;
    zones.leftTriggerActivationZone = 31150;
    zones.rightTriggerActivationZone = 31150;
    save = true;
  }
  if (save) {
    PADSerializeMappings();
  }
}

constexpr const char* kShiftLabel = "Beam shift (hold + D-pad)";

void ShiftTooltip() {
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("While held, the D-pad picks beams instead of switching visors:\n"
                      "each direction gives the beam that direction on the C-stick does.\n"
                      "Under Twin Stick Aim, left shift does it too, and so do L and LB\n"
                      "while no controller button is bound here (LB only while it has\n"
                      "no other binding).");
  }
}

constexpr const char* kTurboLabel = "Turbo fire (hold)";

void TurboTooltip() {
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("While held, fires as if A were tapped as fast as the game accepts.\n"
                      "Unbound by default.");
  }
}

// Says when the beam shift's (or turbo fire's) input also drives a pad button:
// holding it then presses both, which is fine for some (L under twin-stick)
// and not others.
void ShiftOverlapNote(ECapture kind, int slot) {
  const SInput input = RowInput(kind, 0, slot);
  ECapture otherKind = ECapture::kNone;
  int otherIndex = 0;
  int otherSlot = 0;
  if (input.code != -1 && FindConflict(kind, 0, input, otherKind, otherIndex, otherSlot)) {
    ImGui::TextDisabled("  %s is also %s.", InputName(input).c_str(),
                        RowLabel(otherKind, otherIndex, otherSlot).c_str());
  }
}

// Keyboard layouts. Classic is the port's first-run layout; Mouse & keyboard
// is a PC shooter layout for mouse aim.
enum class EKeyPreset { kClassic, kMouseKeyboard };

// The pad input the disc's tweak gives a command, or the retail one before the
// tweaks load.
EFunctionList CommandFunction(ControlMapper::ECommands command, EFunctionList fallback) {
  return gpTweakPlayerControlCurrent != nullptr ? gpTweakPlayerControlCurrent->GetMapping(command) : fallback;
}

// A layout being built: per key slot, a key for each pad button and axis.
struct SKeyLayout {
  s32 buttons[PAD_KEY_SLOT_COUNT][std::size(kControlPadButtons)];
  s32 axes[PAD_KEY_SLOT_COUNT][PAD_AXIS_COUNT];

  SKeyLayout() {
    std::fill_n(&buttons[0][0], sizeof(buttons) / sizeof(s32), PAD_KEY_INVALID);
    std::fill_n(&axes[0][0], sizeof(axes) / sizeof(s32), PAD_KEY_INVALID);
  }

  // Binds key to the pad input with that function, in its first free slot.
  void Bind(EFunctionList function, s32 key) {
    for (int slot = 0; slot < PAD_KEY_SLOT_COUNT; ++slot) {
      for (size_t i = 0; i < std::size(kControlPadButtons); ++i) {
        if (kControlPadButtons[i].function == function && buttons[slot][i] == PAD_KEY_INVALID) {
          buttons[slot][i] = key;
          return;
        }
      }
      for (int i = 0; i < PAD_AXIS_COUNT; ++i) {
        if (kControlPadAxes[i].function == function && axes[slot][i] == PAD_KEY_INVALID) {
          axes[slot][i] = key;
          return;
        }
      }
    }
  }

  void Apply(u32 port) const {
    for (int slot = 0; slot < PAD_KEY_SLOT_COUNT; ++slot) {
      for (size_t i = 0; i < std::size(kControlPadButtons); ++i) {
        PADSetKeyButtonBindingSlot(port, slot, {buttons[slot][i], kControlPadButtons[i].button});
      }
      for (int i = 0; i < PAD_AXIS_COUNT; ++i) {
        PADSetKeyAxisBindingSlot(port, slot, {axes[slot][i], static_cast< PADAxis >(i), 1});
      }
    }
    PADSetKeyboardActive(port, TRUE);
  }
};

void ApplyKeyPreset(EKeyPreset preset) {
  switch (preset) {
  case EKeyPreset::kClassic:
    PortControls::ApplyDefaultKeyBindings(kControlPort);
    break;
  case EKeyPreset::kMouseKeyboard: {
    SKeyLayout layout;
    const struct {
      EFunctionList function;
      s32 key;
    } keys[] = {
        {ControlMapper::kFL_LeftStickUp, SDL_SCANCODE_W},
        {ControlMapper::kFL_LeftStickDown, SDL_SCANCODE_S},
        {ControlMapper::kFL_LeftStickLeft, SDL_SCANCODE_A},
        {ControlMapper::kFL_LeftStickRight, SDL_SCANCODE_D},
        {ControlMapper::kFL_AButton, SDL_SCANCODE_E},     // fire, menu confirm
        {ControlMapper::kFL_BButton, SDL_SCANCODE_SPACE}, // jump
        {ControlMapper::kFL_XButton, SDL_SCANCODE_LCTRL}, // morph ball
        {ControlMapper::kFL_XButton, SDL_SCANCODE_C},
        {ControlMapper::kFL_YButton, SDL_SCANCODE_F}, // missile
        {ControlMapper::kFL_LeftTriggerPress, SDL_SCANCODE_Q}, // lock on
        {ControlMapper::kFL_LeftTrigger, SDL_SCANCODE_Q},
        {ControlMapper::kFL_RightTriggerPress, SDL_SCANCODE_LALT}, // free look
        {ControlMapper::kFL_RightTrigger, SDL_SCANCODE_LALT},
        {ControlMapper::kFL_ZButton, SDL_SCANCODE_TAB}, // map
        {ControlMapper::kFL_ZButton, SDL_SCANCODE_M},
        {ControlMapper::kFL_Start, SDL_SCANCODE_RETURN},
    };
    for (const auto& entry : keys) {
      layout.Bind(entry.function, entry.key);
    }
    // Number keys pick beams and visors directly, through whichever C-stick
    // direction or D-pad button the disc gives each; arrows keep the D-pad.
    const struct {
      ControlMapper::ECommands command;
      EFunctionList fallback;
      s32 key;
    } direct[] = {
        {ControlMapper::kC_PowerBeam, ControlMapper::kFL_RightStickUp, SDL_SCANCODE_1},
        {ControlMapper::kC_WaveBeam, ControlMapper::kFL_RightStickRight, SDL_SCANCODE_2},
        {ControlMapper::kC_IceBeam, ControlMapper::kFL_RightStickDown, SDL_SCANCODE_3},
        {ControlMapper::kC_PlasmaBeam, ControlMapper::kFL_RightStickLeft, SDL_SCANCODE_4},
        {ControlMapper::kC_NoVisor, ControlMapper::kFL_DPadUp, SDL_SCANCODE_5},
        {ControlMapper::kC_EnviroVisor, ControlMapper::kFL_DPadLeft, SDL_SCANCODE_6},
        {ControlMapper::kC_ThermoVisor, ControlMapper::kFL_DPadDown, SDL_SCANCODE_7},
        {ControlMapper::kC_XrayVisor, ControlMapper::kFL_DPadRight, SDL_SCANCODE_8},
    };
    for (const auto& entry : direct) {
      layout.Bind(CommandFunction(entry.command, entry.fallback), entry.key);
    }
    layout.Bind(ControlMapper::kFL_DPadUp, SDL_SCANCODE_UP);
    layout.Bind(ControlMapper::kFL_DPadDown, SDL_SCANCODE_DOWN);
    layout.Bind(ControlMapper::kFL_DPadLeft, SDL_SCANCODE_LEFT);
    layout.Bind(ControlMapper::kFL_DPadRight, SDL_SCANCODE_RIGHT);
    layout.Apply(kControlPort);
    // Twin-stick leaves a C-stick held from the keyboard to the game, so 1-4
    // still pick beams while a pad's right stick aims alongside the mouse.
    PortDebug::SetMouseAim(true);
    PortDebug::SetTwinStick(true);
    break;
  }
  }
  PADSerializeMappings();
  PortDebug::SetShiftBinding(0, SDL_SCANCODE_LSHIFT);
  PortDebug::SetShiftBinding(1, PAD_KEY_INVALID);
  for (int i = 0; i < PortInputMap::kMouseButtonCount; ++i) {
    PortDebug::SetMouseAction(i, PortInputMap::DefaultMouseAction(i));
  }
}

} // namespace

namespace PortControls {

void ApplyDefaultKeyBindings(unsigned port) {
  PADKeyButtonBinding buttons[PAD_BUTTON_COUNT] = {
      {SDL_SCANCODE_X, PAD_BUTTON_A},          {SDL_SCANCODE_Z, PAD_BUTTON_B},
      {SDL_SCANCODE_C, PAD_BUTTON_X},          {SDL_SCANCODE_V, PAD_BUTTON_Y},
      {SDL_SCANCODE_RETURN, PAD_BUTTON_START}, {SDL_SCANCODE_F, PAD_TRIGGER_Z},
      {SDL_SCANCODE_Q, PAD_TRIGGER_L},         {SDL_SCANCODE_E, PAD_TRIGGER_R},
      {SDL_SCANCODE_UP, PAD_BUTTON_UP},        {SDL_SCANCODE_DOWN, PAD_BUTTON_DOWN},
      {SDL_SCANCODE_LEFT, PAD_BUTTON_LEFT},    {SDL_SCANCODE_RIGHT, PAD_BUTTON_RIGHT},
  };
  PADKeyAxisBinding axes[PAD_AXIS_COUNT] = {
      {SDL_SCANCODE_D, PAD_AXIS_LEFT_X_POS, 1},  {SDL_SCANCODE_A, PAD_AXIS_LEFT_X_NEG, 1},
      {SDL_SCANCODE_W, PAD_AXIS_LEFT_Y_POS, 1},  {SDL_SCANCODE_S, PAD_AXIS_LEFT_Y_NEG, 1},
      {SDL_SCANCODE_L, PAD_AXIS_RIGHT_X_POS, 1}, {SDL_SCANCODE_J, PAD_AXIS_RIGHT_X_NEG, 1},
      {SDL_SCANCODE_I, PAD_AXIS_RIGHT_Y_POS, 1}, {SDL_SCANCODE_K, PAD_AXIS_RIGHT_Y_NEG, 1},
      {SDL_SCANCODE_Q, PAD_AXIS_TRIGGER_L, 1},   {SDL_SCANCODE_E, PAD_AXIS_TRIGGER_R, 1},
  };
  if (PADSetKeyButtonBindings(port, buttons) && PADSetKeyAxisBindings(port, axes)) {
    PADSetKeyboardActive(port, TRUE);
  }
  // The defaults have no alt keys.
  for (const PADKeyButtonBinding& binding : buttons) {
    PADSetKeyButtonBindingSlot(port, 1, {PAD_KEY_INVALID, binding.padButton});
  }
  for (const PADKeyAxisBinding& binding : axes) {
    PADSetKeyAxisBindingSlot(port, 1, {PAD_KEY_INVALID, binding.padAxis, 1});
  }
}

bool ApplyKeyPresetNamed(std::string_view name) {
  if (name == "classic") {
    ApplyKeyPreset(EKeyPreset::kClassic);
  } else if (name == "mouse") {
    ApplyKeyPreset(EKeyPreset::kMouseKeyboard);
  } else {
    return false;
  }
  return true;
}

bool ApplyPadPresetNamed(std::string_view name) {
  if (name == "gamecube") {
    ApplyPadPreset(EPadPreset::kGameCube);
  } else if (name == "remastered") {
    ApplyPadPreset(EPadPreset::kRemastered);
  } else if (name == "modern") {
    ApplyPadPreset(EPadPreset::kModern);
  } else if (name == "southpaw") {
    ApplyPadPreset(EPadPreset::kSouthpaw);
  } else {
    return false;
  }
  return true;
}

bool ShiftHeld() {
  for (int slot = 0; slot < PAD_KEY_SLOT_COUNT; ++slot) {
    const SInput key = RowInput(ECapture::kShiftKey, 0, slot);
    if (key.code != PAD_KEY_INVALID && InputHeld(key)) {
      return true;
    }
  }
  const SInput pad = ShiftPadInput();
  return pad.code != -1 && InputHeld(pad);
}

bool TurboHeld() {
  // Retail has no turbo; the bindings stay listed for when it's turned off.
  if (PortDebug::OriginalExperience()) {
    return false;
  }
  for (int slot = 0; slot < PAD_KEY_SLOT_COUNT; ++slot) {
    const SInput key = RowInput(ECapture::kTurboKey, 0, slot);
    if (key.code != PAD_KEY_INVALID && InputHeld(key)) {
      return true;
    }
  }
  const SInput pad = TurboPadInput();
  return pad.code != -1 && InputHeld(pad);
}

unsigned HeldAltPadButtons() {
  unsigned buttons = 0;
  for (const SControlPadButton& row : kControlPadButtons) {
    const SInput alt = NativeCodeInput(PortDebug::PadAltButton(PadBit(row.button)));
    if (alt.code != -1 && InputHeld(alt)) {
      buttons |= row.button;
    }
  }
  return buttons;
}

bool PadButtonBoundBesidesL(s32 native) {
  u32 count = 0;
  const PADButtonMapping* list = PADGetButtonMappings(kControlPort, &count);
  for (u32 i = 0; list != nullptr && i < count; ++i) {
    if (list[i].nativeButton == static_cast< u32 >(native) && list[i].padButton != PAD_TRIGGER_L) {
      return true;
    }
  }
  for (const SControlPadButton& row : kControlPadButtons) {
    if (row.button != PAD_TRIGGER_L && PortDebug::PadAltButton(PadBit(row.button)) == native) {
      return true;
    }
  }
  return false;
}

bool Capturing() {
  // Only the tab polls the capture, so one left running when the overlay closed
  // or the tab changed would otherwise block the overlay's pad navigation.
  if (sCapture.target != ECapture::kNone &&
      (ImGui::GetFrameCount() - sLastDrawFrame > 2 ||
       (!sCapture.conflict && SDL_GetTicks() - sCapture.startMs > kCaptureTimeoutMs))) {
    CancelCapture();
  }
  // A conflict prompt is answered with the pad too, once the captured input
  // is released.
  return sCapture.target != ECapture::kNone && !(sCapture.conflict && sCapture.conflictReleased);
}

// What both sub-tabs share: the capture prompt and conflict popup, and the
// column widths of the binding tables.
struct SBindLayout {
  float bindWidth = 0.f;
  float clearWidth = 0.f;
  float labelWidth = 0.f;
  float slotX[PAD_KEY_SLOT_COUNT] = {};
  std::string buttonLabels[std::size(kControlPadButtons)];
  std::string axisLabels[PAD_AXIS_COUNT];
};

// Polls the capture and draws its prompt. Ends with BeginDisabled for as long as an
// input is being captured: pair it with EndTab.
static void BeginTab(SBindLayout& layout) {
  sLastDrawFrame = ImGui::GetFrameCount();
  PollCapture();
  // Wide enough for the usual key names, so the columns line up at any font
  // scale; a longer name is clipped.
  layout.bindWidth =
      std::max(ImGui::CalcTextSize("Mouse Middle").x, ImGui::CalcTextSize("Press...").x) +
      ImGui::GetStyle().FramePadding.x * 2.f;

  // A modal rather than an inline prompt: when an inline one closed, the rows
  // below moved up under the cursor, so a double-click on Swap could land on
  // a keyboard preset.
  constexpr const char* kConflictPopup = "Binding conflict";
  if (sCapture.conflict && !ImGui::IsPopupOpen(kConflictPopup)) {
    ImGui::OpenPopup(kConflictPopup);
  }
  const ImVec2 overlayCenter(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() * 0.5f,
                             ImGui::GetWindowPos().y + ImGui::GetWindowHeight() * 0.5f);
  ImGui::SetNextWindowPos(overlayCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (ImGui::BeginPopupModal(kConflictPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (sCapture.conflict) {
      DrawConflict();
    }
    if (!sCapture.conflict) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  if (sCapture.target != ECapture::kNone && !sCapture.settling && !sCapture.conflict) {
    const bool keys = sCapture.target == ECapture::kKeyButton || sCapture.target == ECapture::kKeyAxis ||
                      sCapture.target == ECapture::kShiftKey || sCapture.target == ECapture::kTurboKey;
    const Uint64 elapsed = SDL_GetTicks() - sCapture.startMs;
    const unsigned left =
        static_cast< unsigned >((kCaptureTimeoutMs - std::min(elapsed, kCaptureTimeoutMs) + 999) / 1000);
    ImGui::Text("%s (Esc cancels, %us)",
                keys ? "Press a key, or click Press... with a mouse button" : "Press a controller button or stick...",
                left);
    if (!keys) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Cancel")) {
        CancelCapture();
      }
    }
  } else {
    ImGui::TextUnformatted("Pad 1. Click a binding, then press the input to assign it.");
  }

  // Nothing else is clickable while an input is being captured: the capture
  // owns every key, button and click until it binds or is cancelled.
  ImGui::BeginDisabled(sCapture.target != ECapture::kNone);
  float labelWidth = 0.f;
  for (size_t i = 0; i < std::size(kControlPadButtons); ++i) {
    layout.buttonLabels[i] = ActionLabel(kControlPadButtons[i].function, kControlPadButtons[i].label);
    labelWidth = std::max(labelWidth, ImGui::CalcTextSize(layout.buttonLabels[i].c_str()).x);
  }
  for (int i = 0; i < PAD_AXIS_COUNT; ++i) {
    layout.axisLabels[i] = ActionLabel(kControlPadAxes[i].function, kControlPadAxes[i].label);
    labelWidth = std::max(labelWidth, ImGui::CalcTextSize(layout.axisLabels[i].c_str()).x);
  }

  // Columns: the action, then each key slot (a binding button and its clear
  // button). A controller row's binding spans the key column; a button row's
  // alt button takes the alt key column.
  const ImGuiStyle& style = ImGui::GetStyle();
  layout.labelWidth = labelWidth;
  layout.clearWidth = ImGui::CalcTextSize("x").x + style.FramePadding.x * 2.f;
  layout.slotX[0] = labelWidth + style.ItemSpacing.x * 2.f;
  layout.slotX[1] = labelWidth + style.ItemSpacing.x * 4.f + layout.bindWidth + style.ItemInnerSpacing.x +
                    layout.clearWidth;
}

void DrawKeyboardMouse() {
  SBindLayout layout;
  BeginTab(layout);
  const float bindWidth = layout.bindWidth;
  const float labelWidth = layout.labelWidth;
  const float* slotX = layout.slotX;
  const std::string* buttonLabels = layout.buttonLabels;
  const std::string* axisLabels = layout.axisLabels;
  const auto keyPresetButton = [](const char* label, EKeyPreset preset, const char* tooltip) {
    if (ImGui::Button(label)) {
      ApplyKeyPreset(preset);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
      ImGui::SetTooltip("%s", tooltip);
    }
  };
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Keyboard preset:");
  ImGui::SameLine();
  keyPresetButton("Classic", EKeyPreset::kClassic,
                  "The default layout: WASD move, IJKL C-stick, X/Z/C/V for A/B/X/Y,\n"
                  "Q/E for L/R, F map, arrows D-pad, left shift beam shift.\n"
                  "Sets the keys, mouse buttons and beam shift key.");
  ImGui::SameLine();
  keyPresetButton("Mouse & keyboard", EKeyPreset::kMouseKeyboard,
                  "For mouse aim: WASD move, left click or E fire, Space jump,\n"
                  "left ctrl or C morph ball, F or middle click missile,\n"
                  "right click or Q lock on, left alt free look, Tab or M map,\n"
                  "1-4 beams, 5-8 visors (arrows too), left shift beam shift.\n"
                  "Turns on mouse aim and Twin Stick Aim, so a controller's right\n"
                  "stick aims too.");


  const float rowX = ImGui::GetCursorPosX();
  ImGui::TextDisabled("Action");
  ImGui::SameLine(rowX + slotX[0]);
  ImGui::TextDisabled("Key");
  ImGui::SameLine(rowX + slotX[1]);
  ImGui::TextDisabled("Alt key");
  for (int i = 0; i < static_cast< int >(std::size(kControlPadButtons)); ++i) {
    ImGui::PushID(i);
    KeyRow(ECapture::kKeyButton, i, buttonLabels[i], slotX, bindWidth);
    ImGui::PopID();
  }
  for (int i = 0; i < PAD_AXIS_COUNT; ++i) {
    ImGui::PushID(100 + i);
    KeyRow(ECapture::kKeyAxis, i, axisLabels[i], slotX, bindWidth);
    ImGui::PopID();
  }
  ImGui::PushID(150);
  KeyRow(ECapture::kShiftKey, 0, kShiftLabel, slotX, bindWidth);
  ImGui::PopID();
  ShiftTooltip();
  for (int slot = 0; slot < PAD_KEY_SLOT_COUNT; ++slot) {
    ShiftOverlapNote(ECapture::kShiftKey, slot);
  }
  ImGui::PushID(151);
  KeyRow(ECapture::kTurboKey, 0, kTurboLabel, slotX, bindWidth);
  ImGui::PopID();
  TurboTooltip();
  for (int slot = 0; slot < PAD_KEY_SLOT_COUNT; ++slot) {
    ShiftOverlapNote(ECapture::kTurboKey, slot);
  }

  ImGui::SeparatorText("Mouse buttons");
  ImGui::TextDisabled("Under mouse aim; out of it only A and B are pressed (bombs, menus).");
  // The combo's entries follow PortInputMap::EMouseAction: none, the pad
  // buttons in kControlPadButtons order, then the beam shift.
  std::string actionLabels[PortInputMap::kMA_Count];
  actionLabels[PortInputMap::kMA_None] = "None";
  for (size_t i = 0; i < std::size(kControlPadButtons); ++i) {
    actionLabels[i + 1] = buttonLabels[i];
  }
  actionLabels[PortInputMap::kMA_Shift] = kShiftLabel;
  static const char* const kMouseNames[PortInputMap::kMouseButtonCount] = {"Left", "Middle", "Right", "X1 (back)",
                                                                           "X2 (forward)"};
  for (int button = 0; button < PortInputMap::kMouseButtonCount; ++button) {
    ImGui::PushID(160 + button);
    const float rowX = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(kMouseNames[button]);
    ImGui::SameLine(rowX + slotX[0]);
    ImGui::SetNextItemWidth(std::max(bindWidth * 2.f, labelWidth));
    const int current = PortDebug::MouseAction(button);
    if (ImGui::BeginCombo("##action", actionLabels[current].c_str())) {
      for (int action = 0; action < PortInputMap::kMA_Count; ++action) {
        if (ImGui::Selectable(actionLabels[action].c_str(), action == current)) {
          PortDebug::SetMouseAction(button, action);
        }
      }
      ImGui::EndCombo();
    }
    ImGui::PopID();
  }
  ImGui::EndDisabled();
}

void DrawController() {
  SBindLayout layout;
  BeginTab(layout);
  const float bindWidth = layout.bindWidth;
  const float clearWidth = layout.clearWidth;
  const float* slotX = layout.slotX;
  const std::string* buttonLabels = layout.buttonLabels;
  const std::string* axisLabels = layout.axisLabels;
  const ImGuiStyle& style = ImGui::GetStyle();
  u32 padButtonCount = 0;
  if (PADGetButtonMappings(kControlPort, &padButtonCount) == nullptr) {
    ImGui::TextDisabled("No controller on pad 1.");
  } else if (SDL_Gamepad* pad = PADGetSDLGamepadForIndex(kControlPort);
             pad != nullptr && SDL_IsJoystickVirtual(SDL_GetGamepadID(pad))) {
    // Aurora ignores mappings on the virtual touch pad, so there is nothing to edit.
    ImGui::TextWrapped("The touch controls always use the GameCube layout; presets and remaps apply to "
                       "real controllers only.");
  } else {
    const auto presetButton = [](const char* label, EPadPreset preset, const char* tooltip) {
      if (ImGui::Button(label)) {
        ApplyPadPreset(preset);
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", tooltip);
      }
    };
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Preset:");
    ImGui::SameLine();
    presetButton("GameCube", EPadPreset::kGameCube,
                 "The default layout: the face buttons and shoulder as on a GameCube pad.\nTurns off Twin Stick Aim.");
    ImGui::SameLine();
    // A GameCube pad has no right stick click for free look.
    ImGui::BeginDisabled(PADIsGCAdapter(kControlPort));
    presetButton("Remastered", EPadPreset::kRemastered,
                 "Metroid Prime Remastered's Dual Sticks scheme (Xbox labels): RT or B\n"
                 "fire, LT lock on, A or LB jump, X morph ball, RB missile, Menu map,\n"
                 "View pause, right stick click free look. D-pad: up Combat, right Scan,\n"
                 "left X-Ray, down Thermal visor; hold Y for up Power, right Wave, left\n"
                 "Plasma, down Ice beam; in morph ball Y springs (with Spring Ball on).\n"
                 "Turns on Twin Stick Aim and the Scan/X-Ray swap.");
    ImGui::SameLine();
    presetButton("Modern", EPadPreset::kModern,
                 "RT fire, LT lock on, A jump, B morph ball, RB missile, Y map,\n"
                 "right stick click free look. Turns on Twin Stick Aim; hold LB\n"
                 "and press the D-pad to change beams.");
    ImGui::EndDisabled();
    ImGui::SameLine();
    presetButton("Southpaw", EPadPreset::kSouthpaw,
                 "The GameCube layout with the two sticks swapped.\nTurns off Twin Stick Aim.");

    const float padWidth = bindWidth + style.ItemInnerSpacing.x + clearWidth;
    const PADDeadZones* deadZones = PADGetDeadZones(kControlPort);
    const bool emulateTriggers = deadZones != nullptr && deadZones->emulateTriggers;
    const auto padRow = [&](ECapture kind, int index, const std::string& label) {
      const float rowX = ImGui::GetCursorPosX();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(label.c_str());
      ImGui::SameLine(rowX + slotX[0]);
      const SInput input = RowInput(kind, index, 0);
      std::string name = InputName(input);
      // An unbound L or R click follows its analog trigger.
      ECapture pairKind = ECapture::kNone;
      int pairIndex = -1;
      if (kind == ECapture::kPadButton && input.code == -1 && emulateTriggers &&
          PairedRow(kind, index, pairKind, pairIndex)) {
        name = std::string("(") + kControlPadAxes[pairIndex].label + ")";
      }
      BindingButton(kind, index, 0, name, padWidth);
      // A button row's second input (Aurora maps one; the port ORs this in).
      if (kind == ECapture::kPadButton) {
        const SInput alt = RowInput(kind, index, 1);
        ImGui::SameLine(rowX + slotX[1]);
        BindingButton(kind, index, 1, alt.code == -1 ? std::string("-") : InputName(alt), bindWidth);
        ImGui::SameLine(0.f, style.ItemInnerSpacing.x);
        ImGui::BeginDisabled(alt.code == -1);
        if (ImGui::Button("x")) {
          BindRow(kind, index, 1, SInput{});
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
          ImGui::SetTooltip("Clear");
        }
        ImGui::EndDisabled();
      }
    };
    {
      const float rowX = ImGui::GetCursorPosX();
      ImGui::TextDisabled("Action");
      ImGui::SameLine(rowX + slotX[0]);
      ImGui::TextDisabled("Button");
      ImGui::SameLine(rowX + slotX[1]);
      ImGui::TextDisabled("Alt button");
    }
    for (int i = 0; i < static_cast< int >(std::size(kControlPadButtons)); ++i) {
      ImGui::PushID(200 + i);
      padRow(ECapture::kPadButton, i, buttonLabels[i]);
      ImGui::PopID();
    }
    for (int i = 0; i < PAD_AXIS_COUNT; ++i) {
      ImGui::PushID(300 + i);
      padRow(ECapture::kPadAxis, i, axisLabels[i]);
      ImGui::PopID();
    }
    ImGui::PushID(400);
    padRow(ECapture::kShiftPad, 0, kShiftLabel);
    ShiftTooltip();
    ImGui::SameLine(0.f, style.ItemInnerSpacing.x);
    ImGui::BeginDisabled(ShiftPadInput().code == -1);
    if (ImGui::Button("x")) {
      BindRow(ECapture::kShiftPad, 0, 0, SInput{});
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    ShiftOverlapNote(ECapture::kShiftPad, 0);
    ImGui::PushID(401);
    padRow(ECapture::kTurboPad, 0, kTurboLabel);
    TurboTooltip();
    ImGui::SameLine(0.f, style.ItemInnerSpacing.x);
    ImGui::BeginDisabled(TurboPadInput().code == -1);
    if (ImGui::Button("x")) {
      BindRow(ECapture::kTurboPad, 0, 0, SInput{});
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    ShiftOverlapNote(ECapture::kTurboPad, 0);
    if (PADDeadZones* zones = PADGetDeadZones(kControlPort)) {
      DrawDeadZones(*zones);
    }
  }
  ImGui::EndDisabled();
}

} // namespace PortControls
