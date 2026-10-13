#pragma once

#include "port_disc.h"

#include <cstddef>
#include <cstdint>

// The generated skippable-cutscene op streams (port_skip_cutscenes_data.inc),
// looked up per disc version. Split from port_skip_cutscenes.cpp so tests can
// check the selection without the game.
namespace PortSkipCutscenes {

struct SkipStream {
  const uint8_t* ops = nullptr;
  size_t size = 0;
};

// The skip stream of room `mreaId` for `version`: PAL's own where it has one,
// USA 1.02's override (Chapel of the Elders) on 1.02, else the generic USA
// one. False when the room has no skip.
bool SkipStreamFor(PortDisc::Version version, uint32_t mreaId, SkipStream& out);

// randomprime's Landing Site intro skip, applied after the room's stream.
SkipStream LandingStream();

} // namespace PortSkipCutscenes
