#pragma once

#include <cstdint>

#include "port_model_variant.h"

// What the game and the mod loader need to know of the "echoes-suits" mod (port_echoes_suits.h, the import that
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
// 4: the first-person arm cannon and its hologram, under variant ids (Cannon).
inline constexpr int kVersion = 4;

// The first-person arm cannon (CGunWeapon::LoadGunModels: characters 0 and 1 of each beam's ANCS).
// Echoes' gun and hologram have Prime's ids and are skinned on its own CINF 46ED31E0, the same 50
// joints by name and place as Prime's (only the ids differ), so Prime's gun animations drive them.
// Each goes under the variant id Cannon(id); Echoes' five material sets (Power, Dark, Light,
// Annihilator, then the fifth) keep their order, picked by beam like Prime's.
struct Gun {
  const char* what;  // for the log
  uint32_t cmdl, cskr;
};
inline constexpr uint32_t kGunCinf = 0x46ED31E0;
inline constexpr Gun kGuns[] = {
    {"arm cannon", 0x19AE626C, 0x7281CDBA},
    {"cannon hologram", 0xE6F4DEFC, 0x8DDB712A},
};
// The first-person gun's ANCS of each beam (CPlayerState::EBeamId order). The mod writes a variant
// of each, Cannon(ancs), whose characters 0 (the gun) and 1 (its hologram) are Echoes'.
inline constexpr uint32_t kBeamAncs[] = {0x41B63D84, 0xC1135EA3, 0x02B4357C, 0x6AB61E5E, 0x5E7FF762};
inline constexpr int kCannonKey = 0x4D5032;  // "MP2"
inline uint32_t Cannon(uint32_t id) { return PortModelVariant::Id(id, kCannonKey); }
// The ANCS CGunWeapon::LoadGunModels loads for a beam's ANCS: Cannon(ancs) while the mod serves it
// and the setting echoes_first_person_cannon (SetFirstPersonCannonShown) is on, else ancs.
uint32_t GunAncs(uint32_t ancs);
void SetFirstPersonCannonShown(bool shown);
bool FirstPersonCannonShown();

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
