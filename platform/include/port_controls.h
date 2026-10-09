#pragma once

#include <string_view>

// Input rebinding: the port's keyboard defaults and the overlay's Controls page.
// The binding storage, matching and persistence live in Aurora (dolphin/pad.h).

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
// True while the Controls tab is waiting for, or settling after, an input to
// bind; controller navigation of the overlay should ignore the pad then.
bool Capturing();
// True while a bound beam shift key or pad input is held (Controls tab;
// PortDebug::ShiftBinding stores them).
bool ShiftHeld();
// True while a bound turbo fire key or pad input is held (PortDebug::TurboBinding).
bool TurboHeld();
// The PAD bits whose alt controller button (PortDebug::PadAltButton) is held.
unsigned HeldAltPadButtons();
// True if the native controller button (SDL_GamepadButton) presses a PAD button
// other than L, as its main or alt binding.
bool PadButtonBoundBesidesL(int native);
// The Controls page's Keyboard & mouse and Controller sub-tabs of the debug
// overlay. Each polls the capture and shows its prompt, so a binding can be
// started from either.
void DrawKeyboardMouse();
void DrawController();

} // namespace PortControls
