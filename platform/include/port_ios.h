#pragma once

// iOS pieces SDL does not cover. Only built for iOS (CMakeLists.txt).

#include <string>

struct SDL_Window;

namespace PortIos {

// Callback with SDL_DialogFileCallback's shape: a one-entry list with the
// picked path, an empty list for a cancel, nullptr for an error.
using DiscPickedCallback = void (*)(void* userdata, const char* const* files, int filter);

// The app's Documents folder, with a trailing slash: what the Files app shows
// under "On My iPhone > Metroid Prime". Empty if it cannot be found.
std::string DocumentsFolder();

// Shows the system document picker over `window`. The picked file is copied
// into the app (iOS grants no lasting access to a file outside it) and moved
// into DocumentsFolder(), so the saved path still opens on the next launch.
// Returns at once; `callback` runs on the main thread from the event pump.
void ShowDiscPicker(SDL_Window* window, DiscPickedCallback callback, void* userdata);

} // namespace PortIos
