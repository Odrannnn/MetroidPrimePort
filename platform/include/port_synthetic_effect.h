#pragma once

// Remastered particle effects the disc has no PART for (BombAttract, PowerBombSecondary, the
// muzzle flashes, BusterImpact...) are imported under an id derived from their name, in the
// reserved range 0xFE000000..0xFEFFFFFF (no disc resource has one; the import checks). The
// importer and the game both compute it from the lower-case name, so the game finds the
// effect without a table: a mod that lacks it just doesn't have the resource.

#include <cstdint>

#include "port_hash.h"

namespace PortRemastered {

constexpr uint32_t kSyntheticEffectBase = 0xFE000000u;

// FNV-1a of the lower-case name, folded into the reserved range.
constexpr uint32_t SyntheticEffectId(const char* name) {
  uint32_t hash = port::kFnv32Basis;
  for (; *name != '\0'; ++name) {
    const char c = (*name >= 'A' && *name <= 'Z') ? char(*name - 'A' + 'a') : *name;
    hash = port::Fnv1a32Step(hash, uint8_t(c));
  }
  return kSyntheticEffectBase | (hash & 0x00FFFFFFu);
}

constexpr bool IsSyntheticEffectId(uint32_t id) { return (id >> 24) == 0xFEu; }

// Lower-case names of the Remastered-only effects the import writes under SyntheticEffectId().
inline constexpr const char* kSyntheticEffectNames[] = {
    "bombattract",
    "nftlight",
    "nftsourceendcap",
    "nfttargetendcap",
    "icespreadwall",
    "icespreadceiling",
    "powerbombsecondary",
    "bustermuzzle",
    "busterimpact",
    "missilemuzzleflash",
    "supermissilemuzzleflash",
    "powerchargemuzzleflash",
    "icechargemuzzleflash",
    "wavechargemuzzleflash",
    "plasmachargemuzzleflash",
    "phazonchargemuzzleflash",
    "icecombomuzzleflash",
    "ballinnerglow_power",
    "ballinnerglow_varia",
    "ballinnerglow_variawithspiderball",
    "ballinnerglow_gravity",
    "ballinnerglow_phazon",
    "underwaterbubblesweaponfire",
};

// Remastered-only effects with no name that a port actor builds itself (no disc PART, no room
// actor names them). The import writes each under SyntheticEffectId() of its id as it prints
// (8-4-4-4-12, lower case), as it does the rooms' own effects (RoomEffectId).
// Ridley's plasma-beam electric (an ELC2), CPlasmaProjectileMP1's electric from CRidleyData x370.
inline constexpr const char* kRidleyBeamElectric = "c5588467-1dcb-4b76-afaa-675018154a73";
// Its GPSM and EPSM, Remastered-only GENPs the ELC2 names.
inline constexpr const char* kRidleyBeamElectricGpsm = "34a60394-d3ee-4aa1-87de-70d3dc7c20cc";
inline constexpr const char* kRidleyBeamElectricEpsm = "5272fe00-f012-472c-92cf-f7690053b9fa";
inline constexpr const char* kActorEffectGuids[] = {kRidleyBeamElectric, kRidleyBeamElectricGpsm,
                                                    kRidleyBeamElectricEpsm};

}  // namespace PortRemastered
