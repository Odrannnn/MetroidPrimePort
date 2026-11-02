// Remastered's own particle effect actors as a room's .roomfx (port_room_effects.h).

#include "port_remastered_room_internal.h"
#include "port_queen_barrier_assets.h"
#include "port_room_effects.h"
#include "port_maya_spline.h"

#include <iterator>

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

constexpr double kDeg = 3.14159265358979323846 / 180.0;
// Remastered's local axis that GameCube local axis j is (column j of RetailFrame's matrix).
constexpr int kAxis[3] = {0, 2, 1};

// Rz * Ry * Rx of Remastered's angles in GameCube axes (the room geometry's `place`), then
// the area's rotation: column j is where Remastered's local axis kAxis[j] points in the world.
void RetailFrame(const Vec3& rot, const Mat34& xf, double w[3][3]) {
  constexpr double kSign[3] = {-1, 1, 1};
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
  double g[3][3];
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
}

// RetailFrame as retail's editor angles in degrees.
void RetailAngles(const Vec3& rot, const Mat34& xf, float out[3]) {
  double w[3][3];
  RetailFrame(rot, xf, w);
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

// AmbientParticleEffect (kb topic/ambient-particle-effect.md): SLdrAmbientParticleEffect::Load
// 0x4b8624 and its constructor's defaults 0x4b8448.
constexpr uint32_t kAmbientParticleEffect = 0xcbe2042d;
constexpr uint32_t kEffectProxyMP1 = 0xe12aae70;
constexpr uint32_t kPropAmbientShapes = 0xa411bbc7;   // the shapes the points must fall in
constexpr uint32_t kPropAmbientDensity = 0x7fbe0d2c;  // +0xd8
constexpr uint32_t kPropAmbientLife = 0x89d67bfb;     // +0xdc
constexpr uint32_t kPropAmbientSpawn = 0x01ed34eb;    // the camera-space sphere the points come from
constexpr uint32_t kPropAmbientWeights = 0xab7ac437;  // per link index
constexpr uint32_t kLinkAmbientEmitter = 0x2c283df0;
constexpr uint32_t kShapeBox = 0xcef3fcf1;       // COBBoxShapeData
constexpr uint32_t kShapeCylinder = 0x06c7e0ae;  // CCappedCylinderShapeData
constexpr uint32_t kShapeSphere = 0x6ede23e6;    // CSphereShapeData

// One typed shape (u32 type, u16 size, property list) at `b`; `used` its size.
struct ShapeProps {
  uint32_t type = 0;
  std::map<uint32_t, std::pair<const uint8_t*, size_t>> props;
};
bool ReadShape(const uint8_t* b, size_t n, ShapeProps& out, size_t& used) {
  if (n < 6 || n - 6 < ReadLE16(b + 4)) {
    return false;
  }
  out.type = ReadLE32(b);
  const size_t size = ReadLE16(b + 4);
  std::vector<Prop> props;
  if (!PropList(b + 6, size, &props)) {
    return false;
  }
  for (const Prop& p : props) {
    out.props[p.id] = {b + 6 + p.data.start, p.data.size};
  }
  used = 6 + size;
  return true;
}
bool ShapeVec(const ShapeProps& s, uint32_t id, const Vec3& fallback, Vec3& out) {
  const auto it = s.props.find(id);
  if (it == s.props.end()) {
    out = fallback;
    return true;
  }
  if (it->second.second != 12) {
    return false;
  }
  for (size_t k = 0; k < 3; ++k) {
    out[k] = ReadLEFloat(it->second.first + 4 * k);
  }
  return true;
}
bool ShapeReal(const ShapeProps& s, uint32_t id, double fallback, double& out) {
  const auto it = s.props.find(id);
  if (it == s.props.end()) {
    out = fallback;
    return true;
  }
  if (it->second.second != 4) {
    return false;
  }
  out = ReadLEFloat(it->second.first);
  return true;
}

// The ambient record of an AmbientParticleEffect, its emitters the effects (`effectOf`, by
// entity) of the EffectProxyMP1s it links; false with `why` when it cannot be exact.
bool AmbientRecord(const Room& room, const Component& c, const Mat34& xf, const std::map<int, uint32_t>& effectOf,
                   PortRoomEffects::Ambient& out, const char*& why) {
  const std::vector<Component>& comps = room.Components();
  out = PortRoomEffects::Ambient{};
  why = "unreadable";
  Vec3 pos, rot, scale;
  if (!room.Xform(c, pos, rot, scale)) {
    return false;
  }
  // The shapes are in the entity's space, which is the room's in every one shipped.
  for (size_t k = 0; k < 3; ++k) {
    if (pos[k] != 0.0 || rot[k] != 0.0 || scale[k] != 1.0) {
      why = "a placed entity";
      return false;
    }
  }
  const auto own = effectOf.find(c.entity);
  if (own == effectOf.end()) {
    why = "its effect not added";
    return false;
  }
  out.owner = own->second;
  const auto f = room.Flat(c);
  const auto real = [&](uint32_t id, float fallback, float& v) {
    const auto it = f.find(id);
    if (it == f.end()) {
      v = fallback;
      return true;
    }
    v = it->second.size == 4 ? ReadLEFloat(room.Bytes(it->second)) : 0.f;
    return it->second.size == 4;
  };
  float density = 0.f, life = 0.f;
  if (!real(kPropAmbientDensity, 1e-5f, density) || !real(kPropAmbientLife, 4.f, life)) {
    return false;
  }
  // The spawn sphere (CAmbientParticleEffectGOC::Init): its volume, in float as the game has it.
  const auto spawn = f.find(kPropAmbientSpawn);
  ShapeProps sphere;
  size_t used = 0;
  Vec3 center{};
  double radius = 0;
  if (spawn == f.end() || !ReadShape(room.Bytes(spawn->second), spawn->second.size, sphere, used) ||
      used != spawn->second.size || sphere.type != kShapeSphere || !ShapeVec(sphere, 0x1d62d49d, {}, center) ||
      !ShapeReal(sphere, 0x9988156a, 0.5, radius)) {
    why = "no spawn sphere";
    return false;
  }
  const float r = float(radius);
  const float volume = ((r * 1.3333334f) * r) * r * 3.1415927f;
  out.rate = std::fabs(life) < 1e-5f ? 0.f : volume * density / life;
  const Vec3 offset = MulR2G(center);
  for (size_t k = 0; k < 3; ++k) {
    out.offset[k] = float(offset[k]);
  }
  out.radius = r;
  const auto weights = f.find(kPropAmbientWeights);
  if (weights == f.end() || weights->second.size < 4) {
    why = "no weights";
    return false;
  }
  const uint8_t* wb = room.Bytes(weights->second);
  const uint32_t count = ReadLE32(wb);
  if (count > PortRoomEffects::kMaxAmbientWeights || weights->second.size != 4 + 4 * size_t(count)) {
    why = "bad weights";
    return false;
  }
  for (uint32_t k = 0; k < count; ++k) {
    out.weights.push_back(int32_t(ReadLE32(wb + 4 + 4 * k)));
  }
  // SpawnParticlesAtPositions 0x4bb0a0: every proxy linked with the index, a link to an entity
  // meaning its proxies (CLinkedComponentRange::next_item 0x47bb2c). The link data is an empty
  // property list in every one shipped: index 0.
  for (const EntityLink& l : ReadEntityLinkData(room, size_t(&c - comps.data()))) {
    if (l.id != kLinkAmbientEmitter) {
      continue;
    }
    if (l.payload.size != 2 || ReadLE16(room.Bytes(l.payload)) != 0 || l.target < 0) {
      why = "an unknown link";
      return false;
    }
    std::vector<int> proxies;
    if (comps[size_t(l.target)].type == kEffectProxyMP1) {
      proxies.push_back(l.target);
    } else if (comps[size_t(l.target)].type == kEntity) {
      for (size_t k = 0; k < comps.size(); ++k) {
        if (comps[k].type == kEffectProxyMP1 && comps[k].entity == l.target) {
          proxies.push_back(int(k));
        }
      }
    }
    for (int p : proxies) {
      const auto effect = effectOf.find(comps[size_t(p)].entity);
      if (effect == effectOf.end()) {
        why = "a linked effect not added";
        return false;
      }
      if (out.emitters.size() >= PortRoomEffects::kMaxAmbientEmitters) {
        why = "too many links";
        return false;
      }
      out.emitters.push_back({0, effect->second});
    }
  }
  // With no weights nothing is ever spawned (0x4bb0a0).
  if (out.emitters.empty() || out.weights.empty()) {
    why = "no emitter";
    return false;
  }
  const auto shapes = f.find(kPropAmbientShapes);
  if (shapes == f.end() || shapes->second.size < 4) {
    why = "no shapes";
    return false;
  }
  const uint8_t* sb = room.Bytes(shapes->second);
  const size_t sn = shapes->second.size;
  const uint32_t shapeCount = ReadLE32(sb);
  if (shapeCount > PortRoomEffects::kMaxAmbientShapes) {
    why = "too many shapes";
    return false;
  }
  size_t at = 4;
  for (uint32_t k = 0; k < shapeCount; ++k) {
    ShapeProps s;
    if (!ReadShape(sb + at, sn - at, s, used)) {
      return false;
    }
    at += used;
    PortRoomEffects::AmbientShape shape;
    Vec3 sc{}, sr{}, ext{};
    double sradius = 0, height = 0;
    bool ok;
    if (s.type == kShapeBox) {
      shape.type = PortRoomEffects::kAmbientBox;
      ok = ShapeVec(s, 0x7199772e, {}, sc) && ShapeVec(s, 0x2a65c84b, {}, sr) &&
           ShapeVec(s, 0xd4fe58a9, {1, 1, 1}, ext);
    } else if (s.type == kShapeCylinder) {
      shape.type = PortRoomEffects::kAmbientCylinder;
      ok = ShapeVec(s, 0x4a515a01, {}, sc) && ShapeVec(s, 0x25d57772, {}, sr) &&
           ShapeReal(s, 0x042a3629, 0.5, sradius) && ShapeReal(s, 0x5abbfd19, 1.0, height);
    } else if (s.type == kShapeSphere) {
      shape.type = PortRoomEffects::kAmbientSphere;
      ok = ShapeVec(s, 0x1d62d49d, {}, sc) && ShapeReal(s, 0x9988156a, 0.5, sradius);
    } else {
      why = "an unknown shape";
      return false;
    }
    if (!ok) {
      return false;
    }
    const Vec3 wc = Apply(xf, MulR2G(sc));
    double w[3][3];
    RetailFrame(sr, xf, w);
    for (size_t i = 0; i < 3; ++i) {
      shape.center[i] = float(wc[i]);
      for (size_t j = 0; j < 3; ++j) {
        shape.axes[j][i] = float(w[i][j]);
      }
    }
    if (shape.type == PortRoomEffects::kAmbientBox) {
      for (size_t j = 0; j < 3; ++j) {
        shape.half[j] = float(std::fabs(ext[size_t(kAxis[j])]) * 0.5);
      }
    } else if (shape.type == PortRoomEffects::kAmbientCylinder) {
      // Its axis is Remastered's local Y, GameCube's local Z.
      for (size_t i = 0; i < 3; ++i) {
        shape.axes[0][i] = float(w[i][2]);
      }
      shape.half[0] = float(std::fabs(height) * 0.5);
      shape.half[1] = float(std::fabs(sradius));
    } else {
      shape.half[0] = float(std::fabs(sradius));
    }
    out.shapes.push_back(shape);
  }
  if (at != sn) {
    return false;
  }
  return true;
}

// The entities with an EffectMP1 component.
std::set<int> EffectEntities(const Room& room) {
  std::set<int> out;
  for (const Component* c : room.Of(kEffectMP1)) {
    if (c->entity >= 0) {
      out.insert(c->entity);
    }
  }
  return out;
}

// A connection addressed to another component of an effect's entity (a Boolean's c53a2e78 to
// its AmbientParticleEffect) with an action neither CEffectMP1GOC nor IMP1EntityBaseGOC
// translates (0xd9c9e4, 0xc51ea4: 0xffff, no message) never reaches the effect, so it neither
// connects nor drives it. Nor does one on Default, which is never sent (NeverSent).
bool MissesEffect(const Room& room, const std::set<int>& effects, const Connection& c, int t) {
  const Component& target = room.Components()[size_t(t)];
  if (NeverSent(room.Components()[c.sender].type, c.event)) {
    return true;
  }
  return target.type != kEntity && target.type != kEffectMP1 && effects.count(target.entity) != 0 &&
         EffectAct(c.action) == 0 && !IsEntityBaseAction(c.action);
}

}  // namespace

std::set<int> ConnectedEntities(const Room& room) {
  const std::vector<Component>& comps = room.Components();
  const std::set<int> effects = EffectEntities(room);
  std::set<int> out;
  for (const Connection& c : ReadConnections(room)) {
    const int t = room.ByGuid(c.target);
    if (t >= 0 && !MissesEffect(room, effects, c, t)) {
      out.insert(comps[size_t(t)].type == kEntity ? t : comps[size_t(t)].entity);
    }
  }
  return out;
}

std::set<int> DrivableEntities(const Room& room) {
  const std::vector<Component>& comps = room.Components();
  const std::set<int> effects = EffectEntities(room);
  std::set<int> connected, refused;
  for (const Connection& c : ReadConnections(room)) {
    const int t = room.ByGuid(c.target);
    if (t < 0 || MissesEffect(room, effects, c, t)) {
      continue;
    }
    const int entity = comps[size_t(t)].type == kEntity ? t : comps[size_t(t)].entity;
    connected.insert(entity);
    // An ObjectFollowLocator's Play names what it follows and moves (a follow), never sent. The
    // events SpecialFunctionSends knows are retail's or never fire; the walk sorts out which.
    if (comps[c.sender].type == kSpecialFunction &&
        !(c.event == kEventSpecialFunctionPlay && IsFollowLocator(room, comps[c.sender])) &&
        SpecialFunctionSends(room, comps[c.sender], c.event, true) == SpecialFunctionSend::kUnknown &&
        SpecialFunctionSends(room, comps[c.sender], c.event, false) == SpecialFunctionSend::kUnknown) {
      refused.insert(entity);
    }
  }
  std::set<int> out;
  std::set_difference(connected.begin(), connected.end(), refused.begin(), refused.end(),
                      std::inserter(out, out.end()));
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

// PatternedInfo (SLdrPatterned_MP1Typedef::Load; NTypedefToMP1Type converts it at 0xd1c2a0 in
// CPatternedInfo's order). Each enemy nests it under a property of its own type, so it is found
// by what it holds: the death particle and the health info.
constexpr uint32_t kPatternedParticle1 = 0x88df29de;       // x11c_particle1
constexpr uint32_t kPatternedParticle1Scale = 0xd1de58ff;  // x110_particle1Scale
constexpr uint32_t kPatternedParticle2 = 0x5f145600;       // x130_particle2
constexpr uint32_t kPatternedParticle2Scale = 0x558c7e4b;  // x124_particle2Scale
constexpr uint32_t kPatternedHealth = 0x7cee8c14;
struct PatternedSlotProps {
  uint32_t particle;
  uint32_t offset;
};
constexpr PatternedSlotProps kPatternedSlotProps[PortRoomEffects::kPatternedSlots] = {
    {kPatternedParticle1, kPatternedParticle1Scale}, {kPatternedParticle2, kPatternedParticle2Scale}};

// The component's PatternedInfo property list (absolute spans), or false.
bool PatternedInfoOf(const Room& room, const Component& c, std::map<uint32_t, Span>& out) {
  std::vector<Prop> props;
  if (!PropList(room.Bytes(c.raw), c.raw.size, &props)) {
    return false;
  }
  for (const Prop& p : props) {
    const Span at{c.raw.start + p.data.start, p.data.size};
    std::vector<Prop> sub;
    if (!PropList(room.Bytes(at), at.size, &sub)) {
      continue;
    }
    bool particle = false, health = false;
    for (const Prop& s : sub) {
      particle = particle || s.id == kPatternedParticle1;
      health = health || s.id == kPatternedHealth;
    }
    if (particle && health) {
      out.clear();
      for (const Prop& s : sub) {
        out[s.id] = {at.start + s.data.start, s.data.size};
      }
      return true;
    }
  }
  return false;
}

// The PatternedInfo slot's GENP in pak order; false when it is absent or names none.
bool PatternedGenp(const Room& room, const std::map<uint32_t, Span>& info, uint32_t slot, Id16& out) {
  Id16 raw{};
  if (!Property16(room, info, kPatternedSlotProps[slot].particle, raw) || raw == Id16{}) {
    return false;
  }
  out = SwapUuid(raw.data());
  return true;
}

// Remastered GENP properties of a component whose retail object names a PART at a fixed
// offset into its properties (after the name): the slot found by intersecting, over every
// placement whose GENP already pairs with a retail PART, the offsets where the retail object
// holds that PART (build/mpr/kb topic unpaired-retail-particles).
struct PartSlot {
  uint32_t component;
  uint32_t path[4];  // property ids from the component down, zero-terminated
  uint8_t retailType;
  uint32_t offset;
};
constexpr uint32_t kMetroidPrimeRelayMP1 = 0x622f6b23;
// SteamMP1 (retail Steam, 0x46) and the SLdrSteamMP1 properties port_room_effects.h Steam takes.
constexpr uint32_t kSteamMP1 = 0xfc64463e;
constexpr uint32_t kPropSteamColor = 0xfaa2682b;
constexpr uint32_t kPropSteamGenp = 0x4054322b;
constexpr uint32_t kPropSteamSpline1 = 0x8030d7a6;
constexpr uint32_t kPropSteamSpline2 = 0x911d77bd;
constexpr uint32_t kPropSteamA = 0xd560318d;
constexpr uint32_t kPropSteamB = 0x14e3228a;
constexpr uint32_t kPropSteamG = 0x5e432bd2;
constexpr uint32_t kPropSteamH = 0x67d639e2;
constexpr uint32_t kPropSteamI = 0x6b6d170d;
constexpr uint32_t kPropSteamColorFlag = 0x1c983b7f;
// The link CScriptSteamMP1 averages its source position over (its constructor's EScriptLink).
constexpr uint32_t kSteamSourceLink = 0x2ae68d05;
constexpr uint32_t kPrimeData = 0x7e3d43f5;  // SLdrMetroidPrimeData_MP1Typedef
constexpr PartSlot kPartSlots[] = {
    // CScriptWater: splash particles 1-3, visor runoff, unmorph visor runoff
    // (ScriptLoader::LoadWater). 89d688f5 has no paired placement: it is the fifth of
    // CScriptWaterMP1's ten CObjectId arguments (CWaterMP1GOC::Create), after the four above.
    {kWaterMP1, {0xe4c5ecf1}, 0x20, 252},
    {kWaterMP1, {0x6c79eb4d}, 0x20, 256},
    {kWaterMP1, {0xb829b092}, 0x20, 260},
    {kWaterMP1, {0x0e9a68df}, 0x20, 264},
    {kWaterMP1, {0x89d688f5}, 0x20, 268},
    {0xcb5682c4, {0x58e230a7}, 0x53, 12},   // VisorGooMP1: particle (LoadVisorGoo)
    {0x1c3d435d, {0x8f72b975}, 0x7f, 522},  // BurrowerMP1
    // ThardusMP1: particle4 and particle7 (ScriptLoader::LoadThardus), by SLdr slot order
    // between the paired particles.
    {0xeaa40b01, {0x34af0446}, 0x58, 556},
    {0xeaa40b01, {0x04788e48}, 0x58, 568},
    {0x6cfa6902, {0x3ad31b32}, 0x25, 558},  // FlyingPirateMP1: the particle after its three weapons
    // ElitePirateMP1 and OmegaPirateMP1 (CElitePirateData): grenade element gens 1-3, and
    // Omega's launcher particle.
    {0xf4326a02, {0x54eef7aa}, 0x26, 695},
    {0xf4326a02, {0x6903955e}, 0x26, 699},
    {0xf4326a02, {0xe32acfab}, 0x26, 703},
    {0x7f2845c7, {0x63903958}, 0x86, 659},
    {0x7f2845c7, {0x50025ca7}, 0x86, 703},
    // AtomicBetaMP1: the electric effect (ELC2 -> ELSC, LoadAtomicBeta's electricId), ahead
    // of weaponId and the paired particleId. Two different ELC2s take the one retail ELSC by placement.
    {0x87412090, {0xb460f7fc}, 0x77, 482},
    // GunTurretMP1 (retail GunTurret, CScriptGunTurretData x60-x74): the five GENPs
    // CScriptGunTurretMP1 builds element gens from, by its UpdateGunParticles states: idle,
    // deactivate and targetting lights, charging and panning effects. Remastered has no frozen
    // effect slot (x6c); its visor effect (x78) is an ELC2 and has no PART to take.
    {0x1704c1f0, {0x72151d8c}, 0x64, 410},
    {0x1704c1f0, {0xdfdca845}, 0x64, 414},
    {0x1704c1f0, {0x8648f4bd}, 0x64, 418},
    {0x1704c1f0, {0x87cf241f}, 0x64, 426},
    {0x1704c1f0, {0x2107615d}, 0x64, 430},
    // DroneMP1: the laser particles (SLdr +0x18, CDroneMP1 ctor argument 11; retail aId1,
    // x568_laserParticlesId).
    {0x50b7576d, {0x067eb238}, 0x43, 534},
    // MetroidPrimeRelayMP1's CMetroidPrimeData (CMetroidPrimeRelay.cpp), by the
    // CMetroidPrimeDataMP1 ctor and the SLdr typedef converters: ice attack particles 1-3,
    // x460_particle1, beam 0's contact and pulse effects, beam 2's fourth weapon asset, beam
    // 3's contact effect, the poison cloud's particle, particles 3 and 4. Left out: beam
    // asset slots the retail object leaves unset or fills with an ELSC, and the SWHC swoosh.
    {kMetroidPrimeRelayMP1, {kPrimeData, 0x656af105, 0xb23c036d}, 0x84, 1150},
    {kMetroidPrimeRelayMP1, {kPrimeData, 0x656af105, 0x1cf59892}, 0x84, 1154},
    {kMetroidPrimeRelayMP1, {kPrimeData, 0x656af105, 0x18ece050}, 0x84, 1158},
    {kMetroidPrimeRelayMP1, {kPrimeData, 0x1d7a1ccc}, 0x84, 1202},
    {kMetroidPrimeRelayMP1, {kPrimeData, 0x6a5a1bfe, 0x336ca0cf, 0x26e11820}, 0x84, 1214},
    {kMetroidPrimeRelayMP1, {kPrimeData, 0x6a5a1bfe, 0x336ca0cf, 0x5cd90d50}, 0x84, 1218},
    {kMetroidPrimeRelayMP1, {kPrimeData, 0x6e48d90d, 0x0d0e3482, 0xf4d3d594}, 0x84, 1690},
    {kMetroidPrimeRelayMP1, {kPrimeData, 0x728f76b5, 0x336ca0cf, 0x26e11820}, 0x84, 1742},
    {kMetroidPrimeRelayMP1, {kPrimeData, 0x77e825d1, 0x79d26b17}, 0x84, 2178},
    {kMetroidPrimeRelayMP1, {kPrimeData, 0x047ca031}, 0x84, 2358},
    {kMetroidPrimeRelayMP1, {kPrimeData, 0x31f3a3f5}, 0x84, 2362},
};

// Remastered effect properties with no retail field (port_room_effects.h ActorSlotEffect): the
// component, the property path, the paired retail object's type and the slot.
struct ActorSlot {
  uint32_t component;
  uint32_t path[4];
  uint8_t retailType;
  uint32_t slot;
};
constexpr ActorSlot kActorSlotTable[] = {
    // MetroidPrimeRelayMP1: beams 0-3's electric (CMetroidPrimeMP1 +0x12ac, the beam's 5d9bd2fe).
    {kMetroidPrimeRelayMP1, {kPrimeData, 0x6a5a1bfe, 0x5d9bd2fe}, 0x84, PortRoomEffects::kActorSlotExoBeamElectric},
    {kMetroidPrimeRelayMP1, {kPrimeData, 0x194f407d, 0x5d9bd2fe}, 0x84, PortRoomEffects::kActorSlotExoBeamElectric + 1},
    {kMetroidPrimeRelayMP1, {kPrimeData, 0x6e48d90d, 0x5d9bd2fe}, 0x84, PortRoomEffects::kActorSlotExoBeamElectric + 2},
    {kMetroidPrimeRelayMP1, {kPrimeData, 0x728f76b5, 0x5d9bd2fe}, 0x84, PortRoomEffects::kActorSlotExoBeamElectric + 3},
    // GunTurretMP1: the visor effect, an ELC2 or a GENP (retail's visor PART, props 434).
    {0x1704c1f0, {0xde43a94b}, 0x64, PortRoomEffects::kActorSlotGunTurretVisorEffect},
    // DroneMP1: the scanner effect (CDroneMP1 +0x818), which retail draws with a LaserLight.
    {0x50b7576d, {0xd6a76aad}, 0x43, PortRoomEffects::kActorSlotDroneScannerEffect},
};

// The GENP (or other effect) a property path holds, zero when unset or absent. flat caches
// room.Flat(c).
Id16 SlotGenp(const Room& room, const Component& c, const uint32_t (&path)[4], std::map<uint32_t, Span>& flat) {
  size_t depth = 0;
  while (depth < std::size(path) && path[depth] != 0) {
    ++depth;
  }
  Span at{};
  if (depth == 1) {
    if (flat.empty()) {
      flat = room.Flat(c);
    }
    const auto found = flat.find(path[0]);
    if (found == flat.end()) {
      return {};
    }
    at = found->second;
  } else if (!room.Nested(c, path, depth, at)) {
    return {};
  }
  return at.size == 16 ? SwapUuid(room.Bytes(at)) : Id16{};
}

}  // namespace

namespace {
// One pass of WriteRoomEffects. An effect sender (kSenderEffect) in notSenders is taken as one
// that is not added; one a drive names that is not added after all goes in missing, and then
// nothing is written.
std::string WriteRoomEffectsOnce(const std::string& name, const Room& room, uint32_t mrea, const Area& area,
                                 const SceneryScripts& scripts, const RoomIO& io, const std::set<int>& notSenders,
                                 std::set<int>& missing) {
  const std::vector<const Component*> effects = room.Of(kEffectMP1);
  if (!io.effect || !io.write) {
    return {};
  }
  // Script objects with a PatternedInfo the matcher paired with a retail object: their death
  // particles.
  std::vector<std::pair<const ScriptObject*, std::map<uint32_t, Span>>> enemies;
  for (const Component& c : room.Components()) {
    std::map<uint32_t, Span> info;
    const auto matched = c.entity >= 0 ? scripts.retail.find(c.entity) : scripts.retail.end();
    if (matched != scripts.retail.end() && matched->second < area.objects.size() &&
        PatternedInfoOf(room, c, info)) {
      enemies.emplace_back(&area.objects[matched->second], std::move(info));
    }
  }
  const bool slotted = std::any_of(room.Components().begin(), room.Components().end(), [](const Component& c) {
    return std::any_of(std::begin(kPartSlots), std::end(kPartSlots),
                       [&c](const PartSlot& s) { return s.component == c.type; });
  });
  if (effects.empty() && enemies.empty() && !slotted) {
    return {};
  }
  char line[1024];
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
  std::map<int, uint32_t> addedIds;  // the effects added, by entity
  std::set<int> seen;
  std::map<std::string, size_t> connectedWhy;  // connected ones left out, by cause
  std::map<std::string, std::string> connectedWho;  // their entities' guids, by cause (MP_REMASTERED_MISS)
  size_t connectedCount = 0, noPart = 0, noLayer = 0, kept = 0, keptOwn = 0, overridden = 0, full = 0, badVars = 0;
  size_t drivenCount = 0;  // connected ones added, with drives
  std::vector<PortRoomEffects::Drive> allDrives;
  const std::set<int> drivable = DrivableEntities(room);
  // The Remastered timers that can be retail CScriptTimer add-ons (PortRoomEffects::Timer): every
  // connection into them traced to retail messages, on a known layer, and every timer they hear
  // from one too (until nothing more drops out). The same for the TriggerMP1s that can be retail
  // CScriptTrigger add-ons (PortRoomEffects::Trigger), which timers may drive and be driven by, and
  // for the ScriptRelayMP1s that become retail CScriptRelay add-ons (PortRoomEffects::Relay).
  std::set<int> timersOk, triggersOk, relaysOk;
  const auto addOnOk = [&](int entity, bool baseLayer, const std::vector<PortRoomGeo::Link>& links) {
    return scripts.unresolved.count(entity) == 0 && (baseLayer || scripts.layer.count(entity) != 0) &&
           std::all_of(links.begin(), links.end(), [](const PortRoomGeo::Link& l) {
             return (l.action & kLinkMessage) != 0 && l.delay >= 0.f && l.delay <= PortRoomEffects::kMaxDriveDelay;
           });
  };
  for (const auto& [t, timer] : scripts.timers) {
    if (addOnOk(t, timer.baseLayer, timer.links)) {
      timersOk.insert(t);
    }
  }
  for (const auto& [t, trigger] : scripts.triggers) {
    if (addOnOk(t, trigger.baseLayer, trigger.links)) {
      triggersOk.insert(t);
    }
  }
  for (const auto& [r, relay] : scripts.relays) {
    if (addOnOk(r, relay.baseLayer, relay.links)) {
      relaysOk.insert(r);
    }
  }
  const auto addOnSenderOk = [&](const PortRoomGeo::Link& l) {
    return IsTimerSender(l.sender)     ? timersOk.count(int(l.sender & ~kSenderTimer)) != 0
           : IsTriggerSender(l.sender) ? triggersOk.count(int(l.sender & ~kSenderTrigger)) != 0
           : IsRelaySender(l.sender)   ? relaysOk.count(int(l.sender & ~kSenderRelay)) != 0
           : IsEffectSender(l.sender)  ? notSenders.count(int(l.sender & ~kSenderEffect)) == 0
                                       : true;
  };
  for (bool dropped = true; dropped;) {
    dropped = false;
    const auto prune = [&](std::set<int>& ok, const auto& addOns) {
      for (auto it = ok.begin(); it != ok.end();) {
        const std::vector<PortRoomGeo::Link>& links = addOns.at(*it).links;
        if (std::all_of(links.begin(), links.end(), addOnSenderOk)) {
          ++it;
        } else {
          it = ok.erase(it);
          dropped = true;
        }
      }
    };
    prune(timersOk, scripts.timers);
    prune(triggersOk, scripts.triggers);
    prune(relaysOk, scripts.relays);
  }
  // The GeneratorMP1s that copy effects, kept where they are exact (PortRoomEffects::Generator):
  // every connection into them traced, one template, no Follow targets, a spawn count in range.
  struct GeneratorPlan {
    bool ok = false;
    PortRoomEffects::Generator record;  // templates filled as the effect is added
    uint32_t count = 1;
    int index = -1;  // in generators, once written
    const char* why = "its links";  // why it is not exact, for the log
  };
  std::map<int, GeneratorPlan> generatorPlans;  // by generator entity
  for (const auto& [effect, by] : scripts.generatedBy) {
    for (int g : by) {
      generatorPlans[g];
    }
  }
  if (!generatorPlans.empty()) {
    const std::vector<Component>& comps = room.Components();
    const std::vector<Connection> conns = ReadConnections(room);
    for (auto& [g, plan] : generatorPlans) {
      const Component* comp = nullptr;
      for (const Component* c : room.Of(kGeneratorMP1)) {
        if (c->entity == g) {
          comp = c;
          break;
        }
      }
      const auto links = scripts.links.find(g);
      if (comp == nullptr || drivable.count(g) == 0 || links == scripts.links.end() || scripts.unresolved.count(g) != 0 ||
          !std::all_of(links->second.begin(), links->second.end(), [&](const PortRoomGeo::Link& l) {
            return (l.action == PortRoomGeo::kGenerate || l.action == PortRoomGeo::kShow ||
                    l.action == PortRoomGeo::kHide || l.action == PortRoomGeo::kToggle) &&
                   l.delay >= 0.f && l.delay <= PortRoomEffects::kMaxDriveDelay && addOnSenderOk(l);
          })) {
        plan.why = comp == nullptr ? "not a generator" : "its links";
        continue;
      }
      size_t generates = 0;
      bool follows = false, sound = false;
      for (const Connection& c : conns) {
        if (comps[c.sender].type != kGeneratorMP1 || comps[c.sender].entity != g || c.event != kEventGeneratorZero) {
          continue;
        }
        generates += c.action == kActionGenerate ? 1 : 0;
        // SetToZero (0xf442f8) always takes a SoundMP1 template over the others, as retail does.
        const int t = room.ByGuid(c.target);
        if (c.action == kActionGenerate && t >= 0) {
          const int te = comps[size_t(t)].type == kEntity ? t : comps[size_t(t)].entity;
          for (const Component* s : room.Of(kSoundMP1)) {
            sound = sound || s->entity == te;
          }
        }
        follows = follows || c.action == kActionGeneratorFollow;
      }
      const auto f = room.Flat(*comp);
      const auto count = f.find(kPropGeneratorCount);
      if (count != f.end()) {
        plan.count = count->second.size == 4 ? ReadLE32(room.Bytes(count->second)) : 0;
      }
      Vec3 offset{0, 0, 0};
      const auto off = f.find(kPropGeneratorOffset);
      if (off != f.end() && off->second.size == 12) {
        const uint8_t* b = room.Bytes(off->second);
        const Vec3 w = Apply(area.xf, MulR2G({ReadLEFloat(b), ReadLEFloat(b + 4), ReadLEFloat(b + 8)}));
        const Vec3 o = Apply(area.xf, Vec3{0, 0, 0});
        offset = {w[0] - o[0], w[1] - o[1], w[2] - o[2]};
      }
      const auto noInherit = f.find(kPropGeneratorNoInheritXf);
      plan.record.flags = (room.Active(*comp) ? PortRoomEffects::kGeneratorActive : 0u) |
                          (noInherit != f.end() && noInherit->second.size >= 1 && room.Bytes(noInherit->second)[0] != 0
                               ? PortRoomEffects::kGeneratorNoInheritXf
                               : 0u);
      for (int i = 0; i < 3; ++i) {
        plan.record.offset[i] = float(offset[size_t(i)]);
      }
      plan.ok = generates == 1 && !follows && plan.count >= 1 && plan.count <= PortRoomEffects::kMaxGeneratorTemplates &&
                std::isfinite(plan.record.offset[0]) && std::isfinite(plan.record.offset[1]) &&
                std::isfinite(plan.record.offset[2]);
      plan.why = generates != 1 && sound ? "a sound template wins"
                 : generates != 1        ? "not one generate"
                 : follows      ? "it follows"
                 : plan.ok      ? ""
                                : "its count or offset";
    }
  }
  // The unmatched ObjectFollowLocators that move effects, as retail SpecialFunctions where exact:
  // exactly two Play connections, Activate to an actor a retail object matched, Deactivate to the
  // effect (the messages their targets' GOCs translate the actions to), nothing connected to the
  // function itself (it never changes), on a known layer, with a locator name.
  struct FollowPlan {
    bool ok = false;
    int follower = -1;      // the effect's entity
    uint32_t followerId = 0;  // its retail or add-on id, once known
    PortRoomEffects::Follow record;
    uint32_t layer = 0;
    const char* why = "not a follow locator";  // why it is left out, for the log
  };
  std::map<int, FollowPlan> followPlans;  // by SpecialFunctionMP1 entity
  if (!scripts.followedBy.empty()) {
    const std::vector<Component>& comps = room.Components();
    const std::vector<Connection> conns = ReadConnections(room);
    const auto entityOf = [&](int t) { return comps[size_t(t)].type == kEntity ? t : comps[size_t(t)].entity; };
    for (const auto& [effect, by] : scripts.followedBy) {
      for (int sf : by) {
        followPlans[sf];
      }
    }
    for (auto& [sf, plan] : followPlans) {
      const Component* comp = nullptr;
      for (const Component* c : room.Of(kSpecialFunction)) {
        if (c->entity == sf && IsFollowLocator(room, *c)) {
          comp = c;
        }
      }
      if (comp == nullptr || scripts.retail.count(sf) != 0) {
        plan.why = comp == nullptr ? "not a follow locator" : "a retail object";
        continue;
      }
      int followed = -1, follower = -1;
      size_t activates = 0, deactivates = 0;
      bool exact = true;
      for (const Connection& c : conns) {
        const int t = room.ByGuid(c.target);
        if (t >= 0 && entityOf(t) == sf) {
          exact = false;  // something drives the function
        }
        if (comps[c.sender].entity != sf) {
          continue;
        }
        const uint32_t message = t < 0 ? 0 : FollowMessage(comps[size_t(t)].type, c.action);
        if (c.event != kEventSpecialFunctionPlay || message == 0) {
          exact = false;
          continue;
        }
        (message == 1 ? followed : follower) = entityOf(t);
        ++(message == 1 ? activates : deactivates);
        if (message == 4 && comps[size_t(t)].type != kEffectMP1) {
          exact = false;
        }
      }
      const auto matched = scripts.retail.find(followed);
      const auto layer = scripts.layer.find(sf);
      const auto f = room.Flat(*comp);
      const auto locator = f.find(kPropSpecialFunctionLocator);
      std::string name;
      if (!exact || activates != 1 || deactivates != 1 || matched == scripts.retail.end() ||
          (!comp->baseLayer && layer == scripts.layer.end()) || locator == f.end() || locator->second.size != 8 ||
          !room.String(ReadLE32(room.Bytes(locator->second)), ReadLE32(room.Bytes(locator->second) + 4), name)) {
        plan.why = !exact || activates != 1 || deactivates != 1          ? "other connections"
                   : matched == scripts.retail.end()                     ? "unmatched followed actor"
                   : !comp->baseLayer && layer == scripts.layer.end() ? "unknown layer"
                                                                       : "no locator name";
        continue;
      }
      // The pool's strings may carry their NUL.
      name = name.substr(0, name.find('\0'));
      const auto by = scripts.followedBy.find(follower);
      if (name.empty() || name.size() > PortRoomEffects::kMaxLocatorName ||
          std::any_of(name.begin(), name.end(), [](char ch) { return ch < 0x21 || ch > 0x7e; }) ||
          by == scripts.followedBy.end() || std::find(by->second.begin(), by->second.end(), sf) == by->second.end()) {
        plan.why = "bad locator name or follower";
        continue;
      }
      plan.ok = true;
      plan.follower = follower;
      plan.layer = comp->baseLayer ? 0 : layer->second;
      plan.record.followed = objects[matched->second].id;
      plan.record.flags = room.Active(*comp) ? PortRoomEffects::kFollowActive : 0u;
      plan.record.locator = std::move(name);
    }
  }
  const auto followsOk = [&](int effect) {
    const auto by = scripts.followedBy.find(effect);
    return by == scripts.followedBy.end() ||
           std::all_of(by->second.begin(), by->second.end(), [&](int sf) { return followPlans.at(sf).ok; });
  };
  const auto followerIs = [&](int effect, uint32_t id) {
    for (auto& [sf, plan] : followPlans) {
      if (plan.ok && plan.follower == effect) {
        plan.followerId = id;
      }
    }
  };
  std::vector<PortRoomEffects::Generator> generators;
  size_t generatedCount = 0;  // effects added that a generator copies
  size_t reserved = 0;  // the barrier's actors, left to it
  // entity -> the object drawing its EffectMP1: the add-on it became, or the retail effect it
  // matched where that draws its GENP
  std::map<int, uint32_t> effectOf;
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
      followerIs(c->entity, o.id);
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
      const bool replacedPart = std::find(paired.begin(), paired.end(), own) != paired.end();
      if (replacedPart) {
        part = own;
      }
      // A retail object drawing another PART still plays this GENP in Remastered: Lava Lake's
      // five spurts draw EA96EF9F, the parent of the big spit, where Remastered's actors play
      // the spit's GENP itself with their own variables. The object takes the mod's PART for
      // it; without the variables its colours fall back to white.
      const bool pairedPart = !replacedPart && !paired.empty() && (part >> 24) != 0xFE;
      if (pairedPart) {
        part = paired.front();
      }
      const bool ownPart = part != 0 && (part >> 24) == 0xFE && part != own;
      // CEffectMP1GOC::Create (0xd9e460) passes every entity's own placement.
      // Apply it whenever we have its converted GENP: either a synthetic PART,
      // or a paired replacement whose presence io.matched verified. A carried
      // retail-id fallback from io.effect is not proof of converted source data.
      const bool sourcePlacement = replacedPart || pairedPart || ownPart;
      // Where the object draws this GENP, an ambient spawning into it spawns into the object.
      if (sourcePlacement || (part != 0 && part == own)) {
        effectOf[c->entity] = o.id;
      }
      if (!sourcePlacement && (part == 0 || part != own || vars.empty())) {
        ++keptOwn;
        continue;
      }
      PortRoomEffects::Override over{o.id, part, std::move(vars)};
      if (sourcePlacement) {
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
    // A connected one is added only where every connection into it traced to retail states
    // that show, hide or toggle it: those become its drives.
    // One a generator copies takes that generator too, if it is exact.
    std::vector<PortRoomEffects::Drive> drives;
    const auto generatedBy = scripts.generatedBy.find(c->entity);
    const bool generated = generatedBy != scripts.generatedBy.end();
    const bool followed = scripts.followedBy.count(c->entity) != 0;
    if (connected.count(c->entity) != 0) {
      const auto links = scripts.links.find(c->entity);
      // Why it cannot be driven exactly (the first condition it fails), or empty.
      const std::string untraced = [&]() -> std::string {
        if (drivable.count(c->entity) == 0) return "not drivable";
        if (links == scripts.links.end() && !generated && !followed) return "no link";
        if (!followsOk(c->entity)) return "a follow";
        if (scripts.unresolved.count(c->entity) != 0) return "an untraced link";
        for (const PortRoomGeo::Link& l : links != scripts.links.end() ? links->second : std::vector<PortRoomGeo::Link>()) {
          if (l.action != PortRoomGeo::kShow && l.action != PortRoomGeo::kHide && l.action != PortRoomGeo::kToggle) {
            return "a link that does not show or hide";
          }
          if (!(l.delay >= 0.f && l.delay <= PortRoomEffects::kMaxDriveDelay)) return "a link's delay";
          if (!addOnSenderOk(l)) return "an add-on sender";
        }
        for (int g : generated ? generatedBy->second : std::vector<int>()) {
          if (!generatorPlans.at(g).ok) {
            return std::string("its generator: ") + generatorPlans.at(g).why;
          }
        }
        return "";
      }();
      if (!untraced.empty()) {
        ++connectedCount;
        ++connectedWhy[untraced];
        static const bool logMisses = port::EnvFlag("MP_REMASTERED_MISS");
        if (logMisses) {
          const Id16& guid = room.Components()[size_t(c->entity)].guid;
          char text[16];
          std::snprintf(text, sizeof text, " %02x%02x%02x%02x", guid[0], guid[1], guid[2], guid[3]);
          connectedWho[untraced] += text;
        }
        continue;
      }
      for (const PortRoomGeo::Link& l : links != scripts.links.end() ? links->second : std::vector<PortRoomGeo::Link>()) {
        drives.push_back({0, l.sender, l.state,
                          l.action == PortRoomGeo::kShow   ? 1u    // kSM_Activate
                          : l.action == PortRoomGeo::kHide ? 4u    // kSM_Deactivate
                                                           : 17u,  // kSM_ToggleActive
                          l.delay});
      }
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
    followerIs(c->entity, e.id);
    effectOf[c->entity] = e.id;
    addedIds[c->entity] = e.id;
    for (PortRoomEffects::Drive& d : drives) {
      d.target = e.id;
      allDrives.push_back(d);
    }
    drivenCount += drives.empty() && !generated ? 0 : 1;
    // Its generators, each written once with its drives; their one template is this effect.
    for (int g : generated ? generatedBy->second : std::vector<int>()) {
      GeneratorPlan& plan = generatorPlans.at(g);
      if (plan.index >= 0) {
        continue;  // one template: a second effect never names it
      }
      plan.index = int(generators.size());
      plan.record.templates.assign(plan.count, e.id);
      generators.push_back(plan.record);
      for (const PortRoomGeo::Link& l : scripts.links.at(g)) {
        allDrives.push_back({uint32_t(plan.index), l.sender, l.state,
                             l.action == PortRoomGeo::kGenerate ? 13u    // kSM_SetToZero
                             : l.action == PortRoomGeo::kShow   ? 1u     // kSM_Activate
                             : l.action == PortRoomGeo::kHide   ? 4u     // kSM_Deactivate
                                                                : 17u,   // kSM_ToggleActive
                             l.delay,
                             PortRoomEffects::kDriveToGenerator | (l.relayed ? PortRoomEffects::kDriveRelayed : 0u)});
      }
    }
    generatedCount += generated ? 1 : 0;
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
  if (!group && reserved != 0) {
    connectedCount += reserved;
    connectedWhy["the barrier not written"] += reserved;
  }
  // The timers, triggers and relays the drives written hear from, and the ones those hear from:
  // the timers' ids follow the barrier's, the triggers' the timers', the relays' the triggers'.
  std::vector<PortRoomEffects::Timer> timers;
  std::vector<PortRoomEffects::Trigger> triggers;
  std::vector<PortRoomEffects::Relay> relays;
  {
    std::map<int, uint32_t> timerIds, triggerIds, relayIds;  // by entity
    std::vector<uint32_t> queue;                             // senders
    const auto need = [&](uint32_t sender) {
      if ((IsTimerSender(sender) && timerIds.emplace(int(sender & ~kSenderTimer), 0).second) ||
          (IsTriggerSender(sender) && triggerIds.emplace(int(sender & ~kSenderTrigger), 0).second) ||
          (IsRelaySender(sender) && relayIds.emplace(int(sender & ~kSenderRelay), 0).second)) {
        queue.push_back(sender);
      }
    };
    const auto linksOf = [&](uint32_t sender) -> const std::vector<PortRoomGeo::Link>& {
      return IsTimerSender(sender)     ? scripts.timers.at(int(sender & ~kSenderTimer)).links
             : IsTriggerSender(sender) ? scripts.triggers.at(int(sender & ~kSenderTrigger)).links
                                       : scripts.relays.at(int(sender & ~kSenderRelay)).links;
    };
    for (const PortRoomEffects::Drive& d : allDrives) {
      need(d.sender);
    }
    for (size_t q = 0; q < queue.size(); ++q) {
      for (const PortRoomGeo::Link& l : linksOf(queue[q])) {
        need(l.sender);
      }
    }
    const size_t first = added.size() + (group ? barrier.effects.size() : 0);
    if (first + timerIds.size() + triggerIds.size() + relayIds.size() > PortRoomEffects::kMaxAddOns) {
      // Never reached by retail's rooms; the drives they would send are dropped, so say so.
      ++full;
      std::erase_if(allDrives, [](const PortRoomEffects::Drive& d) {
        return IsTimerSender(d.sender) || IsTriggerSender(d.sender) || IsRelaySender(d.sender);
      });
      timerIds.clear();
      triggerIds.clear();
      relayIds.clear();
    }
    for (auto& [t, id] : timerIds) {
      const SceneryScripts::Timer& timer = scripts.timers.at(t);
      const uint32_t retailLayer = timer.baseLayer ? 0 : scripts.layer.at(t);
      id = PortRoomEffects::AddOnId(retailLayer, areaBits, uint32_t(first + timers.size()));
      timers.push_back({id, timer.delay, timer.random,
                        (timer.loop ? PortRoomEffects::kTimerLoop : 0u) |
                            (timer.autoStart ? PortRoomEffects::kTimerAutoStart : 0u) |
                            (timer.active ? PortRoomEffects::kTimerActive : 0u)});
    }
    for (auto& [t, id] : triggerIds) {
      const SceneryScripts::Trigger& trigger = scripts.triggers.at(t);
      const uint32_t retailLayer = trigger.baseLayer ? 0 : scripts.layer.at(t);
      id = PortRoomEffects::AddOnId(retailLayer, areaBits, uint32_t(first + timers.size() + triggers.size()));
      PortRoomEffects::Trigger record;
      record.id = id;
      std::copy(trigger.pos, trigger.pos + 3, record.pos);
      std::copy(trigger.extent, trigger.extent + 3, record.extent);
      record.triggerFlags = trigger.flags;
      record.flags = (trigger.active ? PortRoomEffects::kTriggerActive : 0u) |
                     (trigger.deactivateOnEntered ? PortRoomEffects::kTriggerDeactivateOnEntered : 0u) |
                     (trigger.deactivateOnExited ? PortRoomEffects::kTriggerDeactivateOnExited : 0u);
      triggers.push_back(record);
    }
    for (auto& [r, id] : relayIds) {
      const SceneryScripts::Relay& relay = scripts.relays.at(r);
      const uint32_t retailLayer = relay.baseLayer ? 0 : scripts.layer.at(r);
      id = PortRoomEffects::AddOnId(retailLayer, areaBits,
                                    uint32_t(first + timers.size() + triggers.size() + relays.size()));
      relays.push_back({id, relay.active ? PortRoomEffects::kRelayActive : 0u});
    }
    const auto addDrives = [&](const std::map<int, uint32_t>& ids, const auto& addOns) {
      for (const auto& [t, id] : ids) {
        for (const PortRoomGeo::Link& l : addOns.at(t).links) {
          allDrives.push_back({id, l.sender, l.state, uint32_t(l.action & ~kLinkMessage), l.delay,
                               l.relayed ? PortRoomEffects::kDriveRelayed : 0u});
        }
      }
    };
    addDrives(timerIds, scripts.timers);
    addDrives(triggerIds, scripts.triggers);
    addDrives(relayIds, scripts.relays);
    for (PortRoomEffects::Drive& d : allDrives) {
      if (IsTimerSender(d.sender)) {
        d.sender = timerIds.at(int(d.sender & ~kSenderTimer));
      } else if (IsTriggerSender(d.sender)) {
        d.sender = triggerIds.at(int(d.sender & ~kSenderTrigger));
      } else if (IsRelaySender(d.sender)) {
        d.sender = relayIds.at(int(d.sender & ~kSenderRelay));
      } else if (IsEffectSender(d.sender)) {
        const auto sender = addedIds.find(int(d.sender & ~kSenderEffect));
        if (sender == addedIds.end()) {
          missing.insert(int(d.sender & ~kSenderEffect));
        } else {
          d.sender = sender->second;
        }
      }
    }
  }
  if (!missing.empty()) {
    return {};
  }
  // The follow locators whose effect was written (or is a retail object): their ids follow the
  // relays'.
  std::vector<PortRoomEffects::Follow> follows;
  std::string followsLeftOut;  // "; <reason>" per locator left out
  {
    const size_t first =
        added.size() + (group ? barrier.effects.size() : 0) + timers.size() + triggers.size() + relays.size();
    for (auto& [sf, plan] : followPlans) {
      if (!plan.ok || plan.followerId == 0) {
        followsLeftOut += plan.ok ? "; its effect not written" : std::string("; ") + plan.why;
        continue;
      }
      if (first + follows.size() >= PortRoomEffects::kMaxAddOns) {
        ++full;
        break;
      }
      plan.record.id = PortRoomEffects::AddOnId(plan.layer, areaBits, uint32_t(first + follows.size()));
      plan.record.follower = plan.followerId;
      follows.push_back(plan.record);
    }
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
  // A paired enemy's death particles: the PART the mod has for each slot's GENP, and the slot's
  // offset. A slot Remastered leaves empty keeps retail's particle.
  std::vector<PortRoomEffects::PatternedEffect> patterned;
  std::set<std::pair<uint32_t, uint32_t>> patternedSeen;
  size_t patternedNoPart = 0;
  for (const auto& [o, info] : enemies) {
    for (uint32_t slot = 0; slot < PortRoomEffects::kPatternedSlots; ++slot) {
      Id16 genp{};
      if (!PatternedGenp(room, info, slot, genp) || !patternedSeen.insert({o->id, slot}).second) {
        continue;
      }
      uint32_t part = io.effect(genp);
      if (part == 0 && io.matched != nullptr) {
        const std::vector<uint32_t> paired = io.matched(genp);
        part = paired.size() == 1 ? paired[0] : 0;
      }
      if (part == 0) {
        ++patternedNoPart;
        continue;
      }
      PortRoomEffects::PatternedEffect p;
      p.id = o->id;
      p.slot = slot;
      p.part = part;
      // The offset is Remastered's too: SLdrVector_MP1Typedef's constructor (0xd1e970) zeroes
      // one the PatternedInfo leaves out.
      p.hasOffset = true;
      const auto offset = info.find(kPatternedSlotProps[slot].offset);
      if (offset != info.end() && offset->second.size == 12) {
        const uint8_t* b = room.Bytes(offset->second);
        const Vec3 g = MulR2G({ReadLEFloat(b), ReadLEFloat(b + 4), ReadLEFloat(b + 8)});
        if (!std::isfinite(g[0]) || !std::isfinite(g[1]) || !std::isfinite(g[2])) {
          ++patternedNoPart;
          continue;
        }
        for (int i = 0; i < 3; ++i) {
          p.offset[i] = float(g[size_t(i)]);
        }
      }
      patterned.push_back(p);
    }
  }
  // A paired object's other particle properties (kPartSlots): the PART the mod has for the
  // GENP, written over the retail one as the object loads.
  std::vector<PortRoomEffects::PropertyPatch> patches;
  size_t patchNoPart = 0;
  // And the effects Remastered gives it where retail has no property (kActorSlotTable).
  std::vector<PortRoomEffects::ActorSlotEffect> actorSlots;
  size_t actorSlotNoPart = 0;
  for (const Component& c : room.Components()) {
    const auto matched = c.entity >= 0 ? scripts.retail.find(c.entity) : scripts.retail.end();
    const ScriptObject* paired = matched != scripts.retail.end() && matched->second < objects.size()
                                     ? &objects[matched->second]
                                     : nullptr;
    if (paired == nullptr) {
      // The Metroid Prime relay is unplaced, so the matcher cannot pair it by position: the
      // room's only relay and the area's only type 0x84 object.
      if (c.type != kMetroidPrimeRelayMP1 || room.Of(c.type).size() != 1) {
        continue;
      }
      for (const ScriptObject& o : objects) {
        if (o.type == 0x84) {
          if (paired != nullptr) {
            paired = nullptr;
            break;
          }
          paired = &o;
        }
      }
      if (paired == nullptr) {
        continue;
      }
    }
    const ScriptObject& o = *paired;
    std::map<uint32_t, Span> flat;
    for (const PartSlot& slot : kPartSlots) {
      if (slot.component != c.type || slot.retailType != o.type || o.props.size() < slot.offset + 4) {
        continue;
      }
      const Id16 genp = SlotGenp(room, c, slot.path, flat);
      if (genp == Id16{}) {
        continue;
      }
      uint32_t part = io.effect(genp);
      if (part == 0 && io.matched != nullptr) {
        const std::vector<uint32_t> paired = io.matched(genp);
        part = paired.size() == 1 ? paired[0] : 0;
      }
      if (part == 0) {
        ++patchNoPart;
        continue;
      }
      PortRoomEffects::PropertyPatch p;
      p.id = o.id;
      p.offset = slot.offset;
      p.retail = ReadBE32(&o.props[slot.offset]);
      p.part = part;
      if (p.retail != p.part) {
        patches.push_back(p);
      }
    }
    for (const ActorSlot& slot : kActorSlotTable) {
      if (slot.component != c.type || slot.retailType != o.type) {
        continue;
      }
      const Id16 effect = SlotGenp(room, c, slot.path, flat);
      if (effect == Id16{}) {
        continue;
      }
      const uint32_t part = io.effect(effect);
      if (part == 0) {
        ++actorSlotNoPart;
        continue;
      }
      actorSlots.push_back({o.id, slot.slot, part});
    }
  }
  // A paired Steam object's Remastered visor steam (port_room_effects.h Steam): the SteamMP1's
  // properties, and the objects its source link names.
  std::vector<PortRoomEffects::Steam> steams;
  size_t steamUnpaired = 0, steamNoPart = 0, steamSources = 0, steamSourcesPaired = 0;
  const std::vector<Component>& comps = room.Components();
  for (size_t ci = 0; ci < comps.size(); ++ci) {
    const Component& c = comps[ci];
    if (c.type != kSteamMP1) {
      continue;
    }
    const auto matched = c.entity >= 0 ? scripts.retail.find(c.entity) : scripts.retail.end();
    if (matched == scripts.retail.end() || matched->second >= objects.size() ||
        objects[matched->second].type != 0x46) {
      ++steamUnpaired;
      continue;
    }
    const auto f = room.Flat(c);
    PortRoomEffects::Steam s;
    s.id = objects[matched->second].id;
    Id16 genp{};
    if (Property16(room, f, kPropSteamGenp, genp)) {
      genp = SwapUuid(genp.data());
      uint32_t part = io.effect(genp);
      if (part == 0 && io.matched != nullptr) {
        const std::vector<uint32_t> paired = io.matched(genp);
        part = paired.size() == 1 ? paired[0] : 0;
      }
      if (part != 0) {
        s.part = part;
      }
    }
    if (s.part == 0xFFFFFFFF) {
      // SetVisorSteamShip1 shows nothing without a PART.
      ++steamNoPart;
    }
    const auto color = f.find(kPropSteamColor);
    if (color != f.end() && color->second.size == 16) {
      for (int k = 0; k < 4; ++k) {
        s.color[k] = ReadLEFloat(room.Bytes(color->second) + 4 * k);
      }
    }
    for (const auto& [id, value] : {std::pair{kPropSteamA, &s.a}, std::pair{kPropSteamB, &s.b},
                                    std::pair{kPropSteamG, &s.g}, std::pair{kPropSteamH, &s.h},
                                    std::pair{kPropSteamI, &s.i}}) {
      const auto it = f.find(id);
      if (it != f.end() && it->second.size == 4) {
        *value = ReadLEFloat(room.Bytes(it->second));
      }
    }
    const auto flag = f.find(kPropSteamColorFlag);
    if (flag != f.end() && flag->second.size >= 1) {
      s.colorFlag = room.Bytes(flag->second)[0] != 0;
    }
    bool ok = std::isfinite(s.a) && std::isfinite(s.b) && std::isfinite(s.g) && std::isfinite(s.h) &&
              std::isfinite(s.i);
    for (const auto& [id, spline] : {std::pair{kPropSteamSpline1, &s.s1}, std::pair{kPropSteamSpline2, &s.s2}}) {
      const auto it = f.find(id);
      if (it == f.end()) {
        continue;
      }
      PortMayaSpline check;
      if (it->second.size > PortRoomEffects::kMaxSteamSplineBytes ||
          !check.Load(room.Bytes(it->second), it->second.size)) {
        ok = false;
        continue;
      }
      spline->assign(room.Bytes(it->second), room.Bytes(it->second) + it->second.size);
    }
    // GetAveragedSteamSourcePos counts each linked object once.
    std::set<int> seen;
    for (const auto& [link, target] : ReadEntityLinkIds(room, ci)) {
      if (link != kSteamSourceLink || target < 0) {
        continue;
      }
      Component t = comps[size_t(target)];
      if (t.entity < 0) {
        t.entity = target;  // the link names the entity itself
      }
      Vec3 pos, rot, scale;
      if (!seen.insert(t.entity).second || !room.Xform(t, pos, rot, scale)) {
        continue;
      }
      PortRoomEffects::SteamSource src;
      const Vec3 w = Apply(area.xf, MulR2G(pos));
      for (int k = 0; k < 3; ++k) {
        src.pos[k] = float(w[size_t(k)]);
      }
      const auto paired = scripts.retail.find(t.entity);
      if (paired != scripts.retail.end() && paired->second < objects.size()) {
        src.id = objects[paired->second].id;
        ++steamSourcesPaired;
      }
      ++steamSources;
      if (s.sources.size() < PortRoomEffects::kMaxSteamSources) {
        s.sources.push_back(src);
      }
    }
    if (ok) {
      steams.push_back(std::move(s));
    }
  }
  char steamNote[160] = "";
  if (!steams.empty() || steamUnpaired != 0) {
    std::snprintf(steamNote, sizeof steamNote,
                  ", %zu Remastered steam(s) (%zu without a PART, %zu unpaired), %zu steam source(s) (%zu on a "
                  "retail object)",
                  steams.size(), steamNoPart, steamUnpaired, steamSources, steamSourcesPaired);
  }
  char patternedNote[224] = "";
  if (!patterned.empty() || patternedNoPart != 0 || !patches.empty() || patchNoPart != 0 || !actorSlots.empty() ||
      actorSlotNoPart != 0) {
    std::snprintf(patternedNote, sizeof patternedNote,
                  ", %zu enemy death particle(s) (%zu without a PART), %zu particle propert(ies) (%zu without a "
                  "PART), %zu Remastered-only effect slot(s) (%zu without an asset)",
                  patterned.size(), patternedNoPart, patches.size(), patchNoPart, actorSlots.size(), actorSlotNoPart);
  }
  // The ambient particle spawners, filling the effects they link (CAmbientParticleEffectGOC).
  std::vector<PortRoomEffects::Ambient> ambients;
  std::map<std::string, size_t> ambientsLeftOut;
  for (const Component& c : room.Components()) {
    if (c.type != kAmbientParticleEffect) {
      continue;
    }
    PortRoomEffects::Ambient a;
    const char* why = "";
    if (!AmbientRecord(room, c, area.xf, effectOf, a, why)) {
      ++ambientsLeftOut[why];
    } else if (a.rate > 0.f) {
      ambients.push_back(std::move(a));
    }
  }
  char generatorNote[512] = "";
  if (!generators.empty()) {
    std::snprintf(generatorNote, sizeof generatorNote, ", %zu of them copied by %zu generator(s)", generatedCount,
                  generators.size());
  }
  if (!timers.empty()) {
    const size_t at = std::strlen(generatorNote);
    std::snprintf(generatorNote + at, sizeof generatorNote - at, ", %zu timer(s)", timers.size());
  }
  if (!triggers.empty()) {
    const size_t at = std::strlen(generatorNote);
    std::snprintf(generatorNote + at, sizeof generatorNote - at, ", %zu trigger(s)", triggers.size());
  }
  if (!relays.empty()) {
    const size_t at = std::strlen(generatorNote);
    std::snprintf(generatorNote + at, sizeof generatorNote - at, ", %zu relay(s)", relays.size());
  }
  if (!follows.empty()) {
    const size_t at = std::strlen(generatorNote);
    std::snprintf(generatorNote + at, sizeof generatorNote - at, ", %zu follow(s)", follows.size());
  }
  if (!followsLeftOut.empty()) {
    const size_t at = std::strlen(generatorNote);
    std::snprintf(generatorNote + at, sizeof generatorNote - at, ", follows left out: %s", followsLeftOut.c_str() + 2);
  }
  if (!ambients.empty()) {
    const size_t at = std::strlen(generatorNote);
    std::snprintf(generatorNote + at, sizeof generatorNote - at, ", %zu ambient spawner(s)", ambients.size());
  }
  for (const auto& [why, count] : connectedWhy) {
    const size_t at = std::strlen(generatorNote);
    std::snprintf(generatorNote + at, sizeof generatorNote - at, ", %zu connected left out: %s", count, why.c_str());
  }
  for (const auto& [why, count] : ambientsLeftOut) {
    const size_t at = std::strlen(generatorNote);
    std::snprintf(generatorNote + at, sizeof generatorNote - at, ", %zu ambient(s) left out: %s", count, why.c_str());
  }
  std::snprintf(line, sizeof line,
                "  %s: %zu effect actor(s) added, %zu retail effect(s) replaced, %zu retail effect(s) take "
                "Remastered's effect, %zu connected and left out (%zu more added with drives%s), %zu matched a retail object (%zu of them still on "
                "the retail PART), %zu without a PART, %zu on an unknown layer%s%s%s%s%s",
                name.c_str(), added.size(), replacedNear, overridden, connectedCount, drivenCount, generatorNote, kept + keptOwn + overridden,
                keptOwn, noPart, noLayer, full != 0 ? ", the id range is full" : "",
                badVars != 0 ? ", unreadable particle variables" : "", barrierNote, patternedNote, steamNote);
  std::string summary = line;
  for (const auto& [why, who] : connectedWho) {
    summary += "; left out for " + why + ":" + who;
  }
  if (added.empty() && overrides.empty() && skips.empty() && !group && patterned.empty() && patches.empty() &&
      steams.empty() && actorSlots.empty() && follows.empty()) {
    return summary;
  }
  PortRoomEffects::File file;
  file.skips.assign(skips.begin(), skips.end());
  file.connected = uint32_t(connectedCount);
  for (auto& [index, layer] : layers) {
    file.layers.push_back(std::move(layer));
  }
  file.overrides = std::move(overrides);
  file.patterned = std::move(patterned);
  file.patches = std::move(patches);
  file.steams = std::move(steams);
  file.actorSlots = std::move(actorSlots);
  file.drives = std::move(allDrives);
  file.generators = std::move(generators);
  file.timers = std::move(timers);
  file.triggers = std::move(triggers);
  file.relays = std::move(relays);
  file.follows = std::move(follows);
  file.ambients = std::move(ambients);
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
        for (const PortRoomEffects::Override& over : file.overrides) {
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
  return summary;
}
}  // namespace

std::string WriteRoomEffects(const std::string& name, const Room& room, uint32_t mrea, const Area& area,
                             const SceneryScripts& scripts, const RoomIO& io) {
  // An effect whose Active or Inactive drives an added one is added too, or the one it drives is
  // left out: over again without the senders that were not, until every one named is.
  std::set<int> notSenders;
  for (;;) {
    std::set<int> missing;
    std::string summary = WriteRoomEffectsOnce(name, room, mrea, area, scripts, io, notSenders, missing);
    if (missing.empty()) {
      return summary;
    }
    notSenders.insert(missing.begin(), missing.end());
  }
}

std::vector<std::array<uint8_t, 16>> RoomEffectGenpsOf(const Room& room) {
  std::vector<std::array<uint8_t, 16>> out;
  const std::set<int> connected = ConnectedEntities(room);
  const std::set<int> drivable = DrivableEntities(room);
  for (const Component* c : room.Of(kEffectMP1)) {
    if (c->entity < 0 || (connected.count(c->entity) != 0 && drivable.count(c->entity) == 0)) {
      continue;
    }
    const auto f = room.Flat(*c);
    const auto genp = f.find(kPropGenp);
    if (genp != f.end() && genp->second.size == 16) {
      out.push_back(SwapUuid(room.Bytes(genp->second)));
    }
  }
  // Every enemy's death particles (WriteRoomEffects gives them to the retail enemies matched).
  for (const Component& c : room.Components()) {
    std::map<uint32_t, Span> info;
    if (c.entity < 0 || !PatternedInfoOf(room, c, info)) {
      continue;
    }
    for (uint32_t slot = 0; slot < PortRoomEffects::kPatternedSlots; ++slot) {
      Id16 genp{};
      if (PatternedGenp(room, info, slot, genp)) {
        out.push_back(genp);
      }
    }
  }
  // Every particle property of kPartSlots (WriteRoomEffects writes them into the retail
  // objects matched).
  for (const Component& c : room.Components()) {
    if (c.entity < 0) {
      continue;
    }
    std::map<uint32_t, Span> flat;
    for (const PartSlot& slot : kPartSlots) {
      if (slot.component != c.type) {
        continue;
      }
      const Id16 genp = SlotGenp(room, c, slot.path, flat);
      if (genp != Id16{}) {
        out.push_back(genp);
      }
    }
    for (const ActorSlot& slot : kActorSlotTable) {
      if (slot.component != c.type) {
        continue;
      }
      const Id16 effect = SlotGenp(room, c, slot.path, flat);
      if (effect != Id16{}) {
        out.push_back(effect);
      }
    }
  }
  // Every steam's PART (WriteRoomEffects gives it to the retail Steam matched).
  for (const Component* c : room.Of(kSteamMP1)) {
    Id16 genp{};
    if (c->entity >= 0 && Property16(room, room.Flat(*c), kPropSteamGenp, genp)) {
      out.push_back(SwapUuid(genp.data()));
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
