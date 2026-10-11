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

// The PAD bit's position, PortDebug::PadAltButton's index.
int PadBit(PADButton button) {
  int bit = 0;
  while (bit < PortDebug::kPadAltCount && (static_cast< u32 >(button) >> bit) != 1u) {
    ++bit;
  }
  return bit;
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

void DrawDeadZoneSliders(PADDeadZones& zones) {
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

void DrawDeadZones() {
  if (PADDeadZones* zones = PADGetDeadZones(kControlPort)) {
    DrawDeadZoneSliders(*zones);
  }
}

} // namespace PortControls
