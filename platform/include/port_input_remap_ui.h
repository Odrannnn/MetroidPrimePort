#pragma once

// F1 Controls > Remap: the user's bindings (controls.toml, port_input_bindings.h)
// as profiles of bindings, each an action on an input or a chord of inputs with a
// trigger, turbo and contexts; a pad profile follows a controller (GUID) or a kind
// of controller (SDL type).

#include "port_input_engine.h"

#include <string>

namespace PortInputRemap {

void Draw();
// An input capture is running: the overlay's pad navigation stays off.
bool Capturing();
// The inputs bound to `action` as Remap names them ("F1 or Back + Start"); empty
// when nothing is. Game thread only.
std::string BindingsText(PortInput::Action action, const char* separator = " or ");

} // namespace PortInputRemap
