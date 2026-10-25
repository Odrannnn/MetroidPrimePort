// The world master's script resolver (port_remastered_room_scripts.cpp): FluidSender, which names
// the senders retail has no object for (the player in a fluid, the camera under water, and the
// InventoryActivator the camera water proxy fires), and MasterScripts, which follows a fog hint's
// links back through Relays to those and to camera-detecting TriggerMP1s. Also the rooms' own
// effect actors as a .roomfx (port_remastered_room_effects.cpp). The ROOM files are built here,
// with only the components and connections these read.

#include "port_remastered_room_internal.h"
#include "port_queen_barrier_assets.h"
#include "port_room_effects.h"
#include "port_test.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <filesystem>
#include <cstdlib>

using namespace PortRemastered;
using namespace PortRemastered::RoomDetail;

namespace {

using Bytes = std::vector<uint8_t>;

void PutLE(Bytes& b, uint64_t v, int n) {
  for (int i = 0; i < n; ++i) {
    b.push_back(uint8_t(v >> (8 * i)));
  }
}

void PutFloat(Bytes& b, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  PutLE(b, u, 4);
}

void Append(Bytes& to, const Bytes& from) {
  to.insert(to.end(), from.begin(), from.end());
}

// A chunk (24 byte header) or a form (32 byte header, `id` at 20).
Bytes Chunk(const char* id, const Bytes& body) {
  Bytes b(id, id + 4);
  PutLE(b, body.size(), 8);
  b.resize(24);
  Append(b, body);
  return b;
}

Bytes Form(const char* id, const Bytes& body) {
  Bytes b{'R', 'F', 'R', 'M'};
  PutLE(b, body.size(), 8);
  b.resize(20);
  b.insert(b.end(), id, id + 4);
  b.resize(32);
  Append(b, body);
  return b;
}

// A property list: u16 count, then per property u32 id, u16 size, the data.
struct Props {
  Bytes b{0, 0};
  Props& Add(uint32_t id, const Bytes& data) {
    PutLE(b, id, 4);
    PutLE(b, data.size(), 2);
    Append(b, data);
    ++b[0];
    return *this;
  }
  Props& AddU32(uint32_t id, uint32_t v) {
    Bytes d;
    PutLE(d, v, 4);
    return Add(id, d);
  }
};

Bytes Guid(size_t n) {
  Bytes g(16, 0);
  g[0] = uint8_t(n);
  g[15] = 0xA5;
  return g;
}

// A room of components, each with an entity of its own. Component n has the guid Guid(n) and
// the entity N + n (N components); an entity starts active unless `active` is false, at `pos`
// with `scale` and no rotation.
struct Fixture {
  struct Link {
    size_t target;  // component
    uint32_t event, action;
  };
  struct Comp {
    uint32_t type;
    Bytes props;
    std::vector<Link> links;
    bool active = true;
    float pos[3] = {0, 0, 0};
    float scale[3] = {1, 1, 1};
    Bytes guid;  // the component's own, Guid(n) when empty: the barrier's graph is keyed by them
  };
  std::vector<Comp> comps;

  // Component n's index in the room, its entity's N + n.
  size_t Entity(size_t n) const { return comps.size() + n; }

  Bytes Build() const {
    const size_t n = comps.size();
    std::vector<Bytes> guidOf;
    for (size_t i = 0; i < n; ++i) {
      guidOf.push_back(comps[i].guid.empty() ? Guid(i) : comps[i].guid);
    }
    Bytes sden, idta, records;
    for (size_t i = 0; i < 2 * n; ++i) {
      const bool entity = i >= n;
      const Comp& c = comps[entity ? i - n : i];
      // A component's data starts with 4 bytes the reader skips. An entity's: the active flag,
      // a byte, then position, rotation and scale.
      Bytes raw(4, 0);
      if (entity) {
        raw.push_back(c.active ? 1 : 0);
        raw.push_back(0);
        for (const float f : {c.pos[0], c.pos[1], c.pos[2], 0.f, 0.f, 0.f, c.scale[0], c.scale[1], c.scale[2]}) {
          PutFloat(raw, f);
        }
      } else {
        Append(raw, c.props.empty() ? Bytes{0, 0} : c.props);
      }
      Append(sden, Chunk("SDEN", raw));
      // The id data: the guid, 8 bytes (not 0xffffffff first), the connection count and the
      // connections (event, action, target guid, 2 bytes, two empty optional strings, 19 bytes);
      // an entity names its component instead: 'PMOC' and its guid.
      const Bytes& gid = guidOf[entity ? i - n : i];
      Bytes id = entity ? Guid(i) : gid;  // an entity's own guid stays the fixture's
      PutLE(id, 0, 8);
      if (entity) {
        PutLE(id, 0, 2);
        id.insert(id.end(), {'P', 'M', 'O', 'C'});
        Append(id, gid);
      } else {
        PutLE(id, c.links.size(), 2);
        for (const Link& l : c.links) {
          PutLE(id, l.event, 4);
          PutLE(id, l.action, 4);
          Append(id, guidOf[l.target]);  // the target's own guid, custom or the fixture's
          id.resize(id.size() + 2);
          PutLE(id, 0, 4);
          PutLE(id, 0, 4);
          id.resize(id.size() + 19);
        }
      }
      Append(idta, Chunk("IDTA", id));
      PutLE(records, entity ? kEntity : c.type, 4);
      PutLE(records, i, 4);
      PutLE(records, i, 4);
    }
    Bytes body;
    Append(body, Form("SDTA", [&] {
             Bytes b = sden;
             Append(b, idta);
             return b;
           }()));
    Append(body, Form("LYRS", Form("LAYR", Form("SRIP", Chunk("COMP", records)))));
    return Form("ROOM", body);
  }
};

// The fixture parsed; its bytes live as long as the object.
struct Parsed {
  Bytes bytes;
  Room room;
  explicit Parsed(const Fixture& f) : bytes(f.Build()) {
    std::string error;
    if (!room.Parse(bytes, error)) {
      std::fprintf(stderr, "fixture does not parse: %s\n", error.c_str());
      port_test::CountFailure();
    }
  }
};

// What FluidSender makes of the connection from component `sender` with `event`.
uint32_t Sender(const Fixture& f, size_t sender, uint32_t event, int& state) {
  const Parsed p(f);
  const std::vector<Component>& comps = p.room.Components();
  const std::vector<Connection> conns = ReadConnections(p.room);
  std::map<int, std::vector<const Connection*>> incoming;  // by target entity, as MasterScripts keeps it
  for (const Connection& c : conns) {
    const int t = p.room.ByGuid(c.target);
    const int entity = t < 0 ? -1 : comps[size_t(t)].type == kEntity ? t : comps[size_t(t)].entity;
    if (entity >= 0) {
      incoming[entity].push_back(&c);
    }
  }
  state = -1;
  for (const Connection& c : conns) {
    if (c.sender == sender && c.event == event) {
      return FluidSender(p.room, incoming, c, state);
    }
  }
  std::fprintf(stderr, "no connection from %zu with event %08x\n", sender, event);
  port_test::CountFailure();
  return 0;
}

Bytes U32(uint32_t v) {
  Bytes b;
  PutLE(b, v, 4);
  return b;
}

Fixture::Comp Plain(uint32_t type, std::vector<Fixture::Link> links = {}) {
  Fixture::Comp c;
  c.type = type;
  c.links = std::move(links);
  return c;
}

Fixture::Comp Special(uint32_t type, uint32_t item, std::vector<Fixture::Link> links) {
  Fixture::Comp c = Plain(kSpecialFunction, std::move(links));
  c.props = Props().AddU32(kPropSpecialFunctionType, type).AddU32(kPropSpecialFunctionItem, item).b;
  return c;
}

void TestFluidSenders() {
  int state = -1;
  // 0 ProxyPlayer and 1 CameraWaterProxy, each telling the hint (2) in and out.
  Fixture f;
  f.comps = {Plain(kProxyPlayer, {{2, kEventPlayerFluidIn, kActionHintOn}, {2, kEventPlayerFluidOut, kActionHintOff}}),
             Plain(kCameraWaterProxy, {{2, kEventCameraWaterIn, kActionHintOn}, {2, kEventCameraWaterOut, kActionHintOff}}),
             Plain(kColorGradeHint)};
  CHECK_MSG(Sender(f, 0, kEventPlayerFluidIn, state) == PortRoomEnv::kSenderPlayerFluid && state == 0, "player in a fluid");
  CHECK_MSG(Sender(f, 0, kEventPlayerFluidOut, state) == PortRoomEnv::kSenderPlayerFluid && state == 1, "player out of it");
  CHECK_MSG(Sender(f, 1, kEventCameraWaterIn, state) == PortRoomEnv::kSenderCameraWater && state == 0, "camera under water");
  CHECK_MSG(Sender(f, 1, kEventCameraWaterOut, state) == PortRoomEnv::kSenderCameraWater && state == 1, "camera out of it");

  // 0 CameraWaterProxy -> 1 InventoryActivator (item 21) -> 2 hint, the shape of the masters'
  // water fogs.
  const Fixture::Link feed{1, kEventCameraWaterIn, kActionAction};
  const auto water = [&](uint32_t function, uint32_t item, std::vector<Fixture::Link> feeds) {
    Fixture w;
    w.comps = {Plain(kCameraWaterProxy, std::move(feeds)),
               Special(function, item,
                       {{2, kEventSpecialFunctionZero, kActionFogHintOn}, {2, kEventSpecialFunctionMaxReached, kActionFogHintOff}}),
               Plain(kVolumetricFogHint)};
    return w;
  };
  const uint32_t item21 = PortRoomEnv::kSenderCameraWaterItem + 21;
  const Fixture ok = water(kSpecialFunctionInventoryActivator, 21, {feed});
  CHECK_MSG(Sender(ok, 1, kEventSpecialFunctionZero, state) == item21 && state == 0, "item held: state 0");
  CHECK_MSG(Sender(ok, 1, kEventSpecialFunctionMaxReached, state) == item21 && state == 1, "item missing: state 1");
  CHECK_MSG(Sender(water(kSpecialFunctionInventoryActivator, 0, {feed}), 1, kEventSpecialFunctionZero, state) ==
                    PortRoomEnv::kSenderCameraWaterItem &&
                state == 0,
            "item 0 is an item");
  CHECK_MSG(Sender(water(4, 21, {feed}), 1, kEventSpecialFunctionZero, state) == 0, "another function type");
  CHECK_MSG(Sender(water(kSpecialFunctionInventoryActivator, PortRoomEnv::kCameraWaterItems, {feed}), 1,
                   kEventSpecialFunctionZero, state) == 0,
            "an item past the table");
  CHECK_MSG(Sender(water(kSpecialFunctionInventoryActivator, PortRoomEnv::kCameraWaterItems - 1, {feed}), 1,
                   kEventSpecialFunctionMaxReached, state) == PortRoomEnv::kSenderCameraWaterItem + 63,
            "the last item of the table");
  // A second feed that is no camera water proxy, then one with another event or action.
  Fixture extra = water(kSpecialFunctionInventoryActivator, 21, {feed});
  extra.comps.push_back(Plain(kRelay, {{1, kEventRelayFired, kActionAction}}));
  CHECK_MSG(Sender(extra, 1, kEventSpecialFunctionZero, state) == 0, "a feed that is not a camera water proxy");
  CHECK_MSG(Sender(water(kSpecialFunctionInventoryActivator, 21, {feed, {1, kEventCameraWaterOut, kActionAction}}), 1,
                   kEventSpecialFunctionZero, state) == 0,
            "a feed with another event");
  CHECK_MSG(Sender(water(kSpecialFunctionInventoryActivator, 21, {{1, kEventCameraWaterIn, kActionActivate}}), 1,
                   kEventSpecialFunctionZero, state) == 0,
            "a feed with another action");
  // A function nothing feeds, and an event it does not send.
  Fixture unfed = water(kSpecialFunctionInventoryActivator, 21, {});
  CHECK_MSG(Sender(unfed, 1, kEventSpecialFunctionZero, state) == 0, "nothing feeds it");
  Fixture other = ok;
  other.comps[1].links.push_back({2, kEventEntered, kActionFogHintOn});
  CHECK_MSG(Sender(other, 1, kEventEntered, state) == 0, "an event it does not send");
}

bool Same(const PortRoomGeo::Link& l, uint32_t sender, uint8_t state, uint8_t action) {
  return l.sender == sender && l.state == state && l.action == action;
}

void TestMasterTrigger() {
  // 0 TriggerMP1 (sees the camera) Entered -> 1 Relay -> 3 hint On, Exited -> 2 Relay -> hint Remove.
  Fixture f;
  Fixture::Comp trigger = Plain(kTriggerMP1, {{1, kEventEntered, kActionRelayFire}, {2, kEventExited, kActionRelayFire}});
  trigger.props = Props().AddU32(kPropTriggerFlags, kTriggerDetectCamera | 1).b;
  trigger.pos[0] = 10.f;
  trigger.pos[1] = 20.f;
  trigger.pos[2] = 30.f;
  trigger.scale[0] = trigger.scale[1] = trigger.scale[2] = 8.f;
  f.comps = {trigger, Plain(kRelay, {{3, kEventRelayFired, kActionFogHintOn}}),
             Plain(kRelay, {{3, kEventRelayFired, kActionFogHintRemove}}), Plain(kVolumetricFogHint)};
  const Parsed p(f);
  const int hint = int(f.Entity(3));

  std::vector<PortRoomEnv::WorldVolume> at0, shifted;
  const SceneryScripts base = MasterScripts(p.room, {0, 0, 0}, at0);
  const SceneryScripts moved = MasterScripts(p.room, {100, 200, 300}, shifted);
  CHECK_MSG(base.links.size() == 1 && base.links.count(hint) == 1, "the hint's entity has links");
  CHECK_MSG(base.unresolved.empty(), "nothing left unresolved");
  if (base.links.count(hint) != 0) {
    const auto& links = base.links.at(hint);
    CHECK_MSG(links.size() == 2, "two links");
    bool in = false, out = false;
    for (const PortRoomGeo::Link& l : links) {
      in = in || Same(l, PortRoomEnv::kSenderWorldVolume + 0, 0, PortRoomGeo::kShow);
      out = out || Same(l, PortRoomEnv::kSenderWorldVolume + 0, 1, PortRoomGeo::kHide);
    }
    CHECK_MSG(in, "entering the volume turns the hint on");
    CHECK_MSG(out, "leaving it takes the hint out");
  }
  CHECK_MSG(at0.size() == 1 && shifted.size() == 1, "one volume, shared by both links");
  if (at0.size() == 1 && shifted.size() == 1) {
    // CameraBox's box in retail axes is (-x, z, y) of the entity; the world's shift is added.
    CHECK_MSG(at0[0].centre[0] == -10.f && at0[0].centre[1] == 30.f && at0[0].centre[2] == 20.f, "the box's centre");
    CHECK_MSG(shifted[0].centre[0] == 90.f && shifted[0].centre[1] == 230.f && shifted[0].centre[2] == 320.f,
              "the shift is added to the centre");
    CHECK_MSG(shifted[0].half[0] == at0[0].half[0], "and not to its size");
  }
  // An inactive trigger sees no camera: no volume, and the links are left unresolved.
  Fixture off = f;
  off.comps[0].active = false;
  const Parsed po(off);
  std::vector<PortRoomEnv::WorldVolume> none;
  const SceneryScripts idle = MasterScripts(po.room, {0, 0, 0}, none);
  CHECK_MSG(none.empty() && idle.links.empty() && idle.unresolved.count(hint) == 1, "an inactive trigger has no volume");
  // And one that does not detect the camera.
  Fixture blind = f;
  blind.comps[0].props = Props().AddU32(kPropTriggerFlags, 1).b;
  const Parsed pb(blind);
  const SceneryScripts deaf = MasterScripts(pb.room, {0, 0, 0}, none);
  CHECK_MSG(none.empty() && deaf.links.empty(), "a trigger that misses the camera has no volume");
}

void TestMasterCameraWater() {
  // 0 CameraWaterProxy -> 1 InventoryActivator (item 21): Zero -> 2 Relay -> 4 hint On, MaxReached
  // -> 3 Relay -> hint Remove.
  Fixture f;
  f.comps = {Plain(kCameraWaterProxy, {{1, kEventCameraWaterIn, kActionAction}}),
             Special(kSpecialFunctionInventoryActivator, 21,
                     {{2, kEventSpecialFunctionZero, kActionRelayFire}, {3, kEventSpecialFunctionMaxReached, kActionRelayFire}}),
             Plain(kRelay, {{4, kEventRelayFired, kActionFogHintOn}}),
             Plain(kRelay, {{4, kEventRelayFired, kActionFogHintRemove}}), Plain(kVolumetricFogHint)};
  const Parsed p(f);
  const int hint = int(f.Entity(4));
  std::vector<PortRoomEnv::WorldVolume> volumes;
  const SceneryScripts r = MasterScripts(p.room, {0, 0, 0}, volumes);
  CHECK_MSG(volumes.empty(), "no trigger, no volume");
  CHECK_MSG(r.unresolved.count(hint) == 0 && r.unresolvedHow.count(hint) == 0, "the hint is fully resolved");
  CHECK_MSG(r.links.count(hint) == 1 && r.links.at(hint).size() == 2, "two links");
  if (r.links.count(hint) != 0 && r.links.at(hint).size() == 2) {
    const uint32_t sender = PortRoomEnv::kSenderCameraWaterItem + 21;
    bool held = false, missing = false;
    for (const PortRoomGeo::Link& l : r.links.at(hint)) {
      held = held || Same(l, sender, 0, PortRoomGeo::kShow);
      missing = missing || Same(l, sender, 1, PortRoomGeo::kHide);
    }
    CHECK_MSG(held, "with the item the fog comes on");
    CHECK_MSG(missing, "without it the fog is taken out");
  }
  // The same chain on another function is not resolved.
  Fixture wrong = f;
  wrong.comps[1] = Special(4, 21, f.comps[1].links);
  const Parsed pw(wrong);
  const SceneryScripts u = MasterScripts(pw.room, {0, 0, 0}, volumes);
  CHECK_MSG(u.links.empty() && u.unresolved.count(hint) == 1 && u.unresolved.at(hint) == 2, "another function stays unresolved");
}

// An EffectMP1 with GENP Guid(100 + genp) and, if `vars`, one external variable (a real).
Fixture::Comp EffectActor(int genp, bool vars = false) {
  Fixture::Comp c = Plain(kEffectMP1);
  Props props;
  if (genp >= 0) {
    props.Add(0x88fc08e6, Guid(size_t(100 + genp)));
  }
  if (vars) {
    Props guid;
    guid.Add(0x82dde61c, Guid(200));
    Bytes value;
    PutFloat(value, 0.65f);
    Props entry;
    entry.Add(0xae893172, guid.b).Add(0xfd41dc4b, value);
    Bytes list = U32(1);
    Append(list, entry.b);
    props.Add(0x3def0de6, list);
  }
  c.props = props.b;
  return c;
}

// An Effect object's properties as the room reader hands them over: nine four-byte ones the
// import does not read, then the object's own PART at byte 36 (big endian), which is where it
// compares a matched actor's GENP with it (port_remastered_room_effects.cpp).
Bytes ObjectProps(uint32_t part) {
  Bytes props(36, 0);
  for (int shift = 24; shift >= 0; shift -= 8) {
    props.push_back(uint8_t(part >> shift));
  }
  return props;
}

ScriptObject RetailObject(uint32_t id, uint8_t type, Vec3 pos, std::vector<uint32_t> targets = {},
                          uint32_t ownPart = 0) {
  ScriptObject o;
  o.id = id;
  o.type = type;
  o.hasPos = true;
  o.pos = pos;
  o.targets = std::move(targets);
  if (ownPart != 0) {
    o.props = ObjectProps(ownPart);
  }
  return o;
}

void TestRoomEffects() {
  // 0 added (layer 1, with a variable), 1 matched to a retail object, 2 connected (3 links to it),
  // 4 no GENP, 5 a GENP with no PART, 6 on a layer no entity matched, 7 matched with particle
  // variables (its retail object takes them).
  Fixture f;
  f.comps = {EffectActor(0, true), EffectActor(1), EffectActor(0), Plain(kRelay, {{2, kEventRelayFired, kActionAction}}),
             EffectActor(-1), EffectActor(2), EffectActor(0), EffectActor(3, true)};
  f.comps[0].pos[0] = 1.f;
  f.comps[0].pos[1] = 2.f;
  f.comps[0].pos[2] = 3.f;
  const Parsed p(f);
  Area area;
  area.mrea = 0x1A2B3C4D;
  area.xf = {{{1, 0, 0, 10}, {0, 1, 0, 20}, {0, 0, 1, 30}}};
  const Vec3 w = Apply(area.xf, MulR2G({1, 2, 3}));
  const Vec3 near{w[0] + 1, w[1], w[2]};
  // 0 near the added one: replaced. 1 far. 2 near but a retail connection reaches it. 3 sends
  // that. 4 the one actor 1 matched, whose GENP maps to its own PART, so it keeps it. 5 the one
  // actor 7 matched, which has no PART of its own and takes Remastered's.
  constexpr uint32_t kOwnRetailPart = 0x12345678;  // the PART actor 1's GENP maps to
  area.objects = {RetailObject(0x041A0010, 7, near), RetailObject(0x041A0011, 7, {w[0] + 50, w[1], w[2]}),
                  RetailObject(0x041A0012, 7, near), RetailObject(0x041A0013, 0x15, near, {0x041A0012}),
                  RetailObject(0x041A0014, 7, near, {}, kOwnRetailPart),
                  RetailObject(0x041A0015, 7, {w[0] + 50, w[1], w[2]})};
  SceneryScripts scripts;
  scripts.layer[int(f.Entity(0))] = 1;
  scripts.retail[int(f.Entity(1))] = 4;
  scripts.retail[int(f.Entity(7))] = 5;
  std::map<std::string, Bytes> written;
  RoomIO io;
  io.write = [&](const std::string& name, const Bytes& data) {
    written[name] = data;
    return true;
  };
  io.effect = [](const std::array<uint8_t, 16>& genp) -> uint32_t {
    if (genp == SwapUuid(Guid(101).data())) {
      return kOwnRetailPart;  // the object's own PART: it keeps its retail effect
    }
    return genp == SwapUuid(Guid(102).data()) ? 0 : 0xFE000000u | genp[0];
  };
  const std::string line = WriteRoomEffects("room", p.room, area.mrea, area, scripts, io);
  CHECK_MSG(line == "  room: 1 effect actor(s) added, 1 retail effect(s) replaced, 1 retail effect(s) take "
                    "Remastered's effect, 1 connected and left out, 2 matched a retail object (1 of them still on the "
                    "retail PART), 2 without a PART, 1 on an unknown layer",
            line.c_str());
  REQUIRE_MSG(written.size() == 1 && written.count("1A2B3C4D.roomfx") == 1, "one .roomfx, named by the MREA");
  PortRoomEffects::File file;
  std::string error;
  REQUIRE_MSG(PortRoomEffects::Parse(written.begin()->second, file, error), error.c_str());
  CHECK_MSG(file.skips == std::vector<uint32_t>{0x041A0010}, "the near unmatched, untargeted one is skipped");
  CHECK(file.connected == 1);
  REQUIRE(file.layers.size() == 1 && file.layers[0].layer == 1 && file.layers[0].count == 1);
  PortRoomEffects::Effect e;
  size_t used = 0;
  REQUIRE(PortRoomEffects::ReadEffect(file.layers[0].objects.data(), file.layers[0].objects.size(), e, used));
  CHECK(e.id == PortRoomEffects::AddOnId(1, 0x1A, 0));
  CHECK(e.part == (0xFE000000u | SwapUuid(Guid(100).data())[0]));
  CHECK(e.pos[0] == float(w[0]) && e.pos[1] == float(w[1]) && e.pos[2] == float(w[2]));
  CHECK(e.active && e.scale[0] == 1.f && e.duration == 0.2f && e.xrayVisorVisible);
  REQUIRE(file.layers[0].vars.size() == 1 && file.layers[0].vars[0].size() == 1);
  const PortRoomEffects::Variable& v = file.layers[0].vars[0][0];
  CHECK(v.real == 0.65f && std::equal(v.guid.begin(), v.guid.end(), Guid(200).begin()));

  // The object actor 7 matched takes the actor's PART (it has no PART of its own) and its
  // particle variable, and is left out of the add-ons.
  REQUIRE_MSG(file.overrides.size() == 1, "one override");
  const PortRoomEffects::Override& over = file.overrides[0];
  CHECK(over.id == 0x041A0015 && over.part == (0xFE000000u | SwapUuid(Guid(103).data())[0]));
  CHECK(over.hasPlacement && over.pos[0] == 10.f && over.pos[1] == 20.f && over.pos[2] == 30.f);
  REQUIRE(over.vars.size() == 1);
  CHECK(over.vars[0].real == 0.65f && std::equal(over.vars[0].guid.begin(), over.vars[0].guid.end(), Guid(200).begin()));

  // A carried retail fallback above kept its placement. Once io.matched proves
  // the PART was converted from this GENP, import placement even without variables.
  io.matched = [](const std::array<uint8_t, 16>& genp) -> std::vector<uint32_t> {
    return genp == SwapUuid(Guid(101).data()) ? std::vector<uint32_t>{kOwnRetailPart} : std::vector<uint32_t>{};
  };
  WriteRoomEffects("room", p.room, area.mrea, area, scripts, io);
  REQUIRE_MSG(PortRoomEffects::Parse(written.begin()->second, file, error), error.c_str());
  REQUIRE(file.overrides.size() == 2);
  CHECK(file.overrides[0].id == 0x041A0014 && file.overrides[0].vars.empty());
  CHECK(file.overrides[0].hasPlacement && file.overrides[0].pos[0] == 10.f &&
        file.overrides[0].pos[1] == 20.f && file.overrides[0].pos[2] == 30.f);

  // A retail object already in the add-on range: nothing written.
  written.clear();
  Area taken = area;
  taken.objects.push_back(RetailObject(PortRoomEffects::AddOnId(0, 0x1A, 3), 7, near));
  const std::string refused = WriteRoomEffects("room", p.room, area.mrea, taken, scripts, io);
  CHECK_MSG(refused.find("is in the add-on id range") != std::string::npos && written.empty(), refused.c_str());
  // A GENP several retail PARTs share: the object matched to the actor keeps its own retail id
  // (the mod converted the GENP under it) and takes the actor's particle variables, which set
  // what differs between them (the lava spit's big and small).
  {
    Fixture g;
    g.comps = {EffectActor(0, true)};
    const Parsed q(g);
    constexpr uint32_t kSharedPart = 0x99998888;  // the object's own PART, one the GENP is shared with
    Area shared = area;
    shared.objects = {RetailObject(0x041A0016, 7, Apply(area.xf, MulR2G({0, 0, 0})), {}, kSharedPart)};
    SceneryScripts sharedScripts;
    sharedScripts.retail[int(g.Entity(0))] = 0;
    std::map<std::string, Bytes> out;
    RoomIO io2;
    io2.write = [&](const std::string& name, const Bytes& data) {
      out[name] = data;
      return true;
    };
    io2.effect = [](const std::array<uint8_t, 16>&) -> uint32_t { return 0; };  // no PART under the GENP's id
    io2.matched = [](const std::array<uint8_t, 16>& genp) -> std::vector<uint32_t> {
      return genp == SwapUuid(Guid(100).data()) ? std::vector<uint32_t>{kSharedPart} : std::vector<uint32_t>{};
    };
    const std::string l = WriteRoomEffects("room", q.room, area.mrea, shared, sharedScripts, io2);
    CHECK_MSG(l.find("1 retail effect(s) take Remastered's effect, 0 connected") != std::string::npos, l.c_str());
    PortRoomEffects::File f2;
    std::string e2;
    REQUIRE_MSG(out.count("1A2B3C4D.roomfx") == 1 && PortRoomEffects::Parse(out["1A2B3C4D.roomfx"], f2, e2), e2.c_str());
    REQUIRE_MSG(f2.overrides.size() == 1, "one override");
    CHECK(f2.overrides[0].id == 0x041A0016 && f2.overrides[0].part == kSharedPart);
    REQUIRE(f2.overrides[0].vars.size() == 1 && f2.overrides[0].vars[0].real == 0.65f);
    CHECK_MSG(f2.layers.empty() && f2.skips.empty(), "nothing added, nothing skipped");
  }
  // Without an effect lookup the stage writes nothing and says nothing.
  io.effect = nullptr;
  CHECK(WriteRoomEffects("room", p.room, area.mrea, area, scripts, io).empty() && written.empty());
}


// ---------------------------------------------------------------------------
// The Parasite Queen barrier (port_remastered_room_effects.cpp): its Remastered graph, its
// retail rings, and the .roomfx group the import writes only when both are the room's own.
// ---------------------------------------------------------------------------

// The links the shipped ROOM makes (port_remastered_script_tables.inc has the events).
constexpr uint32_t kBarrierPlay = 0x6b565a4c;       // SpecialFunctionMP1 Play
constexpr uint32_t kBarrierToPlatform = 0x2302215c;
constexpr uint32_t kBarrierToEffect = 0xdbe5cb30;
constexpr uint32_t kBarrierFollow = 0x7dcfa087;
constexpr uint32_t kPlatformActive = 0x28e11b3f;
constexpr uint32_t kPlatformPlay = 0xdb8148e3;
constexpr uint32_t kPlatformWaypoint = 0x5d191757;
constexpr uint32_t kPlatformMP1 = 0xa12967fb;
constexpr uint32_t kWaypointMP1 = 0x8e4c86ec;
constexpr uint32_t kPropModel = 0xf8b4c8da;
constexpr uint32_t kPropCollision = 0x02d5ab29;
constexpr uint32_t kPropDuration = 0xc9b0ad29;
constexpr uint32_t kPropFunctionType = 0x0d86d1bf;
constexpr uint32_t kPropGenp = 0x88fc08e6;
constexpr uint32_t kModelDummy = 0x1C0B0BEF;    // 10000000-0000-f000-f000-00001c0b0bef's id
constexpr uint32_t kCollisionDummy = 0xD4C81052;
constexpr uint32_t kRetailModel = 0x6556EEC5;
// The waypoints' heights, as the shipped ROOM has them.
constexpr float kWaypointY[5] = {-28.226179f, -23.060255f, -17.894337f, -12.728418f, -7.5625f};

int HexNibble(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// A guid's 16 bytes in the order a ROOM property stores them (which is Variable::guid's too),
// from the printed form the asset table holds.
Bytes Stored(const char* text) {
  Bytes canonical(16, 0);
  size_t at = 0;
  int high = -1;
  for (const char* p = text; *p != '\0'; ++p) {
    const int nibble = HexNibble(*p);
    if (nibble < 0) {
      continue;
    }
    if (high < 0) {
      high = nibble;
      continue;
    }
    canonical[at++] = uint8_t(high << 4 | nibble);
    high = -1;
  }
  static const int kOrder[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
  Bytes out(16);
  for (int i = 0; i < 16; ++i) {
    out[size_t(i)] = canonical[size_t(kOrder[i])];
  }
  return out;
}

// The retail id a carried-over model is written with: 10000000-0000-f000-f000-0000XXXXXXXX.
Bytes CarriedModel(uint32_t id) {
  Bytes b = {0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0xf0, 0xf0, 0x00, 0x00, 0x00};
  b.push_back(uint8_t(id >> 24));  // big endian, as a retail property holds it
  b.push_back(uint8_t(id >> 16));
  b.push_back(uint8_t(id >> 8));
  b.push_back(uint8_t(id));
  return b;
}

void PutBE32(Bytes& b, uint32_t v) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    b.push_back(uint8_t(v >> shift));
  }
}

// The retail ring controller: a SpecialFunction (ScriptLoader's 58) whose function sits where
// its head leaves it (position and orientation, 24 bytes), with `functionAt` for the negative
// test that puts it elsewhere.
ScriptObject RetailBarrierController(size_t functionAt, uint32_t function, std::vector<ScriptObject::Link> links) {
  ScriptObject o;
  o.id = PortQueenBarrier::kController;
  o.type = 0x3A;
  o.layer = 0;
  o.propCount = 15;
  o.hasPos = true;
  o.pos = Vec3{0, 0, 0};
  o.props.assign(functionAt, 0);
  PutBE32(o.props, function);
  o.links = std::move(links);
  for (const ScriptObject::Link& l : o.links) {
    o.targets.push_back(l.target);
  }
  return o;
}

// A retail ring platform: type 8, with SScaledActorHead (position, orientation, scale: 36
// bytes), its extent and centroid (12 each) and then its model, where ScriptLoader reads it.
ScriptObject RetailRingPlatform(uint32_t eid, uint32_t modelAt60, std::vector<ScriptObject::Link> links) {
  ScriptObject o;
  o.id = eid;
  o.type = 0x08;
  o.layer = 0;
  o.propCount = 19;
  o.hasPos = true;
  o.pos = Vec3{0, 5.875, 0};
  o.props.assign(24, 0);  // position and orientation
  for (int i = 0; i < 3; ++i) {
    PutBE32(o.props, 0x3F800000u);  // a scale of one
  }
  o.props.resize(60, 0);  // extent and centroid
  PutBE32(o.props, modelAt60);
  o.links = std::move(links);
  for (const ScriptObject::Link& l : o.links) {
    o.targets.push_back(l.target);
  }
  return o;
}

ScriptObject RetailWaypointObject(uint32_t eid, Vec3 pos) {
  ScriptObject o;
  o.id = eid;
  o.type = 0x02;
  o.layer = 0;
  o.propCount = 13;
  o.hasPos = true;
  o.pos = pos;
  o.props.assign(24, 0);
  return o;
}

// A SpecialFunctionMP1 of the given function, with the controller's duration when it is the
// main one.
Fixture::Comp SpecialFunction(uint32_t function, float duration, std::vector<Fixture::Link> links) {
  Fixture::Comp c = Plain(kSpecialFunction, std::move(links));
  Props props;
  props.AddU32(kPropFunctionType, function);
  if (function == 8) {
    Bytes v;
    PutFloat(v, duration);
    props.Add(kPropDuration, v);
  }
  c.props = props.b;
  return c;
}

// A PlatformMP1 with the dummy model and collision the five rings carry.
Fixture::Comp RingPlatform(uint32_t model, uint32_t collision, std::vector<Fixture::Link> links) {
  Fixture::Comp c = Plain(kPlatformMP1, std::move(links));
  Props props;
  props.Add(kPropModel, CarriedModel(model));
  props.Add(kPropCollision, CarriedModel(collision));
  c.props = props.b;
  return c;
}

// An EffectMP1 whose GENP is the table's guid.
Fixture::Comp RingEffect(const char* genp) {
  Fixture::Comp c = Plain(kEffectMP1);
  c.props = Props().Add(kPropGenp, Stored(genp)).b;
  return c;
}

// The room's barrier graph and its retail rings, ready to be mutated before it is parsed.
struct BarrierFixture {
  Fixture room;
  Area area;
  SceneryScripts scripts;
  std::map<std::string, Bytes> written;
  size_t writtenCount = 0;
  // Component indices.
  size_t controller = 0;
  size_t platform[5]{}, refraction[5]{}, glow[5]{}, waypoint[5]{}, follower[5][2]{};
  size_t idleFollower = 0, otherEffect = 0, relay = 0;
  size_t retailController = 0;
  size_t retailPlatform[5]{}, retailWaypoint[5]{};
  // What a conversion is asked for.
  struct Call {
    uint32_t part, feature;
    std::vector<std::array<uint8_t, 16>> vars;
  };
  std::vector<Call> calls;
  bool barrierEffectAnswers = true;   // the callback's answer
  bool barrierEffectAbsent = false;   // no callback at all
  bool noPart = false;                // one of the ten has no PART
};

// The whole valid fixture: one controller, five platforms and their ten effects, five
// waypoints, ten followers, an idle type 19 one, and an unrelated connected effect.
BarrierFixture MakeBarrier() {
  BarrierFixture b;
  std::vector<Fixture::Comp>& comps = b.room.comps;
  const size_t n = PortQueenBarrier::kRings.size();
  comps.push_back(SpecialFunction(8, 1.0f, {}));
  comps.back().guid = Stored(PortQueenBarrier::kControllerGuid);
  b.controller = 0;
  for (size_t r = 0; r < n; ++r) {
    b.platform[r] = comps.size();
    comps.push_back(RingPlatform(kModelDummy, kCollisionDummy, {}));
    comps.back().guid = Stored(PortQueenBarrier::kRings[r].component);
  }
  for (size_t r = 0; r < n; ++r) {
    b.refraction[r] = comps.size();
    comps.push_back(RingEffect(PortQueenBarrier::kRings[r].refraction));
    b.glow[r] = comps.size();
    comps.push_back(RingEffect(PortQueenBarrier::kRings[r].glow));
  }
  for (size_t r = 0; r < n; ++r) {
    for (size_t f = 0; f < 2; ++f) {
      b.follower[r][f] = comps.size();
      comps.push_back(SpecialFunction(19, 0.f, {}));
    }
  }
  for (size_t r = 0; r < n; ++r) {
    b.waypoint[r] = comps.size();
    comps.push_back(Plain(kWaypointMP1));
    comps.back().pos[1] = kWaypointY[r];
  }
  b.idleFollower = comps.size();
  comps.push_back(SpecialFunction(19, 0.f, {}));
  b.otherEffect = comps.size();
  comps.push_back(EffectActor(0));
  b.relay = comps.size();
  comps.push_back(Plain(kRelay, {}));
  const auto pair = [](size_t to, uint32_t event, uint32_t action) {
    return Fixture::Link{to, event, action};
  };
  // The controller plays its five platforms and its ten effects.
  for (size_t r = 0; r < n; ++r) {
    comps[b.controller].links.push_back(pair(b.platform[r], kBarrierPlay, kBarrierToPlatform));
    comps[b.controller].links.push_back(pair(b.refraction[r], kBarrierPlay, kBarrierToEffect));
    comps[b.controller].links.push_back(pair(b.glow[r], kBarrierPlay, kBarrierToEffect));
  }
  // Each platform is activated by the controller, links its two effects and plays its waypoint
  // (and, as the room's do, the shared dummy actor).
  for (size_t r = 0; r < n; ++r) {
    comps[b.platform[r]].links = {pair(b.refraction[r], kPlatformActive, kBarrierToEffect),
                                  pair(b.glow[r], kPlatformActive, kBarrierToEffect),
                                  pair(b.waypoint[r], kPlatformPlay, kPlatformWaypoint),
                                  pair(b.idleFollower, 0x28ade3c2, 0xa34e100f)};
  }
  // Each follower copies its platform's transform to its effect.
  for (size_t r = 0; r < n; ++r) {
    for (size_t f = 0; f < 2; ++f) {
      comps[b.follower[r][f]].links = {pair(b.platform[r], kBarrierPlay, kBarrierToPlatform),
                                       pair(f == 0 ? b.refraction[r] : b.glow[r], kBarrierPlay, kBarrierFollow)};
    }
  }
  // The unrelated connected actor: a Relay reaches it, so the generic pass leaves it out.
  comps[b.relay].links = {pair(b.otherEffect, kEventRelayFired, kActionAction)};
  // The retail room: the ring controller, its five platforms and their waypoints, all on layer
  // 0, with the waypoints where the Remastered ones are.
  b.area.mrea = PortQueenBarrier::kMrea;
  // The controller plays its five platforms (Play/Activate, as the disc's links do).
  std::vector<ScriptObject::Link> plays;
  for (size_t r = 0; r < n; ++r) {
    plays.push_back({18, 1, PortQueenBarrier::kRings[r].platform});  // kSS_Play, kSM_Activate
  }
  b.retailController = b.area.objects.size();
  b.area.objects.push_back(RetailBarrierController(24, 8, plays));
  for (size_t r = 0; r < n; ++r) {
    b.retailPlatform[r] = b.area.objects.size();
    // Each platform follows its waypoint (Play/Follow) and, as the disc's do, also activates
    // the shared dummy actor (Default/Activate).
    b.area.objects.push_back(RetailRingPlatform(
        PortQueenBarrier::kRings[r].platform, kRetailModel,
        {{18, 6, PortQueenBarrier::kRings[r].waypoint}, {30, 1, 0x0019006C}}));
    b.retailWaypoint[r] = b.area.objects.size();
    b.area.objects.push_back(RetailWaypointObject(PortQueenBarrier::kRings[r].waypoint,
                                                  Apply(b.area.xf, MulR2G(Vec3{0, kWaypointY[r], 0}))));
  }
  return b;
}

RoomIO BarrierIO(BarrierFixture& b) {
  RoomIO io;
  io.write = [&b](const std::string& name, const Bytes& data) {
    b.written[name] = data;
    ++b.writtenCount;
    return true;
  };
  io.effect = [&b](const std::array<uint8_t, 16>& genp) -> uint32_t {
    if (b.noPart && genp == SwapUuid(Stored(PortQueenBarrier::kRings[4].glow).data())) {
      return 0;
    }
    return 0xFE000000u | genp[0] | uint32_t(genp[1]) << 8;
  };
  if (!b.barrierEffectAbsent) {
    io.barrierEffect = [&b](uint32_t part, uint32_t feature, const std::vector<std::array<uint8_t, 16>>& vars) {
      b.calls.push_back({part, feature, vars});
      return b.barrierEffectAnswers;
    };
  }
  return io;
}

std::string WriteBarrier(BarrierFixture& b) {
  const Parsed p(b.room);
  RoomIO io = BarrierIO(b);
  return WriteRoomEffects("07_Intro_Reactor", p.room, b.area.mrea, b.area, b.scripts, io);
}

std::vector<std::array<uint8_t, 16>> BarrierGenps(const BarrierFixture& b) {
  const Parsed p(b.room);
  return RoomEffectGenpsOf(p.room);
}

// The group in the .roomfx the fixture wrote, or null when it wrote none.
bool WrittenBarrier(BarrierFixture& b, PortRoomEffects::File& file) {
  const auto it = b.written.find("87452DC1.roomfx");
  if (it == b.written.end()) {
    return false;
  }
  std::string error;
  return PortRoomEffects::Parse(it->second, file, error);
}

// The script matcher pairing the barrier's actor `member` with a retail Effect object, as it may
// for any actor of the room.
size_t MapMemberToRetail(BarrierFixture& b, size_t member) {
  const size_t at = b.area.objects.size();
  b.area.objects.push_back(RetailObject(0x0019A001, 7, Vec3{0, 5.875, 0}, {}, 0x12345678));
  b.scripts.retail[int(b.room.Entity(member))] = at;
  return at;
}

// A mutated fixture writes no group and no half of one: no effect is added, none is skipped and
// none becomes an override. With the Remastered graph itself broken, its ten GENPs are not
// admitted for conversion either (the retail side and the conversions come later).
void Refuses(BarrierFixture& b, const char* what, bool graphValid = false, const char* reason = nullptr) {
  b.written.clear();
  b.calls.clear();
  const std::string line = WriteBarrier(b);
  const std::string said = std::string(what) + " - " + line;
  PortRoomEffects::File file;
  CHECK_MSG((line.find("no barrier group") != std::string::npos), said);
  CHECK_MSG(!WrittenBarrier(b, file) || (file.barriers.empty() && file.layers.empty() && file.skips.empty() &&
                                         file.overrides.empty()),
            said);
  CHECK_MSG(graphValid || BarrierGenps(b).empty(), said);
  if (reason != nullptr) {
    CHECK_MSG(line.find(reason) != std::string::npos, said);
  }
}

void TestBarrierGroup() {
  // The valid graph: ten inactive effects on the controller's layer, the group with the table's
  // platforms, the two effect ids per ring and the three guids, and the callback asked for each
  // effect with its own feature and variables.
  {
    BarrierFixture b = MakeBarrier();
    const std::string line = WriteBarrier(b);
    CHECK_MSG(line.find("the barrier's 10 effects at 1 s") != std::string::npos, line.c_str());
    CHECK_MSG(b.calls.size() == 10, "the conversion is asked about all ten");
    PortRoomEffects::File file;
    REQUIRE_MSG(WrittenBarrier(b, file), "one .roomfx");
    REQUIRE_MSG(file.barriers.size() == 1, "one barrier group");
    const PortRoomEffects::BarrierGroup& group = file.barriers[0];
    CHECK(group.controller == PortQueenBarrier::kController && group.duration == 1.0f);
    CHECK_MSG(file.skips.empty() && file.overrides.empty() && file.connected == 1,
              "nothing skipped, no override, one other connected effect");
    REQUIRE_MSG(file.layers.size() == 1 && file.layers[0].layer == 0 && file.layers[0].count == 10,
                "ten effects on the controller's layer");
    for (size_t r = 0; r < group.rings.size(); ++r) {
      const PortRoomEffects::BarrierRing& ring = group.rings[r];
      const auto& row = PortQueenBarrier::kRings[r];
      CHECK(ring.platform == row.platform);
      CHECK(ring.refraction == PortRoomEffects::AddOnId(0, 0x19, uint32_t(2 * r)));
      CHECK(ring.glow == PortRoomEffects::AddOnId(0, 0x19, uint32_t(2 * r + 1)));
      const Bytes refractionGuid = Stored(row.breakupRefraction);
      const Bytes motionGuid = Stored(row.motion);
      const Bytes glowGuid = Stored(row.breakupGlow);
      CHECK(std::equal(ring.breakupRefraction.begin(), ring.breakupRefraction.end(), refractionGuid.begin()));
      CHECK(std::equal(ring.motion.begin(), ring.motion.end(), motionGuid.begin()));
      CHECK(std::equal(ring.breakupGlow.begin(), ring.breakupGlow.end(), glowGuid.begin()));
      // The callback: the refraction effect first, then the glow one.
      const auto& refraction = b.calls[2 * r];
      const auto& glow = b.calls[2 * r + 1];
      CHECK(refraction.feature == PortQueenBarrier::kRefractionFeature && refraction.vars.size() == 2);
      CHECK(refraction.vars.size() == 2 && std::equal(refraction.vars[0].begin(), refraction.vars[0].end(), refractionGuid.begin()) &&
            std::equal(refraction.vars[1].begin(), refraction.vars[1].end(), motionGuid.begin()));
      CHECK(glow.feature == PortQueenBarrier::kGlowFeature && glow.vars.size() == 1);
      CHECK(glow.vars.size() == 1 && std::equal(glow.vars[0].begin(), glow.vars[0].end(), glowGuid.begin()));
      CHECK(refraction.part != 0 && refraction.part != 0xFFFFFFFF);
    }
    // The effects themselves: inactive, with the ROOM's transform, and the retail PART.
    size_t at = 0;
    for (uint32_t i = 0; i < 10; ++i) {
      PortRoomEffects::Effect e;
      size_t used = 0;
      REQUIRE(PortRoomEffects::ReadEffect(file.layers[0].objects.data() + at, file.layers[0].objects.size() - at, e, used));
      CHECK(!e.active);
      CHECK(e.id == PortRoomEffects::AddOnId(0, 0x19, i));
      CHECK((e.part >> 24) == 0xFE);
      at += used;
    }
    CHECK(at == file.layers[0].objects.size());
    // Only the ten effects of a whole graph are admitted for conversion, in the table's order.
    const std::vector<std::array<uint8_t, 16>> genps = BarrierGenps(b);
    REQUIRE_MSG(genps.size() == 10, "ten GENPs admitted");
    for (size_t r = 0; r < 5; ++r) {
      CHECK(genps[2 * r] == SwapUuid(Stored(PortQueenBarrier::kRings[r].refraction).data()));
      CHECK(genps[2 * r + 1] == SwapUuid(Stored(PortQueenBarrier::kRings[r].glow).data()));
    }
  }
  // An ordinary add-on in the same layer first: the group's ids follow it, and none is used
  // twice.
  {
    BarrierFixture b = MakeBarrier();
    b.room.comps.push_back(EffectActor(1));
    b.room.comps.back().pos[0] = 100.f;
    b.scripts.layer[int(b.room.Entity(b.room.comps.size() - 1))] = 0;
    const std::string line = WriteBarrier(b);
    CHECK_MSG(line.find("1 effect actor(s) added") != std::string::npos &&
                  line.find("the barrier's 10 effects") != std::string::npos,
              line.c_str());
    PortRoomEffects::File file;
    REQUIRE_MSG(WrittenBarrier(b, file), "one .roomfx");
    REQUIRE(file.barriers.size() == 1);
    REQUIRE(file.layers.size() == 1 && file.layers[0].count == 11);
    CHECK(file.barriers[0].rings[0].refraction == PortRoomEffects::AddOnId(0, 0x19, 1));
    CHECK(file.barriers[0].rings[4].glow == PortRoomEffects::AddOnId(0, 0x19, 10));
    // The first object is the ordinary one, at its own id.
    PortRoomEffects::Effect first;
    size_t used = 0;
    REQUIRE(PortRoomEffects::ReadEffect(file.layers[0].objects.data(), file.layers[0].objects.size(), first, used));
    CHECK(first.id == PortRoomEffects::AddOnId(0, 0x19, 0));
  }
  // Absent, failing or partial conversion: nothing of the group is written.
  {
    BarrierFixture b = MakeBarrier();
    b.barrierEffectAbsent = true;
    Refuses(b, "no conversion callback", true, "no conversion for it");
  }
  {
    BarrierFixture b = MakeBarrier();
    b.barrierEffectAnswers = false;
    Refuses(b, "the conversion refuses an effect", true, "conversion is not the barrier's");
  }
  {
    BarrierFixture b = MakeBarrier();
    b.noPart = true;
    Refuses(b, "an effect has no PART", true, "an effect has no PART");
  }
  // The graph itself: a mutated controller, its links, a platform, a follower, a model, a
  // waypoint.
  {
    BarrierFixture b = MakeBarrier();
    b.room.comps[b.controller] = SpecialFunction(9, 1.0f, b.room.comps[b.controller].links);
    Refuses(b, "another function than the ring controller's", "its Remastered graph is not the one the table names");
  }
  {
    BarrierFixture b = MakeBarrier();
    b.room.comps[b.controller] = SpecialFunction(8, 0.f, b.room.comps[b.controller].links);
    Refuses(b, "a duration of zero", "its Remastered graph is not the one the table names");
  }
  {
    BarrierFixture b = MakeBarrier();
    b.room.comps[b.controller] = SpecialFunction(8, 61.f, b.room.comps[b.controller].links);
    Refuses(b, "a duration past a minute", "its Remastered graph is not the one the table names");
  }
  {
    BarrierFixture b = MakeBarrier();
    b.room.comps[b.controller].links.pop_back();
    Refuses(b, "the controller with fourteen links", "its Remastered graph is not the one the table names");
  }
  {
    BarrierFixture b = MakeBarrier();
    b.room.comps[b.controller].links.push_back({b.waypoint[0], kBarrierPlay, kBarrierToPlatform});
    Refuses(b, "the controller with sixteen links", "its Remastered graph is not the one the table names");
  }
  {
    BarrierFixture b = MakeBarrier();
    b.room.comps[b.platform[1]].links.erase(b.room.comps[b.platform[1]].links.begin());
    Refuses(b, "a platform with one effect", "its Remastered graph is not the one the table names");
  }
  {
    BarrierFixture b = MakeBarrier();
    b.room.comps[b.platform[2]].links[0].target = b.refraction[3];  // another ring's effect
    Refuses(b, "a platform linked to another ring's effect", "its Remastered graph is not the one the table names");
  }
  {
    BarrierFixture b = MakeBarrier();
    b.room.comps[b.platform[4]].links.erase(b.room.comps[b.platform[4]].links.begin() + 2);
    Refuses(b, "a platform with no waypoint", "its Remastered graph is not the one the table names");
  }
  {
    BarrierFixture b = MakeBarrier();
    b.room.comps[b.platform[0]] = RingPlatform(0x12345678, kCollisionDummy, b.room.comps[b.platform[0]].links);
    Refuses(b, "another model than the dummy one", "its Remastered graph is not the one the table names");
  }
  {
    BarrierFixture b = MakeBarrier();
    b.room.comps[b.platform[0]] = RingPlatform(kModelDummy, 0x12345678, b.room.comps[b.platform[0]].links);
    Refuses(b, "another collision than the dummy one", "its Remastered graph is not the one the table names");
  }
  {
    BarrierFixture b = MakeBarrier();
    b.room.comps[b.platform[0]].guid = Stored(PortQueenBarrier::kRings[1].component);  // not this room's platform
    Refuses(b, "a platform whose component is not the table's", "its Remastered graph is not the one the table names");
  }
  {
    BarrierFixture b = MakeBarrier();
    b.room.comps[b.follower[2][1]].links.clear();
    Refuses(b, "a pair without its follower", "its Remastered graph is not the one the table names");
  }
  {
    BarrierFixture b = MakeBarrier();
    b.room.comps[b.follower[2][1]].links = b.room.comps[b.follower[2][0]].links;  // two for one pair
    Refuses(b, "a pair with two followers", "its Remastered graph is not the one the table names");
  }
  {
    BarrierFixture b = MakeBarrier();
    b.room.comps[b.follower[0][0]].links[1].target = b.glow[0];  // the pair's other effect
    Refuses(b, "a follower on the wrong effect", "its Remastered graph is not the one the table names");
  }
  {
    BarrierFixture b = MakeBarrier();
    b.room.comps[b.follower[0][0]].links[0].target = b.platform[1];  // another ring's platform
    Refuses(b, "a follower on another ring's platform", "its Remastered graph is not the one the table names");
  }
  {
    BarrierFixture b = MakeBarrier();
    b.room.comps[b.follower[3][0]].links.push_back({b.platform[3], kBarrierPlay, kBarrierToPlatform});
    Refuses(b, "a follower with three links", "its Remastered graph is not the one the table names");
  }
  {
    BarrierFixture b = MakeBarrier();
    b.room.comps[b.refraction[1]] = RingEffect(PortQueenBarrier::kRings[0].refraction);  // a swapped GENP
    Refuses(b, "a ring with the wrong effect family", "its Remastered graph is not the one the table names");
  }
  // The retail side: the controller's function, a platform's model, a waypoint, the layer.
  {
    BarrierFixture b = MakeBarrier();
    std::vector<ScriptObject::Link> plays;
    for (size_t r = 0; r < 5; ++r) {
      plays.push_back({18, 1, PortQueenBarrier::kRings[r].platform});
    }
    b.area.objects[b.retailController] = RetailBarrierController(24, 7, plays);  // another function
    Refuses(b, "a retail controller of another function", true, "not a SpecialFunction (58)");
  }
  {
    BarrierFixture b = MakeBarrier();
    std::vector<ScriptObject::Link> plays;
    for (size_t r = 0; r < 5; ++r) {
      plays.push_back({18, 1, PortQueenBarrier::kRings[r].platform});
    }
    b.area.objects[b.retailController] = RetailBarrierController(36, 8, plays);  // the wrong offset
    Refuses(b, "a retail controller whose function is not at 24", true, "not a SpecialFunction (58)");
  }
  {
    BarrierFixture b = MakeBarrier();
    std::vector<ScriptObject::Link> plays;
    for (size_t r = 0; r < 5; ++r) {
      plays.push_back({18, 1, PortQueenBarrier::kRings[r].platform});
    }
    ScriptObject c = RetailBarrierController(24, 8, plays);
    c.propCount = 14;
    b.area.objects[b.retailController] = c;
    Refuses(b, "a retail controller with fourteen properties", true, "not a SpecialFunction (58)");
  }
  {
    BarrierFixture b = MakeBarrier();
    std::vector<ScriptObject::Link> plays;
    for (size_t r = 0; r < 5; ++r) {
      plays.push_back({18, 1, PortQueenBarrier::kRings[r].platform});
    }
    ScriptObject c = RetailBarrierController(24, 8, plays);
    c.type = 0x15;
    b.area.objects[b.retailController] = c;
    Refuses(b, "a retail controller of another type", true, "not a SpecialFunction (58)");
  }
  // The controller's own links: a wrong message, a wrong state, or one platform unplayed.
  {
    BarrierFixture b = MakeBarrier();
    std::vector<ScriptObject::Link> plays;
    for (size_t r = 0; r < 5; ++r) {
      plays.push_back({18, 1, PortQueenBarrier::kRings[r].platform});
    }
    plays[2].message = 20;  // kSM_Play, not kSM_Activate
    b.area.objects[b.retailController] = RetailBarrierController(24, 8, plays);
    Refuses(b, "a controller link with the wrong message", true, "not the Play/Activate one");
  }
  {
    BarrierFixture b = MakeBarrier();
    std::vector<ScriptObject::Link> plays;
    for (size_t r = 0; r < 5; ++r) {
      plays.push_back({18, 1, PortQueenBarrier::kRings[r].platform});
    }
    plays[0].state = 30;  // kSS_Default, not kSS_Play
    b.area.objects[b.retailController] = RetailBarrierController(24, 8, plays);
    Refuses(b, "a controller link with the wrong state", true, "not the Play/Activate one");
  }
  {
    BarrierFixture b = MakeBarrier();
    std::vector<ScriptObject::Link> plays;
    for (size_t r = 0; r < 4; ++r) {
      plays.push_back({18, 1, PortQueenBarrier::kRings[r].platform});
    }
    b.area.objects[b.retailController] = RetailBarrierController(24, 8, plays);
    Refuses(b, "a platform the controller does not play", true, "exactly five links");
  }
  {
    BarrierFixture b = MakeBarrier();
    b.area.objects[b.retailPlatform[0]] =
        RetailRingPlatform(PortQueenBarrier::kRings[0].platform, 0x12345678,
                           {{18, 6, PortQueenBarrier::kRings[0].waypoint}});
    Refuses(b, "a retail platform of another model", true, "model at 60");
  }
  // The right bytes, but in the wrong place: the model is read at 60, never searched for.
  {
    BarrierFixture b = MakeBarrier();
    ScriptObject p = RetailRingPlatform(PortQueenBarrier::kRings[0].platform, 0x12345678,
                                        {{18, 6, PortQueenBarrier::kRings[0].waypoint}});
    p.props[24] = 0x65;  // the model's bytes, where the scale is
    p.props[25] = 0x56;
    p.props[26] = 0xEE;
    p.props[27] = 0xC5;
    b.area.objects[b.retailPlatform[0]] = p;
    Refuses(b, "a model planted away from its offset", true, "model at 60");
  }
  {
    BarrierFixture b = MakeBarrier();
    ScriptObject p = RetailRingPlatform(PortQueenBarrier::kRings[0].platform, kRetailModel,
                                        {{18, 6, PortQueenBarrier::kRings[0].waypoint}});
    p.propCount = 18;
    b.area.objects[b.retailPlatform[0]] = p;
    Refuses(b, "a retail platform with eighteen properties", true, "19 properties");
  }
  {
    BarrierFixture b = MakeBarrier();
    b.area.objects[b.retailPlatform[0]] =
        RetailRingPlatform(PortQueenBarrier::kRings[0].platform, kRetailModel, {});
    Refuses(b, "a retail platform with no waypoint link", true, "does not link exactly one waypoint");
  }
  {
    BarrierFixture b = MakeBarrier();
    b.area.objects[b.retailPlatform[0]] =
        RetailRingPlatform(PortQueenBarrier::kRings[0].platform, kRetailModel,
                           {{18, 8, PortQueenBarrier::kRings[0].waypoint}});  // kSM_Next, not kSM_Follow
    Refuses(b, "the waypoint reached with the wrong message", true, "not the Play/Follow one");
  }
  {
    BarrierFixture b = MakeBarrier();
    b.area.objects[b.retailPlatform[0]] =
        RetailRingPlatform(PortQueenBarrier::kRings[0].platform, kRetailModel,
                           {{30, 6, PortQueenBarrier::kRings[0].waypoint}});  // the wrong state
    Refuses(b, "the waypoint reached in the wrong state", true, "not the Play/Follow one");
  }
  {
    BarrierFixture b = MakeBarrier();
    b.area.objects[b.retailPlatform[0]] =
        RetailRingPlatform(PortQueenBarrier::kRings[0].platform, kRetailModel,
                           {{18, 6, PortQueenBarrier::kRings[1].waypoint}});  // another ring's waypoint
    Refuses(b, "the wrong ring's waypoint", true, "does not link exactly one waypoint");
  }
  {
    BarrierFixture b = MakeBarrier();
    b.area.objects[b.retailWaypoint[0]].pos[1] += 0.5;  // the waypoint moved
    Refuses(b, "a retail waypoint off the Remastered one", true, "not where the Remastered one is");
  }
  {
    BarrierFixture b = MakeBarrier();
    b.area.objects[b.retailPlatform[0]].layer = 3;  // a platform on another layer than its controller
    Refuses(b, "a retail platform on another layer", true, "the controller's layer");
  }
  {
    BarrierFixture b = MakeBarrier();
    b.area.objects.erase(b.area.objects.begin() + std::ptrdiff_t(b.retailWaypoint[1]));
    Refuses(b, "a retail waypoint gone", true, "waypoint object is missing");
  }
  // A barrier whose conversion fails while the script matcher paired one of its actors with a
  // retail Effect: the actor is the barrier's, so it is neither an override nor an add-on, and
  // none of the group is written.
  {
    BarrierFixture b = MakeBarrier();
    MapMemberToRetail(b, b.refraction[0]);
    b.barrierEffectAnswers = false;
    Refuses(b, "a failed group with a matched actor", true, "conversion is not the barrier's");
    CHECK_MSG(b.written.empty(), "a failed group writes nothing at all");
  }
  // The same with a graph that does not validate: the table's GENP is known before any of it.
  {
    BarrierFixture b = MakeBarrier();
    MapMemberToRetail(b, b.glow[2]);
    b.room.comps[b.controller] = SpecialFunction(9, 1.0f, b.room.comps[b.controller].links);
    Refuses(b, "a malformed graph with a matched actor", false,
            "its Remastered graph is not the one the table names");
    CHECK_MSG(b.written.empty(), "a malformed graph writes nothing at all");
  }
  // The controller's links are exactly the five platforms: a sixth one, or one played twice,
  // is refused.
  {
    BarrierFixture b = MakeBarrier();
    ScriptObject c = b.area.objects[b.retailController];
    c.links.push_back({18, 1, 0x0019006C});
    b.area.objects[b.retailController] = c;
    Refuses(b, "a controller with a sixth link", true, "exactly five links");
  }
  {
    BarrierFixture b = MakeBarrier();
    ScriptObject c = b.area.objects[b.retailController];
    c.links[4].target = 0x0019006C;  // five links, one to something else
    b.area.objects[b.retailController] = c;
    Refuses(b, "a controller link to another object", true, "not to one of the five platforms");
  }
  {
    BarrierFixture b = MakeBarrier();
    ScriptObject c = b.area.objects[b.retailController];
    c.links[2].target = PortQueenBarrier::kRings[0].platform;
    b.area.objects[b.retailController] = c;
    Refuses(b, "one platform played twice", true, "is not the Play/Activate one, or names it twice");
  }
  // No retail objects at all: the stage says so and writes nothing, without touching them.
  {
    BarrierFixture b = MakeBarrier();
    b.area.objects.clear();
    b.written.clear();
    const Parsed p(b.room);
    RoomIO io = BarrierIO(b);
    const std::string line =
        WriteRoomEffects("07_Intro_Reactor", p.room, PortQueenBarrier::kMrea, b.area, b.scripts, io);
    CHECK_MSG(line.find("no retail objects") != std::string::npos, line.c_str());
    CHECK_MSG(b.written.empty() && b.calls.empty(), "no retail objects, nothing written");
  }
}

// The shipped ROOM's graph, when the extracted asset is on disk (it is not part of the
// repository, so this is skipped without it): the controller, its duration, the five platforms
// by the table's guids, and each ring's waypoint height.
void TestRealBarrierRoom() {
  const char* candidates[] = {"../mpr/rooms/07_Intro_Reactor/07_Intro_Reactor.ROOM",
                              "../../mpr/rooms/07_Intro_Reactor/07_Intro_Reactor.ROOM",
                              "../../../mpr/rooms/07_Intro_Reactor/07_Intro_Reactor.ROOM"};
  Bytes data;
  for (const char* path : candidates) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      continue;
    }
    data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    break;
  }
  if (data.empty()) {
    std::printf("note: no extracted 07_Intro_Reactor.ROOM, the real graph is not checked\n");
    return;
  }
  Room room;
  std::string error;
  REQUIRE_MSG(room.Parse(data, error), error.c_str());
  BarrierGraph graph;
  REQUIRE_MSG(BarrierGraphOf(room, graph), "the shipped ROOM's barrier graph");
  CHECK(graph.duration == 1.0f);
  for (size_t r = 0; r < graph.rings.size(); ++r) {
    CHECK_MSG(UuidText(room.Components()[graph.rings[r].platform].guid.data()) ==
                  PortQueenBarrier::kRings[r].component,
              "the table's ring order");
    Vec3 pos, rot, scale;
    REQUIRE(room.Xform(room.Components()[graph.rings[r].waypoint], pos, rot, scale));
    CHECK_MSG(std::fabs(pos[1] - kWaypointY[r]) < 0.001, "the ring's waypoint height");
  }
}

Fixture::Comp Density(float intensity) {
  Fixture::Comp c = Plain(kEnvFxDensityController);
  Bytes v;
  PutFloat(v, intensity);
  c.props = Props().Add(kPropEnvFxIntensity, v).b;
  return c;
}

// The density controller a RelayAutoFire reaches through Relays sets the intensity; one only a
// trigger fires, or nothing, does not.
void TestEnvFxIntensity() {
  Fixture f;
  // 0 controller 3.0 (a Relay's trigger fires it), 1 controller 5.0 (autofire 3 -> relay 2 -> 1),
  // 2 Relay, 3 RelayAutoFire, 4 a TriggerMP1 -> relay 5 -> controller 0, 5 Relay, 6 inert
  // controller 7.0.
  f.comps = {Density(3.f),
             Density(5.f),
             Plain(kRelay, {{1, kEventRelayFired, kActionEnvFxApply}}),
             Plain(kRelayAutoFire, {{2, 0x9a06ae6f, kActionRelayFire}}),
             Plain(kTriggerMP1, {{5, kEventEntered, kActionRelayFire}}),
             Plain(kRelay, {{0, kEventRelayFired, kActionEnvFxApply}}),
             Density(7.f)};
  {
    const Parsed p(f);
    std::vector<std::string> notes;
    CHECK_MSG(EnvFxIntensity(p.room, notes) == 5.f, "envfx: the autofire chain's controller");
    REQUIRE(notes.size() == 2);
    CHECK_MSG(notes[0].find("#0") != std::string::npos && notes[0].find("not fired at load") != std::string::npos &&
                  notes[0].find("F526FC2A") != std::string::npos,
              notes[0].c_str());
    CHECK_MSG(notes[1].find("#6") != std::string::npos && notes[1].find("nothing fires it") != std::string::npos,
              notes[1].c_str());
  }
  // A wrong action on the first hop breaks the chain: nothing fires at load.
  f.comps[3].links[0].action = kActionCounterIncrement;
  {
    const Parsed p(f);
    std::vector<std::string> notes;
    CHECK_MSG(EnvFxIntensity(p.room, notes) == 1.f && notes.size() == 3, "envfx: no chain, intensity 1");
  }
  // Two Relays deep.
  f.comps[3].links[0].action = kActionRelayFire;
  f.comps.push_back(Plain(kRelay, {{2, kEventRelayFired, kActionRelayFire}}));
  f.comps[3].links[0].target = 7;
  {
    const Parsed p(f);
    std::vector<std::string> notes;
    CHECK_MSG(EnvFxIntensity(p.room, notes) == 5.f, "envfx: two relays deep");
  }
  // A cycle of relays ends.
  f.comps[2].links.push_back({7, kEventRelayFired, kActionRelayFire});
  {
    const Parsed p(f);
    std::vector<std::string> notes;
    CHECK_MSG(EnvFxIntensity(p.room, notes) == 5.f, "envfx: relay cycle");
  }
}

void TestDynamoModulateGraph() {
  Fixture f;
  f.comps = {Plain(kActorMP1),
             Plain(kColorModulateMP1, {{0, 0x4ee99d94, kActionActivate}}),
             Plain(kRelay, {{1, kEventRelayFired, 0x144d0f29},
                            {1, kEventRelayFired, 0x8ed2a8c7}})};
  Bytes zero, duration;
  PutFloat(zero, 0.f);
  PutFloat(duration, 1.2f);
  f.comps[1].props = Props().Add(kPropModulateColorB, Props().Add(kPropColorRGBA[3], zero).b)
                               .Add(0x23fc9f0d, duration).Add(0xd8f7b602, zero)
                               .Add(0x3fff0f45, Bytes{1}).b;
  Area area;
  area.mrea = 0x12345678; // no room-specific permission to use the graph
  const Parsed p(f);
  const auto result = MatchScripts(p.room, area);
  REQUIRE(result.group.count(int(f.Entity(0))) == 1);
  REQUIRE(result.script.nodes.size() == 2);
  const auto& node = result.script.nodes[0];
  CHECK(node.kind == PortRoomGeo::kAlphaModulate);
  const auto c = node.AlphaConfig();
  CHECK(c.a == 1.f && c.b == 0.f && c.timeA2B == 1.2f && c.timeB2A == 0.f);
  CHECK(c.flags == (12 | 512));
  REQUIRE(result.script.edges.size() == 3);
  CHECK(result.script.edges[0].action == PortRoomGeo::kGroupModulate);
  CHECK(result.script.edges[1].action == PortRoomGeo::kDecrement);
  CHECK(result.script.edges[2].action == PortRoomGeo::kIncrement);
  // A sibling actor driven only by a relay is not a modulation target, even
  // though a supported modulator lives in the same room.
  f.comps.push_back(Plain(kActorMP1));
  f.comps[2].links.push_back({3, kEventRelayFired, kActionActivate});
  const Parsed unrelated(f);
  CHECK(MatchScripts(unrelated.room, area).group.count(int(f.Entity(3))) == 0);
  f.comps[1].props = Props().Add(0xf41e2ca0, Bytes{1}).b;
  const Parsed autoStart(f);
  CHECK(MatchScripts(autoStart.room, area).script.nodes.empty());
  // The importer must not reinterpret a non-alpha mode as this evaluator.
  f.comps[1].props = Props().AddU32(kPropModulateBlend, kModulateIncandescence).b;
  const Parsed unsupported(f);
  CHECK(MatchScripts(unsupported.room, area).script.nodes.empty());
  // Existing added scenery stays relay-driven when the incandescence
  // modulator is refused; its glow is still owned by port_remastered_room.cpp.
  f.comps[0].props = Props().Add(kPropActorAdded, Bytes{1}).b;
  f.comps[2].links.push_back({0, kEventRelayFired, kActionActivate});
  const Parsed glow(f);
  const auto oldPath = MatchScripts(glow.room, area);
  REQUIRE(oldPath.script.nodes.size() == 1);
  CHECK(oldPath.script.nodes[0].kind == PortRoomGeo::kRelay);
  REQUIRE(oldPath.script.edges.size() == 1);
  CHECK(oldPath.script.edges[0].action == PortRoomGeo::kGroupShow);
}

// Optional read-only shipped-data audit; no import output or game execution.
void AuditModulateRooms() {
  const char* root = std::getenv("PORT_MODULATE_AUDIT_ROOT");
  if (!root) return;
  size_t rooms = 0, affected = 0, nodes = 0, all = 0, autoStart = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    if (entry.path().extension() != ".ROOM") continue;
    std::ifstream in(entry.path(), std::ios::binary);
    Bytes bytes((std::istreambuf_iterator<char>(in)), {});
    Room room;
    std::string error;
    REQUIRE_MSG(room.Parse(bytes, error), entry.path().string().c_str());
    ++rooms;
    for (const auto& c : room.Components()) {
      if (c.type != kColorModulateMP1) continue;
      ++all;
      const auto f = room.Flat(c);
      const auto p = f.find(0xf41e2ca0);
      if (p != f.end() && p->second.size == 1 && room.Bytes(p->second)[0]) ++autoStart;
    }
    const auto result = MatchScripts(room, Area{});
    size_t count = 0;
    for (const auto& node : result.script.nodes) count += node.kind == PortRoomGeo::kAlphaModulate;
    if (count) {
      ++affected;
      nodes += count;
      std::printf("alpha-audit %s %zu\n", entry.path().string().c_str(), count);
    }
  }
  std::printf("alpha-audit totals rooms=%zu modulators=%zu bit1000=%zu new_rooms=%zu new_nodes=%zu\n",
              rooms, all, autoStart, affected, nodes);
}

}  // namespace

int main() {
  TestFluidSenders();
  TestMasterTrigger();
  TestMasterCameraWater();
  TestRoomEffects();
  TestBarrierGroup();
  TestRealBarrierRoom();
  TestEnvFxIntensity();
  TestDynamoModulateGraph();
  AuditModulateRooms();
  if (port_test::Failures() == 0) {
    std::printf("port_remastered_room_scripts_tests: ok\n");
  }
  return PORT_TEST_RESULT();
}
