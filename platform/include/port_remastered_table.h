#pragma once

// The list of Remastered models the importer converts, and the retail model
// each one replaces. Generated; see port_remastered_table.cpp.

#include <cstddef>
#include <cstdint>

namespace PortRemastered {

// What the few models that need more than the defaults get.
struct TableOptions {
  int material;             // retail material every surface is put on, -1 to vote
  int maxTexture;           // largest edge of a TEV path texture
  const char* squeezeRole;  // null for no squeeze; see ConvertOptions
  double squeeze[4];        // from u0, u1 onto lo, hi
};

struct TableEntry {
  uint32_t retail;      // CMDL id
  uint8_t rem[16];      // Remastered model id, in a pak's byte order
  int8_t orient[9];     // gc = orient * remastered + offset, row major
  double offset[3];
  uint16_t firstSkin;   // into TableSkins
  uint16_t skinCount;
  int16_t options;      // into the option sets, -1 for the defaults
  bool pbr;
  // A second look of `retail` (a suit's ball, a beam's gun), written under ids
  // of its own. `key` >= 0: the look for that suit, found by the game under
  // PortModelVariant::Id(id, key) - of `retail` itself for a static model, of
  // `ancs` (a copy of it binding the new model) for a character's. `key` -1 and
  // `ancs` set: `ancs` is replaced in place by such a copy. 0 and -1: neither.
  // kEveryCharacter and -1: `retail` keeps its id but its skins get new ones
  // (another model shares the retail ones), bound in every character.
  uint32_t ancs;
  int8_t key;
  // `ancs` set: the character of it whose pair is rebound, named by its body CMDL (the
  // CCharacterInfo's own model), for a pair bound by several (one ANCS's characters sharing
  // an ice overlay). 0: the pair is bound once.
  uint32_t character = 0;
};

constexpr uint32_t kEveryCharacter = 0xFFFFFFFFu;

const TableEntry* Table(size_t& count);
const uint32_t* TableSkins(const TableEntry& entry);
const TableOptions* TableExtra(const TableEntry& entry);

} // namespace PortRemastered
