#pragma once

// A second look of one retail model or character. Retail draws every suit's
// ball from one model and picks a material set; a Remastered import replaces
// that model with one set of textures, so it writes the other suits' balls as
// separate resources under the id this gives, and the game loads that id when
// a mod has it. Shared by the importer and the game, so both agree on the id.

#include <cstdint>

#include "port_hash.h"

namespace PortModelVariant {

// `key` picks the look: the suit (CPlayerState::EPlayerSuit) for Samus, the
// weapon type for the beam troopers. The id of a variant that is
// also a retail resource's is never written, so it cannot shadow one.
constexpr uint32_t Id(uint32_t retail, int key) {
  uint32_t h = port::kFnv32Basis;
  const auto mix = [&h](uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      h = port::Fnv1a32Step(h, uint8_t(v >> (8 * i)));
    }
  };
  mix(0x56415249u); // "VARI"
  mix(retail);
  mix(uint32_t(key));
  return h;
}

// The look Remastered draws as model 3 (the thermal hot pass): the arm
// cannon's heat mask (a beam's gun character) and the suit arm's
// <Suit>Arm_Thermal (a retail arm CMDL).
constexpr int kThermalLook = 3;

// The look Remastered draws in the X-ray visor where retail has no X-ray model
// (the grapple arm's <Part>_XRAY CMDLs).
constexpr int kXRayLook = 4;

} // namespace PortModelVariant
