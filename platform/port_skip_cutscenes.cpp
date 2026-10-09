#include "port_skip_cutscenes.h"

#include "port_apclient.h"
#include "port_debug.h"
#include "port_disc.h"
#include "port_log.h"
#include "port_randomizer.h"

#include <algorithm>

namespace PortSkipCutscenes {
namespace {

struct SkipRoom {
  uint32_t mrea;
  uint32_t offset;
  uint32_t size;
};

#include "port_skip_cutscenes_data.inc"

using PickupRoom = SkipRoom;
struct PickupModelEntry {
  int key;
  uint32_t model, acs, character, animation;
};

struct PickupGeometry {
  uint32_t model;
  float bounds[6];
  float rotation[3];
  float scale[3];
};

#include "port_ap_pickups_data.inc"

const uint32_t kLandingSite = 0xB2701146;

const SkipRoom* FindRoom(const SkipRoom* begin, const SkipRoom* end, uint32_t mreaId) {
  const SkipRoom* room = std::lower_bound(
      begin, end, mreaId, [](const SkipRoom& r, uint32_t id) { return r.mrea < id; });
  return room != end && room->mrea == mreaId ? room : nullptr;
}

// randomprime's pickup patches (tools/gen_ap_pickup_patches.py), on top of the
// skippable ones: a randomized pickup no longer plays its retail item's
// cutscene, and what that cutscene's end did moves onto a relay the pickup
// fires. Only in AP games, where every pickup holds a multiworld item.
void PatchPickups(uint32_t mreaId, const uint8_t* scly, size_t size, std::vector< uint8_t >& out) {
  out.clear();
  const SkipRoom* room =
      FindRoom(kPickupRooms, kPickupRooms + sizeof(kPickupRooms) / sizeof(kPickupRooms[0]), mreaId);
  if (room == nullptr)
    return;
  const int misses = ApplyOps(scly, size, kPickupOps + room->offset, room->size, out);
  if (misses != 0) {
    PortLog::Write("archipelago: room %08X pickup patch doesn't match (%d), left unpatched\n",
                   mreaId, misses);
    out.clear();
  }
}

const uint32_t kArtifactTemple = 0x2398E906;

// Randomized games' Artifact Temple (applied after the patches above): the
// central item no longer plays the artifact theme or leads into the totem
// cinematic, and the totems give every hint. The per-frame half, which wakes
// the totems when the room loads, is PortArtifactTemple in CStateManager.cpp.
std::vector< uint8_t > TempleOps() {
  std::vector< uint8_t > ops;
  auto u32 = [&](uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8)
      ops.push_back(uint8_t(v >> shift));
  };
  auto conn = [&](uint8_t op, uint32_t sender, uint32_t state, uint32_t msg, uint32_t target) {
    ops.push_back(op);
    u32(sender);
    u32(state);
    u32(msg);
    u32(target);
  };
  auto remove = [&](uint32_t id) {
    ops.push_back(6);
    u32(id);
  };
  // The artifact theme, and the music fade around it.
  remove(0x0410033C);
  remove(0x04100269);
  conn(3, 0x041001D4, 1 /* Arrived */, 13 /* SetToZero */, 0x04100268);
  // "Timer - Delay Enter Logbook Screen" starts the timer that arms the
  // cinematic's trigger, and opens the logbook.
  conn(3, 0x001003BE, 9 /* Zero */, 11 /* ResetAndStart */, 0x0010057A);
  conn(3, 0x001003BE, 9, 19 /* Action */, 0x001003BF);
  // Saves that already took the central item arm the trigger on load.
  conn(3, 0x0010030C, 0 /* Active */, 1 /* Activate */, 0x50100470);
  // Truth's totem lights up only once Truth is found (randomprime's
  // fix_artifact_of_truth_requirements); the room hook sends it.
  conn(3, 0x04100574, 9, 13, 0x00100125);
  // The first-stones timer lights the other six hints too (randomprime's
  // artifact hint behaviour "all").
  for (uint32_t relay : {0x04100127u, 0x0410012Du, 0x04100133u, 0x04100139u, 0x0410013Fu,
                         0x04100145u})
    conn(4, 0x0010017C, 9, 13, relay);
  return ops;
}

} // namespace

bool Forced() { return PortRandomizer::Enabled() || PortAp::RandomizedGame(); }

bool Active() { return PortDebug::SkippableCutscenes() || Forced(); }

bool PatchArea(uint32_t mreaId, const uint8_t* scly, size_t size, std::vector< uint8_t >& out) {
  out.clear();
  if (!Active())
    return false;
  // PAL lays some rooms out differently: its own streams, where the USA one
  // doesn't do the same there.
  const SkipRoom* room = nullptr;
  const uint8_t* skipOps = kSkipOps;
  if (PortDisc::Current() == PortDisc::Version::Pal) {
    room = FindRoom(kSkipRoomsPal, kSkipRoomsPal + sizeof(kSkipRoomsPal) / sizeof(kSkipRoomsPal[0]),
                    mreaId);
    skipOps = kSkipOpsPal;
  }
  if (room == nullptr) {
    room = FindRoom(kSkipRooms, kSkipRooms + sizeof(kSkipRooms) / sizeof(kSkipRooms[0]), mreaId);
    skipOps = kSkipOps;
  }
  bool skipped = false;
  if (room != nullptr) {
    const int misses = ApplyOps(scly, size, skipOps + room->offset, room->size, out);
    if (misses != 0) {
      // A mod replaced the room, or the disc isn't one the streams were made
      // from: the patch could leave the script half-edited, so keep the room
      // as it is. Only the skip is dropped; the patches below still apply.
      PortLog::Write("skippable cutscenes: room %08X doesn't match (%d), left unpatched\n",
                     mreaId, misses);
      out.clear();
    } else {
      skipped = true;
    }
  }
  const bool apGame = PortAp::RandomizedGame();
  if (skipped && mreaId == kLandingSite && apGame) {
    // Archipelago games start with Samus already out of the ship, as
    // randomprime's patch_landing_site_cutscene_triggers does.
    std::vector< uint8_t > landed;
    if (ApplyOps(out.data(), out.size(), kLandingOps, sizeof(kLandingOps), landed) == 0)
      out.swap(landed);
    else
      PortLog::Write("skippable cutscenes: Landing Site intro skip doesn't match, left as is\n");
  }
  if (apGame) {
    std::vector< uint8_t > pickups;
    if (skipped)
      PatchPickups(mreaId, out.data(), out.size(), pickups);
    else
      PatchPickups(mreaId, scly, size, pickups);
    if (!pickups.empty())
      out.swap(pickups);
  }
  if (mreaId == kArtifactTemple && Forced()) {
    const std::vector< uint8_t > ops = TempleOps();
    std::vector< uint8_t > temple;
    const bool patched = !out.empty();
    if (ApplyOps(patched ? out.data() : scly, patched ? out.size() : size, ops.data(), ops.size(),
                 temple) == 0)
      out.swap(temple);
    else
      PortLog::Write("randomizer: Artifact Temple patch doesn't match, left as is\n");
  }
  std::vector< uint8_t > seedOps;
  if (mreaId == kArtifactTemple && !out.empty() && PortAp::TempleOps(seedOps)) {
    // The seed's own temple rules, on top of the randomizer's temple.
    std::vector< uint8_t > temple;
    if (ApplyOps(out.data(), out.size(), seedOps.data(), seedOps.size(), temple) == 0)
      out.swap(temple);
    else
      PortLog::Write("archipelago: the seed's Artifact Temple patch doesn't match, left as is\n");
  }
  std::vector< uint8_t > roomOps;
  if (PortAp::RoomOps(mreaId, out.empty() ? scly : out.data(), out.empty() ? size : out.size(),
                      roomOps)) {
    // The seed's smaller options.
    std::vector< uint8_t > changed;
    if (ApplyOps(out.empty() ? scly : out.data(), out.empty() ? size : out.size(), roomOps.data(),
                 roomOps.size(), changed) == 0)
      out.swap(changed);
    else
      PortLog::Write("archipelago: room %08X's option patch doesn't match, left as is\n", mreaId);
  }
  std::vector< uint8_t > doorOps;
  const bool patched = !out.empty();
  if (PortAp::DoorOps(mreaId, patched ? out.data() : scly, patched ? out.size() : size, doorOps)) {
    // The seed's door types.
    std::vector< uint8_t > doors;
    if (ApplyOps(patched ? out.data() : scly, patched ? out.size() : size, doorOps.data(),
                 doorOps.size(), doors) == 0)
      out.swap(doors);
    else
      PortLog::Write("archipelago: room %08X's door patch doesn't match, left as is\n", mreaId);
  }
  return !out.empty();
}

bool PickupModel(int key, PortRandomizer::PickupModel& out) {
  for (const PickupModelEntry& entry : kPickupModels) {
    if (entry.key == key) {
      out.model = entry.model;
      out.acs = entry.acs;
      out.character = entry.character;
      out.animation = entry.animation;
      return true;
    }
  }
  return false;
}

bool IsPickupDependency(uint32_t id) {
  return std::binary_search(std::begin(kPickupDependencies), std::end(kPickupDependencies), id);
}

bool PickupPlacementFor(uint32_t model, PickupPlacement& out) {
  const PickupGeometry* it = std::lower_bound(
      std::begin(kPickupGeometry), std::end(kPickupGeometry), model,
      [](const PickupGeometry& entry, uint32_t id) { return entry.model < id; });
  if (it == std::end(kPickupGeometry) || it->model != model)
    return false;
  std::copy(std::begin(it->bounds), std::end(it->bounds), out.bounds);
  std::copy(std::begin(it->rotation), std::end(it->rotation), out.rotation);
  std::copy(std::begin(it->scale), std::end(it->scale), out.scale);
  return true;
}

} // namespace PortSkipCutscenes
