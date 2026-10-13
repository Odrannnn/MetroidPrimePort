#include "port_revision_scripts.h"

#include "port_skip_cutscenes.h"

#include <cstring>

namespace PortRevisionScripts {
namespace {

using Version = PortDisc::Version;

const uint32_t kGeothermalCore = 0xC0498676;
const uint32_t kMainQuarry = 0x643D038F;
const uint32_t kOreProcessing = 0x97D2B2F6;
const uint32_t kRuinedCourtyard = 0x1921876D;
const uint32_t kEliteResearch = 0x8A97BB54;
const uint32_t kArboretum = 0x18AB6106;
const uint32_t kHiveTotem = 0xC8309DF6;
const uint32_t kCentralDynamo = 0xFEA372E2;

// Op codes and conn state/message codes (port_skip_cutscenes_ops.cpp).
enum : uint8_t { kEdit = 2, kRemConn = 3, kAddConn = 4, kDelete = 6 };
enum : uint32_t {
  kActive = 0,
  kArrived = 1,
  kInactive = 5,
  kDead = 0xE,
  kActivate = 1,
  kDeactivate = 4,
  kSetToZero = 0xD,
};

struct Writer {
  std::vector< uint8_t > ops;
  void u32(uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8)
      ops.push_back(uint8_t(v >> shift));
  }
  void conn(uint8_t op, uint32_t sender, uint32_t state, uint32_t msg, uint32_t target) {
    ops.push_back(op);
    u32(sender);
    u32(state);
    u32(msg);
    u32(target);
  }
  void remove(uint32_t id) {
    ops.push_back(kDelete);
    u32(id);
  }
  void u16(uint16_t v) {
    ops.push_back(uint8_t(v >> 8));
    ops.push_back(uint8_t(v));
  }
  // EDIT of one byte at `off` after the object's name.
  void setByte(uint32_t id, uint16_t off, uint8_t value) {
    ops.push_back(kEdit);
    u32(id);
    u16(0);
    u16(1);
    u16(off);
    u16(1);
    ops.push_back(value);
  }
  // EDIT of `count` floats from `off` after the object's name.
  void setFloats(uint32_t id, uint16_t off, const float* values, uint16_t count) {
    ops.push_back(kEdit);
    u32(id);
    u16(0);
    u16(1);
    u16(off);
    u16(uint16_t(count * 4));
    for (uint16_t i = 0; i < count; ++i) {
      uint32_t bits;
      std::memcpy(&bits, &values[i], 4);
      u32(bits);
    }
  }
};

// Property offsets after the name (structs of the randomprime scly_props; the
// prop counts are fixed so the layouts are constant, checked on every disc).
const uint16_t kTriggerPosition = 0;                // 3 floats, then the scale
const uint16_t kActorModel = 196, kActorActive = 341, kActorTail = 354;
const uint16_t kPlatformActive = 205, kPlatformTail = 352;
const uint16_t kPoiActive = 24, kPoiTail = 37;
const uint8_t kTypeActor = 0x00, kTypeTrigger = 0x04, kTypePlatform = 0x08, kTypePoi = 0x42;
const uint32_t kDoorLockModelVertical = 0x6E5D6796;

// PAL's Power Bomb rock and door blocker in front of Plasma Processing (the
// whole set: generator, debris, effect, rumble, memory relay, scan, blocker).
std::vector< uint8_t > GeothermalPal() {
  Writer w;
  for (uint32_t id = 0x001403AA; id <= 0x001403B5; ++id)
    w.remove(id);
  return std::move(w.ops);
}

// USA 1.01/1.02's invisible platform over the runic gate, and the message
// that turns it off.
std::vector< uint8_t > ArboretumUsa() {
  Writer w;
  w.conn(kRemConn, 0x00130140, kInactive, kDeactivate, 0x001302AA);
  w.remove(0x001302AA);
  return std::move(w.ops);
}

// The 1.02 locks are an actor, an inventory activator and a 0.02 s timer;
// dropping the three leaves the 1.00 room (Grapple Beam here).
std::vector< uint8_t > GeothermalUsa102() {
  Writer w;
  w.remove(0x0014039E);
  w.remove(0x0014039F);
  w.remove(0x001403A0);
  return std::move(w.ops);
}

// Power Bomb lock.
std::vector< uint8_t > MainQuarryUsa102() {
  Writer w;
  w.remove(0x000205D3);
  w.remove(0x000205D4);
  w.remove(0x000205D5);
  return std::move(w.ops);
}

// Power Bomb lock.
std::vector< uint8_t > OreProcessingUsa102() {
  Writer w;
  w.remove(0x0006036A);
  w.remove(0x0006036B);
  w.remove(0x0006036C);
  return std::move(w.ops);
}

// Thermal Visor lock: the relay F01D9 stops feeding the lock's objects, which
// go away, and is wired to F01C8 as in 1.00 (Dead -> Activate, and F01C8's
// Active -> Deactivate back).
std::vector< uint8_t > RuinedCourtyardUsa102() {
  Writer w;
  w.conn(kRemConn, 0x000F01D9, kDead, kSetToZero, 0x000F0315);
  w.conn(kRemConn, 0x000F01D9, kDead, kSetToZero, 0x000F0312);
  w.conn(kAddConn, 0x000F01D9, kDead, kActivate, 0x000F01C8);
  w.conn(kAddConn, 0x000F01C8, kActive, kDeactivate, 0x000F01D9);
  for (uint32_t id = 0x000F0312; id <= 0x000F0315; ++id)
    w.remove(id);
  return std::move(w.ops);
}

// Warrior exit lock: the relay D0517 that held the exit goes, and the pirate
// 40D01A4's death resets the exit's objects directly again. (They land after
// its other conns, not at their 1.00 positions.)
std::vector< uint8_t > EliteResearchUsa102() {
  Writer w;
  w.conn(kRemConn, 0x000D0340, kArrived, kSetToZero, 0x000D0517);
  w.remove(0x000D0517);
  w.conn(kAddConn, 0x040D01A4, kDead, kSetToZero, 0x040D0407);
  w.conn(kAddConn, 0x040D01A4, kDead, kSetToZero, 0x000D01F6);
  return std::move(w.ops);
}

// USA 1.02 Hive Totem: the boss trigger's box is back at randomprime's values
// (position as on disc, the scale's X was widened to 19.09 in 1.02; 1.00/1.01
// hold 5.138) and the one door lock actor 1.02 added to layer 0 (a lock model
// actor, the same model randomprime's remove_door_locks drops) goes.
std::vector< uint8_t > HiveTotemUsa102() {
  Writer w;
  const float box[6] = {94.57105f, 301.61603f, 0.344905f, 6.052994f, 24.659973f, 7.878154f};
  w.setFloats(0x04240140, kTriggerPosition, box, 6);
  w.remove(0x0024028D);
  return std::move(w.ops);
}

// PAL's destructible Power Bomb rock stays off, as upstream's PAL patch does:
// the platform, its scan point and the collision blocker (nothing else
// activates them; only 0x6036E and 0x60377 send them Deactivate).
std::vector< uint8_t > OreProcessingPal() {
  Writer w;
  w.setByte(0x00060372, kPlatformActive, 0);
  w.setByte(0x00060378, kPoiActive, 0);
  w.setByte(0x00060379, kActorActive, 0);
  return std::move(w.ops);
}

// PAL's extra Power Bomb door lock on layer 7 stays off (nothing activates it).
std::vector< uint8_t > MainQuarryPal() {
  Writer w;
  w.setByte(0x1C0205DB, kActorActive, 0);
  return std::move(w.ops);
}

// PAL's extra door lock to Quarantine Access A.
std::vector< uint8_t > CentralDynamoPal() {
  Writer w;
  w.remove(0x041B06A1);
  return std::move(w.ops);
}

// What an op's target must look like before the stream may run: the room is
// the one the ids were read from (layer, type, property size, lock model).
struct Guard {
  int layer;
  uint32_t id;
  uint8_t type;
  size_t tail;      // property bytes after the name
  uint32_t model;   // actor lock model at kActorModel, 0 for none
  int activeOff;    // property byte expected to be 1 (not for deletes), -1 for none
};

std::vector< Guard > GuardsFor(Version version, uint32_t mreaId) {
  if (version == Version::Usa102 && mreaId == kHiveTotem)
    return {{1, 0x04240140, kTypeTrigger, 63, 0, -1},
            {0, 0x0024028D, kTypeActor, kActorTail, kDoorLockModelVertical, -1}};
  if (version == Version::Pal) {
    switch (mreaId) {
    case kOreProcessing:
      return {{0, 0x00060372, kTypePlatform, kPlatformTail, 0, kPlatformActive},
              {0, 0x00060378, kTypePoi, kPoiTail, 0, kPoiActive},
              {0, 0x00060379, kTypeActor, kActorTail, 0, kActorActive}};
    case kMainQuarry:
      return {{7, 0x1C0205DB, kTypeActor, kActorTail, kDoorLockModelVertical, kActorActive}};
    case kCentralDynamo:
      return {{1, 0x041B06A1, kTypeActor, kActorTail, kDoorLockModelVertical, -1}};
    }
  }
  return {};
}

uint32_t BigEndian32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// Number of guards the room's objects don't satisfy (-1: not parseable).
int GuardMisses(const std::vector< Guard >& guards, const uint8_t* scly, size_t size) {
  std::vector< PortSkipCutscenes::ScriptObject > objects;
  if (!PortSkipCutscenes::ScanObjects(scly, size, objects))
    return -1;
  int misses = 0;
  for (const Guard& g : guards) {
    bool ok = false;
    for (const auto& o : objects) {
      if (o.id != g.id)
        continue;
      // Property count word plus at least the name's NUL.
      if (o.layer != g.layer || o.type != g.type || o.props.size() < 5)
        break;
      const uint8_t* end = static_cast< const uint8_t* >(
          std::memchr(o.props.data() + 4, 0, o.props.size() - 4));
      if (end == nullptr)
        break;
      const uint8_t* tail = end + 1;
      const size_t tailSize = size_t(o.props.data() + o.props.size() - tail);
      ok = (g.tail == 0 || tailSize == g.tail) &&
           (g.model == 0 || (tailSize >= kActorModel + 4U &&
                             BigEndian32(tail + kActorModel) == g.model)) &&
           (g.activeOff < 0 || (tailSize > size_t(g.activeOff) && tail[g.activeOff] == 1));
      break;
    }
    if (!ok)
      ++misses;
  }
  return misses;
}

} // namespace

std::vector< uint8_t > OpsFor(Version version, uint32_t mreaId) {
  if (version == Version::Pal) {
    switch (mreaId) {
    case kGeothermalCore:
      return GeothermalPal();
    case kOreProcessing:
      return OreProcessingPal();
    case kMainQuarry:
      return MainQuarryPal();
    case kCentralDynamo:
      return CentralDynamoPal();
    }
  }
  if ((version == Version::Usa101 || version == Version::Usa102) && mreaId == kArboretum)
    return ArboretumUsa();
  if (version == Version::Usa102) {
    switch (mreaId) {
    case kGeothermalCore:
      return GeothermalUsa102();
    case kMainQuarry:
      return MainQuarryUsa102();
    case kOreProcessing:
      return OreProcessingUsa102();
    case kRuinedCourtyard:
      return RuinedCourtyardUsa102();
    case kEliteResearch:
      return EliteResearchUsa102();
    case kHiveTotem:
      return HiveTotemUsa102();
    }
  }
  return {};
}

Result Apply(Version version, uint32_t mreaId, const uint8_t* scly, size_t size,
             std::vector< uint8_t >& out, int* misses) {
  out.clear();
  if (misses != nullptr)
    *misses = 0;
  const std::vector< uint8_t > ops = OpsFor(version, mreaId);
  if (ops.empty())
    return Result::NoPatch;
  const std::vector< Guard > guards = GuardsFor(version, mreaId);
  if (!guards.empty()) {
    const int bad = GuardMisses(guards, scly, size);
    if (bad != 0) {
      if (misses != nullptr)
        *misses = bad;
      return Result::Missed;
    }
  }
  std::vector< uint8_t > patched;
  const int missed = PortSkipCutscenes::ApplyOps(scly, size, ops.data(), ops.size(), patched);
  if (missed != 0) {
    if (misses != nullptr)
      *misses = missed;
    return Result::Missed;
  }
  out.swap(patched);
  return Result::Patched;
}

const char* Describe(Version version, uint32_t mreaId) {
  if (version == Version::Pal) {
    switch (mreaId) {
    case kGeothermalCore:
      return "PAL Geothermal Core Power Bomb rock";
    case kOreProcessing:
      return "PAL Ore Processing Power Bomb rock";
    case kMainQuarry:
      return "PAL Main Quarry door lock";
    case kCentralDynamo:
      return "PAL Central Dynamo door lock";
    }
  }
  if (version == Version::Usa101 && mreaId == kArboretum)
    return "USA 1.01 Arboretum gate platform";
  if (version == Version::Usa102) {
    switch (mreaId) {
    case kArboretum:
      return "USA 1.02 Arboretum gate platform";
    case kGeothermalCore:
      return "USA 1.02 Geothermal Core Grapple lock";
    case kMainQuarry:
      return "USA 1.02 Main Quarry Power Bomb lock";
    case kOreProcessing:
      return "USA 1.02 Ore Processing Power Bomb lock";
    case kRuinedCourtyard:
      return "USA 1.02 Ruined Courtyard Thermal lock";
    case kEliteResearch:
      return "USA 1.02 Elite Research Warrior lock";
    case kHiveTotem:
      return "USA 1.02 Hive Totem trigger and door lock";
    }
  }
  return "revision script";
}

} // namespace PortRevisionScripts
