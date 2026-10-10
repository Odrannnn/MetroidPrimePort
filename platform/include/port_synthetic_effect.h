#pragma once

// Remastered particle effects the disc has no PART for (BombAttract, PowerBombSecondary, the
// muzzle flashes, BusterImpact...) are imported under an id derived from their name, in the
// reserved range 0xFE000000..0xFEFFFFFF (no disc resource has one; the import checks). The
// importer and the game both compute it from the lower-case name, so the game finds the
// effect without a table: a mod that lacks it just doesn't have the resource.

#include <cstdint>

namespace PortRemastered {

constexpr uint32_t kSyntheticEffectBase = 0xFE000000u;

// FNV-1a of the lower-case name, folded into the reserved range.
constexpr uint32_t SyntheticEffectId(const char* name) {
  uint32_t hash = 2166136261u;
  for (; *name != '\0'; ++name) {
    const char c = (*name >= 'A' && *name <= 'Z') ? char(*name - 'A' + 'a') : *name;
    hash = (hash ^ uint8_t(c)) * 16777619u;
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
};

}  // namespace PortRemastered
