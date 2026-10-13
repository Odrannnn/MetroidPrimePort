#pragma once

#include "port_disc.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// Room script (SCLY) patches that make a later disc revision play like USA
// 1.00 (and so like the randomizer logic, built from 1.00): the PAL Geothermal
// Core Power Bomb rock, the USA 1.01/1.02 Arboretum gate platform, and the
// four USA 1.02 inventory locks plus the Elite Research exit lock, the USA 1.02
// Hive Totem trigger and door lock, and the PAL Ore Processing rock, Main
// Quarry lock and Central Dynamo lock (ops guarded by the targets' layer, type,
// size and model; a mismatch is Missed). Pure: the
// caller decides when they apply (PortSkipCutscenes::PatchArea).
namespace PortRevisionScripts {

enum class Result {
  NoPatch, // not this version/room, nothing to do
  Patched, // `out` holds the patched script
  Missed,  // the script doesn't match, `out` is empty and nothing changed
};

// The op stream (ApplyOps format) for a room on a disc version; empty when
// there is none. Only the versions the ids were taken from get one.
std::vector< uint8_t > OpsFor(PortDisc::Version version, uint32_t mreaId);

// Applies OpsFor() to `scly` (all or nothing). On Missed, `misses` is the
// number of ops whose target was absent (-1 for a malformed stream).
Result Apply(PortDisc::Version version, uint32_t mreaId, const uint8_t* scly, size_t size,
             std::vector< uint8_t >& out, int* misses = nullptr);

// Short text for log lines ("USA 1.02 Ruined Courtyard lock").
const char* Describe(PortDisc::Version version, uint32_t mreaId);

} // namespace PortRevisionScripts
