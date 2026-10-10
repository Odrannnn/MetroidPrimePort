#pragma once

// F1 Controls > Remap: the user's bindings (controls.toml, port_input_bindings.h)
// as profiles of bindings, each an action on an input or a chord of inputs with a
// trigger, turbo and contexts; a pad profile follows a controller (GUID) or a kind
// of controller (SDL type).

namespace PortInputRemap {

void Draw();
// An input capture is running: the overlay's pad navigation stays off.
bool Capturing();

} // namespace PortInputRemap
