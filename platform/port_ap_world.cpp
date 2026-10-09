#include "port_strings.h"
#include "port_ap_world.h"

#include <cstdio>

#include "port_custom_res.h"
#include "port_json.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <sstream>

namespace PortApWorld {
namespace {

struct Room {
  int area;
  uint32_t mlvl;
  uint32_t mrea;
  uint32_t mapa;
  const char* name;
};

struct Elevator {
  const char* name;
  int area;
  uint32_t mlvl;
  uint32_t mrea;
  uint32_t editorId;
  const char* shown;
  uint32_t strgs[3];
};

struct Door {
  int room;
  int dock;
  int dest;
  int defaultLock;
  int lock;
  int shield;
  int flags;
  int subDoor;
  uint32_t doorId;
  float rotation[3];
  uint32_t forces[2];
  uint32_t shields[2];
};

enum { kDoorExcluded = 1, kDoorVertical = 2 };

#include "port_ap_world_data.inc"

const int kDoorCount = static_cast< int >(sizeof(kDoors) / sizeof(kDoors[0]));

const uint32_t kTallonWorld = 0x39F2DE28;
const uint32_t kCraterWorld = 0xC13B09D1;
// randomprime's "Credits" destination: the ending cinematic's only room.
const Place kCredits = {0x13D79165, 0xB4B41C48};

// Names are compared the way randomprime does: without case or outer spaces.
std::string Folded(const std::string& text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && std::isspace(static_cast< unsigned char >(text[begin])))
    ++begin;
  while (end > begin && std::isspace(static_cast< unsigned char >(text[end - 1])))
    --end;
  std::string out = text.substr(begin, end - begin);
  for (char& c : out)
    c = static_cast< char >(std::tolower(static_cast< unsigned char >(c)));
  return out;
}

const Elevator* FindElevator(const std::string& name) {
  const std::string folded = Folded(name);
  for (const Elevator& elevator : kElevators) {
    if (Folded(elevator.name) == folded)
      return &elevator;
  }
  return nullptr;
}

// Index of `name` in a name table, -1 when it has no such name.
template < size_t N >
int NameIndex(const char* const (&names)[N], const std::string& name) {
  const std::string folded = Folded(name);
  for (size_t i = 0; i < N; ++i) {
    if (Folded(names[i]) == folded)
      return static_cast< int >(i);
  }
  return -1;
}

// The first door of the room behind `door` that leads back to `door`'s room
// (the apworld's RoomData.get_matching_door), -1 when there is none.
int PairedDoor(int door) {
  const Door& d = kDoors[door];
  if (d.dest < 0)
    return -1;
  for (int i = 0; i < kDoorCount; ++i) {
    if (kDoors[i].room == d.dest && kDoors[i].dest == d.room)
      return i;
  }
  return -1;
}

// What the apworld's region pass leaves on every door: its lock (-1 when the
// table's and the seed's leave it alone) and its blast shield (-1 not said),
// both as indices in kLocks / kShields.
struct Resolved {
  std::vector< int > lock;
  std::vector< int > shield;
};

Resolved Resolve(const Layout& layout) {
  enum { kBlue = 0 };
  const int kNoShield = NameIndex(kShields, "None");
  Resolved out;
  out.lock.resize(kDoorCount);
  out.shield.resize(kDoorCount);
  for (int i = 0; i < kDoorCount; ++i) {
    out.lock[i] = kDoors[i].lock;
    out.shield[i] = kDoors[i].shield;
  }
  if (layout.hasShields) {
    // apply_blast_shield_mapping: the disc's shields go, the seed's come.
    for (int i = 0; i < kDoorCount; ++i) {
      if (out.shield[i] >= 0)
        out.shield[i] = kNoShield;
    }
    for (const auto& area : layout.shields) {
      for (const auto& room : area.second) {
        const std::string name = Folded(room.first);
        for (int i = 0; i < kDoorCount; ++i) {
          const Room& r = kRooms[kDoors[i].room];
          if (Folded(kAreas[r.area]) != Folded(area.first) || Folded(r.name) != name)
            continue;
          const auto entry = room.second.find(kDoors[i].dock);
          if (entry == room.second.end())
            continue;
          const int shield = NameIndex(kShields, entry->second);
          if (shield >= 0)
            out.shield[i] = shield;
        }
      }
    }
  }
  // A shield on either side of a door ends up on both, and the door under it
  // is a plain one.
  auto mirror = [&](int door) {
    const int paired = PairedDoor(door);
    if (paired < 0)
      return;
    if (out.shield[paired] >= 0 && out.shield[paired] != kNoShield) {
      out.shield[door] = out.shield[paired];
      out.lock[door] = kBlue;
    } else if (out.shield[door] >= 0 && out.shield[paired] != kNoShield) {
      out.shield[paired] = out.shield[door];
      out.lock[door] = kBlue;
    }
  };
  for (int i = 0; i < kDoorCount; ++i) {
    const Door& d = kDoors[i];
    if (d.dest < 0)
      continue;
    if (layout.doorColorRandomization && layout.hasDoorColors && (d.flags & kDoorExcluded) == 0) {
      const auto area = layout.doorColors.find(kAreas[kRooms[d.room].area]);
      if (area != layout.doorColors.end()) {
        const auto entry = area->second.find(kLocks[d.defaultLock]);
        if (entry != area->second.end()) {
          const int lock = NameIndex(kLocks, entry->second);
          if (lock >= 0)
            out.lock[i] = lock;
        }
      }
    }
    mirror(i);
    if (d.subDoor >= 0) {
      for (int k = 0; k < kDoorCount; ++k) {
        if (kDoors[k].room == d.dest && kDoors[k].dock == d.subDoor) {
          mirror(k);
          break;
        }
      }
    }
  }
  return out;
}

} // namespace

void Parse(const PortJson::Value& data, Layout& layout) {
  layout = Layout();
  const PortJson::Value* room = data.Find("starting_room_name");
  if (room != nullptr && room->IsString())
    layout.startRoom = room->AsString();
  const PortJson::Value* bosses = data.Find("final_bosses");
  if (bosses != nullptr && bosses->IsNumber()) {
    const int64_t value = bosses->AsInt();
    layout.finalBosses = value >= 0 && value <= 3 ? static_cast< int >(value) : 0;
  }
  const PortJson::Value* artifacts = data.Find("required_artifacts");
  if (artifacts != nullptr && artifacts->IsNumber()) {
    const int64_t value = artifacts->AsInt();
    layout.requiredArtifacts = value >= 0 && value <= 12 ? static_cast< int >(value) : 12;
  }
  const PortJson::Value* elevators = data.Find("elevator_mapping");
  if (elevators != nullptr && elevators->IsObject()) {
    for (const auto& area : elevators->AsObject()) {
      if (!area.second.IsObject())
        continue;
      for (const auto& entry : area.second.AsObject()) {
        if (entry.second.IsString())
          layout.elevators[area.first][entry.first] = entry.second.AsString();
      }
    }
  }
  // Both door mappings are {area: {"area": area, "type_mapping": {...}}}, and
  // absent when the seed doesn't randomize them.
  const PortJson::Value* colors = data.Find("door_color_mapping");
  if (colors != nullptr && colors->IsObject()) {
    layout.hasDoorColors = true;
    for (const auto& area : colors->AsObject()) {
      const PortJson::Value* types = area.second.IsObject() ? area.second.Find("type_mapping") : nullptr;
      if (types == nullptr || !types->IsObject())
        continue;
      for (const auto& entry : types->AsObject()) {
        if (entry.second.IsString())
          layout.doorColors[area.first][entry.first] = entry.second.AsString();
      }
    }
  }
  const PortJson::Value* colorOption = data.Find("door_color_randomization");
  layout.doorColorRandomization = colorOption != nullptr && colorOption->IsNumber()
                                      ? colorOption->AsInt() != 0
                                      : layout.hasDoorColors;
  // Options arrive as numbers; Text() and older servers may say true/false.
  const auto flag = [&data](const char* name) {
    const PortJson::Value* value = data.Find(name);
    if (value == nullptr)
      return false;
    return value->IsBool() ? value->AsBool() : value->IsNumber() && value->AsInt() != 0;
  };
  layout.removeHiveMecha = flag("remove_hive_mecha");
  layout.backwardsLowerMines = flag("backwards_lower_mines");
  layout.flaahgraPowerBombs = flag("flaahgra_power_bombs");
  const PortJson::Value* capacity = data.Find("etank_capacity");
  if (capacity != nullptr && capacity->IsNumber()) {
    const int64_t value = capacity->AsInt();
    layout.etankCapacity = value >= 1 && value <= 1000 ? static_cast< int >(value) : 100;
  }
  const PortJson::Value* shields = data.Find("blast_shield_mapping");
  if (shields != nullptr && shields->IsObject()) {
    layout.hasShields = true;
    for (const auto& area : shields->AsObject()) {
      const PortJson::Value* rooms = area.second.IsObject() ? area.second.Find("type_mapping") : nullptr;
      if (rooms == nullptr || !rooms->IsObject())
        continue;
      for (const auto& room : rooms->AsObject()) {
        if (!room.second.IsObject())
          continue;
        for (const auto& entry : room.second.AsObject()) {
          char* end = nullptr;
          const long dock = std::strtol(entry.first.c_str(), &end, 10);
          if (end != entry.first.c_str() && *end == '\0' && entry.second.IsString())
            layout.shields[area.first][room.first][static_cast< int >(dock)] =
                entry.second.AsString();
        }
      }
    }
  }
}

std::string Text(const Layout& layout) {
  std::ostringstream text;
  text << "{\"starting_room_name\":" << port::JsonQuote(layout.startRoom)
       << ",\"final_bosses\":" << layout.finalBosses
       << ",\"required_artifacts\":" << layout.requiredArtifacts << ",\"elevator_mapping\":{";
  bool firstArea = true;
  for (const auto& area : layout.elevators) {
    text << (firstArea ? "" : ",") << port::JsonQuote(area.first) << ":{";
    firstArea = false;
    bool first = true;
    for (const auto& entry : area.second) {
      text << (first ? "" : ",") << port::JsonQuote(entry.first) << ':' << port::JsonQuote(entry.second);
      first = false;
    }
    text << '}';
  }
  text << "},\"door_color_randomization\":" << (layout.doorColorRandomization ? 1 : 0)
       << ",\"remove_hive_mecha\":" << (layout.removeHiveMecha ? 1 : 0)
       << ",\"backwards_lower_mines\":" << (layout.backwardsLowerMines ? 1 : 0)
       << ",\"flaahgra_power_bombs\":" << (layout.flaahgraPowerBombs ? 1 : 0)
       << ",\"etank_capacity\":" << layout.etankCapacity;
  if (layout.hasDoorColors) {
    text << ",\"door_color_mapping\":{";
    firstArea = true;
    for (const auto& area : layout.doorColors) {
      text << (firstArea ? "" : ",") << port::JsonQuote(area.first) << ":{\"type_mapping\":{";
      firstArea = false;
      bool first = true;
      for (const auto& entry : area.second) {
        text << (first ? "" : ",") << port::JsonQuote(entry.first) << ':' << port::JsonQuote(entry.second);
        first = false;
      }
      text << "}}";
    }
    text << '}';
  }
  if (layout.hasShields) {
    text << ",\"blast_shield_mapping\":{";
    firstArea = true;
    for (const auto& area : layout.shields) {
      text << (firstArea ? "" : ",") << port::JsonQuote(area.first) << ":{\"type_mapping\":{";
      firstArea = false;
      bool firstRoom = true;
      for (const auto& room : area.second) {
        text << (firstRoom ? "" : ",") << port::JsonQuote(room.first) << ":{";
        firstRoom = false;
        bool first = true;
        for (const auto& entry : room.second) {
          text << (first ? "" : ",") << '"' << entry.first << "\":" << port::JsonQuote(entry.second);
          first = false;
        }
        text << '}';
      }
      text << "}}";
    }
    text << '}';
  }
  text << '}';
  return text.str();
}

int DockTo(const std::string& area, const std::string& room, const std::string& dest) {
  const std::string areaName = Folded(area);
  const std::string roomName = Folded(room);
  const std::string destName = Folded(dest);
  for (const Door& door : kDoors) {
    const Room& r = kRooms[door.room];
    if (door.dest >= 0 && Folded(kAreas[r.area]) == areaName && Folded(r.name) == roomName &&
        Folded(kRooms[door.dest].name) == destName)
      return door.dock;
  }
  return -1;
}

std::vector< std::pair< std::string, int > > DiscShields(const std::string& area) {
  const std::string areaName = Folded(area);
  std::vector< std::pair< std::string, int > > out;
  for (const Door& door : kDoors) {
    const Room& r = kRooms[door.room];
    if (door.shield >= 0 && Folded(kAreas[r.area]) == areaName)
      out.emplace_back(r.name, door.dock);
  }
  return out;
}

bool StartRoom(const Layout& layout, Place& out) {
  std::string name = Folded(layout.startRoom);
  if (name.empty())
    return false;
  // "Area: Room" (or randomprime's "Area:Room") picks among rooms two areas
  // both have; a bare name is the first area's, in the apworld's order.
  std::string area;
  const size_t colon = name.find(':');
  if (colon != std::string::npos) {
    area = Folded(name.substr(0, colon));
    name = Folded(name.substr(colon + 1));
  }
  for (const Room& room : kRooms) {
    if (Folded(room.name) != name || (!area.empty() && Folded(kAreas[room.area]) != area))
      continue;
    out.mlvl = room.mlvl;
    out.mrea = room.mrea;
    return true;
  }
  return false;
}

bool TeleporterDestination(const Layout& layout, uint32_t mlvl, uint32_t editorId,
                           const Place& retail, Place& out) {
  // The Artifact Temple's portal leads to the Crater; without Metroid Prime
  // in the seed it leads to the credits (the apworld's temple_dest).
  if (mlvl == kTallonWorld && retail.mlvl == kCraterWorld) {
    if (layout.finalBosses != 1 && layout.finalBosses != 3)
      return false;
    out = kCredits;
    return true;
  }
  for (const Elevator& elevator : kElevators) {
    // The id's top bits are the layer, which a room patch may have changed.
    if (elevator.mlvl != mlvl || ((elevator.editorId ^ editorId) & 0x03FFFFFF) != 0)
      continue;
    const auto area = layout.elevators.find(kAreas[elevator.area]);
    if (area == layout.elevators.end())
      return false;
    const auto entry = area->second.find(elevator.name);
    if (entry == area->second.end())
      return false;
    const Elevator* target = FindElevator(entry->second);
    if (target == nullptr)
      return false;
    out.mlvl = target->mlvl;
    out.mrea = target->mrea;
    return out.mlvl != retail.mlvl || out.mrea != retail.mrea;
  }
  return false;
}

float SuitDamageReduction(int mode, bool varia, bool gravity, bool phazon) {
  if (mode == 1) {
    static const float kByCount[] = {0.f, 0.1f, 0.2f, 0.5f};
    return kByCount[(varia ? 1 : 0) + (gravity ? 1 : 0) + (phazon ? 1 : 0)];
  }
  if (mode == 2) {
    // Indexed by gravity | varia << 1 | phazon << 2, as randomprime's table.
    static const float kBySuit[] = {0.f, 0.1f, 0.1f, 0.2f, 0.3f, 0.4f, 0.4f, 0.5f};
    return kBySuit[(gravity ? 1 : 0) | (varia ? 2 : 0) | (phazon ? 4 : 0)];
  }
  return -1.f;
}

bool Strings(const Layout& layout, uint32_t strg, std::vector< std::string >& out) {
  // The Temple Security Station's objective scan (the apworld's get_strg).
  if (strg == 0xB389B6D6) {
    std::string objective = "Current Mission: Retrieve " + std::to_string(layout.requiredArtifacts) +
                            " Chozo Artifact" + (layout.requiredArtifacts != 1 ? "s" : "");
    if (layout.finalBosses == 0 || layout.finalBosses == 1)
      objective += "\nDefeat Meta Ridley";
    if (layout.finalBosses == 0 || layout.finalBosses == 2)
      objective += "\nDefeat Metroid Prime";
    out = {"Objective data decoded\n", "Mission Objectives", objective};
    return true;
  }
  for (const Elevator& elevator : kElevators) {
    int kind = 0;
    while (kind < 3 && elevator.strgs[kind] != strg)
      ++kind;
    if (kind == 3)
      continue;
    const auto area = layout.elevators.find(kAreas[elevator.area]);
    if (area == layout.elevators.end())
      return false;
    const auto entry = area->second.find(elevator.name);
    if (entry == area->second.end())
      return false;
    const Elevator* target = FindElevator(entry->second);
    if (target == nullptr)
      return false;
    // The room's scan keeps the name's two lines; the messages have it on one.
    std::string name = target->shown;
    if (kind != 0)
      std::replace(name.begin(), name.end(), '\n', ' ');
    if (kind == 0)
      out = {"Transport to " + name};
    else if (kind == 1)
      out = {"Access to &main-color=#FF3333;" + name +
             " &main-color=#89D6FF;granted. Please step into the hologram."};
    else
      out = {"Transport to &main-color=#FF3333;" + name + "&main-color=#89D6FF; active."};
    return true;
  }
  return false;
}

bool SkipsRidley(const Layout& layout) { return layout.finalBosses >= 2; }

std::vector< uint8_t > TempleOps(const Layout& layout) {
  std::vector< uint8_t > ops;
  auto u16 = [&](uint32_t v) {
    ops.push_back(uint8_t(v >> 8));
    ops.push_back(uint8_t(v));
  };
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
  // One run of bytes at `off` in the properties after the object's name.
  auto edit = [&](uint32_t id, uint32_t off, std::initializer_list< uint8_t > data) {
    ops.push_back(2);
    u32(id);
    u16(0);
    u16(1);
    u16(off);
    u16(static_cast< uint32_t >(data.size()));
    ops.insert(ops.end(), data);
  };
  enum { kZero = 9 };
  enum { kActivate = 1, kDeactivate = 4, kDecrement = 5, kResetAndStart = 11, kSetToZero = 13 };

  if (layout.requiredArtifacts != 12) {
    const uint32_t kCounter = 0x0410011F;  // Counter - Monoliths left to Activate
    const uint32_t kComplete = 0x0410057B; // Relay Monoliths Complete
    // The counter stops at zero instead of starting over, since more stones
    // than it counts may light up.
    if (layout.requiredArtifacts == 0)
      edit(kComplete, 0, {1});
    else
      edit(kCounter, 0, {0, 0, 0, uint8_t(layout.requiredArtifacts)});
    edit(kCounter, 8, {0});
    // The stones of the artifacts that weren't needed light up as well.
    for (uint32_t relay : {0x0010001Fu, 0x0010007Eu, 0x00100032u, 0x0010006Bu, 0x00100045u,
                           0x00100058u, 0x001000DDu, 0x001000CAu, 0x001000F0u, 0x001000B7u,
                           0x00100091u, 0x001000A4u})
      conn(4, kComplete, kZero, kSetToZero, relay);
  }

  if (SkipsRidley(layout)) {
    const uint32_t kPortalTimer = 0x3410039A; // Timer- center teleport
    const uint32_t kTotemCine = 0x341004DA;   // Relay - start totem cine
    const uint32_t kAfterIntro = 0x38100213;  // !Relay End of Ridley Intro Cinematic
    edit(kPortalTimer, 0, {0x3D, 0xCC, 0xCC, 0xCD}); // 0.1 s
    // The totem cinematic goes straight to what follows Ridley's intro...
    conn(3, kTotemCine, kZero, kSetToZero, 0x341001CA);
    conn(4, kTotemCine, kZero, kSetToZero, kAfterIntro);
    // ...which leaves the temple as the fight would: portal open, the fight's
    // layers off, the totem and the stones gone.
    conn(3, kAfterIntro, kZero, 20 /* Play */, 0x001002DB);
    conn(4, kAfterIntro, kZero, kActivate, 0x001002D2);
    conn(4, kAfterIntro, kZero, kSetToZero, 0x38100541);
    for (uint32_t layerOff : {0x04100482u, 0x04100581u, 0x04100309u})
      conn(4, kAfterIntro, kZero, kDecrement, layerOff);
    conn(4, kAfterIntro, kZero, kResetAndStart, kPortalTimer);
    for (uint32_t part : {0x341001C5u, 0x341001C7u, 0x341001C8u})
      conn(4, kAfterIntro, kZero, kDeactivate, part);
    for (uint32_t i = 0; i < 12; ++i) {
      conn(4, kAfterIntro, kZero, kDeactivate, 0x0410000E + i * 0x13); // hint stone
      conn(4, kAfterIntro, kZero, kDeactivate, 0x04100170 + i);        // hologram
      conn(4, kAfterIntro, kZero, kDeactivate, 0x0410001C + i * 0x13); // blue lines
    }
  }
  return ops;
}

namespace {

const uint32_t kChozoWorld = 0x83F6FF6F;
const uint32_t kHiveTotem = 0xC8309DF6;
const uint32_t kSunchamberLobby = 0x18AB6106;      // 08_courtyard
const uint32_t kProcessingCenterAccess = 0xED6DE73B;
const uint32_t kEliteQuartersAccess = 0x71343C3F;
const uint32_t kMetroidQuarantineA = 0xFB051F5A;
const uint32_t kMetroidQuarantineB = 0xBB3AFC4E;
const uint32_t kEliteControl = 0xC50AF17A;
const uint32_t kCentralDynamo = 0xFEA372E2;
const uint32_t kMinesWorld = 0xB1AC4D65;
const uint32_t kEliteResearch = 0x8A97BB54;
const uint32_t kMainPlaza = 0xD5CDB809;
const uint32_t kMainVentilationShaftB = 0xAFD4E038; // 08b_under_intro_ventshaft
const uint32_t kResearchLabHydra = 0x43E4CC25;      // 10_ice_research_a
const uint32_t kMainQuarry = 0x643D038F;            // 01_mines_mainplaza
const uint32_t kSunTower = 0xDE161372;              // 0v_connect_tunnel
const uint32_t kSunchamber = 0x9A0A03EB;            // 22_Flaahgra
const uint32_t kResearchCore = 0xA49B2544;          // 13_ice_vault
const uint32_t kResearchLabAether = 0x21B4BFF6;     // 12_ice_research_b
const uint32_t kObservatory = 0x3FB4A34E;           // 11_ice_observatory
const uint32_t kMinesSecurityStation = 0x956F1552;  // 02_mines_shotemup
const uint32_t kGravityChamber = 0x49175472;        // 18_ice_gravity_chamber
const uint32_t kEliteQuarters = 0x3953C353;         // 12_mines_eliteboss

void LogicOps(uint32_t mrea, const std::vector< PortSkipCutscenes::ScriptObject >& objects,
              std::vector< uint8_t >& ops);

} // namespace

void FillLogic(const Layout& layout, PortApLogic::Options& options) {
  // The lock a door is left with under each blast shield, as in Doors().
  static const char* const kUnderShield[] = {"Bomb",       "Blue", "Plasma Beam",     "Ice Beam",
                                             "Wave Beam",  "Blue", "Power Beam Only", "Blue",
                                             "Disabled",   "Blue"};
  const int kDisabled = NameIndex(kShields, "Disabled");
  const int kNoShield = NameIndex(kShields, "None");
  options.startRoom = layout.startRoom;
  options.removeHiveMecha = layout.removeHiveMecha;
  options.backwardsLowerMines = layout.backwardsLowerMines;
  options.elevators = layout.elevators;
  options.doorColors.clear();
  if (layout.hasDoorColors)
    options.doorColors = layout.doorColors;
  options.doors.clear();
  const Resolved resolved = Resolve(layout);
  for (int i = 0; i < kDoorCount; ++i) {
    const Door& d = kDoors[i];
    if (d.dest < 0)
      continue;
    const Room& room = kRooms[d.room];
    PortApLogic::Options::Door door;
    door.lock = kLocks[resolved.lock[i] >= 0 ? resolved.lock[i] : d.defaultLock];
    if ((resolved.lock[i] < 0 || resolved.lock[i] == d.defaultLock) && layout.hasDoorColors) {
      const auto area = layout.doorColors.find(kAreas[room.area]);
      if (area != layout.doorColors.end()) {
        const auto entry = area->second.find(kLocks[d.defaultLock]);
        if (entry != area->second.end())
          door.lock = entry->second;
      }
    }
    if (resolved.shield[i] >= 0) {
      door.lock = kUnderShield[resolved.shield[i]];
      if (resolved.shield[i] != kDisabled && resolved.shield[i] != kNoShield)
        door.shield = kShields[resolved.shield[i]];
    }
    options.doors[std::string(kAreas[room.area]) + '|' + room.name + '|' + kRooms[d.dest].name] = door;
  }
}

std::vector< LayerChange > Layers(const Layout& layout) {
  std::vector< LayerChange > out;
  if (layout.removeHiveMecha) {
    LayerChange firstPass; // "1st pass": the Hive Mecha and its wasps
    firstPass.mlvl = kChozoWorld;
    firstPass.mrea = kHiveTotem;
    firstPass.area = 0x24;
    firstPass.layer = 1;
    firstPass.active = false;
    out.push_back(firstPass);
  }
  // The Phazon Elite is there without Central Dynamo's power having been
  // restored (randomprime's phazonEliteWithoutDynamo, which the apworld's
  // logic counts on). The dummy's layer is on until this has been done, and
  // nothing else switches it, so the elite's own layer is left to the game
  // afterwards: beating it turns it off for good.
  LayerChange elite;
  elite.mlvl = kMinesWorld;
  elite.mrea = kEliteResearch;
  elite.area = 0x0D;
  elite.layer = 1; // "3rd pass elite bustout"
  elite.active = true;
  elite.whileLayer = 5;
  out.push_back(elite);
  elite.layer = 5; // the dummy elite
  elite.active = false;
  elite.whileLayer = -1;
  out.push_back(elite);
  return out;
}

std::vector< uint8_t > RoomOps(const Layout& layout, uint32_t mrea,
                               const std::vector< PortSkipCutscenes::ScriptObject >& objects) {
  std::vector< uint8_t > ops;
  auto u16 = [&](uint32_t v) {
    ops.push_back(uint8_t(v >> 8));
    ops.push_back(uint8_t(v));
  };
  auto u32 = [&](uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8)
      ops.push_back(uint8_t(v >> shift));
  };
  // The object, when it is of that type and has `size` bytes of properties
  // after its name.
  auto has = [&](uint32_t id, uint8_t type, size_t size) {
    for (const PortSkipCutscenes::ScriptObject& o : objects) {
      if (o.id != id)
        continue;
      const size_t start = o.props.size() > 4
                               ? static_cast< size_t >(std::find(o.props.begin() + 4, o.props.end(), 0) -
                                                       o.props.begin()) + 1
                               : 0;
      return o.type == type && start != 0 && o.props.size() >= start + size;
    }
    return false;
  };
  auto edit = [&](uint32_t id, uint32_t off, std::initializer_list< uint8_t > data) {
    ops.push_back(2);
    u32(id);
    u16(0);
    u16(1);
    u16(off);
    u16(static_cast< uint32_t >(data.size()));
    ops.insert(ops.end(), data);
  };
  auto remove = [&](uint32_t id) {
    ops.push_back(6);
    u32(id);
  };
  enum { kActor = 0x00, kTrigger = 0x04, kTimer = 0x05, kPlatform = 0x08, kRelay = 0x15,
         kDamageableTrigger = 0x1A };
  enum { kEntered = 3, kZero = 9 };
  enum { kSetToZero = 13 };
  // An actor's visor parameters: whether a lock-on passes through it.
  const uint32_t kActorPassthrough = 325;
  const size_t kActorSize = 354;

  if (layout.removeHiveMecha && mrea == kHiveTotem) {
    const uint32_t kVisited = 0x0024008C; // Relay - Make Room Already Visited
    const uint32_t kStart = 0x00246FF0;   // below the ids DoorOps hands out
    if (has(kVisited, kRelay, 1) && !has(kStart, kTimer, 0)) {
      // A timer that fires the relay as the room loads: the room is as the
      // fight leaves it.
      static const char kName[] = "Auto start relay";
      ops.push_back(5);
      ops.push_back(0); // layer
      ops.push_back(kTimer);
      u32(kStart);
      u16(1);
      u32(kZero);
      u32(kSetToZero);
      u32(kVisited);
      u32(4 + sizeof(kName) + 11);
      u32(6);
      ops.insert(ops.end(), kName, kName + sizeof(kName));
      for (uint8_t b : {0x3A, 0x83, 0x12, 0x6F}) // 0.001 s
        ops.push_back(b);
      u32(0); // no random time
      ops.push_back(0); // looping
      ops.push_back(1); // starts at once
      ops.push_back(1); // active
    }
  }

  if (layout.flaahgraPowerBombs && mrea == kSunchamberLobby) {
    // The sandstone's vulnerability to power bombs: reflect -> normal.
    const uint32_t kSandstone = 0x001300D7;
    if (has(kSandstone, kDamageableTrigger, 170))
      edit(kSandstone, 56, {0, 0, 0, 1});
  }

  if (layout.backwardsLowerMines) {
    // The pins and the ice that close the way from the far side are platforms.
    if (mrea == kProcessingCenterAccess || mrea == kEliteQuartersAccess) {
      for (const PortSkipCutscenes::ScriptObject& o : objects) {
        if (o.type == kPlatform)
          remove(o.id);
      }
    }
    // The force fields can be shot through from behind.
    if (mrea == kMetroidQuarantineB && has(0x081F0018, kActor, kActorSize))
      edit(0x081F0018, kActorPassthrough, {1});
    if (mrea == kEliteControl && has(0x04100086, kActor, kActorSize))
      edit(0x04100086, kActorPassthrough, {1});
    // Walking in from the far side runs what the near side's entrance does.
    if (mrea == kMetroidQuarantineA && has(0x00200214, kTrigger, 63) &&
        has(0x00200464, kRelay, 1)) {
      ops.push_back(4);
      u32(0x00200214);
      u32(kEntered);
      u32(kSetToZero);
      u32(0x00200464);
    }
    // The block behind Central Dynamo's door.
    if (mrea == kCentralDynamo && has(0x001B065F, kActor, kActorSize))
      remove(0x001B065F);
  }
  LogicOps(mrea, objects, ops);
  return ops;
}

std::vector< DoorChange > Doors(const Layout& layout, uint32_t mrea, const uint32_t* broken) {
  std::vector< DoorChange > out;
  if (!layout.hasDoorColors && !layout.hasShields)
    return out;
  // DoorShieldFromBlastShieldType: the door a blast shield sits on, per kShields.
  static const char* const kUnderShield[] = {"Bomb",       "Blue", "Plasma Beam",     "Ice Beam",
                                             "Wave Beam",  "Blue", "Power Beam Only", "Blue",
                                             "Disabled",   "Blue"};
  const int kDisabled = NameIndex(kShields, "Disabled");
  const int kNoShield = NameIndex(kShields, "None");
  const Resolved resolved = Resolve(layout);
  auto shielded = [&](int door) {
    return resolved.shield[door] >= 0 && resolved.shield[door] != kDisabled &&
           resolved.shield[door] != kNoShield;
  };
  // The door that stands for both sides of a shielded doorway.
  auto owner = [&](int door) {
    const int pair = PairedDoor(door);
    return pair >= 0 && pair < door && shielded(pair) ? pair : door;
  };
  for (int i = 0; i < kDoorCount; ++i) {
    const Door& d = kDoors[i];
    const Room& room = kRooms[d.room];
    if (room.mrea != mrea || d.doorId == 0)
      continue;
    DoorChange change;
    if (resolved.lock[i] >= 0 && resolved.lock[i] != d.defaultLock) {
      change.type = kLocks[resolved.lock[i]];
    } else if (layout.hasDoorColors) {
      const auto area = layout.doorColors.find(kAreas[room.area]);
      if (area != layout.doorColors.end()) {
        const auto entry = area->second.find(kLocks[d.defaultLock]);
        if (entry != area->second.end())
          change.type = entry->second;
      }
    }
    if (resolved.shield[i] >= 0) {
      change.type = kUnderShield[resolved.shield[i]];
      if (resolved.shield[i] != kDisabled)
        change.shield = kShields[resolved.shield[i]];
      change.replacesShield = change.shield != "Missile";
      if (shielded(i)) {
        const int first = owner(i);
        change.shieldBit = 0;
        for (int j = 0; j < first; ++j)
          if (shielded(j) && owner(j) == j)
            ++change.shieldBit;
        if (broken != nullptr && change.shieldBit < kShieldBits &&
            ((broken[change.shieldBit / 32] >> (change.shieldBit % 32)) & 1) != 0) {
          change.shield.clear();
          if (change.type == "Bomb" || change.type == "Power Beam Only")
            change.type = "Blue";
        }
      }
    }
    if (change.type.empty() && change.shield.empty())
      continue;
    change.index = i;
    change.pair = PairedDoor(i);
    change.mrea = mrea;
    change.dock = d.dock;
    change.vertical = (d.flags & kDoorVertical) != 0;
    change.doorId = d.doorId;
    for (int k = 0; k < 3; ++k)
      change.rotation[k] = d.rotation[k];
    for (int k = 0; k < 2; ++k) {
      change.forces[k] = d.forces[k];
      change.shieldActors[k] = d.shields[k];
    }
    out.push_back(change);
  }
  return out;
}

namespace {

enum { kNormal = 1, kReflect = 2, kImmune = 3 };

// A CDamageVulnerability as a script stores it. randomprime's door_meta.rs
// tables: `name` is a door type (kLocks) or a blast shield type (kShields).
std::vector< uint8_t > Vulnerability(const std::string& name) {
  // 15 weapons (power, ice, wave, plasma, bomb, power bomb, missile, boost
  // ball, phazon, four enemy weapons, two unknown), then the charged beams
  // and the beam combos (power, ice, wave, plasma).
  uint8_t weapon[15] = {kReflect, kReflect, kReflect, kReflect, kImmune,
                        kReflect, kReflect, kImmune,  kImmune,  kImmune,
                        kImmune,  kImmune,  kImmune,  kReflect, kReflect};
  uint8_t charged[5] = {kReflect, kReflect, kReflect, kReflect, kNormal};
  uint8_t combo[5] = {kReflect, kReflect, kReflect, kReflect, kNormal};
  auto all = [&](uint8_t v) {
    for (int i = 0; i < 13; ++i)
      weapon[i] = v;
    for (int i = 0; i < 5; ++i)
      charged[i] = combo[i] = v;
  };
  auto beam = [&](int i) { weapon[i] = charged[i] = combo[i] = kNormal; };
  if (name == "Blue") {
    all(kNormal);
    weapon[7] = kReflect;
    for (int i = 9; i < 13; ++i)
      weapon[i] = kImmune;
  } else if (name == "Disabled") {
    all(kImmune);
  } else if (name == "Power Beam Only") {
    beam(0);
  } else if (name == "Ice Beam") {
    beam(1);
  } else if (name == "Wave Beam") {
    beam(2);
  } else if (name == "Plasma Beam") {
    beam(3);
  } else if (name == "Bomb") {
    weapon[4] = kNormal;
  } else if (name == "Power Bomb") {
    weapon[5] = kNormal;
  } else if (name == "Missile") {
    weapon[6] = kNormal;
    for (int i = 0; i < 4; ++i)
      combo[i] = kNormal;
  } else if (name == "Charge Beam") {
    for (int i = 0; i < 4; ++i)
      charged[i] = kNormal;
  } else if (name == "Super Missile") {
    combo[0] = kNormal;
  } else if (name == "Ice Spreader") {
    combo[1] = kNormal;
  } else if (name == "Wavebuster") {
    combo[2] = kNormal;
  } else if (name == "Flamethrower") {
    combo[3] = kNormal;
  }
  std::vector< uint8_t > out;
  auto u32 = [&](uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8)
      out.push_back(uint8_t(v >> shift));
  };
  u32(18);
  for (uint8_t v : weapon)
    u32(v);
  u32(kNormal);
  u32(5);
  for (uint8_t v : charged)
    u32(v);
  u32(5);
  for (uint8_t v : combo)
    u32(v);
  return out;
}

constexpr uint32_t kDoorPattern = 0x544A9892; // testb.TXTR

struct DoorType {
  const char* name;
  uint32_t cmdl, cmdlVertical;
  uint32_t pattern0, pattern1, color;
  int mapType;      // CMappableObject::EMappableObjectType
  const char* scan; // what opens it, for types the game doesn't have
};

const DoorType kDoorTypes[] = {
    {"Blue", 0x0734977A, 0x18D0AEE6, kDoorPattern, kDoorPattern, 0x8A7F3683, 0, nullptr},
    {"Wave Beam", 0x33188D1B, 0x095B0B93, kDoorPattern, kDoorPattern, 0xF68DF7F1, 3, nullptr},
    {"Ice Beam", 0x59649E9D, 0xB7A8A4C9, kDoorPattern, kDoorPattern, 0xBE4CD99D, 2, nullptr},
    {"Plasma Beam", 0xBBBA1EC7, PortCustomRes::kDoorPlasmaVerticalCmdl, kDoorPattern, kDoorPattern,
     0xFC095F6C, 4, nullptr},
    {"Missile", PortCustomRes::kDoorMissileCmdl, PortCustomRes::kDoorMissileCmdl + 1, kDoorPattern,
     kDoorPattern, 0x8344BEC8, 1,
     "This door will open with &push;&main-color=#D91818;Missiles&pop;."},
    {"Power Beam Only", PortCustomRes::kDoorPowerCmdl, PortCustomRes::kDoorPowerCmdl + 1,
     kDoorPattern, kDoorPattern, 0x1D588B22, 0,
     "This door will only open with &push;&main-color=#D91818;Power Beam&pop;."},
    {"Bomb", PortCustomRes::kDoorBombCmdl, PortCustomRes::kDoorBombCmdl + 1,
     PortCustomRes::kDoorBombPatternTxtr, 0xCFA9DFF3, PortCustomRes::kDoorBombColorTxtr, 0,
     "This door will open with &push;&main-color=#D91818;Morph Ball Bombs&pop;."},
    {"Disabled", PortCustomRes::kDoorDisabledCmdl, PortCustomRes::kDoorDisabledCmdl + 1,
     kDoorPattern, kDoorPattern, 0x717AABCE, 1, "This door cannot be opened."},
};

const DoorType* FindDoorType(const std::string& name) {
  for (const DoorType& type : kDoorTypes)
    if (name == type.name)
      return &type;
  return nullptr;
}

uint32_t Read32(const std::vector< uint8_t >& data, size_t at) {
  return uint32_t(data[at]) << 24 | uint32_t(data[at + 1]) << 16 | uint32_t(data[at + 2]) << 8 |
         data[at + 3];
}

float ReadFloat(const std::vector< uint8_t >& data, size_t at) {
  const uint32_t bits = Read32(data, at);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// Where an object's properties start, after its name; 0 when it has none.
size_t NameEnd(const std::vector< uint8_t >& props) {
  for (size_t i = 4; i < props.size(); ++i)
    if (props[i] == 0)
      return i + 1;
  return 0;
}

// Script object types, and where things are in their properties (after the
// name): a DamageableTrigger's weaknesses and its three textures, an Actor's
// model, and in a Door the animation set, the light parameters' count, the
// scan and, from the end, the orbit position.
enum {
  kActorType = 0x00,
  kDoorType = 0x03,
  kTriggerType = 0x04,
  kTimerType = 0x05,
  kSoundType = 0x09,
  kRelayType = 0x15,
  kDamageableTriggerType = 0x1A,
  kPointOfInterestType = 0x42,
  kStreamedAudioType = 0x61,
  kCameraShakerType = 0x89,
};
// Script states and messages.
enum { kClosedState = 2, kMaxReachedState = 7, kZeroState = 9, kDeadState = 14 };
enum {
  kActivateMsg = 1,
  kDeactivateMsg = 4,
  kDecrementMsg = 5,
  kIncrementMsg = 7,
  kOpenMsg = 9,
  kSetToZeroMsg = 13,
  kActionMsg = 19,
  kPlayMsg = 20,
};

constexpr uint32_t kMissileShieldCmdl = 0xEFDFFB8C;
constexpr uint32_t kHatchAnimSet = 0xF57DD484;

struct ShieldType {
  const char* name;
  uint32_t cmdl;
  bool canOrbit;
  const char* scan;
};

#define PORT_AP_RED(text) "&push;&main-color=#D91818;" text "&pop;"
#define PORT_AP_ADVANCED(what)                                                                     \
  "There is an Advanced Blast Shield on the door blocking access. Analysis indicates that the "    \
  "Blast Shield is reinforced with " PORT_AP_RED(what) ", rendering it invulnerable to most "      \
  "weapons."
#define PORT_AP_ELEMENTAL(how)                                                                     \
  "There is an Elemental Blast Shield on the door blocking access. Analysis indicates that the "   \
  "Blast Shield is invulnerable to standard Beam fire. " how " may damage it."
// randomprime's scan texts (door_meta.rs); the missile shield's is the port's.
const ShieldType kShieldTypes[] = {
    {"Missile", kMissileShieldCmdl, true,
     "There is a Blast Shield on the door blocking access. Analysis indicates that the Blast "
     "Shield is invulnerable to most weapons. " "A " PORT_AP_RED("Missile") " may damage it."},
    {"Bomb", PortCustomRes::ShieldCmdl(PortCustomRes::kShieldBomb), false,
     "There is a Blast Shield on the door blocking access. Analysis indicates that the Blast "
     "Shield is reinforced with " PORT_AP_RED("Sandstone") ", rendering it invulnerable to most "
     "weapons."},
    {"Charge Beam", PortCustomRes::ShieldCmdl(PortCustomRes::kShieldCharge), true,
     "This Blast Shield can be destroyed with a " PORT_AP_RED("Concussive Blast") "."},
    {"Flamethrower", PortCustomRes::ShieldCmdl(PortCustomRes::kShieldFlamethrower), true,
     PORT_AP_ELEMENTAL("Continuous exposure to " PORT_AP_RED("Extreme Heat"))},
    {"Ice Spreader", PortCustomRes::ShieldCmdl(PortCustomRes::kShieldIceSpreader), true,
     PORT_AP_ELEMENTAL("A concussive blast augmented with " PORT_AP_RED("Extreme Cold"))},
    {"Wavebuster", PortCustomRes::ShieldCmdl(PortCustomRes::kShieldWavebuster), true,
     PORT_AP_ELEMENTAL("Continuous exposure to " PORT_AP_RED("Extreme Amperage"))},
    {"Power Bomb", PortCustomRes::ShieldCmdl(PortCustomRes::kShieldPowerBomb), false,
     PORT_AP_ADVANCED("Bendezium")},
    {"Super Missile", PortCustomRes::ShieldCmdl(PortCustomRes::kShieldSuperMissile), true,
     PORT_AP_ADVANCED("Cordite")},
};
#undef PORT_AP_ADVANCED
#undef PORT_AP_ELEMENTAL
#undef PORT_AP_RED

const ShieldType* FindShieldType(const std::string& name) {
  for (const ShieldType& type : kShieldTypes)
    if (name == type.name)
      return &type;
  return nullptr;
}

// A script object's properties, as the loaders read them.
struct Props {
  std::vector< uint8_t > data;
  Props(uint32_t count, const char* name) {
    U32(count);
    Str(name);
  }
  Props& U32(uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8)
      data.push_back(uint8_t(v >> shift));
    return *this;
  }
  Props& F(float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    return U32(bits);
  }
  Props& Vec(float x, float y, float z) { return F(x).F(y).F(z); }
  Props& Vec(const float* v) { return Vec(v[0], v[1], v[2]); }
  Props& B(bool v) {
    data.push_back(v ? 1 : 0);
    return *this;
  }
  Props& Str(const char* text) {
    data.insert(data.end(), text, text + std::strlen(text) + 1);
    return *this;
  }
  Props& Raw(const std::vector< uint8_t >& bytes) {
    data.insert(data.end(), bytes.begin(), bytes.end());
    return *this;
  }
  Props& Health() { return U32(2).F(1.f).F(1.f); }
  Props& Visor() { return U32(3).B(false).B(true).U32(15); }
};

// Where a blast shield goes on a door, from the position of the door's own
// shield actor (randomprime's measured offsets). False for a door that
// faces no way it knows.
struct ShieldPlace {
  float position[3];
  float rotation[3];
  float scale[3];
  float trigger[3];
  float triggerSize[3];
};

bool PlaceShield(const DoorChange& door, const float* at, const float* actorRotation,
                 ShieldPlace& out) {
  auto set = [](float* v, float x, float y, float z) {
    v[0] = x;
    v[1] = y;
    v[2] = z;
  };
  const float* r = door.rotation;
  float offset[3];
  if (door.vertical) {
    set(out.scale, 1.1776f, 1.8f, 1.8f);
    set(out.triggerSize, 5.f, 5.f, 0.875f);
    if (r[0] > -90.f && r[0] < 90.f) { // in a ceiling
      set(offset, 0.016708f, -2.141243f, 0.40522f);
      set(out.rotation, 0.f, -90.f, -90.f);
    } else if (r[0] < -90.f && r[0] > -270.f) { // in a floor
      set(offset, -0.0112f, -2.140015f, -0.371151f);
      set(out.rotation, -90.f, 90.f, 0.f);
    } else {
      return false;
    }
    for (int k = 0; k < 3; ++k)
      out.position[k] = at[k] + offset[k];
    set(out.trigger, out.position[0], out.position[1] + 2.f,
        out.position[2] + (out.rotation[0] == 0.f ? -1.f : 1.f));
    return true;
  }

  set(out.scale, 1.f, 1.5f, 1.5f);
  set(out.rotation, r[0], r[1], r[2]);
  const bool west = (r[2] >= 135.f && r[2] < 225.f) || (r[2] < -135.f && r[2] > -225.f);
  // The tilted doors of Biotech Research Area 1 and the Hive Totem first.
  if (r[0] >= 11.f && r[0] < 13.f)
    set(offset, 0.374077f, -0.406525f, -1.762893f);
  else if (r[0] >= -13.f && r[0] < -11.f)
    set(offset, 0.374184f, 0.392502f, -1.763191f);
  else if (r[1] >= 8.f && r[1] < 9.f) {
    set(offset, -0.00595f, 0.383209f, -1.801748f);
    set(out.rotation, actorRotation[0], actorRotation[1], actorRotation[2]);
  } else if (r[0] >= 8.f && r[0] < 9.f)
    set(offset, -0.406285f, -0.27829f, -1.780129f);
  else if (r[0] >= -9.f && r[0] < -7.f)
    set(offset, 0.392498f, -0.27829f, -1.780126f);
  else if (r[2] >= 45.f && r[2] < 135.f)
    set(offset, -0.00595f, 0.383209f, -1.801748f);
  else if (west)
    set(offset, -0.383225f, 0.f, -1.80175f);
  else if (r[2] >= -135.f && r[2] < -45.f)
    set(offset, -0.00769f, -0.383224f, -1.801752f);
  else if (r[2] >= -45.f && r[2] < 45.f)
    set(offset, 0.392517f, 0.f, -1.801746f);
  else
    return false;
  for (int k = 0; k < 3; ++k)
    out.position[k] = at[k] + offset[k];

  // The trigger stands a unit in front of the shield, and two up.
  float step[3] = {0.f, 0.f, 2.f};
  bool alongY = true;
  if (r[0] >= -15.f && r[0] < -10.f)
    set(step, -0.35f, -1.f, 2.f);
  else if (r[0] >= 10.f && r[0] < 15.f)
    set(step, -0.35f, 1.f, 2.f);
  else if (r[0] >= 8.f && r[0] < 9.f) {
    set(step, 1.f, 0.35f, 2.f);
    alongY = false;
  } else if (r[0] >= -9.f && r[0] < -7.f) {
    set(step, -1.f, 0.35f, 2.f);
    alongY = false;
  } else if (r[2] >= 45.f && r[2] < 135.f)
    step[1] = -1.f;
  else if (west) {
    step[0] = 1.f;
    alongY = false;
  } else if (r[2] >= -135.f && r[2] < -45.f)
    step[1] = 1.f;
  else {
    step[0] = -1.f;
    alongY = false;
  }
  for (int k = 0; k < 3; ++k)
    out.trigger[k] = out.position[k] + step[k];
  if (alongY)
    set(out.triggerSize, 5.f, 0.875f, 4.f);
  else
    set(out.triggerSize, 0.875f, 5.f, 4.f);
  return true;
}
enum {
  kForceVulnerability = 36,
  kForceTextures = 156,
  kForceSize = 170,
  kActorModel = 196,
  kDoorRotation = 12,
  kDoorAnimSet = 36,
  kDoorLightCount = 52,
  kDoorScanCount = 123,
  kDoorScan = 127,
  kDoorTail13 = 43, // orbit position to the end, for 13 properties
};

// The changes every Archipelago seed has, because the apworld's logic counts
// on them (its Config.py: mainPlazaDoor, backwardsFrigate, backwardsLabs,
// backwardsUpperMines, phazonEliteWithoutDynamo).
void LogicOps(uint32_t mrea, const std::vector< PortSkipCutscenes::ScriptObject >& objects,
              std::vector< uint8_t >& ops) {
  auto u16 = [&](uint32_t v) {
    ops.push_back(uint8_t(v >> 8));
    ops.push_back(uint8_t(v));
  };
  auto u32 = [&](uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8)
      ops.push_back(uint8_t(v >> shift));
  };
  auto find = [&](uint32_t id, uint8_t type,
                  size_t size) -> const PortSkipCutscenes::ScriptObject* {
    for (const PortSkipCutscenes::ScriptObject& o : objects) {
      if (o.id != id)
        continue;
      const size_t start = NameEnd(o.props);
      return o.type == type && start != 0 && o.props.size() >= start + size ? &o : nullptr;
    }
    return nullptr;
  };
  auto exists = [&](uint32_t id) {
    for (const PortSkipCutscenes::ScriptObject& o : objects)
      if (o.id == id)
        return true;
    return false;
  };
  struct Conn {
    uint32_t state, msg, target;
  };
  auto push = [&](uint8_t layer, uint8_t type, uint32_t id, const std::vector< Conn >& conns,
                  const std::vector< uint8_t >& props) {
    ops.push_back(5);
    ops.push_back(layer);
    ops.push_back(type);
    u32(id);
    u16(static_cast< uint32_t >(conns.size()));
    for (const Conn& c : conns) {
      u32(c.state);
      u32(c.msg);
      u32(c.target);
    }
    u32(static_cast< uint32_t >(props.size()));
    ops.insert(ops.end(), props.begin(), props.end());
  };
  auto connect = [&](uint32_t sender, const Conn& c) {
    ops.push_back(4);
    u32(sender);
    u32(c.state);
    u32(c.msg);
    u32(c.target);
  };
  auto edit = [&](uint32_t id, uint32_t off, std::initializer_list< uint8_t > data) {
    ops.push_back(2);
    u32(id);
    u16(0);
    u16(1);
    u16(off);
    u16(static_cast< uint32_t >(data.size()));
    ops.insert(ops.end(), data);
  };
  auto trigger = [](const char* name, float x, float y, float z, float sx, float sy, float sz,
                    bool active, bool offOnEnter) {
    Props props(9, name);
    props.Vec(x, y, z).Vec(sx, sy, sz).U32(4).U32(0).F(0.f).F(0.f).F(0.f).Vec(0.f, 0.f, 0.f);
    props.U32(1).B(active).B(offOnEnter).B(false); // detects the player
    return props.data;
  };
  enum { kSpecialFunctionType = 0x3A };
  enum { kEnteredState = 3, kInsideState = 6, kOpenState = 8, kReflectedState = 0x1F };
  enum { kCloseMsg = 3, kResetAndStartMsg = 11, kStartMsg = 14 };
  const uint32_t kActorPassthrough = 325;
  const size_t kActorSize = 354;

  if (mrea == kMainPlaza) {
    // The door to the Plaza Access ledge opens from the plaza too
    // (make_main_plaza_locked_door_two_ways): it gets the shield, triggers
    // and timer a plain door has, copied from the room's door to Ruined
    // Shrine Access and moved over.
    const uint32_t kDoor = 0x00020060, kScan = 0x000202F4, kScanSwitch = 0x000202B8,
                   kIneffective = 0x000202FD;
    const uint32_t kUnlock = 0x0002000F, kShield = 0x00020004, kOpen = 0x00020007,
                   kTimer = 0x00020008, kRelay = 0x00020010;
    const PortSkipCutscenes::ScriptObject* unlock = find(0x00020016, kDamageableTriggerType, 170);
    const PortSkipCutscenes::ScriptObject* open = find(0x00020017, kTriggerType, 63);
    const PortSkipCutscenes::ScriptObject* shield = find(0x00020018, kActorType, kActorSize);
    const PortSkipCutscenes::ScriptObject* timer = find(0x00020019, kTimerType, 11);
    bool free = true;
    for (uint32_t id : {kUnlock, kShield, kOpen, kTimer, kRelay})
      free = free && !exists(id);
    if (free && unlock != nullptr && open != nullptr && shield != nullptr && timer != nullptr &&
        find(kDoor, kDoorType, 217) != nullptr && find(kScan, kPointOfInterestType, 37) != nullptr &&
        find(kScanSwitch, kTriggerType, 63) != nullptr && find(kIneffective, kRelayType, 1) != nullptr) {
      auto moved = [](const PortSkipCutscenes::ScriptObject& from, float x, float y, float z) {
        std::vector< uint8_t > props = from.props;
        const std::vector< uint8_t > at = Props(0, "").Vec(x, y, z).data;
        std::copy(at.begin() + 5, at.end(), props.begin() + NameEnd(props));
        return props;
      };
      std::vector< uint8_t > props = moved(*unlock, 152.23212f, 86.45113f, 24.472418f);
      props[NameEnd(props) + 155] = 8; // seen from the other side
      push(0, kDamageableTriggerType, kUnlock,
           {{kReflectedState, kSetToZeroMsg, kIneffective},
            {kDeadState, kDeactivateMsg, kShield},
            {kMaxReachedState, kActivateMsg, kShield},
            {kDeadState, kActivateMsg, kOpen},
            {kDeadState, kSetToZeroMsg, kDoor}},
           props);
      push(0, kRelayType, kRelay,
           {{kZeroState, kActivateMsg, kShield}, {kZeroState, kActivateMsg, kUnlock}},
           Props(2, "Relay_Unlock").B(true).data);
      push(0, kTriggerType, kOpen,
           {{kInsideState, kOpenMsg, kDoor}, {kInsideState, kResetAndStartMsg, kTimer}},
           moved(*open, 147.6384f, 86.56792f, 24.701054f));
      push(0, kActorType, kShield, {}, moved(*shield, 151.95119f, 86.46258f, 24.503178f));
      push(0, kTimerType, kTimer,
           {{kZeroState, kCloseMsg, kDoor}, {kZeroState, kDeactivateMsg, kOpen}}, timer->props);
      // The "locked from this side" scan goes.
      edit(kScan, 24, {0, 0, 0, 0, 1, 0xFF, 0xFF, 0xFF, 0xFF});
      edit(kScanSwitch, 60, {0});
      // A plain door's looks, and shots stop at the shield, not the door.
      edit(kDoor, 36, {0x26, 0x88, 0x69, 0x45, 0, 0, 0, 0, 0, 0, 0, 2});
      edit(kDoor, 211, {0});
      for (const Conn& c : {Conn{kOpenState, kActivateMsg, kOpen},
                            Conn{kOpenState, kStartMsg, kTimer},
                            Conn{kClosedState, kDeactivateMsg, kOpen},
                            Conn{kOpenState, kDeactivateMsg, kUnlock},
                            Conn{kOpenState, kDeactivateMsg, kShield},
                            Conn{kClosedState, kSetToZeroMsg, kRelay},
                            Conn{kMaxReachedState, kDeactivateMsg, kShield},
                            Conn{kMaxReachedState, kDeactivateMsg, kUnlock}})
        connect(kDoor, c);
    }
  }

  // The crashed frigate from the far end: standing behind the door that has
  // no power switches it on.
  if (mrea == kMainVentilationShaftB && find(0x0015006F, kRelayType, 1) != nullptr &&
      !exists(0x00156FF0))
    push(0, kTriggerType, 0x00156FF0, {{kInsideState, kSetToZeroMsg, 0x0015006F}},
         trigger("Trigger_DoorOpen-component", 31.232622f, 442.69165f, -64.20529f, 6.f, 17.f, 6.f,
                 true, false));

  // The labs from the far end: the force field can be scanned through.
  if (mrea == kResearchLabHydra && find(0x0C190332, kActorType, kActorSize) != nullptr)
    edit(0x0C190332, kActorPassthrough, {1});

  // Main Quarry from the far end: walking up behind the barrier drops it.
  if (mrea == kMainQuarry && find(0x100201DA, kActorType, kActorSize) != nullptr &&
      find(0x000202B5, kSpecialFunctionType, 70) != nullptr && !exists(0x10026FF0))
    push(4, kTriggerType, 0x10026FF0,
         {{kEnteredState, kDeactivateMsg, 0x100201DA}, {kEnteredState, kDecrementMsg, 0x000202B5}},
         trigger("Trigger - Disable Main Quarry barrier", 82.412056f, 9.354454f, 2.807631f, 10.f,
                 5.f, 7.f, true, true));

  // Central Dynamo no longer decides whether the Phazon Elite is there.
  if (mrea == kCentralDynamo) {
    for (uint32_t id : {0x001B0525u, 0x001B0522u}) {
      if (find(id, kSpecialFunctionType, 70) != nullptr) {
        ops.push_back(6);
        u32(id);
      }
    }
  }

  // The softlocks an item shuffle opens up (randomprime's qolGameBreaking,
  // on in every seed). The crash fixes of that set are for the console's
  // memory, and the ones for later disc revisions don't apply to this one.
  auto has = [&](uint32_t id, uint8_t type) {
    for (const PortSkipCutscenes::ScriptObject& o : objects)
      if (o.id == id)
        return o.type == type;
    return false;
  };
  auto connected = [&](uint32_t id, const Conn& c) {
    for (const PortSkipCutscenes::ScriptObject& o : objects)
      if (o.id == id)
        for (const PortSkipCutscenes::ScriptObject::Connection& x : o.connections)
          if (x.state == c.state && x.message == c.msg && x.target == c.target)
            return true;
    return false;
  };
  auto link = [&](uint32_t sender, const Conn& c) {
    if (!connected(sender, c))
      connect(sender, c);
  };
  auto unlink = [&](uint32_t sender, const Conn& c) {
    if (!connected(sender, c))
      return;
    ops.push_back(3);
    u32(sender);
    u32(c.state);
    u32(c.msg);
    u32(c.target);
  };
  auto remove = [&](uint32_t id) {
    ops.push_back(6);
    u32(id);
  };
  auto real = [&](uint32_t id, uint32_t off, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    edit(id, off, {uint8_t(bits >> 24), uint8_t(bits >> 16), uint8_t(bits >> 8), uint8_t(bits)});
  };
  auto layerSwitch = [](const char* name, uint32_t area, uint32_t layer) {
    Props props(15, name);
    props.Vec(0.f, 0.f, 0.f).Vec(0.f, 0.f, 0.f).U32(16).Str("").F(0.f).F(0.f).F(0.f);
    props.U32(area).U32(layer).U32(0).B(true).F(0.f).U32(0xFFFFFFFF).U32(0xFFFFFFFF);
    props.U32(0xFFFFFFFF);
    return props.data;
  };
  enum { kPickupType = 0x11, kPirateType = 0x24 };
  enum { kArrivedState = 1, kDeathRattleState = 0x14 };
  const size_t kTriggerSize = 63;
  const uint32_t kTriggerScale = 12;

  // Sun Tower's trigger that sets the Sunchamber up for the ghosts would do it
  // before Flaahgra, taking her item away: it waits on a layer of its own,
  // which Flaahgra's death switches on.
  if (mrea == kSunTower && find(0x001D015B, kTriggerType, kTriggerSize) != nullptr &&
      find(0x001D015B, kTriggerType, kTriggerSize)->layer == 0) {
    for (const PortSkipCutscenes::ScriptObject& o : objects)
      if (o.layer == 1)
        remove(o.id);
    ops.push_back(7);
    ops.push_back(1);
    u32(0x001D015B);
  }
  if (mrea == kSunchamber && has(0x042500D4, kRelayType) && !exists(0x04256FF0)) {
    push(1, kSpecialFunctionType, 0x04256FF0, {},
         layerSwitch("Enable Sun Tower Layer Change Trigger", 0xCF4C7AA5, 1));
    connect(0x042500D4, {kZeroState, kIncrementMsg, 0x04256FF0});
  }

  // Research Lab Aether entered from below: the wall is gone once the labs
  // have gone dark, and the lower trigger breaks the glass itself instead of
  // waiting for the pirate to jump through it.
  if (mrea == kResearchCore && has(0x00280468, kRelayType) && !exists(0x00286FF0)) {
    push(0, kSpecialFunctionType, 0x00286FF0, {},
         layerSwitch("SpecialFunction - Remove Research Lab Aether wall", 0x354889CE, 3));
    connect(0x00280468, {kZeroState, kDecrementMsg, 0x00286FF0});
  }
  if (mrea == kResearchLabAether && has(0x04330219, kTriggerType) &&
      has(0x0433005D, kTimerType)) {
    link(0x04330219, {kEnteredState, kResetAndStartMsg, 0x0433005D});
    if (exists(0x1433007C))
      link(0x04330219, {kEnteredState, kDeactivateMsg, 0x1433007C});
  }

  // Observatory: the two pirates of the second pass count towards the panel,
  // and the door lock of the first pass also arms when the room is entered
  // from the save station or from the labs.
  if (mrea == kObservatory) {
    for (uint32_t pirate : {0x081E0460u, 0x081E0461u})
      if (has(pirate, kPirateType) && exists(0x001E02EA))
        link(pirate, {kDeathRattleState, kIncrementMsg, 0x001E02EA});
    const uint32_t lock = 0x041E0381, relay = 0x041E037A;
    const uint32_t south = 0x041E6FF0, north = 0x041E6FF1;
    if (has(lock, kTriggerType) && has(relay, kRelayType) && !exists(south) && !exists(north)) {
      connect(lock, {kEnteredState, kDeactivateMsg, south});
      connect(lock, {kEnteredState, kDeactivateMsg, north});
      push(1, kTriggerType, south,
           {{kEnteredState, kDeactivateMsg, lock},
            {kEnteredState, kDeactivateMsg, north},
            {kEnteredState, kSetToZeroMsg, relay}},
           trigger("Trigger", -71.30155f, -941.33795f, 129.97682f, 10.516006f, 6.079956f,
                   7.128998f, true, true));
      push(1, kTriggerType, north,
           {{kEnteredState, kDeactivateMsg, lock},
            {kEnteredState, kDeactivateMsg, south},
            {kEnteredState, kSetToZeroMsg, relay}},
           trigger("Trigger", -71.30155f, -853.69434f, 129.97682f, 10.516006f, 6.079956f,
                   7.128998f, true, true));
    }
  }

  // Triggers that could be walked around: the Security Station's alert and
  // the Hive Totem's falling platforms.
  if (mrea == kMinesSecurityStation && find(0x0407033F, kTriggerType, kTriggerSize) != nullptr) {
    real(0x0407033F, kTriggerScale, 50.f);
    real(0x0407033F, kTriggerScale + 4, 100.f);
    real(0x0407033F, kTriggerScale + 8, 40.f);
  }
  if (mrea == kHiveTotem && find(0x002400CA, kTriggerType, kTriggerSize) != nullptr)
    real(0x002400CA, kTriggerScale + 4, 60.f);

  // Gravity Chamber keeps its stalactite, and with it the grapple point.
  if (mrea == kGravityChamber && has(0x0035013A, kSpecialFunctionType))
    remove(0x0035013A);

  // Elite Research: the platforms wait out the elite's longest death.
  if (mrea == kEliteResearch && find(0x000D02F2, kTimerType, 11) != nullptr)
    real(0x000D02F2, 0, 5.f);

  // Elite Quarters: taking the item is what unlocks the room, not the Omega
  // Pirate's death.
  if (mrea == kEliteQuarters) {
    for (uint32_t pickup : {0x001A04B8u, 0x041A04C5u}) {
      if (!has(pickup, kPickupType))
        continue;
      if (exists(0x041A0348))
        link(pickup, {kArrivedState, kSetToZeroMsg, 0x041A0348});
      if (exists(0x001A03D9))
        link(pickup, {kArrivedState, kDecrementMsg, 0x001A03D9});
      if (exists(0x141A0328))
        link(pickup, {kArrivedState, kSetToZeroMsg, 0x141A0328});
    }
    if (has(0x001A04B8, kPickupType) || has(0x041A04C5, kPickupType)) {
      unlink(0x141A0126, {kDeadState, kSetToZeroMsg, 0x141A0328});
      unlink(0x141A0126, {kDeadState, kDecrementMsg, 0x001A03D9});
    }
  }
}

} // namespace

std::vector< uint8_t > DoorOps(const std::vector< DoorChange >& doors,
                               const std::vector< PortSkipCutscenes::ScriptObject >& objects,
                               const ScanMaker& scan, std::vector< PlacedShield >* placed) {
  std::vector< uint8_t > ops;
  auto u16 = [&](uint32_t v) {
    ops.push_back(uint8_t(v >> 8));
    ops.push_back(uint8_t(v));
  };
  auto u32 = [&](uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8)
      ops.push_back(uint8_t(v >> shift));
  };
  struct Run {
    uint32_t off;
    std::vector< uint8_t > data;
  };
  auto edit = [&](uint32_t id, const std::vector< Run >& runs) {
    ops.push_back(2);
    u32(id);
    u16(0);
    u16(static_cast< uint32_t >(runs.size()));
    for (const Run& run : runs) {
      u16(run.off);
      u16(static_cast< uint32_t >(run.data.size()));
      ops.insert(ops.end(), run.data.begin(), run.data.end());
    }
  };
  auto bytes = [](std::initializer_list< uint32_t > values) {
    std::vector< uint8_t > out;
    for (uint32_t v : values)
      for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(uint8_t(v >> shift));
    return out;
  };
  // The object, when it is of that type and has `size` bytes of properties.
  auto find = [&](uint32_t id, uint8_t type,
                  size_t size) -> const PortSkipCutscenes::ScriptObject* {
    for (const PortSkipCutscenes::ScriptObject& o : objects) {
      if (o.id != id)
        continue;
      const size_t start = NameEnd(o.props);
      return o.type == type && start != 0 && o.props.size() >= start + size ? &o : nullptr;
    }
    return nullptr;
  };

  struct Conn {
    uint32_t state, msg, target;
  };
  auto push = [&](uint8_t type, uint32_t id, const std::vector< Conn >& conns,
                  const Props& props) {
    ops.push_back(5);
    ops.push_back(0); // layer
    ops.push_back(type);
    u32(id);
    u16(static_cast< uint32_t >(conns.size()));
    for (const Conn& c : conns) {
      u32(c.state);
      u32(c.msg);
      u32(c.target);
    }
    u32(static_cast< uint32_t >(props.data.size()));
    ops.insert(ops.end(), props.data.begin(), props.data.end());
  };
  auto connect = [&](uint32_t sender, const Conn& c) {
    ops.push_back(4);
    u32(sender);
    u32(c.state);
    u32(c.msg);
    u32(c.target);
  };
  // New objects take instance numbers past the room's, and past the ones
  // other patches hand out.
  uint32_t nextInstance = 0x7000;
  for (const PortSkipCutscenes::ScriptObject& o : objects)
    nextInstance = std::max(nextInstance, (o.id & 0xFFFF) + 1);

  // The disc's missile shield on a door and its scan point (randomprime's
  // patch_remove_blast_shield: within 5 units of the doorway on every axis).
  const uint32_t kDiscShieldScan = 0x05F56F9D;
  auto discShield = [&](const PortSkipCutscenes::ScriptObject& doorObject,
                        std::vector< uint32_t >* ids) {
    const size_t doorStart = NameEnd(doorObject.props);
    bool found = false;
    for (const PortSkipCutscenes::ScriptObject& o : objects) {
      const size_t start = NameEnd(o.props);
      if (start == 0)
        continue;
      bool shield = false;
      if (o.type == kActorType && o.props.size() >= start + kActorModel + 4)
        shield = Read32(o.props, start + kActorModel) == kMissileShieldCmdl;
      else if (o.type == kPointOfInterestType && o.props.size() >= start + 33)
        shield = Read32(o.props, start + 29) == kDiscShieldScan;
      if (!shield)
        continue;
      bool close = true;
      for (int k = 0; k < 3; ++k) {
        const float d = ReadFloat(o.props, start + 4 * k) - ReadFloat(doorObject.props, doorStart + 4 * k);
        close = close && d > -5.f && d < 5.f;
      }
      if (!close)
        continue;
      found = found || o.type == kActorType;
      if (ids != nullptr)
        ids->push_back(o.id);
    }
    return found;
  };
  auto removeDiscShield = [&](const DoorChange& door) {
    const PortSkipCutscenes::ScriptObject* object = find(door.doorId, kDoorType, 12);
    std::vector< uint32_t > ids;
    if (object == nullptr || !discShield(*object, &ids))
      return;
    for (uint32_t id : ids) {
      ops.push_back(6);
      u32(id);
    }
  };

  // The shield over `door`, and everything that goes with it.
  auto addShield = [&](const DoorChange& door) {
    const ShieldType* shield = FindShieldType(door.shield);
    const PortSkipCutscenes::ScriptObject* object = find(door.doorId, kDoorType, kDoorAnimSet + 4);
    if (shield == nullptr || object == nullptr || door.doorId == 0x002C0186 ||
        nextInstance + 8 > 0xFFFF)
      return;
    if (Read32(object->props, NameEnd(object->props) + kDoorAnimSet) == kHatchAnimSet)
      return;
    // Main Plaza's door to the Plaza Access ledge has its shield elsewhere.
    const uint32_t anchorId =
        door.mrea == 0xD5CDB809 && door.dock == 4 ? 0x00020004 : door.shieldActors[0];
    const PortSkipCutscenes::ScriptObject* anchor = find(anchorId, kActorType, 24);
    if (anchor == nullptr)
      return;
    float at[3], actorRotation[3];
    const size_t anchorStart = NameEnd(anchor->props);
    for (int k = 0; k < 3; ++k) {
      at[k] = ReadFloat(anchor->props, anchorStart + 4 * k);
      actorRotation[k] = ReadFloat(anchor->props, anchorStart + 12 + 4 * k);
    }
    ShieldPlace place;
    if (!PlaceShield(door, at, actorRotation, place))
      return;
    // A missile shield where the disc has one: the disc's serves.
    if (!door.replacesShield && discShield(*object, nullptr))
      return;

    const uint32_t base = (door.doorId & 0x03FF0000) | nextInstance;
    nextInstance += 8;
    const uint32_t actorId = base, triggerId = base + 1, poiId = base + 2, relayId = base + 3,
                   soundId = base + 4, musicId = base + 5, shakerId = base + 6,
                   timerId = base + 7;
    const uint32_t scanId = scan ? scan(shield->scan) : 0;
    const uint32_t kNone = 0xFFFFFFFF;

    Props actor(24, "Blast Shield");
    actor.Vec(place.position).Vec(place.rotation).Vec(place.scale);
    actor.Vec(0.f, 0.f, 0.f).Vec(0.f, 0.f, 0.f).F(1.f).F(0.f).Health();
    actor.Raw(Vulnerability(door.shield)).U32(shield->cmdl);
    actor.U32(kNone).U32(0).U32(kNone); // no animation
    // ActorParameters: lights, scan, visor models, visibility.
    actor.U32(14);
    actor.U32(14).B(true).F(1.f).U32(0).F(1.f).F(20.f).F(1.f).F(1.f).F(1.f).F(1.f);
    actor.B(true).U32(1).U32(1).Vec(0.f, 0.f, 0.f).U32(4).U32(4).B(false).U32(0);
    actor.U32(1).U32(kNone);
    actor.U32(kNone).U32(kNone).U32(kNone).U32(kNone);
    actor.B(true).F(1.f).F(1.f).Visor().B(false).B(false).B(false).F(1.f);
    // looping, immovable, not solid, no camera passthrough, active
    actor.B(true).B(true).B(false).B(false).B(true).U32(0).F(1.f);
    actor.B(false).B(false).B(false).B(false);
    push(kActorType, actorId, {}, actor);

    Props trigger(12, "Blast Shield Trigger");
    trigger.Vec(place.trigger).Vec(place.triggerSize).Health();
    trigger.Raw(Vulnerability(door.shield)).U32(0).U32(kNone).U32(kNone).U32(kNone);
    trigger.B(shield->canOrbit).B(true).Visor();
    push(kDamageableTriggerType, triggerId, {{kDeadState, kSetToZeroMsg, relayId}}, trigger);

    if (scanId != 0) {
      Props poi(6, "Blast Shield Scan");
      poi.Vec(place.trigger[0], place.trigger[1], place.trigger[2] + 0.5f).Vec(0.f, 0.f, 0.f);
      poi.B(true).U32(1).U32(scanId).F(0.f);
      push(kPointOfInterestType, poiId, {}, poi);
    }

    // What breaking the shield does: it goes with a bang, and the door, no
    // longer protected, opens as if shot.
    std::vector< Conn > broke{{kZeroState, kDeactivateMsg, actorId},
                              {kZeroState, kPlayMsg, soundId},
                              {kZeroState, kPlayMsg, musicId},
                              {kZeroState, kDeactivateMsg, triggerId},
                              {kZeroState, kActionMsg, shakerId},
                              {kZeroState, kDeactivateMsg, poiId},
                              {kZeroState, kDeactivateMsg, timerId}};
    // The door's own trigger that opens it on approach; a door without power
    // has it switched on by the room instead.
    uint32_t openTrigger = 0;
    for (const PortSkipCutscenes::ScriptObject& o : objects) {
      if (o.type != kTriggerType)
        continue;
      for (const PortSkipCutscenes::ScriptObject::Connection& c : o.connections)
        if (c.message == kOpenMsg && c.target == door.doorId)
          openTrigger = o.id;
    }
    static const uint32_t kUnpowered[] = {0x001E000B, 0x0020000D, 0x000F01D1, 0x001B0088,
                                          0x0028005C};
    const bool powered =
        openTrigger != 0 && std::find(std::begin(kUnpowered), std::end(kUnpowered),
                                      openTrigger & 0x03FFFFFF) == std::end(kUnpowered);
    std::vector< Conn > guard; // the door's force field takes no damage
    for (int k = 0; k < 2; ++k) {
      if (door.forces[k] == 0)
        continue;
      guard.push_back({kZeroState, kIncrementMsg, door.forces[k]});
      broke.push_back({kZeroState, kDecrementMsg, door.forces[k]});
      if (powered)
        broke.push_back({kZeroState, kDeactivateMsg, door.forces[k]});
      connect(door.doorId, {kMaxReachedState, kDecrementMsg, door.forces[k]});
    }
    if (powered) {
      for (int k = 0; k < 2; ++k)
        if (door.shieldActors[k] != 0)
          broke.push_back({kZeroState, kDeactivateMsg, door.shieldActors[k]});
      broke.push_back({kZeroState, kActivateMsg, openTrigger});
      broke.push_back({kZeroState, kSetToZeroMsg, door.doorId});
    }
    push(kRelayType, relayId, broke, Props(2, "Blast Shield Broken").B(true));
    push(kTimerType, timerId, guard,
         Props(6, "Blast Shield Guard").F(0.1f).F(0.f).B(false).B(true).B(true));

    Props sound(20, "Blast Shield Explosion");
    sound.Vec(place.position).Vec(0.f, 0.f, 0.f).U32(3621).B(true).F(100.f).F(0.2f).F(0.f);
    sound.U32(20).U32(127).U32(127).U32(64);
    sound.B(false).B(false).B(false).B(false).B(true).B(false).B(false).U32(0);
    push(kSoundType, soundId, {}, sound);

    Props music(9, "Blast Shield Jingle");
    music.B(true).Str("/audio/evt_x_event_00.dsp").B(false).F(0.f).F(0.f).U32(92).U32(1).B(true);
    push(kStreamedAudioType, musicId, {}, music);

    Props shaker(8, "Blast Shield Shake");
    shaker.Vec(place.position).B(true).U32(1).B(false).F(0.5f).F(10.f);
    auto point = [&](bool flag, float attack, float sustain, float duration, float magnitude) {
      shaker.U32(1).B(flag).F(attack).F(sustain).F(duration).F(magnitude);
    };
    shaker.U32(1).B(true);
    point(false, 0.1f, 0.f, 0.4f, 0.2f);
    point(false, 0.1f, 0.f, 0.2f, 2.f);
    shaker.U32(1).B(false);
    point(true, 0.f, 0.f, 0.f, 0.f);
    point(true, 0.f, 0.f, 0.f, 0.f);
    shaker.U32(1).B(true);
    point(false, 0.2f, 0.f, 0.3f, 0.2f);
    point(false, 0.f, 0.f, 0.3f, 2.f);
    push(kCameraShakerType, shakerId, {}, shaker);

    // Opened from the other side, the shield is gone from this one too.
    for (uint32_t target : {actorId, triggerId, poiId, timerId})
      connect(door.doorId, {kMaxReachedState, kDeactivateMsg, target});
    if (placed != nullptr)
      placed->push_back({triggerId, door.shieldBit});
  };

  for (const DoorChange& door : doors) {
    const DoorType* type = FindDoorType(door.type);
    if (type == nullptr)
      continue;
    bool matches = true;
    for (int k = 0; k < 2; ++k) {
      if (door.forces[k] != 0)
        matches = matches && find(door.forces[k], kDamageableTriggerType, kForceSize) != nullptr;
      if (door.shieldActors[k] != 0)
        matches = matches && find(door.shieldActors[k], kActorType, kActorModel + 4) != nullptr;
    }
    if (!matches)
      continue;
    for (int k = 0; k < 2; ++k) {
      if (door.forces[k] != 0)
        edit(door.forces[k],
             {{kForceVulnerability, Vulnerability(door.type)},
              {kForceTextures, bytes({type->pattern0, type->pattern1, type->color})}});
      if (door.shieldActors[k] != 0)
        edit(door.shieldActors[k],
             {{kActorModel, bytes({door.vertical ? type->cmdlVertical : type->cmdl})}});
    }

    if (door.replacesShield)
      removeDiscShield(door);
    if (!door.shield.empty())
      addShield(door);

    // The scan sits on the door itself, which a blast shield would cover.
    const PortSkipCutscenes::ScriptObject* object =
        find(door.doorId, kDoorType, kDoorScan + 4 + kDoorTail13 + 1);
    if (type->scan == nullptr || !door.shield.empty() || !scan || object == nullptr)
      continue;
    const std::vector< uint8_t >& props = object->props;
    const size_t start = NameEnd(props);
    const uint32_t count = Read32(props, 0);
    if ((count != 13 && count != 14) || Read32(props, start + kDoorLightCount) != 14 ||
        Read32(props, start + kDoorScanCount) != 1)
      continue;
    const uint32_t id = scan(type->scan);
    if (id == 0)
      continue;
    std::vector< Run > runs{{kDoorScan, bytes({id})}};
    // Vertical and morph ball doors are looked at from the side they face.
    const float pitch = ReadFloat(props, start + kDoorRotation);
    const bool hatch = Read32(props, start + kDoorAnimSet) == kHatchAnimSet;
    float height = 0.f;
    if (hatch && pitch > -90.f && pitch < 90.f)
      height = -2.5f;
    else if (hatch && pitch > -270.f && pitch < -90.f)
      height = 2.5f;
    else if (count == 14 && props.back() != 0)
      height = 1.f;
    if (height != 0.f) {
      uint32_t bits;
      std::memcpy(&bits, &height, sizeof(bits));
      const size_t tail = kDoorTail13 + (count == 14 ? 1 : 0);
      runs.push_back({static_cast< uint32_t >(props.size() - tail - start), bytes({0, 0, bits})});
    }
    edit(door.doorId, runs);
  }
  return ops;
}

bool IsElevatorRoom(uint32_t mrea) {
  for (const Elevator& elevator : kElevators) {
    if (elevator.mrea == mrea)
      return true;
  }
  return false;
}

bool IsDoorDependency(uint32_t id) {
  static const uint32_t kIds[] = {
      // shields, their rims, and the force field's textures
      0x0734977A, 0x33188D1B, 0x59649E9D, 0xBBBA1EC7, 0x18D0AEE6, 0x095B0B93, 0xB7A8A4C9,
      0x88ED4593, 0xAB031EA9, 0xF6870C9F, 0x61A6945B, 0x459582C1, 0x717AABCE,
      0x8A7F3683, 0x1D588B22, 0xF68DF7F1, 0xBE4CD99D, 0xFC095F6C, 0x8344BEC8,
      0x544A9892, 0xCFA9DFF3,
      // the missile blast shield and its textures
      kMissileShieldCmdl, 0x5B97098E, 0x5C7B215C, 0x6E09EA6B, 0xFA0C2AE8, 0xFDE0023A,
  };
  for (uint32_t known : kIds)
    if (known == id)
      return true;
  return false;
}

std::vector< MapDoor > MapDoors(const Layout& layout, uint32_t mapa, const uint32_t* broken) {
  std::vector< MapDoor > out;
  for (const Room& room : kRooms) {
    if (room.mapa != mapa)
      continue;
    for (const DoorChange& door : Doors(layout, room.mrea, broken)) {
      const DoorType* type = FindDoorType(door.type);
      if (type == nullptr)
        continue;
      const bool shielded = !door.shield.empty() && door.shield != "None";
      out.push_back({door.doorId, shielded ? 1 : type->mapType});
    }
    break;
  }
  return out;
}

} // namespace PortApWorld
