// Remastered's own particle effect actors as a room's .roomfx (port_room_effects.h).

#include "port_remastered_room_internal.h"
#include "port_queen_barrier_assets.h"
#include "port_room_effects.h"

namespace PortRemastered {
namespace RoomDetail {
namespace {

// EffectMP1 properties (SLdrEffectMP1::Load, 0xd9cd58), with the loader's defaults.
constexpr uint32_t kPropGenp = 0x88fc08e6;
constexpr uint32_t kPropHotInThermal = 0x7f176a6a;
constexpr uint32_t kPropNoTimer = 0x0baeccf6;
constexpr uint32_t kPropNoTimer2 = 0xff985718;  // CEffectMP1GOC::Create ORs it into the same flag
constexpr uint32_t kPropRebuild = 0x85963ef9;
constexpr uint32_t kPropUseRateInvCamDist = 0x40cff441;
constexpr uint32_t kPropRateInvCamDist = 0xcf6c4262;
constexpr uint32_t kPropRateInvCamDistRate = 0xed2add6d;
constexpr uint32_t kPropDuration = 0x4fca7292;
constexpr uint32_t kPropDurationResetWhileVisible = 0xeb2e007b;
constexpr uint32_t kPropUseCamDistRange = 0x86e4b31b;
constexpr uint32_t kPropCamDistRangeMin = 0x8dc7fd5e;
constexpr uint32_t kPropCamDistRangeMax = 0x45375a0b;
constexpr uint32_t kPropFarRate = 0x6d12ca92;
constexpr uint32_t kPropCombatVisor = 0x155fa3e4;
constexpr uint32_t kPropThermalVisor = 0x880e9b91;
constexpr uint32_t kPropXrayVisor = 0x2ada726b;
constexpr uint32_t kPropDieWhenDone = 0xf13f4e03;
constexpr uint32_t kPropExternalVariables = 0x3def0de6;  // CScriptEffectMP1::InitializeExternalVariables

constexpr uint8_t kRetailEffect = 7;
// A retail effect no object matched, with an added one this near, is the one it replaces.
constexpr double kReplaceDistance = 5.0;

// Rz * Ry * Rx of Remastered's angles in GameCube axes (the room geometry's `place`), then
// the area's rotation, as retail's editor angles in degrees.
void RetailAngles(const Vec3& rot, const Mat34& xf, float out[3]) {
  constexpr int kAxis[3] = {0, 2, 1};
  constexpr double kSign[3] = {-1, 1, 1};
  constexpr double kDeg = 3.14159265358979323846 / 180.0;
  double s[3], k[3];
  for (int i = 0; i < 3; ++i) {
    s[i] = std::sin(rot[size_t(i)] * kDeg);
    k[i] = std::cos(rot[size_t(i)] * kDeg);
  }
  const double m[3][3] = {
      {k[2] * k[1], k[2] * s[1] * s[0] - s[2] * k[0], k[2] * s[1] * k[0] + s[2] * s[0]},
      {s[2] * k[1], s[2] * s[1] * s[0] + k[2] * k[0], s[2] * s[1] * k[0] - k[2] * s[0]},
      {-s[1], k[1] * s[0], k[1] * k[0]},
  };
  double g[3][3], w[3][3];
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      g[row][col] = kSign[row] * kSign[col] * m[kAxis[row]][kAxis[col]];
    }
  }
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      w[row][col] = xf[size_t(row)][0] * g[0][col] + xf[size_t(row)][1] * g[1][col] + xf[size_t(row)][2] * g[2][col];
    }
  }
  const double sy = std::clamp(-w[2][0], -1.0, 1.0);
  double x, z;
  if (std::fabs(sy) > 0.99999) {
    x = 0;
    z = std::atan2(-w[0][1], w[1][1]);
  } else {
    x = std::atan2(w[2][1], w[2][2]);
    z = std::atan2(w[1][0], w[0][0]);
  }
  out[0] = float(x / kDeg);
  out[1] = float(std::asin(sy) / kDeg);
  out[2] = float(z / kDeg);
}

}  // namespace

// ---------------------------------------------------------------------------
// The Parasite Queen barrier (port_queen_barrier_assets.h): the one connected-effect graph the
// import admits, and only when the ROOM ships it whole.
// ---------------------------------------------------------------------------
namespace {

// What its components' own properties are read through.
constexpr uint32_t kPropModel = 0xf8b4c8da;            // PlatformMP1's model
constexpr uint32_t kPropCollision = 0x02d5ab29;        // and its collision
constexpr uint32_t kPropBarrierDuration = 0xc9b0ad29;  // the controller's fourth float, in seconds
constexpr uint32_t kPlatformMP1 = 0xa12967fb;
constexpr uint32_t kWaypointMP1 = 0x8e4c86ec;
constexpr uint32_t kSpecialFunctionController = 8;  // kPropSpecialFunctionType of the main one
constexpr uint32_t kSpecialFunctionFollower = 19;   // and of the ten it drives
// The links the controller, the platforms and the followers make: their events are the sender's
// Play states and their actions the messages (port_remastered_script_tables.inc names them).
constexpr uint32_t kEventBarrierPlay = 0x6b565a4c;       // SpecialFunctionMP1 Play
constexpr uint32_t kActionBarrierPlatform = 0x2302215c;  // Play to a ring's platform
constexpr uint32_t kActionBarrierEffect = 0xdbe5cb30;    // to an effect, from the controller or a platform
constexpr uint32_t kActionBarrierFollower = 0x7dcfa087;  // a follower to the effect it carries
constexpr uint32_t kEventPlatformActive = 0x28e11b3f;    // PlatformMP1 Active
constexpr uint32_t kEventPlatformPlay = 0xdb8148e3;      // PlatformMP1 Play
constexpr uint32_t kActionPlatformWaypoint = 0x5d191757;
constexpr uint8_t kRetailWaypoint = 0x02;
constexpr double kWaypointTolerance = 0.01;  // metres, world space
constexpr uint8_t kRetailSpecialFunction = 0x3A;  // ScriptLoader's SpecialFunction (CEntityInfo's 58)
// Where a retail object's own fields sit, from the end of its name (ScriptLoader.cpp: a
// SpecialFunction's head is its position and orientation, 24 bytes, and the function follows at
// once; a Platform's is SScaledActorHead, 36 bytes, then its extent and centroid, 12 each, and
// the model follows at 60).
constexpr size_t kRetailFunctionAt = 24;
constexpr size_t kRetailPlatformModelAt = 60;
constexpr uint32_t kRetailControllerProps = 15;  // a SpecialFunction's property count
constexpr uint32_t kRetailPlatformProps = 19;    // a Platform's
// The states and messages of the links the retail script makes (EScriptObjectState and
// EScriptObjectMessage in MetroidPrime/CEntityInfo.hpp): the ring controller plays its five
// platforms, and each of them follows its waypoint.
constexpr uint32_t kStatePlay = 18;   // kSS_Play
constexpr uint32_t kMsgActivate = 1;  // kSM_Activate
constexpr uint32_t kMsgFollow = 6;    // kSM_Follow
// The five Remastered platforms carry a dummy model and its collision, not the retail ones: the
// barrier is drawn by the effects, and those platforms only carry them.
constexpr const char* kBarrierPlatformModel = "10000000-0000-f000-f000-00001c0b0bef";
constexpr const char* kBarrierPlatformCollision = "10000000-0000-f000-f000-0000d4c81052";
constexpr uint32_t kBarrierRetailModel = 0x6556EEC5;
constexpr uint32_t kRetailControllerFunction = 8;
constexpr size_t kNoIndex = static_cast<size_t>(-1);

int HexDigit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// A guid in the text the asset table holds it (8-4-4-4-12, its digits' case free), as the 16
// bytes a ROOM property stores it: the same order Variable::guid uses.
bool ParseGuid(const char* text, Id16& out) {
  std::array<uint8_t, 16> canonical{};
  size_t at = 0;
  int high = -1;
  for (const char* p = text; *p != '\0'; ++p) {
    const int digit = HexDigit(*p);
    if (digit < 0) {
      if (*p == '-') {
        continue;
      }
      return false;
    }
    if (high < 0) {
      high = digit;
      continue;
    }
    if (at >= canonical.size()) {
      return false;
    }
    canonical[at++] = uint8_t(high << 4 | digit);
    high = -1;
  }
  if (at != canonical.size() || high >= 0) {
    return false;
  }
  out = SwapUuid(canonical.data());
  return true;
}

bool Property16(const Room& room, const std::map<uint32_t, Span>& flat, uint32_t id, Id16& out) {
  const auto it = flat.find(id);
  if (it == flat.end() || it->second.size != 16) {
    return false;
  }
  std::memcpy(out.data(), room.Bytes(it->second), 16);
  return true;
}

bool PropertyU32(const Room& room, const std::map<uint32_t, Span>& flat, uint32_t id, uint32_t& out) {
  const auto it = flat.find(id);
  if (it == flat.end() || it->second.size < 4) {
    return false;
  }
  out = ReadLE32(room.Bytes(it->second));
  return true;
}

// Whether a 16 byte property holds the guid `text` names.
bool PropertyIs(const Room& room, const std::map<uint32_t, Span>& flat, uint32_t id, const char* text) {
  Id16 value{}, want{};
  return Property16(room, flat, id, value) && ParseGuid(text, want) && value == want;
}

// The ten GENPs of the barrier as a ROOM property stores them: the actors the barrier is made
// of, known from the table alone (so a graph that does not validate still reserves them).
std::set<Id16> BarrierGenps() {
  std::set<Id16> out;
  for (const auto& ring : PortQueenBarrier::kRings) {
    for (const char* text : {ring.refraction, ring.glow}) {
      Id16 id{};
      if (ParseGuid(text, id)) {
        out.insert(id);
      }
    }
  }
  return out;
}

// An EffectMP1 component's GENP as the asset table prints it, empty when it has none.
std::string GenpTextOf(const Room& room, size_t component) {
  Id16 genp{};
  if (!Property16(room, room.Flat(room.Components()[component]), kPropGenp, genp)) {
    return {};
  }
  return UuidText(genp.data());
}

// The retail Effect object an EffectMP1 actor becomes: its entity's transform in the area's
// space, its own properties, and the actor's particle variables (unreadable ones empty, counted).
bool ActorEffect(const Room& room, const Component& c, const Area& area, uint32_t part, bool active,
                 PortRoomEffects::Effect& e, std::vector<PortRoomEffects::Variable>& vars, size_t& badVars,
                 Vec3& world) {
  Vec3 pos, rot, scale;
  if (!room.Xform(c, pos, rot, scale)) {
    return false;
  }
  const auto f = room.Flat(c);
  const auto genp = f.find(kPropGenp);
  e = PortRoomEffects::Effect{};
  e.name = "Remastered effect ";
  e.name += genp != f.end() && genp->second.size == 16 ? UuidText(room.Bytes(genp->second)) : std::string("without a GENP");
  const Vec3 w = Apply(area.xf, MulR2G(pos));
  world = w;
  constexpr int kAxis[3] = {0, 2, 1};
  for (size_t i = 0; i < 3; ++i) {
    e.pos[i] = float(w[i]);
    e.scale[i] = float(scale[size_t(kAxis[i])]);
  }
  RetailAngles(rot, area.xf, e.rot);
  e.part = part;
  e.active = active;
  auto flag = [&](uint32_t id, bool fallback) {
    const auto it = f.find(id);
    return it != f.end() && it->second.size >= 1 ? room.Bytes(it->second)[0] != 0 : fallback;
  };
  auto real = [&](uint32_t id, float fallback) {
    const auto it = f.find(id);
    return it != f.end() && it->second.size >= 4 ? ReadLEFloat(room.Bytes(it->second)) : fallback;
  };
  e.hotInThermal = flag(kPropHotInThermal, false);
  e.noTimerUnlessAreaOccluded = flag(kPropNoTimer, false) || flag(kPropNoTimer2, false);
  e.rebuildSystemsOnActivate = flag(kPropRebuild, false);
  e.useRateInverseCamDist = flag(kPropUseRateInvCamDist, false);
  e.rateInverseCamDist = real(kPropRateInvCamDist, 5.0f);
  e.rateInverseCamDistRate = real(kPropRateInvCamDistRate, 0.5f);
  e.duration = real(kPropDuration, 0.2f);
  e.durationResetWhileVisible = real(kPropDurationResetWhileVisible, 0.1f);
  e.useRateCamDistRange = flag(kPropUseCamDistRange, false);
  e.rateCamDistRangeMin = real(kPropCamDistRangeMin, 20.0f);
  e.rateCamDistRangeMax = real(kPropCamDistRangeMax, 30.0f);
  e.rateCamDistRangeFarRate = real(kPropFarRate, 0.0f);
  e.combatVisorVisible = flag(kPropCombatVisor, true);
  e.thermalVisorVisible = flag(kPropThermalVisor, true);
  e.xrayVisorVisible = flag(kPropXrayVisor, true);
  e.dieWhenSystemsDone = flag(kPropDieWhenDone, false);
  vars.clear();
  const auto ext = f.find(kPropExternalVariables);
  if (ext != f.end() && !PortRoomEffects::ReadExternalVariables(room.Bytes(ext->second), ext->second.size, vars)) {
    ++badVars;  // the effect still comes, with its system's defaults
    vars.clear();
  }
  return true;
}

}  // namespace

std::set<int> ConnectedEntities(const Room& room) {
  const std::vector<Component>& comps = room.Components();
  std::set<int> out;
  for (const Connection& c : ReadConnections(room)) {
    const int t = room.ByGuid(c.target);
    if (t >= 0) {
      out.insert(comps[size_t(t)].type == kEntity ? t : comps[size_t(t)].entity);
    }
  }
  return out;
}

Id16 EffectGenpOf(const Room& room, size_t component) {
  Id16 genp{}, value{};
  if (Property16(room, room.Flat(room.Components()[component]), kPropGenp, value)) {
    genp = SwapUuid(value.data());
  }
  return genp;
}

bool BarrierGraphOf(const Room& room, BarrierGraph& out, std::string* why) {
  auto fail = [why](const char* text) {
    if (why != nullptr) {
      *why = text;
    }
    return false;
  };
  out = BarrierGraph{};
  const std::vector<Component>& comps = room.Components();
  constexpr size_t kRings = PortQueenBarrier::kRings.size();
  static_assert(PortQueenBarrier::kRings.size() == 5, "the barrier has five rings");
  // The main controller: the one component carrying the table's guid, a SpecialFunctionMP1 whose
  // function is 8, with a finite duration of at most a minute in its fourth float.
  size_t controller = kNoIndex;
  size_t controllers = 0;
  for (size_t i = 0; i < comps.size(); ++i) {
    if (comps[i].type == kSpecialFunction && comps[i].hasGuid &&
        UuidText(comps[i].guid.data()) == PortQueenBarrier::kControllerGuid) {
      controller = i;
      ++controllers;
    }
  }
  if (controllers != 1) {
    return fail("no single ring controller");
  }
  const auto flat = room.Flat(comps[controller]);
  uint32_t function = 0;
  if (!PropertyU32(room, flat, kPropSpecialFunctionType, function) || function != kSpecialFunctionController) {
    return fail("the controller's function is not the ring controller's");
  }
  uint32_t durationBits = 0;
  if (!PropertyU32(room, flat, kPropBarrierDuration, durationBits)) {
    return fail("the controller has no duration");
  }
  float duration = 0;
  std::memcpy(&duration, &durationBits, 4);
  if (!std::isfinite(duration) || !(duration > 0.f) || duration > 60.f) {
    return fail("the controller's duration is not a finite number of seconds up to a minute");
  }
  const std::vector<Connection> conns = ReadConnections(room);
  // Exactly its fifteen links: five platforms and ten effects, nothing else, each once.
  std::vector<size_t> platformComps, effectComps;
  size_t links = 0;
  for (const Connection& c : conns) {
    if (c.sender != controller) {
      continue;
    }
    ++links;
    const int t = room.ByGuid(c.target);
    if (t < 0 || c.event != kEventBarrierPlay) {
      return fail("a controller link is not a Play link, or names nothing the room has");
    }
    if (c.action == kActionBarrierPlatform && comps[size_t(t)].type == kPlatformMP1) {
      platformComps.push_back(size_t(t));
    } else if (c.action == kActionBarrierEffect && comps[size_t(t)].type == kEffectMP1) {
      effectComps.push_back(size_t(t));
    } else {
      return fail("a controller link is neither to a platform nor to an effect");
    }
  }
  if (links != kRings * 3 || platformComps.size() != kRings || effectComps.size() != 2 * kRings) {
    return fail("the controller does not have five platform and ten effect links");
  }
  // Each platform is the one the table names by its component guid, matched once each.
  std::array<size_t, kRings> platformOf{};
  std::array<bool, kRings> ringSeen{};
  for (size_t p : platformComps) {
    size_t ring = kRings;
    if (comps[p].hasGuid) {
      const std::string text = UuidText(comps[p].guid.data());
      for (size_t r = 0; r < kRings; ++r) {
        if (text == PortQueenBarrier::kRings[r].component) {
          ring = r;
        }
      }
    }
    if (ring == kRings || ringSeen[ring]) {
      return fail("a platform is not one the table names, or two are");
    }
    ringSeen[ring] = true;
    platformOf[ring] = p;
  }
  // Per ring: its dummy model and collision, its two effects (the table's two GENPs) and the
  // waypoint its Play link names.
  std::map<size_t, size_t> ringOfEffect;  // effect component -> ring
  std::set<size_t> waypoints;            // each ring's own
  BarrierGraph graph;
  graph.controller = controller;
  graph.duration = duration;
  for (size_t r = 0; r < kRings; ++r) {
    const size_t p = platformOf[r];
    const auto pf = room.Flat(comps[p]);
    if (!PropertyIs(room, pf, kPropModel, kBarrierPlatformModel) ||
        !PropertyIs(room, pf, kPropCollision, kBarrierPlatformCollision)) {
      return fail("a platform's model or collision is not the dummy pair");
    }
    std::array<size_t, 2> effects{};
    size_t effectLinks = 0;
    size_t waypoint = kNoIndex;
    size_t waypointLinks = 0;
    for (const Connection& c : conns) {
      if (c.sender != p) {
        continue;
      }
      const int t = room.ByGuid(c.target);
      if (t < 0) {
        return fail("a platform link names a component the room lacks");
      }
      if (c.event == kEventPlatformActive && c.action == kActionBarrierEffect) {
        if (comps[size_t(t)].type != kEffectMP1 || effectLinks >= effects.size()) {
          return fail("a platform has more than two effect links, or one is no effect");
        }
        effects[effectLinks++] = size_t(t);
      } else if (c.event == kEventPlatformPlay && c.action == kActionPlatformWaypoint) {
        if (comps[size_t(t)].type != kWaypointMP1 || waypointLinks != 0) {
          return fail("a platform's Play link names no waypoint, or two");
        }
        waypoint = size_t(t);
        ++waypointLinks;
      }
    }
    if (effectLinks != 2 || waypointLinks != 1 || effects[0] == effects[1] || !waypoints.insert(waypoint).second) {
      return fail("a platform does not have two effects and one waypoint of its own");
    }
    bool refraction = false, glow = false;
    for (size_t e : effects) {
      const std::string genp = GenpTextOf(room, e);
      const bool isRefraction = genp == PortQueenBarrier::kRings[r].refraction;
      const bool isGlow = genp == PortQueenBarrier::kRings[r].glow;
      if (!isRefraction && !isGlow) {
        return fail("an effect is not the pair the table names for its ring");
      }
      if (ringOfEffect.count(e) != 0) {
        return fail("an effect is in two rings");
      }
      ringOfEffect[e] = r;
      refraction = refraction || isRefraction;
      glow = glow || isGlow;
      (isRefraction ? graph.rings[r].refraction : graph.rings[r].glow) = e;
    }
    if (!refraction || !glow) {
      return fail("a ring is missing one of its two effect families");
    }
    graph.rings[r].platform = p;
    graph.rings[r].waypoint = waypoint;
  }
  // The controller's ten effect links are exactly those pairs, each named once.
  if (ringOfEffect.size() != 2 * kRings) {
    return fail("the ten effects are not the five pairs'");
  }
  std::set<size_t> named;
  for (size_t e : effectComps) {
    if (ringOfEffect.count(e) == 0 || !named.insert(e).second) {
      return fail("the controller's effect links are not exactly the pairs, or name one twice");
    }
  }
  if (named.size() != 2 * kRings) {
    return fail("the ten effects are not all named");
  }
  // One type 19 follower per platform-effect pair, linking its platform and its effect; a type 19
  // function with no links at all is idle (the ROOM has one) and allowed.
  std::map<std::pair<size_t, size_t>, size_t> followers;
  for (size_t i = 0; i < comps.size(); ++i) {
    if (comps[i].type != kSpecialFunction) {
      continue;
    }
    uint32_t type = 0;
    if (!PropertyU32(room, room.Flat(comps[i]), kPropSpecialFunctionType, type) || type != kSpecialFunctionFollower) {
      continue;
    }
    size_t from = kNoIndex, to = kNoIndex, count = 0;
    for (const Connection& c : conns) {
      if (c.sender != i) {
        continue;
      }
      ++count;
      const int t = room.ByGuid(c.target);
      if (t < 0 || c.event != kEventBarrierPlay) {
        return fail("a follower's link is not a Play link, or names nothing the room has");
      }
      if (c.action == kActionBarrierPlatform) {
        from = size_t(t);
      } else if (c.action == kActionBarrierFollower) {
        to = size_t(t);
      } else {
        return fail("a follower's link is neither to a platform nor to an effect");
      }
    }
    if (count == 0) {
      continue;
    }
    if (count != 2 || from == kNoIndex || to == kNoIndex) {
      return fail("a follower has two links but not one of each");
    }
    const auto effect = ringOfEffect.find(to);
    if (effect == ringOfEffect.end() || platformOf[effect->second] != from) {
      return fail("a follower links a pair the table does not know");
    }
    ++followers[{from, to}];
  }
  for (size_t r = 0; r < kRings; ++r) {
    for (size_t e : {graph.rings[r].refraction, graph.rings[r].glow}) {
      if (followers[{platformOf[r], e}] != 1) {
        return fail("a platform-effect pair does not have exactly one follower");
      }
    }
  }
  out = graph;
  return true;
}

// ---------------------------------------------------------------------------
// The same barrier on the disc's side: its retail controller, the five platforms the waypoint
// links pick out, and what the writer stages for them.
// ---------------------------------------------------------------------------
namespace {

// Whether the disc's own script ships the barrier the Remastered graph belongs to: its ring
// controller, the five platforms with their waypoint links, on one layer; `layer` is that layer,
// which its add-ons are created on.
bool RetailBarrierOf(const Room& room, const Area& area, const BarrierGraph& graph, uint32_t& layer,
                     std::string* why) {
  auto fail = [why](const char* text) {
    if (why != nullptr) {
      *why = text;
    }
    return false;
  };
  constexpr size_t kRings = PortQueenBarrier::kRings.size();
  const std::vector<ScriptObject>& objects = area.objects;
  const std::vector<Component>& comps = room.Components();
  if (objects.empty()) {
    return fail("no retail objects");
  }
  // The add-ons are retail objects of the controller's own area, on its layer: the codec
  // compares every member's layer and area bits against the controller's.
  layer = PortQueenBarrier::kController >> 26;
  auto findId = [&objects](uint32_t id) {
    size_t found = kNoIndex;
    for (size_t i = 0; i < objects.size(); ++i) {
      if (objects[i].id != id) {
        continue;
      }
      if (found != kNoIndex) {
        return kNoIndex;  // named twice: ambiguous
      }
      found = i;
    }
    return found;
  };
  // The ring controller: a retail SpecialFunction (ScriptLoader's 58) whose function is 8, read
  // right after its 24 byte head. Its own fourth float is zero, so the duration comes from the
  // ROOM, never from it.
  const size_t controllerAt = findId(PortQueenBarrier::kController);
  if (controllerAt == kNoIndex) {
    return fail("no ring controller 00190066");
  }
  const ScriptObject& controller = objects[controllerAt];
  if (controller.type != kRetailSpecialFunction || controller.layer != int(layer) ||
      controller.propCount != kRetailControllerProps || controller.props.size() < kRetailFunctionAt + 4 ||
      ReadBE32(&controller.props[kRetailFunctionAt]) != kRetailControllerFunction) {
    return fail("not a SpecialFunction (58) of 15 properties with function 8 at 24");
  }
  // Its links are exactly the five table platforms, each played once with Play/Activate: a
  // sixth link, or one somewhere else, is another script's barrier.
  if (controller.links.size() != kRings) {
    return fail("the controller does not have exactly five links");
  }
  std::array<bool, kRings> played{};
  for (const ScriptObject::Link& link : controller.links) {
    size_t ring = kRings;
    for (size_t r = 0; r < kRings; ++r) {
      if (link.target == PortQueenBarrier::kRings[r].platform) {
        ring = r;
      }
    }
    if (ring == kRings) {
      return fail("a controller link is not to one of the five platforms");
    }
    if (link.state != kStatePlay || link.message != kMsgActivate || played[ring]) {
      return fail("a controller link to a platform is not the Play/Activate one, or names it twice");
    }
    played[ring] = true;
  }
  if (std::find(played.begin(), played.end(), false) != played.end()) {
    return fail("the controller does not play its five platforms");
  }
  // The five platforms: their model where a Platform keeps it (never found by searching its
  // bytes), one Play/Follow link to their own waypoint, whose world place is where Remastered
  // puts the one its platform plays to. The starting platforms are all coincident, so that
  // position, not proximity, is what tells them apart.
  for (size_t r = 0; r < kRings; ++r) {
    const auto& ring = PortQueenBarrier::kRings[r];
    const size_t p = findId(ring.platform);
    if (p == kNoIndex) {
      return fail("a ring platform is missing");
    }
    const ScriptObject& platform = objects[p];
    if (platform.type != kRetailPlatform || platform.layer != int(layer) ||
        platform.propCount != kRetailPlatformProps ||
        platform.props.size() < kRetailPlatformModelAt + 4 ||
        ReadBE32(&platform.props[kRetailPlatformModelAt]) != kBarrierRetailModel) {
      return fail("not a Platform (8) of 19 properties, model at 60, on the controller's layer");
    }
    // Exactly one link to its waypoint, and it is the Play/Follow one that moves the platform.
    size_t toWaypoint = 0;
    bool followed = false;
    for (const ScriptObject::Link& link : platform.links) {
      if (link.target != ring.waypoint) {
        continue;
      }
      ++toWaypoint;
      if (link.state != kStatePlay || link.message != kMsgFollow) {
        return fail("a platform's link to its waypoint is not the Play/Follow one");
      }
      followed = true;
    }
    if (toWaypoint != 1 || !followed) {
      return fail("a platform does not link exactly one waypoint");
    }
    // And no other Play/Follow link: one platform follows one waypoint.
    size_t follows = 0;
    for (const ScriptObject::Link& link : platform.links) {
      if (link.state == kStatePlay && link.message == kMsgFollow) {
        ++follows;
      }
    }
    if (follows != 1) {
      return fail("a platform has more than one Play/Follow link");
    }
    const size_t w = findId(ring.waypoint);
    if (w == kNoIndex) {
      return fail("a waypoint object is missing");
    }
    const ScriptObject& waypoint = objects[w];
    Vec3 pos, rot, scale;
    if (waypoint.type != kRetailWaypoint || waypoint.layer != int(layer) || !waypoint.hasPos ||
        !room.Xform(comps[graph.rings[r].waypoint], pos, rot, scale)) {
      return fail("a waypoint object is not a waypoint, has no place, or is on another layer");
    }
    if (MaxAbs(Apply(area.xf, MulR2G(pos)), waypoint.pos) > kWaypointTolerance) {
      return fail("a waypoint is not where the Remastered one is");
    }
  }
  return true;
}

// The barrier's ten effects, all of them or none: each converts, each is checked against its
// own conversion, and each is staged inactive with the ROOM's properties, transform and
// particle variables. Their ids follow the generic add-ons', so no index is used twice.
struct BarrierStage {
  bool ok = false;
  std::string why;  // for the log, when it is not
  uint32_t layer = 0;
  float duration = 0;
  std::array<PortRoomEffects::Effect, 10> effects{};
  std::array<std::vector<PortRoomEffects::Variable>, 10> vars;
  std::array<uint32_t, 10> ids{};
};

BarrierStage StageBarrier(const Room& room, const Area& area, const RoomIO& io, uint32_t areaBits) {
  constexpr size_t kRings = PortQueenBarrier::kRings.size();
  BarrierStage stage;
  auto fail = [&stage](const char* why) {
    stage.ok = false;
    stage.why = why;
    return stage;
  };
  if (!io.effect || !io.barrierEffect) {
    return fail("no conversion for it");
  }
  // Its add-ons take the area's bits, which must be the controller's own (the codec compares
  // every member's area and layer against the controller's).
  if (((PortQueenBarrier::kController >> 16) & 0x3FF) != areaBits) {
    return fail("the room's retail ids are not the barrier's area");
  }
  BarrierGraph graph;
  std::string why;
  if (!BarrierGraphOf(room, graph, &why)) {
    return fail(("its Remastered graph is not the one the table names: " + why).c_str());
  }
  std::string retailWhy;
  if (!RetailBarrierOf(room, area, graph, stage.layer, &retailWhy)) {
    return fail(("the retail rings do not match it: " + retailWhy).c_str());
  }
  stage.duration = graph.duration;
  size_t slot = 0;
  std::set<int> entities;  // the ten actors must be ten: two rings sharing one is no barrier
  for (size_t r = 0; r < kRings; ++r) {
    for (int family = 0; family < 2; ++family) {
      const bool refraction = family == 0;
      const auto& ring = PortQueenBarrier::kRings[r];
      const size_t comp = refraction ? graph.rings[r].refraction : graph.rings[r].glow;
      const uint32_t feature = refraction ? PortQueenBarrier::kRefractionFeature : PortQueenBarrier::kGlowFeature;
      // The real variables the controller animates, in the order the runtime binds them.
      const std::vector<const char*> texts =
          refraction ? std::vector<const char*>{ring.breakupRefraction, ring.motion}
                     : std::vector<const char*>{ring.breakupGlow};
      std::vector<std::array<uint8_t, 16>> realVars;
      for (const char* text : texts) {
        Id16 guid{};
        if (!ParseGuid(text, guid)) {
          return fail("a variable guid in the table is malformed");
        }
        realVars.push_back(guid);
      }
      const Id16 genp = EffectGenpOf(room, comp);
      if (genp == Id16{}) {
        return fail("an effect has no GENP");
      }
      const uint32_t part = io.effect(genp);
      if (part == 0 || part == 0xFFFFFFFF) {
        return fail("an effect has no PART");
      }
      if (!io.barrierEffect(part, feature, realVars)) {
        return fail("an effect's conversion is not the barrier's");
      }
      size_t badVars = 0;
      Vec3 world{};
      if (!ActorEffect(room, room.Components()[comp], area, part, false, stage.effects[slot], stage.vars[slot], badVars,
                       world)) {
        return fail("an effect has no transform");
      }
      if (badVars != 0) {
        return fail("an effect's particle variables are unreadable");
      }
      if (!entities.insert(room.Components()[comp].entity).second) {
        return fail("two of its effects are one actor");
      }
      ++slot;
    }
  }
  if (slot != stage.effects.size()) {
    return fail("it does not name ten effects");
  }
  stage.ok = true;
  return stage;
}

}  // namespace

std::string WriteRoomEffects(const std::string& name, const Room& room, uint32_t mrea, const Area& area,
                             const SceneryScripts& scripts, const RoomIO& io) {
  const std::vector<const Component*> effects = room.Of(kEffectMP1);
  if (!io.effect || !io.write || effects.empty()) {
    return {};
  }
  char line[384];
  const std::vector<ScriptObject>& objects = area.objects;
  if (objects.empty()) {
    return "  " + name + ": no retail objects, its effect actors left out";
  }
  for (const ScriptObject& o : objects) {
    if (PortRoomEffects::IsAddOnId(o.id)) {
      std::snprintf(line, sizeof line, "  %s: retail object %08X is in the add-on id range, its effect actors left out",
                    name.c_str(), o.id);
      return line;
    }
  }
  const uint32_t areaBits = (objects[0].id >> 16) & 0x3FF;
  const std::set<int> connected = ConnectedEntities(room);
  std::set<uint32_t> targeted;  // retail ids some retail connection reaches
  for (const ScriptObject& o : objects) {
    for (uint32_t t : o.targets) {
      targeted.insert(t & 0x3FFFFFF);
    }
  }
  std::set<size_t> matchedObjects;
  for (const auto& [entity, index] : scripts.retail) {
    matchedObjects.insert(index);
  }
  // The barrier first: its ten effects are connected, so the generic pass would leave them out,
  // and the script matcher may have paired them with retail objects of its own choosing. It is
  // taken whole or not at all.
  BarrierStage barrier;
  if (mrea == PortQueenBarrier::kMrea) {
    barrier = StageBarrier(room, area, io, areaBits);
  }
  // The table's ten GENPs are the barrier's actors whether or not its graph validates or its
  // conversion succeeds: no generic path may match, add, override or skip one of them, or a
  // failed group would leave half of it behind as a replaced retail effect. A good stage adds
  // them itself, below; a failed one adds nothing and they stay out.
  const std::set<Id16> barrierGenps = mrea == PortQueenBarrier::kMrea ? BarrierGenps() : std::set<Id16>();

  std::map<uint32_t, PortRoomEffects::Layer> layers;
  std::set<uint32_t> skips;
  std::vector<PortRoomEffects::Override> overrides;
  std::vector<Vec3> added;
  std::set<int> seen;
  size_t connectedCount = 0, noPart = 0, noLayer = 0, kept = 0, keptOwn = 0, overridden = 0, full = 0, badVars = 0;
  size_t reserved = 0;  // the barrier's actors, left to it
  for (const Component* c : effects) {
    if (c->entity < 0 || !seen.insert(c->entity).second) {
      continue;
    }
    if (!barrierGenps.empty()) {
      Id16 genp{};
      if (Property16(room, room.Flat(*c), kPropGenp, genp) && barrierGenps.count(genp) != 0) {
        ++reserved;  // the barrier's own actor: never matched, added, overridden or skipped
        continue;
      }
    }
    const auto f = room.Flat(*c);
    const auto genp = f.find(kPropGenp);
    if (genp == f.end() || genp->second.size != 16) {
      ++noPart;
      continue;
    }
    const std::array<uint8_t, 16> genpId = SwapUuid(room.Bytes(genp->second));
    uint32_t part = io.effect(genpId);
    // The retail PARTs the effects step wrote this GENP under by its table: one GENP can stand
    // for several, the lava spit's big and small. A retail object drawing one of them keeps its
    // own id, which is now the mod's conversion of that GENP.
    const std::vector<uint32_t> paired = io.matched != nullptr ? io.matched(genpId) : std::vector<uint32_t>();
    // One the matcher paired with a retail object is that object, which stays, whatever
    // connects to it; if Remastered gave it an effect of its own, or it has particle
    // variables, the object takes those (an override).
    const auto matched = scripts.retail.find(c->entity);
    if (matched != scripts.retail.end()) {
      const ScriptObject& o = objects[matched->second];
      if (o.type != kRetailEffect) {
        ++kept;
        continue;
      }
      std::vector<PortRoomEffects::Variable> vars;
      const auto ext = f.find(kPropExternalVariables);
      if (ext != f.end() && !PortRoomEffects::ReadExternalVariables(room.Bytes(ext->second), ext->second.size, vars)) {
        ++badVars;
        vars.clear();
      }
      // The variables are the GENP's: they go only with its PART, the Remastered-only one or
      // the one the mod gives the object's own retail id (its own of the GENP's, as big and
      // small spit share one).
      const uint32_t own = o.props.size() >= 40 ? ReadBE32(&o.props[36]) : 0;
      if (std::find(paired.begin(), paired.end(), own) != paired.end()) {
        part = own;
      }
      const bool ownPart = part != 0 && (part >> 24) == 0xFE && part != own;
      // The flamejet GENP emits along source +Y (port +Z), unlike retail's PART.
      // CEffectMP1GOC::Create (0xd9e460) passes its entity's placement directly.
      // Keeping the retail rotation after replacing its PART turns the jet sideways.
      const bool flamejet = own == 0x3A617BEB && part == own &&
          UuidText(room.Bytes(genp->second)) == "6916272d-4272-4ec4-a0b2-a4f9b3160ef6";
      if (!flamejet && !ownPart && (part == 0 || part != own || vars.empty())) {
        ++keptOwn;
        continue;
      }
      PortRoomEffects::Override over{o.id, part, std::move(vars)};
      if (flamejet) {
        PortRoomEffects::Effect placement;
        std::vector<PortRoomEffects::Variable> placementVars;
        Vec3 world;
        if (ActorEffect(room, *c, area, part, true, placement, placementVars, badVars, world)) {
          over.hasPlacement = true;
          std::copy_n(placement.pos, 3, over.pos);
          std::copy_n(placement.rot, 3, over.rot);
          std::copy_n(placement.scale, 3, over.scale);
        }
      }
      overrides.push_back(std::move(over));
      ++overridden;
      continue;
    }
    if (connected.count(c->entity) != 0) {
      ++connectedCount;
      continue;
    }
    if (part == 0) {
      ++noPart;
      continue;
    }
    uint32_t retailLayer = 0;
    if (!c->baseLayer) {
      const auto layer = scripts.layer.find(c->entity);
      if (layer == scripts.layer.end()) {
        ++noLayer;  // a layer none of whose entities matched: which retail one it is is unknown
        continue;
      }
      retailLayer = layer->second;
    }
    if (added.size() >= PortRoomEffects::kMaxAddOns) {
      ++full;
      continue;
    }
    PortRoomEffects::Effect e;
    std::vector<PortRoomEffects::Variable> vars;
    Vec3 world{};
    if (!ActorEffect(room, *c, area, part, room.Active(*c), e, vars, badVars, world)) {
      ++noPart;
      continue;
    }
    e.id = PortRoomEffects::AddOnId(retailLayer, areaBits, uint32_t(added.size()));
    PortRoomEffects::Layer& layer = layers[retailLayer];
    layer.layer = retailLayer;
    PortRoomEffects::AppendEffect(e, layer.objects);
    layer.vars.push_back(std::move(vars));
    ++layer.count;
    added.push_back(world);
  }
  // The barrier's ten effects, once every one of them converted: their ids follow the generic
  // add-ons', so no index is used twice, and nothing is written unless all ten are there.
  bool group = barrier.ok && added.size() + barrier.effects.size() <= PortRoomEffects::kMaxAddOns;
  if (barrier.ok && !group) {
    barrier.ok = false;
    barrier.why = "the add-on id range is full";
  }
  if (group) {
    PortRoomEffects::Layer& layer = layers[barrier.layer];
    layer.layer = barrier.layer;
    for (size_t i = 0; i < barrier.effects.size(); ++i) {
      PortRoomEffects::Effect e = barrier.effects[i];
      e.id = PortRoomEffects::AddOnId(barrier.layer, areaBits, uint32_t(added.size() + i));
      barrier.ids[i] = e.id;
      PortRoomEffects::AppendEffect(e, layer.objects);
      layer.vars.push_back(std::move(barrier.vars[i]));
      ++layer.count;
    }
  }
  // A barrier that was not written leaves its ten actors out, as connected ones are: say so.
  if (!group) {
    connectedCount += reserved;
  }
  // A retail effect no Remastered entity matched, that nothing sends a message to, next to an
  // added one: Remastered replaced it.
  size_t replacedNear = 0;
  for (size_t j = 0; j < objects.size(); ++j) {
    const ScriptObject& o = objects[j];
    if (o.type != kRetailEffect || !o.hasPos || matchedObjects.count(j) != 0 || skips.count(o.id) != 0 ||
        targeted.count(o.id & 0x3FFFFFF) != 0) {
      continue;
    }
    for (const Vec3& w : added) {
      if (Distance(w, o.pos) < kReplaceDistance) {
        skips.insert(o.id);
        ++replacedNear;
        break;
      }
    }
  }
  char barrierNote[256] = "";
  if (mrea == PortQueenBarrier::kMrea) {
    if (group) {
      std::snprintf(barrierNote, sizeof barrierNote, ", the barrier's 10 effects at %.4g s",
                    static_cast<double>(barrier.duration));
    } else {
      std::snprintf(barrierNote, sizeof barrierNote, ", no barrier group (%s)",
                    barrier.why.empty() ? "not the barrier's room" : barrier.why.c_str());
    }
  }
  std::snprintf(line, sizeof line,
                "  %s: %zu effect actor(s) added, %zu retail effect(s) replaced, %zu retail effect(s) take "
                "Remastered's effect, %zu connected and left out, %zu matched a retail object (%zu of them still on "
                "the retail PART), %zu without a PART, %zu on an unknown layer%s%s%s",
                name.c_str(), added.size(), replacedNear, overridden, connectedCount, kept + keptOwn + overridden,
                keptOwn, noPart, noLayer, full != 0 ? ", the id range is full" : "",
                badVars != 0 ? ", unreadable particle variables" : "", barrierNote);
  if (added.empty() && overrides.empty() && skips.empty() && !group) {
    return line;
  }
  PortRoomEffects::File file;
  file.skips.assign(skips.begin(), skips.end());
  file.connected = uint32_t(connectedCount);
  for (auto& [index, layer] : layers) {
    file.layers.push_back(std::move(layer));
  }
  file.overrides = std::move(overrides);
  if (group) {
    PortRoomEffects::BarrierGroup barrierGroup;
    barrierGroup.controller = PortQueenBarrier::kController;
    barrierGroup.duration = barrier.duration;
    bool guids = true;
    for (size_t r = 0; r < barrierGroup.rings.size(); ++r) {
      PortRoomEffects::BarrierRing& ring = barrierGroup.rings[r];
      const auto& row = PortQueenBarrier::kRings[r];
      ring.platform = row.platform;
      ring.refraction = barrier.ids[2 * r];
      ring.glow = barrier.ids[2 * r + 1];
      Id16 guid{};
      guids = guids && ParseGuid(row.breakupRefraction, guid);
      ring.breakupRefraction = guid;
      guids = guids && ParseGuid(row.motion, guid);
      ring.motion = guid;
      guids = guids && ParseGuid(row.breakupGlow, guid);
      ring.breakupGlow = guid;
    }
    // The members are the retail objects the room's own script keeps: none of them is one this
    // file leaves out or rewrites.
    bool listed = false;
    for (size_t r = 0; r < barrierGroup.rings.size(); ++r) {
      for (uint32_t member : {barrierGroup.controller, barrierGroup.rings[r].platform}) {
        listed = listed || skips.count(member) != 0;
        for (const PortRoomEffects::Override& over : overrides) {
          listed = listed || over.id == member;
        }
      }
    }
    if (guids && !listed) {
      file.barriers.push_back(barrierGroup);
    }
  }
  char fileName[32];
  std::snprintf(fileName, sizeof fileName, "%08X.roomfx", mrea);
  if (!io.write(fileName, PortRoomEffects::Write(file))) {
    return "  " + name + ": could not write " + fileName;
  }
  return line;
}

std::vector<std::array<uint8_t, 16>> RoomEffectGenpsOf(const Room& room) {
  std::vector<std::array<uint8_t, 16>> out;
  const std::set<int> connected = ConnectedEntities(room);
  for (const Component* c : room.Of(kEffectMP1)) {
    if (c->entity < 0 || connected.count(c->entity) != 0) {
      continue;
    }
    const auto f = room.Flat(*c);
    const auto genp = f.find(kPropGenp);
    if (genp != f.end() && genp->second.size == 16) {
      out.push_back(SwapUuid(room.Bytes(genp->second)));
    }
  }
  // The barrier's ten effects, once its whole graph validates: the only connected actors the
  // import admits this way (its controller drives them, one ring at a time).
  BarrierGraph graph;
  if (BarrierGraphOf(room, graph)) {
    for (const BarrierRingRefs& ring : graph.rings) {
      for (size_t comp : {ring.refraction, ring.glow}) {
        out.push_back(EffectGenpOf(room, comp));
      }
    }
  }
  return out;
}

}  // namespace RoomDetail

std::vector<std::array<uint8_t, 16>> RoomEffectGenps(const std::vector<RoomPak>& rooms) {
  std::vector<std::array<uint8_t, 16>> out;
  std::set<std::array<uint8_t, 16>> seen;
  for (const RoomPak& rp : rooms) {
    if (rp.pak == nullptr) {
      continue;
    }
    for (const PakAsset& asset : rp.pak->Assets()) {
      if (asset.type != port::FourCC("ROOM")) {
        continue;
      }
      std::vector<uint8_t> bytes;
      std::string error;
      RoomDetail::Room room;
      if (!rp.pak->ReadAsset(asset, bytes, error) || !room.Parse(bytes, error)) {
        continue;
      }
      for (const auto& genp : RoomDetail::RoomEffectGenpsOf(room)) {
        if (seen.insert(genp).second) {
          out.push_back(genp);
        }
      }
      break;
    }
  }
  return out;
}

}  // namespace PortRemastered
