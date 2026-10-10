#pragma once

#include <string_view>

// The port's keyboard and controller presets, over the Aurora PAD mappings
// (dolphin/pad.h). Binding itself is the overlay's Controls > Remap page.

namespace PortControls {

// Binds the port's default keyboard layout (WASD move, IJKL look, X/Z/C/V face
// buttons) to the port and turns its keyboard on. Doesn't save.
void ApplyDefaultKeyBindings(unsigned port);
// The Controls page's keyboard preset button: "classic" or "mouse" (Mouse &
// keyboard). Saves. False for any other name.
bool ApplyKeyPresetNamed(std::string_view name);
// The Controls page's controller preset button: "gamecube", "remastered",
// "modern" or "southpaw". Saves. False for any other name.
bool ApplyPadPresetNamed(std::string_view name);
// True if the native controller button (SDL_GamepadButton) presses a PAD button
// other than L, as its main or alt binding.
bool PadButtonBoundBesidesL(int native);
// The stick deadzone and trigger click-point sliders of the Controller settings
// page (nothing when no controller is on pad 1).
void DrawDeadZones();

} // namespace PortControls
