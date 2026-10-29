// The Remastered room environment writer's script links of scenery actors, a port of
// build/mpr/scenery/match.py and trace.py. See port_remastered_room_internal.h.

#include "port_remastered_room_internal.h"

namespace PortRemastered {
namespace RoomDetail {

// ---------------------------------------------------------------------------
// Script links of scenery actors (build/mpr/scenery/match.py and trace.py)
// ---------------------------------------------------------------------------
//
// Remastered's scripts show and hide the scenery actors it added; the port draws them as
// room geometry, so what drives them has to be found among retail's objects. Remastered's
// entities are matched to the area's retail objects (same type at the same place, then
// the nearest one, then the same connection targets), and an actor's incoming
// connections are followed back, through Remastered's own relays and timers, to a matched
// sender. Which retail state a Remastered event is was voted on over every matched room.

struct RemasteredScriptType {
  uint32_t type;
  uint8_t retailType;
};
struct RemasteredScriptEvent {
  uint32_t type;
  uint32_t event;
  int state;
};
#include "port_remastered_script_tables.inc"

bool HintTakes(uint32_t action) {
  return action == kActionHintOn || action == kActionHintOff || action == kActionHintRemove ||
         action == kActionHintDelete || action == kActionEntityActivate || action == kActionEntityDeactivate ||
         action == kActionFogHintOn || action == kActionFogHintOff || action == kActionFogHintRemove;
}
uint8_t TransitionAct(uint32_t action) {
  switch (action) {
  case kActionTransitionStart:
    return PortRoomEnv::kTransitionStart;
  case kActionTransitionRestart:
    return PortRoomEnv::kTransitionRestart;
  case kActionTransitionStop:
    return PortRoomEnv::kTransitionStop;
  case kActionHintDelete:
    return PortRoomEnv::kTransitionDelete;
  case kActionEntityActivate:
  case kActionComponentActivate:
    return PortRoomGeo::kShow;
  case kActionEntityDeactivate:
  case kActionComponentDeactivate:
    return PortRoomGeo::kHide;
  default:
    return 0;
  }
}
// What a LightDynamic takes (CLightDynamicGOC::AcceptScriptMsg): its timeline's Start, Stop,
// Reset, PlayForward, PlayBackward and TogglePlaybackDirection, and (de)activation.
uint8_t LightAct(uint32_t action) {
  switch (action) {
  case 0x01bedeb9:
    return PortRoomEnv::kLightStart;
  case 0x2ebaa1ae:
    return PortRoomEnv::kLightStop;
  case 0x360ef3eb:
    return PortRoomEnv::kLightReset;
  case 0x2bebd3d6:
    return PortRoomEnv::kLightForward;
  case 0x65a5300e:
    return PortRoomEnv::kLightBackward;
  case 0x91651688:
    return PortRoomEnv::kLightReverse;
  case kActionComponentActivate:
    return PortRoomGeo::kShow;
  case kActionComponentDeactivate:
    return PortRoomGeo::kHide;
  default:
    return 0;
  }
}
// Retail types of the Remastered objects a message passes through, and the actions that
// make each pass it on.
struct Pass {
  uint8_t retailType;
  uint32_t actions[2];
};
// A TimerMP1's wait, in seconds.
constexpr uint32_t kPropTimerDelay = 0xfb2a2cf0;
constexpr Pass kPasses[] = {
    {0x15, {0x379d362e, 0x379d362e}},  // Relay
    {kRetailTimer, {0xd63b8f04, 0x55193b90}},  // Timer
    {0x13, {0x326ddb0d, 0x326ddb0d}},  // MemoryRelay
    {0x5e, {0x144d0f29, 0x8ed2a8c7}},  // ColorModulate
};

// The retail type of a Remastered component type, -1 for one retail has no object for.
int RetailType(uint32_t type) {
  static const std::map<uint32_t, int> table = [] {
    std::map<uint32_t, int> t;
    for (const RemasteredScriptType& row : kRemasteredScriptTypes) {
      t[row.type] = row.retailType;
    }
    return t;
  }();
  const auto it = table.find(type);
  return it == table.end() ? -1 : it->second;
}

// The retail state a component type's event stands for, -1 when unknown.
int RetailState(uint32_t type, uint32_t event) {
  static const std::map<std::pair<uint32_t, uint32_t>, int> table = [] {
    std::map<std::pair<uint32_t, uint32_t>, int> t;
    for (const RemasteredScriptEvent& row : kRemasteredScriptEvents) {
      t[{row.type, row.event}] = row.state;
    }
    return t;
  }();
  const auto it = table.find({type, event});
  return it == table.end() ? -1 : it->second;
}

// What a connection's action does to the geometry or hint it reaches.
uint8_t LinkAct(uint32_t action) {
  return action == kActionActivate || action == kActionIncrement || action == kActionEntityActivate ||
                 action == kActionHintOn || action == kActionFogHintOn
             ? PortRoomGeo::kShow
         : action == kActionDeactivate || action == kActionDecrement || action == kActionEntityDeactivate ||
                 action == kActionHintOff || action == kActionHintRemove || action == kActionHintDelete ||
                 action == kActionFogHintOff || action == kActionFogHintRemove
             ? PortRoomGeo::kHide
         : action == kActionToggleActive ? PortRoomGeo::kToggle
                                         : 0;
}

bool IsPass(int retailType) {
  for (const Pass& p : kPasses) {
    if (p.retailType == retailType) {
      return true;
    }
  }
  return false;
}

bool PassFires(int retailType, uint32_t action) {
  for (const Pass& p : kPasses) {
    if (p.retailType == retailType && (p.actions[0] == action || p.actions[1] == action)) {
      return true;
    }
  }
  return false;
}

// A grade hint's sender that is no retail object: PortRoomEnv's kSenderPlayerFluid or
// kSenderCameraWater, with the state (0: in, 1: out), or kSenderCameraWaterItem + item (0: in
// with the item, 1: in without); 0 for any other. `incoming` is by target entity.
uint32_t FluidSender(const Room& room, const std::map<int, std::vector<const Connection*>>& incoming,
                     const Connection& c, int& state) {
  const std::vector<Component>& comps = room.Components();
  const Component& sender = comps[c.sender];
  if (sender.type == kProxyPlayer && (c.event == kEventPlayerFluidIn || c.event == kEventPlayerFluidOut)) {
    state = c.event == kEventPlayerFluidIn ? 0 : 1;
    return PortRoomEnv::kSenderPlayerFluid;
  }
  if (sender.type == kCameraWaterProxy && (c.event == kEventCameraWaterIn || c.event == kEventCameraWaterOut)) {
    state = c.event == kEventCameraWaterIn ? 0 : 1;
    return PortRoomEnv::kSenderCameraWater;
  }
  // An InventoryActivator fired only by the camera going under water: Zero (the item held) or
  // MaxReached (not) as it goes in (the water fogs of Tallon's and Phendrana's masters).
  if (sender.type == kSpecialFunction && sender.entity >= 0 &&
      (c.event == kEventSpecialFunctionZero || c.event == kEventSpecialFunctionMaxReached)) {
    const auto f = room.Flat(sender);
    const auto type = f.find(kPropSpecialFunctionType);
    const auto item = f.find(kPropSpecialFunctionItem);
    if (type == f.end() || type->second.size != 4 || ReadLE32(room.Bytes(type->second)) != kSpecialFunctionInventoryActivator ||
        item == f.end() || item->second.size != 4 || ReadLE32(room.Bytes(item->second)) >= PortRoomEnv::kCameraWaterItems) {
      return 0;
    }
    const auto in = incoming.find(sender.entity);
    if (in == incoming.end() || in->second.empty()) {
      return 0;
    }
    for (const Connection* feed : in->second) {
      if (comps[feed->sender].type != kCameraWaterProxy || feed->event != kEventCameraWaterIn ||
          feed->action != kActionAction) {
        return 0;
      }
    }
    state = c.event == kEventSpecialFunctionZero ? 0 : 1;
    return PortRoomEnv::kSenderCameraWaterItem + ReadLE32(room.Bytes(item->second));
  }
  // A Counter fed only by camera water proxies is non-zero while the camera is in water.
  if (sender.type != kCounter || sender.entity < 0) {
    return 0;
  }
  const bool up = c.event == kEventCounterUp || c.event == kEventCounterNonZero;
  if (!up && c.event != kEventCounterDown && c.event != kEventCounterZero) {
    return 0;
  }
  const auto in = incoming.find(sender.entity);
  if (in == incoming.end() || in->second.empty()) {
    return 0;
  }
  for (const Connection* feed : in->second) {
    if (comps[feed->sender].type != kCameraWaterProxy) {
      return 0;
    }
  }
  state = up ? 0 : 1;
  return PortRoomEnv::kSenderCameraWater;
}

// A TriggerMP1's box: centred on the entity, `scale` across, turned Rz * Ry * Rx, in GameCube
// axes (gc[i] = kSign[i] * remastered[kAxis[i]], MulR2G); axes[3 * i..] is the box's axis i.
void CameraBox(const Vec3& pos, const Vec3& rot, const Vec3& scale, float centre[3], float half[3], float axes[9]) {
  static const int kAxis[3] = {0, 2, 1};
  static const double kSign[3] = {-1, 1, 1};
  double s[3], k[3];
  for (int i = 0; i < 3; ++i) {
    s[i] = std::sin(rot[i] * (3.14159265358979323846 / 180.0));
    k[i] = std::cos(rot[i] * (3.14159265358979323846 / 180.0));
  }
  const double m[3][3] = {
      {k[2] * k[1], k[2] * s[1] * s[0] - s[2] * k[0], k[2] * s[1] * k[0] + s[2] * s[0]},
      {s[2] * k[1], s[2] * s[1] * s[0] + k[2] * k[0], s[2] * s[1] * k[0] - k[2] * s[0]},
      {-s[1], k[1] * s[0], k[1] * k[0]},
  };
  for (int col = 0; col < 3; ++col) {
    centre[col] = float(kSign[col] * pos[kAxis[col]]);
    half[col] = float(std::fabs(scale[kAxis[col]]) / 2);
    for (int row = 0; row < 3; ++row) {
      axes[3 * col + row] = float(kSign[row] * kSign[col] * m[kAxis[row]][kAxis[col]]);
    }
  }
}

// Component `i`'s outgoing connections, from its id data: guid, a block that may be
// skipped, then the list. False when the list does not read; `end` is where it ends.
bool ReadComponentConnections(const Room& room, size_t i, std::vector<Connection>& mine, size_t& end) {
  const std::vector<Component>& comps = room.Components();
  {
    const uint8_t* const b = room.Bytes(comps[i].idta);
    const size_t n = comps[i].idta.size;
    size_t o = 16;
    auto has = [&](size_t k) { return o <= n && n - o >= k; };
    if (!has(8)) {
      return false;
    }
    const uint32_t x = ReadLE32(b + o), y = ReadLE32(b + o + 4);
    o += 8;
    if (x == 0xffffffff) {
      if (!has(y)) {
        return false;
      }
      o += y;
    }
    if (!has(2)) {
      return false;
    }
    const size_t count = ReadLE16(b + o);
    o += 2;
    bool ok = true;
    // Two optional payloads (a type, then its property list and a sized extra), then a fixed tail.
    auto block = [&](Connection& c) {
      if (!has(4)) {
        return false;
      }
      const uint32_t present = ReadLE32(b + o);
      o += 4;
      if (present == 0) {
        return true;
      }
      if (!has(2) || (o += 2, !has(ReadLE16(b + o - 2)))) {
        return false;
      }
      const size_t len = ReadLE16(b + o - 2);
      std::vector<Prop> props;
      if ((present == kEnvCriteria || present == kEnvModifierPayload) && PropList(b + o, len, &props)) {
        const bool criteria = present == kEnvCriteria;
        (criteria ? c.hasCriteria : c.hasPayload) = true;
        for (const Prop& p : props) {
          if (p.data.size != 4) {
            continue;
          }
          const uint32_t v = ReadLE32(b + o + p.data.start);
          if (criteria && p.id == kPropCriteriaValue) {
            c.criteriaValue = int32_t(v);
          } else if (criteria && p.id == kPropCriteriaOp) {
            c.criteriaOp = v;
          } else if (!criteria && p.id == kPropModifierPayloadValue) {
            c.payloadValue = int32_t(v);
          }
        }
      }
      o += len;
      if (!has(4)) {
        return false;
      }
      const uint32_t size = ReadLE32(b + o);
      o += 4;
      if (!has(size)) {
        return false;
      }
      o += size;
      return true;
    };
    for (size_t k = 0; k < count && ok; ++k) {
      if (!has(26)) {
        ok = false;
        break;
      }
      Connection c{i, ReadLE32(b + o), ReadLE32(b + o + 4), {}};
      std::memcpy(c.target.data(), b + o + 8, 16);
      o += 26;
      ok = block(c) && block(c) && has(19);
      o += 19;
      mine.push_back(c);
    }
    end = o;
    return ok;
  }
}

std::vector<Connection> ReadConnections(const Room& room) {
  std::vector<Connection> out;
  for (size_t i = 0; i < room.Components().size(); ++i) {
    std::vector<Connection> mine;
    size_t end = 0;
    if (ReadComponentConnections(room, i, mine, end)) {
      out.insert(out.end(), mine.begin(), mine.end());
    }
  }
  return out;
}

bool EnvModifierOp(const Room& room, const Component& modifier, const Connection& c, PortRoomEnv::EnvOp& op) {
  const bool payload = c.action == kActionEnvSetPayload || c.action == kActionEnvAddPayload ||
                       c.action == kActionEnvSubPayload;
  const bool own = c.action == kActionEnvSet || c.action == kActionEnvAdd || c.action == kActionEnvSub;
  if (!payload && !own) {
    return false;
  }
  const auto f = room.Flat(modifier);
  const auto var = f.find(kPropModifierVar);
  if (var == f.end() || var->second.size != 16) {
    return false;
  }
  std::memcpy(op.var.data(), room.Bytes(var->second), 16);
  op.op = c.action == kActionEnvSetPayload || c.action == kActionEnvSet   ? PortRoomEnv::kEnvSet
          : c.action == kActionEnvAddPayload || c.action == kActionEnvAdd ? PortRoomEnv::kEnvAdd
                                                                          : PortRoomEnv::kEnvSub;
  if (payload) {
    op.value = c.payloadValue;  // an empty (or absent) payload is 0
  } else {
    const auto value = f.find(kPropModifierValue);
    op.value = value != f.end() && value->second.size == 4 ? int32_t(ReadLE32(room.Bytes(value->second))) : 0;
  }
  return true;
}

std::vector<PortRoomEnv::EnvVar> ReadEnvVars(const Room& room) {
  // META: fields not followed yet, up to the first `ffff 00000000`; then u16 count, and per
  // variable guid[16], u32 name length, name, u32 n and n bytes skipped, u32 value, u32 flags,
  // u32 min if flags & 1, u32 max if flags & 2 (NGameArea::get_meta_data_from_stream).
  std::vector<PortRoomEnv::EnvVar> out;
  const Span& meta = room.Meta();
  if (meta.size == 0) {
    return out;
  }
  const uint8_t* const b = room.Bytes(meta);
  const size_t n = meta.size;
  static constexpr uint8_t kMark[] = {0xff, 0xff, 0, 0, 0, 0};
  const uint8_t* const mark = std::search(b, b + n, kMark, kMark + sizeof kMark);
  if (mark == b + n) {
    return out;
  }
  size_t o = size_t(mark - b) + sizeof kMark;
  auto has = [&](size_t k) { return o <= n && n - o >= k; };
  if (!has(2)) {
    return out;
  }
  const size_t count = ReadLE16(b + o);
  o += 2;
  for (size_t i = 0; i < count; ++i) {
    PortRoomEnv::EnvVar v;
    if (!has(20)) {
      return {};
    }
    std::memcpy(v.guid.data(), b + o, 16);
    const size_t nameLen = ReadLE32(b + o + 16);
    o += 20;
    if (nameLen > 1024 || !has(nameLen)) {
      return {};
    }
    v.name.assign(reinterpret_cast<const char*>(b + o), nameLen);
    o += nameLen;
    if (!has(4) || (o += 4, !has(ReadLE32(b + o - 4)))) {
      return {};
    }
    o += ReadLE32(b + o - 4);
    if (!has(8)) {
      return {};
    }
    v.value = int32_t(ReadLE32(b + o));
    v.flags = ReadLE32(b + o + 4);
    o += 8;
    for (const uint32_t bit : {PortRoomEnv::kEnvVarMin, PortRoomEnv::kEnvVarMax}) {
      if ((v.flags & bit) == 0) {
        continue;
      }
      if (!has(4)) {
        return {};
      }
      (bit == PortRoomEnv::kEnvVarMin ? v.min : v.max) = int32_t(ReadLE32(b + o));
      o += 4;
    }
    out.push_back(std::move(v));
  }
  return out;
}

// A component's typed links, after its connections in its id data: u16 count, then per link
// u32 link id, the target entity's guid, u32, u8, u8, and a guid (42 bytes). The linked
// components (-1 for a guid the room lacks), in order.
std::vector<std::pair<uint32_t, int>> ReadEntityLinkIds(const Room& room, size_t i) {
  std::vector<std::pair<uint32_t, int>> out;
  std::vector<Connection> conns;
  size_t o = 0;
  if (!ReadComponentConnections(room, i, conns, o)) {
    return out;
  }
  const Span& idta = room.Components()[i].idta;
  const uint8_t* const b = room.Bytes(idta);
  const size_t n = idta.size;
  if (o > n || n - o < 2) {
    return out;
  }
  const size_t count = ReadLE16(b + o);
  o += 2;
  if ((n - o) / 42 < count) {
    return out;
  }
  for (size_t k = 0; k < count; ++k, o += 42) {
    Id16 target;
    std::memcpy(target.data(), b + o + 4, 16);
    out.emplace_back(ReadLE32(b + o), room.ByGuid(target));
  }
  return out;
}

std::vector<int> ReadEntityLinks(const Room& room, size_t i) {
  std::vector<int> out;
  for (const auto& l : ReadEntityLinkIds(room, i)) {
    out.push_back(l.second);
  }
  return out;
}

float EnvFxIntensity(const Room& room, std::vector<std::string>& notes) {
  const std::vector<Component>& comps = room.Components();
  const std::vector<Connection> conns = ReadConnections(room);
  const auto entityOf = [&](const Id16& guid) {
    const int t = room.ByGuid(guid);
    return t < 0 ? -1 : comps[size_t(t)].type == kEntity ? t : comps[size_t(t)].entity;
  };
  const auto typeOn = [&](int entity, uint32_t type) {
    std::vector<size_t> out;
    for (size_t i = 0; i < comps.size(); ++i) {
      if (comps[i].type == type && entity >= 0 && comps[i].entity == entity) {
        out.push_back(i);
      }
    }
    return out;
  };
  const auto intensityOf = [&](size_t goc, float& value) {
    const auto f = room.Flat(comps[goc]);
    const auto it = f.find(kPropEnvFxIntensity);
    value = it != f.end() && it->second.size >= 4 ? ReadLEFloat(room.Bytes(it->second)) : 1.f;
    return std::isfinite(value);
  };
  constexpr int kMaxDepth = 6;
  // Forward from every RelayAutoFire: a Relay fire passes through a Relay; the apply action
  // lands on the density controllers of the target's entity.
  std::set<size_t> fired;
  std::function<void(size_t, int, std::vector<size_t>&)> forward = [&](size_t from, int depth,
                                                                         std::vector<size_t>& path) {
    for (const Connection& c : conns) {
      if (c.sender != from) {
        continue;
      }
      const int target = entityOf(c.target);
      if (c.action == kActionEnvFxApply) {
        for (const size_t g : typeOn(target, kEnvFxDensityController)) {
          fired.insert(g);
        }
      } else if (c.action == kActionRelayFire && depth < kMaxDepth) {
        for (const size_t relay : typeOn(target, kRelay)) {
          if (std::find(path.begin(), path.end(), relay) == path.end()) {
            path.push_back(relay);
            forward(relay, depth + 1, path);
            path.pop_back();
          }
        }
      }
    }
  };
  for (size_t i = 0; i < comps.size(); ++i) {
    if (comps[i].type == kRelayAutoFire) {
      std::vector<size_t> path{i};
      forward(i, 0, path);
    }
  }
  // What else feeds a controller nothing fires at load: the types that send the apply action,
  // through Relays.
  std::function<void(size_t, int, std::set<size_t>&, std::set<uint32_t>&)> backward =
      [&](size_t to, int depth, std::set<size_t>& seen, std::set<uint32_t>& senders) {
        const int entity = comps[to].type == kEntity ? int(to) : comps[to].entity;
        for (const Connection& c : conns) {
          if (entityOf(c.target) != entity || entity < 0 || (depth == 0 ? c.action != kActionEnvFxApply : c.action != kActionRelayFire)) {
            continue;
          }
          if (comps[c.sender].type == kRelay && depth < kMaxDepth) {
            if (seen.insert(c.sender).second) {
              backward(c.sender, depth + 1, seen, senders);
            }
          } else if (comps[c.sender].type != kRelay) {
            senders.insert(comps[c.sender].type);
          }
        }
      };
  float result = 1.f;
  bool have = false;
  for (const Component& goc : comps) {
    if (goc.type != kEnvFxDensityController) {
      continue;
    }
    const size_t g = size_t(&goc - comps.data());
    float value = 1.f;
    if (!intensityOf(g, value)) {
      notes.push_back("density controller #" + std::to_string(g) + " has a bad intensity, skipped");
      continue;
    }
    char text[160];
    if (fired.count(g) != 0) {
      if (have && value != result) {
        std::snprintf(text, sizeof text, "density controller #%zu (intensity %g) fires at load after another (%g); it wins", g,
                      value, result);
        notes.push_back(text);
      }
      result = value;  // the last in component order wins
      have = true;
      continue;
    }
    if (!room.Flat(goc).count(kPropEnvFxIntensity)) {
      continue;  // the other density controllers carry no intensity (the default, 1)
    }
    std::set<size_t> seen;
    std::set<uint32_t> senders;
    backward(g, 0, seen, senders);
    std::string by;
    for (const uint32_t t : senders) {
      std::snprintf(text, sizeof text, "%s%08X", by.empty() ? "" : ", ", t);
      by += text;
    }
    // Other actions sent straight to it (the shipped triggers' toggles are not the apply action).
    std::set<std::string> others;
    for (const Connection& c : conns) {
      if (entityOf(c.target) == goc.entity && goc.entity >= 0 && c.action != kActionEnvFxApply) {
        std::snprintf(text, sizeof text, "%08X sends %08X", comps[c.sender].type, c.action);
        others.insert(text);
      }
    }
    for (const std::string& o : others) {
      by += (by.empty() ? "" : "; ") + o;
    }
    std::snprintf(text, sizeof text, "density controller #%zu (intensity %g) skipped: ", g, value);
    notes.push_back(text + (by.empty() ? std::string("nothing fires it")
                                       : "not fired at load by the apply action; reached by type " + by));
  }
  return result;
}

std::vector<IceDarkenerAtLoad> IceDarkeners(const Room& room, std::vector<std::string>& notes) {
  const std::vector<Component>& comps = room.Components();
  const std::vector<Connection> conns = ReadConnections(room);
  const auto entityOf = [&](const Id16& guid) {
    const int t = room.ByGuid(guid);
    return t < 0 ? -1 : comps[size_t(t)].type == kEntity ? t : comps[size_t(t)].entity;
  };
  const auto floatOf = [&](size_t c, uint32_t prop, float def) {
    const auto f = room.Flat(comps[c]);
    const auto it = f.find(prop);
    return it != f.end() && it->second.size >= 4 ? ReadLEFloat(room.Bytes(it->second)) : def;
  };
  const auto byteOf = [&](size_t c, uint32_t prop) {
    const auto f = room.Flat(comps[c]);
    const auto it = f.find(prop);
    return it != f.end() && it->second.size >= 1 ? room.Bytes(it->second)[0] : uint8_t(0);
  };
  constexpr int kMaxDepth = 6;
  std::vector<IceDarkenerAtLoad> out;
  char text[192];
  for (size_t d = 0; d < comps.size(); ++d) {
    if (comps[d].type != kIceDarkener) {
      continue;
    }
    const float factor = floatOf(d, kPropIceDarkeningFactor, 1.f);
    if (!std::isfinite(factor)) {
      notes.push_back("ice darkener #" + std::to_string(d) + " has a bad factor, skipped");
      continue;
    }
    if (byteOf(d, kPropIceDarkenEveryFrame) != 0) {
      notes.push_back("ice darkener #" + std::to_string(d) + " applies every frame; applied once at load");
    }
    // Back from the darkener: who sends what reaches `to`'s entity, adding the timers' waits.
    bool atLoad = false;
    std::set<std::string> others;
    std::function<void(size_t, bool, float, int, std::vector<size_t>&)> back =
        [&](size_t to, bool darken, float delay, int depth, std::vector<size_t>& path) {
          const int entity = comps[to].type == kEntity ? int(to) : comps[to].entity;
          for (const Connection& c : conns) {
            if (entity < 0 || entityOf(c.target) != entity) {
              continue;
            }
            const uint32_t want = comps[to].type;
            const bool passes = darken ? c.action == kActionIceDarken
                                : want == kTimerMP1 ? c.action == kActionTimerStart[0] || c.action == kActionTimerStart[1]
                                                    : c.action == kActionScriptRelayFire;
            if (!passes) {
              continue;
            }
            const size_t s = c.sender;
            const uint32_t type = comps[s].type;
            float wait = 0.f;
            if (type == kTimerMP1) {
              wait = floatOf(s, kPropTimerDelay, 0.f);
              if (!std::isfinite(wait) || wait < 0.f) {
                wait = 0.f;
              }
            }
            if (type == kRelayAutoFire || (type == kTimerMP1 && byteOf(s, kPropTimerAutostart) != 0)) {
              if (!room.Active(comps[s])) {
                std::snprintf(text, sizeof text, "ice darkener #%zu: its load sender #%zu starts inactive", d, s);
                notes.push_back(text);
                continue;
              }
              out.push_back({factor, delay + wait});
              atLoad = true;
              std::snprintf(text, sizeof text, "ice darkener #%zu (factor %g) fires %gs after load from #%zu", d, factor,
                            delay + wait, s);
              notes.push_back(text);
            } else if ((type == kTimerMP1 || type == kScriptRelayMP1) && depth < kMaxDepth &&
                       std::find(path.begin(), path.end(), s) == path.end()) {
              path.push_back(s);
              back(s, false, delay + wait, depth + 1, path);
              path.pop_back();
            } else {
              std::snprintf(text, sizeof text, "%08X", type);
              others.insert(text);
            }
          }
        };
    std::vector<size_t> path{d};
    back(d, true, 0.f, 0, path);
    if (!atLoad) {
      std::string by;
      for (const std::string& o : others) {
        by += (by.empty() ? "" : ", ") + o;
      }
      std::snprintf(text, sizeof text, "ice darkener #%zu (factor %g) skipped: ", d, factor);
      notes.push_back(text + (by.empty() ? std::string("nothing fires it") : "not fired at load; reached by type " + by));
    }
  }
  return out;
}

double MaxAbs(const Vec3& a, const Vec3& b) {
  return std::max({std::fabs(a[0] - b[0]), std::fabs(a[1] - b[1]), std::fabs(a[2] - b[2])});
}


// The one start of a driver: every link the walk found is a Show of the same state of the same
// sender after the same wait. False for none, or for several that differ.
bool SingleDriverStart(const std::vector<PortRoomGeo::Link>& starts, PortRoomGeo::Link& first) {
  if (starts.empty()) {
    return false;
  }
  first = starts[0];
  return std::all_of(starts.begin(), starts.end(), [&](const PortRoomGeo::Link& l) {
    return l.action == PortRoomGeo::kShow && l.sender == first.sender && l.state == first.state && l.delay == first.delay;
  });
}

// The kind parameter the driver's variable is bound to in the material of its set's name; -1 when
// the model has none or the port cannot drive what it binds.
int DriverSlot(const std::vector<PortRemastered::ModelMaterial>& materials, const SceneryScripts::Driver& d) {
  if (d.variable.size() != 4) {
    return -1;
  }
  // The runtime override reaches every kind 7 material of the model, so it must be the only one.
  if (std::count_if(materials.begin(), materials.end(),
                    [](const PortRemastered::ModelMaterial& m) { return PortRemastered::MaterialKind(m) == 7; }) != 1) {
    return -1;
  }
  for (const PortRemastered::ModelMaterial& m : materials) {
    if (m.name == d.set) {
      return PortRemastered::MaterialDriverSlot(
          m, port::FourCC(d.variable.c_str()));
    }
  }
  return -1;
}

// Float drivers of material variables, by the entity of each actor they feed: each
// MaterialVariableDriverMP1 fed by exactly one RealValueRelay that exactly one RealNumberDriver
// feeds, with a readable spline and the names of a material set and variable. `starts` gets the
// RealNumberDrivers' entities.
std::map<int, std::vector<SceneryScripts::Driver>> FindMaterialDrivers(const Room& room, std::set<int>& driverStarts) {
  std::map<int, std::vector<SceneryScripts::Driver>> result;
  const std::vector<Component>& comps = room.Components();
  const auto reaches = [&](const std::vector<int>& targets, size_t to) {
    return std::any_of(targets.begin(), targets.end(), [&](int t) {
      // A link names a component, or the entity that owns it (the Entity component itself).
      return t >= 0 && (size_t(t) == to || (comps[to].entity >= 0 && t == comps[to].entity));
    });
  };
  const auto feeding = [&](uint32_t type, size_t to) {
    std::vector<size_t> from;
    for (size_t i = 0; i < comps.size(); ++i) {
      if (comps[i].type == type && reaches(ReadEntityLinks(room, i), to)) {
        from.push_back(i);
      }
    }
    return from;
  };
  for (size_t m = 0; m < comps.size(); ++m) {
    if (comps[m].type != kMaterialVariableDriver) {
      continue;
    }
    const std::vector<size_t> relays = feeding(kRealValueRelay, m);
    const std::vector<size_t> drivers = relays.size() == 1 ? feeding(kRealNumberDriver, relays[0]) : std::vector<size_t>{};
    if (drivers.size() != 1 || comps[drivers[0]].entity < 0) {
      continue;
    }
    SceneryScripts::Driver driver;
    driver.start = comps[drivers[0]].entity;
    Span sp;
    PortMayaSpline check;
    size_t used = 0;
    if (!room.Nested(comps[drivers[0]], {kPropDriverSpline}, sp) || !check.Load(room.Bytes(sp), sp.size, &used) ||
        used != sp.size || check.Size() == 0) {
      continue;
    }
    driver.spline.assign(room.Bytes(sp), room.Bytes(sp) + sp.size);
    bool named = true;
    for (int k = 0; k < 2; ++k) {
      const uint32_t* path = k == 0 ? kPropMaterialSet : kPropMaterialVariable;
      std::string& to = k == 0 ? driver.set : driver.variable;
      named = named && room.Nested(comps[m], {path[0], path[1]}, sp) && sp.size >= 8 &&
              room.String(ReadLE32(room.Bytes(sp)), ReadLE32(room.Bytes(sp) + 4), to);
    }
    if (!named) {
      continue;
    }
    for (int t : ReadEntityLinks(room, m)) {
      const int entity = t < 0 ? -1 : comps[size_t(t)].type == kEntity ? t : comps[size_t(t)].entity;
      if (entity < 0) {
        continue;
      }
      const bool actor = std::any_of(comps.begin(), comps.end(), [&](const Component& c) {
        return c.entity == entity && c.type == kActorMP1;
      });
      if (actor) {
        result[entity].push_back(driver);
      }
    }
    driverStarts.insert(driver.start);
  }
  return result;
}

// Supported subset is explicit: no guessed modes, splines, RGB tint or extra flags.
// Loader 0xd60e1c, defaults 0xd60d00/0xd1e3dc, Create 0xd61bb0.
static bool ReadAlphaModulate(const Room& room, const Component& c, PortRoomGeo::ScriptNode& node) {
  const auto f = room.Flat(c);
  if (f.count(kPropModulateBlend) || f.count(0x2e456002) || f.count(0xb031a3b8)) {
    return false;
  }
  auto scalar = [&](uint32_t key, float def) {
    const auto p = f.find(key);
    return p != f.end() && p->second.size == 4 ? ReadLEFloat(room.Bytes(p->second)) : def;
  };
  auto boolean = [&](uint32_t key, bool def) {
    const auto p = f.find(key);
    return p != f.end() && p->second.size == 1 ? room.Bytes(p->second)[0] != 0 : def;
  };
  if (scalar(0x5b75f4b0, 1.f) != 1.f || scalar(0xf936079e, 1.f) != 1.f ||
      boolean(0xf7d4111c, false) || boolean(0xeecd6985, false) ||
      boolean(0xbc6c51a1, false)) {
    return false;
  }
  for (int color = 0; color < 2; ++color) {
    float rgba[4] = {1, 1, 1, 1};
    for (int channel = 0; channel < 4; ++channel) {
      Span p;
      if (room.Nested(c, {color == 0 ? 0x0e9d55ffu : kPropModulateColorB, kPropColorRGBA[channel]}, p) && p.size == 4) {
        rgba[channel] = ReadLEFloat(room.Bytes(p));
      }
    }
    if (rgba[0] != 1.f || rgba[1] != 1.f || rgba[2] != 1.f || !std::isfinite(rgba[3])) {
      return false;
    }
    node.centre[color] = rgba[3];
  }
  node.half[0] = scalar(0x23fc9f0d, 1.f);
  node.half[1] = scalar(0xd8f7b602, 1.f);
  if (!std::isfinite(node.half[0]) || !std::isfinite(node.half[1])) {
    return false;
  }
  node.max = (boolean(0x976a1cfc, false) ? 1u : 0u) | (boolean(0xa51953a4, false) ? 2u : 0u) |
             (boolean(0xee2b08a7, true) ? 4u : 0u) | (boolean(0x48c52c78, true) ? 8u : 0u) |
             (boolean(0x22ec5b19, false) ? 16u : 0u) | (boolean(0x3fff0f45, false) ? 512u : 0u) |
             (boolean(0xf41e2ca0, false) ? 0x1000u : 0u);
  return true;
}

SceneryScripts MatchScripts(const Room& room, const Area& area) {
  SceneryScripts result;
  const std::vector<Component>& comps = room.Components();
  const std::vector<ScriptObject>& objects = area.objects;
  // Entities in the order their first component comes, as that component's retail type
  // and layer.
  struct Ent {
    int entity;
    int layer;
    bool baseLayer;  // always loaded: drawn on no retail layer
    int type;
    bool hasPos;
    Vec3 w;  // GameCube world position
  };
  std::vector<Ent> ents;
  std::map<int, size_t> entIndex;
  for (const Component& c : comps) {
    if (c.entity < 0 || c.type == kEntity || entIndex.count(c.entity) != 0) {
      continue;
    }
    Ent e{c.entity, c.layer, c.baseLayer, RetailType(c.type), false, {}};
    Vec3 pos, rot, scale;
    if (room.Xform(c, pos, rot, scale)) {
      e.hasPos = true;
      e.w = Apply(area.xf, MulR2G(pos));
    }
    entIndex[c.entity] = ents.size();
    ents.push_back(e);
  }
  result.entities = ents.size();
  auto valid = [](const ScriptObject& o) {
    return o.hasPos && std::isfinite(o.pos[0]) && std::isfinite(o.pos[1]) && std::isfinite(o.pos[2]) &&
           std::fabs(o.pos[0]) < 1e5 && std::fabs(o.pos[1]) < 1e5 && std::fabs(o.pos[2]) < 1e5;
  };
  std::vector<int> match(ents.size(), -1);  // object index
  std::vector<bool> used(objects.size(), false);
  auto take = [&](size_t k, size_t j) {
    match[k] = int(j);
    used[j] = true;
  };
  // Same type at the same place; among several, the one on the same layer.
  for (size_t k = 0; k < ents.size(); ++k) {
    const Ent& e = ents[k];
    if (!e.hasPos || e.type < 0) {
      continue;
    }
    std::vector<size_t> cands, same;
    for (size_t j = 0; j < objects.size(); ++j) {
      if (objects[j].type == e.type && !used[j] && valid(objects[j]) && MaxAbs(objects[j].pos, e.w) < kMatchTolerance) {
        cands.push_back(j);
        if (objects[j].layer == e.layer) {
          same.push_back(j);
        }
      }
    }
    if (cands.size() > 1 && !same.empty()) {
      cands = same;
    }
    if (cands.size() == 1) {
      take(k, cands[0]);
    }
  }
  // Moved a little (triggers resized): the mutual nearest of the same type.
  {
    std::vector<size_t> left;
    for (size_t k = 0; k < ents.size(); ++k) {
      if (match[k] < 0 && ents[k].hasPos && ents[k].type >= 0) {
        left.push_back(k);
      }
    }
    std::vector<size_t> pool;
    for (size_t j = 0; j < objects.size(); ++j) {
      if (!used[j] && valid(objects[j])) {
        pool.push_back(j);
      }
    }
    std::vector<bool> alive(ents.size(), false);
    for (size_t k : left) {
      alive[k] = true;
    }
    for (size_t k : left) {
      size_t best = pool.size();
      double bestDist = kMatchNear;
      for (size_t i = 0; i < pool.size(); ++i) {
        const double d = MaxAbs(objects[pool[i]].pos, ents[k].w);
        if (objects[pool[i]].type == ents[k].type && d < bestDist) {
          best = i;
          bestDist = d;
        }
      }
      if (best == pool.size()) {
        continue;
      }
      const Vec3& p = objects[pool[best]].pos;
      size_t back = ents.size();
      double backDist = 0;
      for (size_t k2 : left) {
        const double d = MaxAbs(ents[k2].w, p);
        if (alive[k2] && ents[k2].type == ents[k].type && (back == ents.size() || d < backDist)) {
          back = k2;
          backDist = d;
        }
      }
      if (back == k) {
        take(k, pool[best]);
        pool.erase(pool.begin() + std::ptrdiff_t(best));
        alive[k] = false;
      }
    }
  }
  // The rest by where their connections go: one retail object of the type whose targets
  // are the matched targets.
  const std::vector<Connection> conns = ReadConnections(room);
  auto entityOf = [&](int comp) -> int {
    if (comp < 0 || comps[size_t(comp)].entity < 0) {
      return -1;
    }
    const auto it = entIndex.find(comps[size_t(comp)].entity);
    return it == entIndex.end() ? -1 : int(it->second);
  };
  std::set<uint32_t> ids;
  for (const ScriptObject& o : objects) {
    ids.insert(o.id);
  }
  // A round takes exact target sets first; when none is left, an entity whose matched targets
  // are a subset of one unused retail object's targets alone (a cycle, as the Watery Hall gate's
  // keyframe and the relay that re-arms it, leaves each with an unmatched target).
  std::map<uint32_t, size_t> objIndex;
  for (size_t j = 0; j < objects.size(); ++j) {
    objIndex.emplace(objects[j].id, j);
  }
  for (int round = 0, subset = 0; round < 32; ++round) {
    std::vector<std::vector<uint32_t>> out(ents.size());
    std::vector<std::multiset<int>> open(ents.size());  // types of each entity's unmatched targets
    for (const Connection& c : conns) {
      const int s = entityOf(int(c.sender)), t = entityOf(room.ByGuid(c.target));
      if (s >= 0 && t >= 0 && match[size_t(t)] >= 0) {
        out[size_t(s)].push_back(objects[size_t(match[size_t(t)])].id);
      } else if (s >= 0 && t >= 0) {
        open[size_t(s)].insert(ents[size_t(t)].type);
      }
    }
    for (auto& o : out) {
      std::sort(o.begin(), o.end());
      o.erase(std::unique(o.begin(), o.end()), o.end());
    }
    std::map<std::pair<int, std::vector<uint32_t>>, std::vector<size_t>> sig;
    for (size_t j = 0; j < objects.size(); ++j) {
      if (used[j]) {
        continue;
      }
      std::vector<uint32_t> s;
      for (uint32_t t : objects[j].targets) {
        if (ids.count(t) != 0) {
          s.push_back(t);
        }
      }
      std::sort(s.begin(), s.end());
      s.erase(std::unique(s.begin(), s.end()), s.end());
      if (!s.empty()) {
        sig[{objects[j].type, s}].push_back(j);
      }
    }
    int found = 0;
    std::vector<int> pick(ents.size(), -1), claims(objects.size(), 0);
    for (size_t k = 0; k < ents.size(); ++k) {
      if (match[k] >= 0 || out[k].empty() || ents[k].type < 0) {
        continue;
      }
      if (subset == 0) {
        const auto it = sig.find({ents[k].type, out[k]});
        if (it != sig.end() && it->second.size() == 1 && !used[it->second[0]]) {
          take(k, it->second[0]);
          ++found;
        }
        continue;
      }
      // Each retail target the entity lacks must be an unused object of a type the entity
      // still sends to an unmatched entity of.
      auto accounted = [&](const std::vector<uint32_t>& retail) {
        std::multiset<int> left = open[k];
        for (uint32_t t : retail) {
          if (std::binary_search(out[k].begin(), out[k].end(), t)) {
            continue;
          }
          const size_t j = objIndex.at(t);
          const auto it = left.find(objects[j].type);
          if (used[j] || it == left.end()) {
            return false;
          }
          left.erase(it);
        }
        return true;
      };
      int only = -1, count = 0;
      for (const auto& [key, js] : sig) {
        if (key.first == ents[k].type &&
            std::includes(key.second.begin(), key.second.end(), out[k].begin(), out[k].end()) &&
            accounted(key.second)) {
          count += int(js.size());
          only = int(js[0]);
        }
      }
      if (count == 1 && !used[size_t(only)]) {
        pick[k] = only;
        ++claims[size_t(only)];
      }
    }
    // Only a retail object one entity alone can take.
    for (size_t k = 0; subset != 0 && k < ents.size(); ++k) {
      if (pick[k] >= 0 && claims[size_t(pick[k])] == 1) {
        take(k, size_t(pick[k]));
        ++found;
      }
    }
    if (found != 0) {
      subset = 0;
    } else if (subset == 0) {
      subset = 1;
    } else {
      break;
    }
  }
  // A Remastered layer is drawn on the retail layer most of its matched entities are on.
  // Remastered's base layer is always loaded, whatever its entities' retail layers (in
  // Phendrana Shorelines its vote picked a 15-object layer), so it is on no layer.
  std::map<int, std::vector<std::pair<uint8_t, int>>> votes;
  for (size_t k = 0; k < ents.size(); ++k) {
    if (match[k] < 0) {
      continue;
    }
    ++result.matched;
    result.retail[ents[k].entity] = size_t(match[k]);
    const uint8_t layer = uint8_t(objects[size_t(match[k])].id >> 26);
    auto& v = votes[ents[k].layer];
    auto it = std::find_if(v.begin(), v.end(), [&](const auto& p) { return p.first == layer; });
    if (it == v.end()) {
      v.push_back({layer, 1});
    } else {
      ++it->second;
    }
  }
  // Incoming connections, followed back to matched senders.
  // A connection to an Entity is to that entity (its own `entity` is its parent's), one to
  // any other component is to the entity holding it.
  auto targetEntity = [&](const Connection& c) -> int {
    const int t = room.ByGuid(c.target);
    return t < 0 ? -1 : comps[size_t(t)].type == kEntity ? t : comps[size_t(t)].entity;
  };
  std::map<int, std::vector<const Connection*>> incoming;  // by target entity component
  for (const Connection& c : conns) {
    const int t = targetEntity(c);
    if (t >= 0) {
      incoming[t].push_back(&c);
    }
  }
  // The entities that are Remastered's own script objects, and the component of each that
  // says what it is.
  std::map<int, std::pair<uint8_t, size_t>> scriptObject;  // by entity: node kind, component
  for (size_t i = 0; i < comps.size(); ++i) {
    const Component& c = comps[i];
    if (c.entity < 0) {
      continue;
    }
    uint8_t kind = 0;
    if (c.type == kTriggerMP1) {
      const auto f = room.Flat(c);
      const auto flags = f.find(kPropTriggerFlags);
      if (flags != f.end() && flags->second.size == 4 &&
          (ReadLE32(room.Bytes(flags->second)) & kTriggerDetectCamera) != 0) {
        kind = PortRoomGeo::kCameraVolume;
      }
    } else if (c.type == kCounter) {
      kind = PortRoomGeo::kCounter;
    } else if (c.type == kRelay) {
      kind = PortRoomGeo::kRelay;
    } else if (c.type == kColorModulateMP1) {
      PortRoomGeo::ScriptNode config;
      if (ReadAlphaModulate(room, c, config)) {
        kind = PortRoomGeo::kAlphaModulate;
      }
    }
    if (kind != 0) {
      scriptObject[c.entity] = {kind, i};
    }
  }
  auto isScriptObject = [&](size_t comp) { return comps[comp].entity >= 0 && scriptObject.count(comps[comp].entity) != 0; };
  // CPlatformMP1GOC::Create0xecaca4 gives each model the entity transform.
  // ActorRotateMP1::Think0xf549cc applies the same rotation to each target.
  // Reuse a matched platform only for an identical co-driven platform: same
  // initial transform, movement properties and full incoming/outgoing graph.
  // This replaces the old room/model-id table with a data-derived relation.
  const auto platformSignature = [&](const Component& c) {
    std::vector<std::tuple<int, uint32_t, uint32_t, int>> sig;
    for (const auto& link : conns) {
      const int target = targetEntity(link);
      if (target == c.entity) sig.emplace_back(int(link.sender), link.event, link.action, -1);
      if (comps[link.sender].entity == c.entity) sig.emplace_back(-1, link.event, link.action, target);
    }
    std::sort(sig.begin(), sig.end());
    return sig;
  };
  const auto movementProperties = [&](const Component& c) {
    std::map<uint32_t, std::vector<uint8_t>> out;
    for (const auto& [key, span] : room.Flat(c)) {
      // Model and ActorInformation affect rendering, not the platform's motion.
      if (key == kPropPlatformModel || key == 0x11d0467f) continue;
      out[key] = std::vector<uint8_t>(room.Bytes(span), room.Bytes(span) + span.size);
    }
    return out;
  };
  const auto platforms = room.Of(kPlatformComponent);
  for (const Component* c : platforms) {
    const auto own = result.retail.find(c->entity);
    if (own != result.retail.end() && objects[own->second].type == kRetailPlatform) {
      result.platformTransform[c->entity] = &objects[own->second];
      continue;
    }
    Vec3 pos, rot, scale;
    if (!room.Xform(*c, pos, rot, scale)) continue;
    const auto signature = platformSignature(*c);
    if (signature.empty()) continue; // proximity alone is not a movement binding
    const ScriptObject* carrier = nullptr;
    bool ambiguous = false;
    for (const Component* other : platforms) {
      const auto matched = result.retail.find(other->entity);
      Vec3 p, r, s;
      if (matched == result.retail.end() || objects[matched->second].type != kRetailPlatform ||
          !room.Xform(*other, p, r, s) || p != pos || r != rot || s != scale ||
          platformSignature(*other) != signature || movementProperties(*other) != movementProperties(*c)) continue;
      if (carrier && carrier != &objects[matched->second]) ambiguous = true;
      carrier = &objects[matched->second];
    }
    if (carrier && !ambiguous) result.platformTransform[c->entity] = carrier;
  }
  // A grade hint's senders that are no retail object: the sender (PortRoomEnv's
  // kSenderPlayerFluid/kSenderCameraWater) and state (0: in, 1: out), or sender 0.
  std::set<int> hintEntities;
  for (const Component& c : comps) {
    if ((c.type == kColorGradeHint || c.type == kBacklight || c.type == kVolumetricFogHint ||
         c.type == kVolumetricFogRegion || c.type == kAutoExposureHint) &&
        c.entity >= 0) {
      hintEntities.insert(c.entity);
    }
  }
  std::set<int> transitionEntities;
  for (const Component& c : comps) {
    if (c.type == kVolumetricFogRegionTransition && c.entity >= 0) {
      hintEntities.insert(c.entity);
      transitionEntities.insert(c.entity);
    }
  }
  // Point and spot lights, whose links also play their timelines (LightAct).
  std::set<int> lightEntities;
  for (const Component& c : comps) {
    if (c.type != kLightDynamic || c.entity < 0) {
      continue;
    }
    const auto f = room.Flat(c);
    const auto type = f.find(kPropLightType);
    const uint32_t kind = type != f.end() && type->second.size >= 4 ? ReadLE32(room.Bytes(type->second)) : 0;
    if (kind == kLightPoint || kind == kLightSpot) {
      lightEntities.insert(c.entity);
    }
  }
  auto fluidSender = [&](const Connection& c, int& state) { return FluidSender(room, incoming, c, state); };
  // A counter Remastered added for a grade hint that counts what a retail counter counts (a
  // fight's deaths, say) is that retail counter: the one every matched sender feeding it also targets.
  auto RetailCounterFor = [&](int entity) -> const ScriptObject* {
    const auto in = incoming.find(entity);
    if (in == incoming.end()) {
      return nullptr;
    }
    std::vector<const ScriptObject*> feeds;
    for (const Connection* feed : in->second) {
      const int f = entityOf(int(feed->sender));
      if (f >= 0 && match[size_t(f)] >= 0) {
        feeds.push_back(&objects[size_t(match[size_t(f)])]);
      }
    }
    if (feeds.empty()) {
      return nullptr;
    }
    auto targets = [](const ScriptObject& from, const ScriptObject& to) {
      return std::find_if(from.targets.begin(), from.targets.end(), [&](uint32_t t) {
               return (t & 0x3ffffff) == (to.id & 0x3ffffff);
             }) != from.targets.end();
    };
    // Fed by every matched sender, and by as many retail objects as the Remastered counter has
    // feeds; one such counter, or none.
    const ScriptObject* found = nullptr;
    for (const ScriptObject& o : objects) {
      if (o.type != kRetailCounter ||
          !std::all_of(feeds.begin(), feeds.end(), [&](const ScriptObject* feed) { return targets(*feed, o); })) {
        continue;
      }
      size_t fedBy = 0;
      for (const ScriptObject& from : objects) {
        fedBy += targets(from, o) ? 1 : 0;
      }
      if (fedBy == in->second.size()) {
        // Two counting the same senders from the same start to the same maximum reach it
        // together (a Counter's first properties: initial count, maximum).
        if (found != nullptr && (!found->hasPos || !o.hasPos || std::memcmp(&found->pos, &o.pos, 8) != 0)) {
          return nullptr;
        }
        if (found == nullptr) {
          found = &o;
        }
      }
    }
    return found;
  };
  // A platform's Play -> Activate connections name the actors it carries
  // (CScriptPlatform::BuildSlaveList).
  for (const Connection& c : conns) {
    const int t = room.ByGuid(c.target), s = entityOf(int(c.sender));
    if (t < 0 || comps[size_t(t)].entity < 0 || s < 0 || match[size_t(s)] < 0 || c.action != kActionActivate) {
      continue;
    }
    const ScriptObject& sender = objects[size_t(match[size_t(s)])];
    if (sender.type == kRetailPlatform && RetailState(comps[c.sender].type, c.event) == kStatePlay) {
      result.platform[comps[size_t(t)].entity] = &sender;
    }
  }
  std::set<int> driverStarts;
  result.drivers = FindMaterialDrivers(room, driverStarts);
  // A room TriggerMP1 no retail object stands for, that a hint hears through Entered/Exited: its
  // box in the file's room volume table, by component; -1 when it detects nothing here (neither
  // the player nor the camera, or it starts inactive and nothing here activates it). An inactive
  // trigger tests nothing (Remastered's Touch, Think and camera test each check the active bit,
  // and Deactivate clears the touch list and the camera-inside bit without an Exited; kb topic
  // triggermp1-active-state.md), so one the room's script switches keeps its box with `on` clear
  // and the links that switch it.
  std::map<size_t, int> roomVolumeOf;
  const auto roomVolumeFor = [&](size_t comp) -> int {
    const auto known = roomVolumeOf.find(comp);
    if (known != roomVolumeOf.end()) {
      return known->second;
    }
    const Component& c = comps[comp];
    const auto f = room.Flat(c);
    const auto prop = f.find(kPropTriggerFlags);
    uint32_t flags = 1;  // absent: DetectPlayer
    if (prop != f.end() && prop->second.size == 4) {
      flags = ReadLE32(room.Bytes(prop->second));
    }
    const uint8_t detect = uint8_t(((flags & 0x1) != 0 ? PortRoomEnv::kRoomVolumePlayer : 0) |
                                   ((flags & kTriggerDetectCamera) != 0 ? PortRoomEnv::kRoomVolumeCamera : 0));
    // The script that switches this trigger, followed back to matched senders through whatever
    // passes the action on (a Relay's own): show activates it, hide deactivates it.
    std::vector<PortRoomEnv::GradeLink> toggles;
    std::vector<PortRoomEnv::GradeLink> selfToggles;
    std::vector<int> toggleSeen{int(c.entity)};
    std::function<void(int, uint8_t, int, int)> togglesFor = [&](int entity, uint8_t act, int depth, int via) {
      const auto in = incoming.find(entity);
      if (in == incoming.end()) {
        return;
      }
      for (const Connection* cc : in->second) {
        const Component& sender = comps[cc->sender];
        // Only the trigger takes the activate actions; what passes them on takes what fires it.
        uint8_t next = act;
        if (depth == 0) {
          const bool on = cc->action == kActionActivate || cc->action == kActionEntityActivate ||
                          cc->action == kActionTriggerActivate;
          const bool off = cc->action == kActionDeactivate || cc->action == kActionEntityDeactivate ||
                           cc->action == kActionTriggerDeactivate;
          next = on ? PortRoomGeo::kShow : off ? PortRoomGeo::kHide
                                               : cc->action == kActionToggleActive ? PortRoomGeo::kToggle : uint8_t(0);
        } else if (!PassFires(via, cc->action)) {
          continue;
        }
        if (next == 0) {
          continue;
        }
        const int s = entityOf(int(cc->sender));
        // The trigger switching itself on its own Entered or Exited (07_Intro_Reactor #884 turns
        // off as the player leaves): by its own volume, numbered once it is kept.
        if (sender.entity == c.entity && sender.type == kTriggerMP1 &&
            (cc->event == kEventEntered || cc->event == kEventExited)) {
          selfToggles.push_back({0, uint8_t(cc->event == kEventEntered ? 0 : 1), next});
          continue;
        }
        const int state = RetailState(sender.type, cc->event);
        if (s >= 0 && match[size_t(s)] >= 0 && state >= 0 && state < 256) {
          toggles.push_back({objects[size_t(match[size_t(s)])].id, uint8_t(state), next});
          continue;
        }
        const int type = RetailType(sender.type);
        if (IsPass(type) && sender.entity >= 0 && depth < 6 &&
            std::find(toggleSeen.begin(), toggleSeen.end(), sender.entity) == toggleSeen.end()) {
          toggleSeen.push_back(sender.entity);
          togglesFor(sender.entity, next, depth + 1, type);
          toggleSeen.pop_back();
        }
      }
    };
    if (c.entity >= 0) {
      togglesFor(c.entity, 0, 0, -1);
    }
    Vec3 pos, rot, scale;
    int index = -1;
    // An inactive trigger nothing here turns on (or toggles) never sees the player or the camera.
    const bool startsOn = room.Active(c);
    const bool switchedOn =
        std::any_of(toggles.begin(), toggles.end(), [](const PortRoomEnv::GradeLink& l) {
          return l.action == PortRoomGeo::kShow || l.action == PortRoomGeo::kToggle;
        });
    if (detect != 0 && (startsOn || switchedOn) && room.Xform(c, pos, rot, scale) &&
        result.roomVolumes.size() < PortRoomEnv::kMaxRoomVolumes) {
      PortRoomEnv::RoomVolume v;
      CameraBox(pos, rot, scale, v.centre, v.half, v.axes);
      v.detect = detect;
      v.on = startsOn;
      v.links = std::move(toggles);
      index = int(result.roomVolumes.size());
      for (PortRoomEnv::GradeLink& l : selfToggles) {
        l.sender = PortRoomEnv::kSenderRoomVolume + uint32_t(index);
        v.links.push_back(l);
      }
      result.roomVolumes.push_back(std::move(v));
    }
    roomVolumeOf[comp] = index;
    return index;
  };
  // A room's own variable modifiers, fired by a TriggerMP1 no retail object stands for (as
  // 07_Intro_Reactor's): by its room volume. A matched trigger's, or another sender's, is not
  // followed.
  for (const Connection& c : conns) {
    const int t = room.ByGuid(c.target);
    if (t < 0) {
      continue;
    }
    int modifier = comps[size_t(t)].type == kEnvModifier ? t : -1;
    for (size_t k = 0; modifier < 0 && comps[size_t(t)].type == kEntity && k < comps.size(); ++k) {
      if (comps[k].type == kEnvModifier && comps[k].entity == t) {
        modifier = int(k);
      }
    }
    PortRoomEnv::EnvOp op;
    if (modifier < 0 || !EnvModifierOp(room, comps[size_t(modifier)], c, op)) {
      continue;
    }
    const int s = entityOf(int(c.sender));
    const int volume = comps[c.sender].type == kTriggerMP1 && (c.event == kEventEntered || c.event == kEventExited) &&
                               (s < 0 || match[size_t(s)] < 0)
                           ? roomVolumeFor(c.sender)
                           : -1;
    if (volume < 0) {
      ++result.envUnresolved;
      continue;
    }
    op.sender = PortRoomEnv::kSenderRoomVolume + uint32_t(volume);
    op.state = uint8_t(c.event == kEventEntered ? 0 : 1);
    result.envOps.push_back(op);
  }
  for (const Ent& e : ents) {
    const auto v = votes.find(e.layer);
    if (v != votes.end() && !e.baseLayer) {
      auto best = v->second.begin();
      for (auto it = v->second.begin(); it != v->second.end(); ++it) {
        if (it->second > best->second) {
          best = it;
        }
      }
      result.layer[e.entity] = best->first;
    }
    std::vector<PortRoomGeo::Link> links;
    size_t bad = 0;
    std::string how;
    static const bool logMisses = port::EnvFlag("MP_REMASTERED_MISS");
    // One more connection not traced; a grade hint's are named in the import log.
    const auto miss = [&](const Component& sender, const Connection& c, uint32_t first, int depth) {
      ++bad;
      if (logMisses) {
        const int ms = entityOf(int(c.sender));
        const bool unmatched = ms < 0 || match[size_t(ms)] < 0;
        const bool nostate = RetailState(sender.type, c.event) < 0;
        char key[160];
        std::snprintf(key, sizeof key, "%s%s%s (target %08X, sender %08X event %08X action %08X, depth %d)",
                      unmatched ? "unmatched " : "", nostate ? "nostate " : "",
                      !unmatched && !nostate ? (depth >= 6 ? "depth " : "other ") : "", e.type, sender.type, c.event, first,
                      depth);
        auto& stat = result.missStats[key];
        ++stat.first;
        stat.second.insert(e.entity);
      }
      if (hintEntities.count(e.entity) != 0) {
        const int s = entityOf(int(c.sender));
        char text[96];
        std::snprintf(text, sizeof(text), " %08X/%08X/%08X->%08X@%d%s%s", sender.type, c.event, c.action, first, depth,
                      s < 0 || match[size_t(s)] < 0 ? " unmatched" : "",
                      RetailState(sender.type, c.event) < 0 ? " nostate" : "");
        how += text;
        const auto feeds = incoming.find(sender.entity);
        for (size_t f = 0; sender.entity >= 0 && feeds != incoming.end() && f < feeds->second.size(); ++f) {
          const Connection& fc = *feeds->second[f];
          const int fs = entityOf(int(fc.sender));
          std::snprintf(text, sizeof(text), " [%08X/%08X/%08X%s]", comps[fc.sender].type, fc.event, fc.action,
                        fs < 0 || match[size_t(fs)] < 0 ? " u" : "");
          how += text;
        }
      }
    };
    std::vector<int> seen{e.entity};
    const bool transition = transitionEntities.count(e.entity) != 0;
    const bool light = lightEntities.count(e.entity) != 0;
    // A driver's start (what the timers wait before it) is shown like an actor, to be read back.
    const bool driverStart = driverStarts.count(e.entity) != 0;
    // The show and hide of an actor a driver fades come through the script's longest chains.
    const int maxDepth = driverStart || result.drivers.count(e.entity) != 0 ? 8 : 6;
    const auto act = [&](uint32_t action) -> uint8_t {
      if (driverStart && action == kActionDriverStart) {
        return PortRoomGeo::kShow;
      }
      if (transition) {
        return TransitionAct(action);
      }
      const uint8_t lightAct = light ? LightAct(action) : 0;
      return lightAct != 0 ? lightAct : LinkAct(action);
    };
    // `delay`: what the timers passed through so far wait.
    std::function<void(int, uint32_t, int, int, float)> walk = [&](int entity, uint32_t action, int depth, int via,
                                                                   float delay) {
      const auto in = incoming.find(entity);
      if (in == incoming.end()) {
        return;
      }
      for (const Connection* c : in->second) {
        const Component& sender = comps[c->sender];
        int fluidState = 0;
        const uint32_t fluid =
            depth == 0 && hintEntities.count(entity) != 0 ? fluidSender(*c, fluidState) : 0;
        // Remastered drops what a hint does not take (a Relay's own action, say).
        if (depth == 0 && hintEntities.count(entity) != 0 &&
            (transition ? TransitionAct(c->action) == 0 : !HintTakes(c->action))) {
          continue;
        }
        if (fluid != 0) {
          const uint8_t fluidAct = act(c->action);
          if (fluidAct != 0) {
            links.push_back({fluid, uint8_t(fluidState), fluidAct});
          } else {
            miss(sender, *c, c->action, depth);
          }
          continue;
        }
        // A light that switches itself off as a backward play reaches its start (PointLightData::offAtStart).
        if (light && depth == 0 && sender.entity == e.entity && c->event == kEventLightReachedStart) {
          continue;
        }
        // A room TriggerMP1 of its own that switches a hint (directly or through relays): the
        // player or camera in its box, as a room volume. Camera ones are script objects too;
        // the geometry's script follows those for the actors.
        if (hintEntities.count(e.entity) != 0 && sender.type == kTriggerMP1 &&
            (c->event == kEventEntered || c->event == kEventExited) &&
            (depth == 0 || PassFires(via, c->action))) {
          const int ts = entityOf(int(c->sender));
          if (ts < 0 || match[size_t(ts)] < 0) {
            const uint8_t volumeAct = act(depth > 0 ? action : c->action);
            const int volume = volumeAct != 0 ? roomVolumeFor(c->sender) : -1;
            if (volume >= 0) {
              links.push_back({PortRoomEnv::kSenderRoomVolume + uint32_t(volume),
                               uint8_t(c->event == kEventEntered ? 0 : 1), volumeAct, delay});
              continue;
            }
          }
        }
        // Remastered's own objects, and keyframes, are followed by the script (below).
        if (sender.type == kTemplateManager || sender.type == kDebugOptions || isScriptObject(c->sender) ||
            (sender.type == kActorKeyframeMP1 && IsKeyframePlay(c->action)) ||
            (depth > 0 && !PassFires(via, c->action))) {
          continue;
        }
        const int s = entityOf(int(c->sender));
        const int state = RetailState(sender.type, c->event);
        const uint32_t first = depth > 0 ? action : c->action;
        const int type = RetailType(sender.type);
        if (s >= 0 && match[size_t(s)] >= 0 && state >= 0) {
          // A DamageableTrigger reads its own MaxReached -> Activate connections and never
          // sends them, so only a direct one counts.
          const bool follows = depth == 0 && type == kRetailDamageableTrigger && state == kStateMaxReached &&
                               first == kActionActivate;
          const uint8_t linkAct = follows ? static_cast<uint8_t>(PortRoomGeo::kFollow) : act(first);
          if (linkAct != 0 && state < 256) {
            links.push_back({objects[size_t(match[size_t(s)])].id, uint8_t(state), linkAct, delay});
          }
        } else if (IsPass(type) && sender.entity >= 0 && depth < maxDepth &&
                   std::find(seen.begin(), seen.end(), sender.entity) == seen.end()) {
          seen.push_back(sender.entity);
          float wait = 0.f;
          if (type == kRetailTimer) {
            const auto f = room.Flat(sender);
            const auto prop = f.find(kPropTimerDelay);
            if (prop != f.end() && prop->second.size == 4) {
              wait = ReadLEFloat(room.Bytes(prop->second));
              wait = std::isfinite(wait) ? std::clamp(wait, 0.f, 600.f) : 0.f;
            }
          }
          walk(sender.entity, first, depth + 1, type, delay + wait);
          seen.pop_back();
        } else if (const ScriptObject* counter = hintEntities.count(e.entity) != 0 && sender.type == kCounterMP1 &&
                                                         c->event == kEventCounterMP1Max
                                                     ? RetailCounterFor(sender.entity)
                                                     : nullptr) {
          const uint8_t counterAct = act(first);
          if (counterAct != 0) {
            links.push_back({counter->id, uint8_t(kStateMaxReached), counterAct, delay});
          } else {
            miss(sender, *c, first, depth);
          }
        } else {
          miss(sender, *c, first, depth);
        }
      }
    };
    walk(e.entity, 0, 0, -1, 0.f);
    if (!links.empty()) {
      result.links[e.entity] = std::move(links);
    }
    if (bad != 0) {
      result.unresolved[e.entity] = bad;
      if (!how.empty()) {
        result.unresolvedHow[e.entity] = how;
      }
    }
  }
  // Remastered's own objects that show and hide geometry, followed back from the geometry
  // through whatever drives them: the camera volumes that count which side of a wall the
  // camera is on, the counters they count with and the relays in between.
  // The same objects switch the suns (the hangar's intro shots and its escape's camera).
  std::set<int> geometry;  // entities with a ModCon, an added actor or a sun, point or spot light
  // CacheTargetFlags visits Play/Activate actor targets, regardless of room.
  // Refused modulators are absent from scriptObject and retain existing paths.
  std::set<int> alphaTargets;
  for (const Connection& link : conns) {
    const auto from = scriptObject.find(comps[link.sender].entity);
    if (from != scriptObject.end() && from->second.first == PortRoomGeo::kAlphaModulate &&
        link.event == 0x4ee99d94 && link.action == kActionActivate) {
      const int target = targetEntity(link);
      if (target >= 0) alphaTargets.insert(target);
    }
  }
  for (const Component& c : comps) {
    if (c.entity >= 0 && (c.type == kModCon || c.type == kPlatformComponent || (c.type == kActorMP1 &&
        (room.Flat(c).count(kPropActorAdded) != 0 || alphaTargets.count(c.entity) != 0)))) {
      geometry.insert(c.entity);
    } else if (c.entity >= 0 && c.type == kLightDynamic) {
      const auto f = room.Flat(c);
      const auto type = f.find(kPropLightType);
      if (type != f.end() && type->second.size >= 4) {
        const uint32_t kind = ReadLE32(room.Bytes(type->second));
        if (kind == kLightDirectional || kind == kLightPoint || kind == kLightSpot) {
          geometry.insert(c.entity);
        }
      }
    }
  }
  PortRoomGeo::Script& script = result.script;
  std::map<int, uint32_t> nodeOf;  // by entity
  std::vector<int> pending;
  auto nodeFor = [&](int entity) -> uint32_t {
    const auto known = nodeOf.find(entity);
    if (known != nodeOf.end()) {
      return known->second;
    }
    const auto& [kind, comp] = scriptObject.at(entity);
    const Component& c = comps[comp];
    PortRoomGeo::ScriptNode node;
    node.kind = kind;
    node.active = room.Active(c);
    if (kind == PortRoomGeo::kAlphaModulate) {
      ReadAlphaModulate(room, c, node);
    }
    if (kind == PortRoomGeo::kCounter) {
      const auto f = room.Flat(c);
      const auto max = f.find(kPropCounterMax);
      if (max != f.end() && max->second.size == 4) {
        node.max = ReadLE32(room.Bytes(max->second));
      }
    }
    Vec3 pos, rot, scale;
    if (kind == PortRoomGeo::kCameraVolume && !room.Xform(c, pos, rot, scale)) {
      // No box to be in: a volume no point is inside, even once something activates it.
      for (float& half : node.half) {
        half = -1.f;
      }
    } else if (kind == PortRoomGeo::kCameraVolume) {
      // The box is the entity's, here in area space as the instances are.
      CameraBox(pos, rot, scale, node.centre, node.half, node.axes);
    }
    nodeOf[entity] = uint32_t(script.nodes.size());
    script.nodes.push_back(node);
    pending.push_back(entity);
    return nodeOf[entity];
  };
  // The node event a script object's event is, -1 for one it does not send.
  auto nodeEvent = [](uint8_t kind, uint32_t event) -> int {
    switch (kind) {
    case PortRoomGeo::kCameraVolume:
      return event == kEventEntered ? 0 : event == kEventExited ? 1 : -1;
    case PortRoomGeo::kCounter:
      return event == kEventCounterNonZero ? 0 : event == kEventCounterZero ? 1 : event == kEventCounterMax ? 2 : -1;
    case PortRoomGeo::kAlphaModulate:
      return event == 0x4ee99d94 ? 3 : RetailState(kColorModulateMP1, event) == kStateMaxReached ? 2 : -1;
    default:
      return event == kEventRelayFired ? 0 : -1;
    }
  };
  // What an action asks of a group (kind 0) or a node of this kind, 0 for nothing known.
  auto scriptAction = [](uint8_t kind, uint32_t action) -> uint8_t {
    const bool on = action == kActionActivate || action == kActionEntityActivate;
    const bool off = action == kActionDeactivate || action == kActionEntityDeactivate;
    if (kind == 0) {
      return on ? PortRoomGeo::kGroupShow
             : off ? PortRoomGeo::kGroupHide
             : action == kActionToggleActive ? PortRoomGeo::kGroupToggle
                                              : 0;
    }
    if (on || (kind == PortRoomGeo::kCameraVolume && action == kActionTriggerActivate)) {
      return PortRoomGeo::kNodeActivate;
    }
    if (off || (kind == PortRoomGeo::kCameraVolume && action == kActionTriggerDeactivate)) {
      return PortRoomGeo::kNodeDeactivate;
    }
    if (kind == PortRoomGeo::kCounter) {
      return action == kActionCounterIncrement ? PortRoomGeo::kIncrement
             : action == kActionCounterDecrement ? PortRoomGeo::kDecrement
                                                 : 0;
    }
    if (kind == PortRoomGeo::kAlphaModulate) {
      return action == 0x8ed2a8c7 ? PortRoomGeo::kIncrement :
             action == 0x144d0f29 ? PortRoomGeo::kDecrement : 0;
    }
    return kind == PortRoomGeo::kRelay && action == kActionRelayFire ? PortRoomGeo::kFire : 0;
  };
  // The retail objects (id, state) a connection from outside the script comes from: its sender,
  // or what fires an unmatched Relay or MemoryRelay on the way (the relays the hangar's intro
  // shots count up and down the hull's counter with). Not through a Timer: an edge has no wait.
  std::function<void(const Connection&, int, std::vector<std::pair<uint32_t, int>>&)> fromRetail =
      [&](const Connection& c, int depth, std::vector<std::pair<uint32_t, int>>& out) {
        const Component& sender = comps[c.sender];
        const int s = entityOf(int(c.sender));
        const int state = RetailState(sender.type, c.event);
        if (s < 0 || state < 0 || state >= 256) {
          return;
        }
        if (match[size_t(s)] >= 0) {
          out.emplace_back(objects[size_t(match[size_t(s)])].id, state);
          return;
        }
        const int type = RetailType(sender.type);
        const auto in = sender.entity >= 0 ? incoming.find(sender.entity) : incoming.end();
        if (type == kRetailTimer || !IsPass(type) || depth >= 6 || in == incoming.end()) {
          return;
        }
        for (const Connection* fire : in->second) {
          if (PassFires(type, fire->action) && !isScriptObject(fire->sender)) {
            fromRetail(*fire, depth + 1, out);
          }
        }
      };
  // The edges into a geometry entity (group: from the script objects only, the walk above
  // has the rest) or a node (from anything).
  auto inputs = [&](int target, bool group, uint8_t kind) {
    const auto in = incoming.find(target);
    if (in == incoming.end()) {
      return;
    }
    for (const Connection* c : in->second) {
      const Component& sender = comps[c->sender];
      const bool fromScript = isScriptObject(c->sender);
      // A matched ActorKeyframe's Play moves an animated actor on to its next clip.
      const bool keyframe = group && sender.type == kActorKeyframeMP1 && IsKeyframePlay(c->action);
      if (sender.type == kTemplateManager || sender.type == kDebugOptions || (group && !fromScript && !keyframe)) {
        continue;
      }
      const bool modulate = group && fromScript && scriptObject.at(sender.entity).first == PortRoomGeo::kAlphaModulate &&
                            c->event == 0x4ee99d94 && c->action == kActionActivate;
      const uint8_t action = modulate ? static_cast<uint8_t>(PortRoomGeo::kGroupModulate) :
          keyframe ? static_cast<uint8_t>(PortRoomGeo::kGroupNextClip) : scriptAction(kind, c->action);
      std::vector<std::pair<uint32_t, int>> senders;  // (from, event)
      if (fromScript) {
        const int event = nodeEvent(scriptObject.at(sender.entity).first, c->event);
        if (event >= 0) {
          senders.emplace_back(0, event);
        }
      } else if (keyframe) {
        const int s = entityOf(int(c->sender));
        const int state = RetailState(sender.type, c->event);
        if (s >= 0 && match[size_t(s)] >= 0 && state >= 0 && state < 256) {
          senders.emplace_back(objects[size_t(match[size_t(s)])].id, state);
        }
      } else {
        fromRetail(*c, 0, senders);
      }
      if (action == 0 || senders.empty()) {
        ++result.scriptUnresolved;
        continue;
      }
      PortRoomGeo::ScriptEdge edge;
      edge.retail = !fromScript;
      edge.action = action;
      if (group) {
        const uint32_t next = uint32_t(result.group.size());
        edge.to = result.group.emplace(target, next).first->second;
        if (keyframe) {
          result.keyframes[target].push_back(c->sender);
        } else if (action != PortRoomGeo::kGroupHide) {
          result.scriptShows.insert(target);
        }
      } else {
        edge.to = nodeOf.at(target);
      }
      for (const auto& [from, event] : senders) {
        edge.from = fromScript ? nodeFor(sender.entity) : from;
        edge.event = uint8_t(event);
        script.edges.push_back(edge);
      }
    }
  };
  for (int entity : geometry) {
    inputs(entity, true, 0);
  }
  for (size_t i = 0; i < pending.size(); ++i) {
    const int entity = pending[i];
    inputs(entity, false, scriptObject.at(entity).first);
  }
  return result;
}

// The world master's hints (grade, backlight and fog hints that are not global) are turned on
// and off by the master's own script objects, which match no retail object: camera-detecting
// TriggerMP1s, through Relays, and the camera water proxies. Each trigger an active hint link
// reaches becomes an entry of `volumes` (its box in retail world: GameCube axes plus `shift`,
// the world's), and the link's sender kSenderWorldVolume + its index. What else drives them (a
// SpecialFunction, say) stays unresolved and is counted in `unresolved` / `unresolvedHow`.
SceneryScripts MasterScripts(const Room& room, const Vec3& shift, std::vector<PortRoomEnv::WorldVolume>& volumes) {
  SceneryScripts result;
  const std::vector<Component>& comps = room.Components();
  const std::vector<Connection> conns = ReadConnections(room);
  std::map<int, std::vector<const Connection*>> incoming;  // by target entity component
  for (const Connection& c : conns) {
    const int t = room.ByGuid(c.target);
    const int entity = t < 0 ? -1 : comps[size_t(t)].type == kEntity ? t : comps[size_t(t)].entity;
    if (entity >= 0) {
      incoming[entity].push_back(&c);
    }
  }
  std::set<int> hints;
  for (const Component& c : comps) {
    if ((c.type == kColorGradeHint || c.type == kBacklight || c.type == kVolumetricFogHint) && c.entity >= 0) {
      hints.insert(c.entity);
    }
  }
  std::map<size_t, int> volumeOf;  // trigger component -> index, -1: none to be in
  auto volumeFor = [&](size_t comp) -> int {
    const auto known = volumeOf.find(comp);
    if (known != volumeOf.end()) {
      return known->second;
    }
    const Component& c = comps[comp];
    const auto f = room.Flat(c);
    const auto prop = f.find(kPropTriggerFlags);
    uint32_t flags = 1;  // absent: DetectPlayer
    if (prop != f.end() && prop->second.size == 4) {
      flags = ReadLE32(room.Bytes(prop->second));
    }
    const uint8_t detect = uint8_t(((flags & 0x1) != 0 ? PortRoomEnv::kRoomVolumePlayer : 0) |
                                   ((flags & kTriggerDetectCamera) != 0 ? PortRoomEnv::kRoomVolumeCamera : 0));
    Vec3 pos, rot, scale;
    int index = -1;
    // Only an active trigger sees the player or the camera, and nothing here activates one.
    if (detect != 0 && room.Active(c) && room.Xform(c, pos, rot, scale) &&
        volumes.size() < PortRoomEnv::kMaxWorldVolumes) {
      PortRoomEnv::WorldVolume v;
      CameraBox(pos, rot, scale, v.centre, v.half, v.axes);
      v.detect = detect;
      for (int i = 0; i < 3; ++i) {
        v.centre[i] += float(shift[size_t(i)]);
      }
      index = int(volumes.size());
      volumes.push_back(v);
    }
    volumeOf[comp] = index;
    return index;
  };
  // An EnvironmentVarQuery's criteria connection, as a branch: its index, -1 when it does not read.
  std::map<const Connection*, int> branchOf;
  const auto branchFor = [&](const Connection& c) -> int {
    const auto known = branchOf.find(&c);
    if (known != branchOf.end()) {
      return known->second;
    }
    const auto f = room.Flat(comps[c.sender]);
    const auto var = f.find(kPropQueryVar);
    const auto flag = [&](uint32_t id) {
      const auto p = f.find(id);
      return p != f.end() && p->second.size >= 1 && room.Bytes(p->second)[0] != 0;
    };
    int index = -1;
    if (var != f.end() && var->second.size == 16 && c.hasCriteria && c.criteriaOp <= 5 &&
        result.envBranches.size() < PortRoomEnv::kMaxEnvBranches) {
      PortRoomEnv::EnvBranch b;
      std::memcpy(b.var.data(), room.Bytes(var->second), 16);
      b.relop = uint8_t(c.criteriaOp);
      b.value = c.criteriaValue;
      b.flags = uint8_t((flag(kPropQueryOnLoad) ? PortRoomEnv::kEnvOnLoad : 0) |
                        (flag(kPropQueryOnChange) ? PortRoomEnv::kEnvOnChange : 0) |
                        (flag(kPropQuerySkipUnchanged) ? PortRoomEnv::kEnvSkipUnchanged : 0));
      index = int(result.envBranches.size());
      result.envBranches.push_back(b);
    }
    branchOf[&c] = index;
    return index;
  };
  for (const int hint : hints) {
    std::vector<PortRoomGeo::Link> links;
    size_t bad = 0;
    std::string how;
    std::vector<int> seen{hint};
    const auto miss = [&](const Connection& c, uint32_t first, int depth) {
      ++bad;
      char text[64];
      std::snprintf(text, sizeof(text), " %08X/%08X/%08X->%08X@%d", comps[c.sender].type, c.event, c.action, first, depth);
      how += text;
    };
    std::function<void(int, uint32_t, int)> walk = [&](int entity, uint32_t action, int depth) {
      const auto in = incoming.find(entity);
      if (in == incoming.end()) {
        return;
      }
      for (const Connection* c : in->second) {
        const Component& sender = comps[c->sender];
        // A hint drops what it does not take; a Relay passes on only what fires it.
        if (depth == 0 ? !HintTakes(c->action) : c->action != kActionRelayFire) {
          continue;
        }
        const uint32_t first = depth == 0 ? c->action : action;
        const uint8_t act = LinkAct(first);
        int state = 0;
        const uint32_t fluid = FluidSender(room, incoming, *c, state);
        if (fluid != 0 && act != 0) {
          links.push_back({fluid, uint8_t(state), act});
        } else if (sender.type == kTriggerMP1 && (c->event == kEventEntered || c->event == kEventExited) && act != 0 &&
                   volumeFor(c->sender) >= 0) {
          links.push_back({PortRoomEnv::kSenderWorldVolume + uint32_t(volumeFor(c->sender)),
                           uint8_t(c->event == kEventEntered ? 0 : 1), act});
        } else if (sender.type == kEnvQuery && c->event == kEventEnvQueryTrue && act != 0 && branchFor(*c) >= 0) {
          links.push_back({PortRoomEnv::kSenderEnvQuery + uint32_t(branchFor(*c)), 0, act});
        } else if (sender.type == kRelay && c->event == kEventRelayFired && sender.entity >= 0 && depth < 6 &&
                   std::find(seen.begin(), seen.end(), sender.entity) == seen.end()) {
          seen.push_back(sender.entity);
          walk(sender.entity, first, depth + 1);
          seen.pop_back();
        } else {
          miss(*c, first, depth);
        }
      }
    };
    walk(hint, 0, 0);
    if (!links.empty()) {
      result.links[hint] = std::move(links);
    }
    if (bad != 0) {
      result.unresolved[hint] = bad;
      result.unresolvedHow[hint] = how;
    }
  }
  // The world's variables, and the master's triggers that change them.
  result.envVars = ReadEnvVars(room);
  for (const Connection& c : conns) {
    const int t = room.ByGuid(c.target);
    if (t < 0) {
      continue;
    }
    int modifier = comps[size_t(t)].type == kEnvModifier ? t : -1;
    for (size_t k = 0; modifier < 0 && comps[size_t(t)].type == kEntity && k < comps.size(); ++k) {
      if (comps[k].type == kEnvModifier && comps[k].entity == t) {
        modifier = int(k);
      }
    }
    PortRoomEnv::EnvOp op;
    if (modifier < 0 || !EnvModifierOp(room, comps[size_t(modifier)], c, op)) {
      continue;
    }
    const int volume = comps[c.sender].type == kTriggerMP1 && (c.event == kEventEntered || c.event == kEventExited)
                           ? volumeFor(c.sender)
                           : -1;
    if (volume < 0) {
      ++result.envUnresolved;
      continue;
    }
    op.sender = PortRoomEnv::kSenderWorldVolume + uint32_t(volume);
    op.state = uint8_t(c.event == kEventEntered ? 0 : 1);
    result.envOps.push_back(op);
  }
  return result;
}

}  // namespace RoomDetail
}  // namespace PortRemastered
