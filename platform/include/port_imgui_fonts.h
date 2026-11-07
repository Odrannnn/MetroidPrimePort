#ifndef METROID_PRIME_PORT_PORT_IMGUI_FONTS_H
#define METROID_PRIME_PORT_PORT_IMGUI_FONTS_H

#include <aurora/aurora.h>

namespace PortImGuiFonts {

// Aurora's imGuiInitCallback: builds the overlay's font set before the atlas
// is first built.
void Init(const AuroraWindowSize* size);

} // namespace PortImGuiFonts

#endif
