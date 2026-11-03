#pragma once

#include <cstdint>

// What the mod loader needs to know of the "echoes-suits" mod (port_echoes_suits.h, the import that
// makes it), without the import's headers.
namespace PortEchoesSuits {

// The mod's folder in the mods folder, and its stamp: kVersion, then the build's commit. Bump
// kVersion when the output changes.
inline constexpr const char* kModName = "echoes-suits";
inline constexpr const char* kStampName = ".echoes-suits-version";
// 1: Varia, Gravity (Echoes' Dark Suit) and Phazon (its Light Suit), cutscene and in-game bodies.
// 2: the pause screen's doll bodies; the Morph Balls (Varia, Dark as Gravity, Light as Phazon) and
//    the Dark Spider Ball as Gravity's.
// 3: the first-person arms, right and left (grapple).
inline constexpr int kVersion = 3;

// The Prime body CMDLs the mod writes (PortEchoesSuits::kTargets and kCopies): cutscene Varia,
// Gravity, Phazon, the Tallon Phazon copy, in-game Varia, Gravity, Phazon, then the doll's Varia,
// Gravity, Phazon. While the mod serves one, PortDreadSamus::Replaces holds for it: Echoes' bodies
// carry their own cannon, boots and visor.
inline constexpr uint32_t kBodies[] = {0x776C1A96, 0x57F5C930, 0x2782648E, 0xB333E1D7, 0xE449874A,
                                       0x57489AD3, 0x891E00FD, 0xA602C954, 0x11A5BC8A, 0x5644E7A1};
inline bool IsBody(uint32_t id) {
  for (const uint32_t body : kBodies) {
    if (body == id) return true;
  }
  return false;
}

}  // namespace PortEchoesSuits
