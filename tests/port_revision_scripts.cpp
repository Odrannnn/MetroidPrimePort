// Revision script patches (PortRevisionScripts) on synthetic rooms: the
// version/room gate, the content of every op stream, the all-or-nothing miss
// path, the five USA 1.02 restores and the PAL / USA 1.01 normalizations.
// No disc data is used; the all-disc replay is tools/ (see docs).
#include "port_revision_scripts.h"
#include "port_skip_cutscenes.h"
#include "port_test.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace {

using PortDisc::Version;
using PortRevisionScripts::Result;
using Conn = PortSkipCutscenes::ScriptObject::Connection;

const uint32_t kGeothermal = 0xC0498676, kQuarry = 0x643D038F, kOre = 0x97D2B2F6,
               kCourtyard = 0x1921876D, kElite = 0x8A97BB54, kArboretum = 0x18AB6106,
               kHive = 0xC8309DF6, kDynamo = 0xFEA372E2;

struct Obj {
  std::vector< Conn > conns;
  int layer = 0;
  uint8_t type = 5;
  std::vector< uint8_t > props = {0, 0, 0, 1, 0}; // count 1, empty name
};
using Room = std::map< uint32_t, Obj >;

void Put32(std::vector< uint8_t >& v, uint32_t x) {
  for (int s = 24; s >= 0; s -= 8)
    v.push_back(uint8_t(x >> s));
}

// An SCLY section holding the objects on their layers (default: type 5, empty
// name, layer 0).
std::vector< uint8_t > Build(const Room& room) {
  int layers = 1;
  for (const auto& [id, obj] : room)
    layers = std::max(layers, obj.layer + 1);
  std::vector< uint8_t > scly = {'S', 'C', 'L', 'Y', 0, 0, 0, 1};
  Put32(scly, uint32_t(layers));
  std::vector< std::vector< uint8_t > > blobs;
  for (int l = 0; l < layers; ++l) {
  std::vector< uint8_t > layer = {0};
  uint32_t count = 0;
  for (const auto& [id, obj] : room)
    count += obj.layer == l;
  Put32(layer, count);
  for (const auto& [id, obj] : room) {
    if (obj.layer != l)
      continue;
    std::vector< uint8_t > body;
    Put32(body, id);
    Put32(body, uint32_t(obj.conns.size()));
    for (const Conn& c : obj.conns) {
      Put32(body, c.state);
      Put32(body, c.message);
      Put32(body, c.target);
    }
    body.insert(body.end(), obj.props.begin(), obj.props.end());
    layer.push_back(obj.type);
    Put32(layer, uint32_t(body.size()));
    layer.insert(layer.end(), body.begin(), body.end());
  }
  blobs.push_back(layer);
  }
  for (const auto& b : blobs)
    Put32(scly, uint32_t(b.size()));
  for (const auto& b : blobs)
    scly.insert(scly.end(), b.begin(), b.end());
  return scly;
}

Room Parse(const std::vector< uint8_t >& scly) {
  std::vector< PortSkipCutscenes::ScriptObject > objs;
  Room room;
  CHECK_MSG(PortSkipCutscenes::ScanObjects(scly.data(), scly.size(), objs), "result parses");
  for (const auto& o : objs) {
    room[o.id].conns = o.connections;
    room[o.id].layer = o.layer;
    room[o.id].type = o.type;
    room[o.id].props = o.props;
  }
  return room;
}

bool Has(const Room& r, uint32_t id) { return r.count(id) != 0; }
bool HasConn(const Room& r, uint32_t sender, uint32_t state, uint32_t msg, uint32_t target) {
  auto it = r.find(sender);
  if (it == r.end())
    return false;
  for (const Conn& c : it->second.conns)
    if (c.state == state && c.message == msg && c.target == target)
      return true;
  return false;
}

Room Ids(std::initializer_list< uint32_t > ids) {
  Room r;
  for (uint32_t id : ids)
    r[id];
  return r;
}

Result Run(Version v, uint32_t mrea, const Room& in, Room& out, int* misses = nullptr) {
  const std::vector< uint8_t > scly = Build(in);
  std::vector< uint8_t > bytes;
  const Result res = PortRevisionScripts::Apply(v, mrea, scly.data(), scly.size(), bytes, misses);
  if (res == Result::Patched)
    out = Parse(bytes);
  else
    CHECK_MSG(bytes.empty(), "out is empty unless patched");
  return res;
}

// Object props (count, empty name, `tail` zero bytes) of a given type and layer.
Obj Make(int layer, uint8_t type, size_t tail) {
  Obj o;
  o.layer = layer;
  o.type = type;
  o.props = {0, 0, 0, 1, 0};
  o.props.resize(5 + tail, 0);
  return o;
}
// Actor with a door lock model at 196 and the active byte at 341.
Obj Actor(int layer, uint32_t model, uint8_t active) {
  Obj o = Make(layer, 0x00, 354);
  for (int i = 0; i < 4; ++i)
    o.props[5 + 196 + i] = uint8_t(model >> (24 - 8 * i));
  o.props[5 + 341] = active;
  return o;
}
float Float(const Obj& o, size_t off) {
  uint32_t bits = uint32_t(o.props[5 + off]) << 24 | uint32_t(o.props[6 + off]) << 16 |
                  uint32_t(o.props[7 + off]) << 8 | o.props[8 + off];
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
uint8_t Byte(const Obj& o, size_t off) { return o.props[5 + off]; }

const Version kAll[] = {Version::Unknown, Version::Usa100, Version::Usa101, Version::Usa102,
                        Version::Pal, Version::Trilogy, Version::TrilogyPal};
const uint32_t kRooms[] = {kGeothermal, kQuarry, kOre, kCourtyard, kElite, kArboretum, kHive, kDynamo};

} // namespace

int main() {
  // Gate: exactly these (version, room) pairs have ops.
  for (Version v : kAll) {
    for (uint32_t room : kRooms) {
      bool expect = false;
      if (v == Version::Usa102)
        expect = room != kDynamo; // five locks + Arboretum + Hive Totem
      else if (v == Version::Usa101)
        expect = room == kArboretum;
      else if (v == Version::Pal)
        expect = room == kGeothermal || room == kOre || room == kQuarry || room == kDynamo;
      else if (PortDisc::IsTrilogy(v)) // PAL's, but its Main Quarry has no extra lock
        expect = room == kGeothermal || room == kOre || room == kDynamo;
      CHECK_MSG(PortRevisionScripts::OpsFor(v, room).empty() != expect, "version/room gate");
    }
    CHECK_MSG(PortRevisionScripts::OpsFor(v, 0x12345678).empty(), "unknown room has no ops");
  }
  CHECK_MSG(PortRevisionScripts::OpsFor(Version::Usa100, kGeothermal).empty(), "1.00 untouched");
  // Stream sizes: DELETE is 5 bytes, a conn op 17.
  CHECK_MSG(PortRevisionScripts::OpsFor(Version::Pal, kGeothermal).size() == 12 * 5, "PAL: 12 deletes");
  CHECK_MSG(PortRevisionScripts::OpsFor(Version::Usa101, kArboretum).size() == 17 + 5, "Arboretum ops");
  CHECK_MSG(PortRevisionScripts::OpsFor(Version::Usa102, kArboretum) ==
                PortRevisionScripts::OpsFor(Version::Usa101, kArboretum),
            "Arboretum 1.01 == 1.02");
  CHECK_MSG(PortRevisionScripts::OpsFor(Version::Usa102, kGeothermal).size() == 3 * 5, "1.02 Geothermal");
  CHECK_MSG(PortRevisionScripts::OpsFor(Version::Usa102, kQuarry).size() == 3 * 5, "1.02 Quarry");
  CHECK_MSG(PortRevisionScripts::OpsFor(Version::Usa102, kOre).size() == 3 * 5, "1.02 Ore");
  CHECK_MSG(PortRevisionScripts::OpsFor(Version::Usa102, kCourtyard).size() == 4 * 17 + 4 * 5,
            "1.02 Courtyard");
  CHECK_MSG(PortRevisionScripts::OpsFor(Version::Usa102, kElite).size() == 3 * 17 + 5, "1.02 Elite");

  Room out;
  int misses = -1;

  // USA 1.02 Hive Totem: trigger box (layer 1, type 4, 63 property bytes) and
  // the layer-0 lock actor; a lock model elsewhere or other ids are untouched.
  {
    Room hive;
    hive[0x04240140] = Make(1, 0x04, 63);
    hive[0x0024028D] = Actor(0, 0x6E5D6796, 0);
    hive[0x0024028E] = Actor(0, 0x6E5D6796, 0); // another lock model actor: not ours to touch
    hive[0x00240001] = Actor(0, 0x12345678, 1);
    CHECK_MSG(Run(Version::Usa102, kHive, hive, out, &misses) == Result::Patched && misses == 0,
              "Hive applies");
    CHECK_MSG(!Has(out, 0x0024028D) && Has(out, 0x0024028E) && Has(out, 0x00240001),
              "Hive drops only the verified lock");
    const Obj& t = out[0x04240140];
    CHECK_MSG(Float(t, 0) == 94.57105f && Float(t, 4) == 301.61603f && Float(t, 8) == 0.344905f &&
                  Float(t, 12) == 6.052994f && Float(t, 16) == 24.659973f && Float(t, 20) == 7.878154f,
              "Hive trigger box");
    CHECK_MSG(t.layer == 1 && t.type == 0x04 && t.props.size() == 5 + 63, "Hive trigger shape kept");
    Room wrong = hive; // wrong layer / type / size / model all miss, nothing changes
    wrong[0x04240140].layer = 2;
    CHECK_MSG(Run(Version::Usa102, kHive, wrong, out, &misses) == Result::Missed && misses == 1,
              "Hive trigger on the wrong layer misses");
    wrong = hive;
    wrong[0x0024028D] = Actor(0, 0x5391EDB6, 0);
    CHECK_MSG(Run(Version::Usa102, kHive, wrong, out, &misses) == Result::Missed && misses == 1,
              "Hive lock with another model misses");
    wrong = hive;
    wrong[0x0024028D].type = 0x08;
    CHECK_MSG(Run(Version::Usa102, kHive, wrong, out, &misses) == Result::Missed, "Hive lock type");
    wrong = hive;
    wrong.erase(0x04240140);
    CHECK_MSG(Run(Version::Usa102, kHive, wrong, out, &misses) == Result::Missed && misses == 1,
              "Hive without the trigger misses");
    for (size_t n : {size_t(0), size_t(3), size_t(4)}) { // truncated / no NUL: miss, no crash
      wrong = hive;
      wrong[0x0024028D].props.assign(n, 0xFF);
      CHECK_MSG(Run(Version::Usa102, kHive, wrong, out, &misses) == Result::Missed && misses == 1,
                "Hive lock with tiny props misses");
    }
    wrong = hive;
    wrong[0x0024028D].props.assign(8, 0xFF); // long enough, but no NUL name end
    CHECK_MSG(Run(Version::Usa102, kHive, wrong, out) == Result::Missed, "Hive lock without NUL misses");
    CHECK_MSG(Run(Version::Usa101, kHive, hive, out) == Result::NoPatch &&
                  Run(Version::Pal, kHive, hive, out) == Result::NoPatch,
              "Hive only on 1.02");
  }

  // PAL Ore Processing: platform (layer 0, type 8), POI (0x42), blocker actor go inactive.
  {
    Room ore;
    ore[0x00060372] = Make(0, 0x08, 352);
    ore[0x00060372].props[5 + 205] = 1;
    ore[0x00060378] = Make(0, 0x42, 37);
    ore[0x00060378].props[5 + 24] = 1;
    ore[0x00060379] = Actor(0, 0x11111111, 1);
    ore[0x0006036E] = Actor(0, 0x22222222, 1);
    ore[0x00060377].conns = {{0, 4, 0x60372}, {0, 4, 0x60378}, {0, 4, 0x60379}};
    CHECK_MSG(Run(Version::Pal, kOre, ore, out, &misses) == Result::Patched && misses == 0,
              "PAL Ore applies");
    CHECK_MSG(Byte(out[0x00060372], 205) == 0 && Byte(out[0x00060378], 24) == 0 &&
                  Byte(out[0x00060379], 341) == 0 && Byte(out[0x0006036E], 341) == 1,
              "PAL Ore active bytes only");
    CHECK_MSG(out.size() == 5 && out[0x00060377].conns.size() == 3, "PAL Ore nothing removed");
    Room wrong = ore;
    wrong[0x00060378].type = 0x00; // a POI that isn't one
    CHECK_MSG(Run(Version::Pal, kOre, wrong, out, &misses) == Result::Missed && misses == 1,
              "PAL Ore type guard");
    wrong = ore;
    wrong[0x00060372].props[5 + 205] = 0; // already off: not the PAL room we read
    CHECK_MSG(Run(Version::Pal, kOre, wrong, out, &misses) == Result::Missed, "PAL Ore active guard");
    wrong = ore;
    wrong[0x00060379].props.pop_back(); // layout of another version
    CHECK_MSG(Run(Version::Pal, kOre, wrong, out, &misses) == Result::Missed, "PAL Ore size guard");
  }

  // PAL Main Quarry: the layer-7 lock goes inactive; Central Dynamo: its lock is removed.
  {
    Room quarry;
    quarry[0x1C0205DB] = Actor(7, 0x6E5D6796, 1);
    quarry[0x000205DB] = Actor(0, 0x6E5D6796, 1); // the USA id collides in low bits only
    CHECK_MSG(Run(Version::Pal, kQuarry, quarry, out, &misses) == Result::Patched && misses == 0,
              "PAL Quarry applies");
    CHECK_MSG(Byte(out[0x1C0205DB], 341) == 0 && Byte(out[0x000205DB], 341) == 1 && out.size() == 2,
              "PAL Quarry edits the full id only");
    Room wrong = quarry;
    wrong[0x1C0205DB].layer = 6;
    CHECK_MSG(Run(Version::Pal, kQuarry, wrong, out) == Result::Missed, "PAL Quarry layer guard");
    wrong = quarry;
    wrong[0x1C0205DB] = Actor(7, 0x5391EDB6, 1);
    CHECK_MSG(Run(Version::Pal, kQuarry, wrong, out) == Result::Missed, "PAL Quarry model guard");
    CHECK_MSG(Run(Version::Usa102, kQuarry, quarry, out) == Result::Patched && Has(out, 0x1C0205DB) &&
                  Byte(out[0x1C0205DB], 341) == 1,
              "1.02 Quarry rule leaves the PAL lock alone");

    Room dyn;
    dyn[0x041B06A1] = Actor(1, 0x6E5D6796, 0);
    dyn[0x041B068B] = Actor(1, 0x6E5D6796, 0);
    dyn[0x041B0368] = Actor(1, 0x6E5D6796, 0);
    dyn[0x041B06A3].conns = {{9, 5, 0x041B06A1}};
    CHECK_MSG(Run(Version::Pal, kDynamo, dyn, out, &misses) == Result::Patched && misses == 0,
              "PAL Dynamo applies");
    CHECK_MSG(!Has(out, 0x041B06A1) && Has(out, 0x041B068B) && Has(out, 0x041B0368) &&
                  Has(out, 0x041B06A3),
              "PAL Dynamo drops only its lock");
    wrong = dyn;
    wrong.erase(0x041B06A1);
    CHECK_MSG(Run(Version::Pal, kDynamo, wrong, out) == Result::Missed, "PAL Dynamo missing lock");
    wrong = dyn;
    wrong[0x041B06A1].layer = 0;
    CHECK_MSG(Run(Version::Pal, kDynamo, wrong, out) == Result::Missed, "PAL Dynamo layer guard");
    CHECK_MSG(Run(Version::Usa102, kDynamo, dyn, out) == Result::NoPatch, "Dynamo is PAL only");
  }

  // Old PAL Geothermal: 12 ids 1403AA..1403B5 go, the neighbours stay.
  {
    Room in = Ids({0x1403A9, 0x1403B6});
    for (uint32_t id = 0x1403AA; id <= 0x1403B5; ++id)
      in[id];
    CHECK_MSG(Run(Version::Pal, kGeothermal, in, out, &misses) == Result::Patched && misses == 0,
              "PAL Geothermal applies");
    CHECK_MSG(out.size() == 2 && Has(out, 0x1403A9) && Has(out, 0x1403B6), "PAL Geothermal drops the set");
    // The 1.02 ids collide with different PAL objects: PAL must not delete them.
    Room pal = Ids({0x14039E, 0x14039F, 0x1403A0});
    for (uint32_t id = 0x1403AA; id <= 0x1403B5; ++id)
      pal[id];
    CHECK_MSG(Run(Version::Pal, kGeothermal, pal, out) == Result::Patched && out.size() == 3 &&
                  Has(out, 0x14039E) && Has(out, 0x14039F) && Has(out, 0x1403A0),
              "PAL keeps the 1.02 lock ids");
    // Deletes of absent ids are silent, so a 1.00 USA-like room still "applies"
    // and USA discs never reach it (gate): check the gate instead.
    CHECK_MSG(Run(Version::Usa100, kGeothermal, in, out) == Result::NoPatch, "1.00 Geothermal no patch");
    CHECK_MSG(Run(Version::Usa101, kGeothermal, in, out) == Result::NoPatch, "1.01 Geothermal no patch");
  }

  // Old Arboretum (1.01 and 1.02): the platform and the conn that disables it.
  for (Version v : {Version::Usa101, Version::Usa102}) {
    Room in = Ids({0x1302AA, 0x1302AB});
    in[0x130140].conns = {{5, 4, 0x1302AA}, {5, 4, 0x1302AB}};
    CHECK_MSG(Run(v, kArboretum, in, out) == Result::Patched, "Arboretum applies");
    CHECK_MSG(!Has(out, 0x1302AA) && Has(out, 0x1302AB), "Arboretum drops only the platform");
    CHECK_MSG(!HasConn(out, 0x130140, 5, 4, 0x1302AA) && HasConn(out, 0x130140, 5, 4, 0x1302AB),
              "Arboretum drops only its message");
    Room bad = Ids({0x1302AA}); // the conn is missing: fail-safe
    CHECK_MSG(Run(v, kArboretum, bad, out, &misses) == Result::Missed && misses == 1,
              "Arboretum without the conn misses");
    CHECK_MSG(Run(Version::Usa100, kArboretum, in, out) == Result::NoPatch, "1.00 Arboretum untouched");
    CHECK_MSG(Run(Version::Pal, kArboretum, in, out) == Result::NoPatch, "PAL Arboretum untouched");
  }

  // The five 1.02 restores.
  CHECK_MSG(Run(Version::Usa102, kGeothermal, Ids({0x14039D, 0x14039E, 0x14039F, 0x1403A0, 0x1403A1}),
                out) == Result::Patched &&
                out.size() == 2 && Has(out, 0x14039D) && Has(out, 0x1403A1),
            "1.02 Geothermal lock");
  CHECK_MSG(Run(Version::Usa102, kQuarry, Ids({0x205D2, 0x205D3, 0x205D4, 0x205D5, 0x205D6}), out) ==
                    Result::Patched &&
                out.size() == 2 && Has(out, 0x205D2) && Has(out, 0x205D6),
            "1.02 Quarry lock");
  CHECK_MSG(Run(Version::Usa102, kOre, Ids({0x60369, 0x6036A, 0x6036B, 0x6036C, 0x6036D}), out) ==
                    Result::Patched &&
                out.size() == 2 && Has(out, 0x60369) && Has(out, 0x6036D),
            "1.02 Ore lock");

  Room court = Ids({0xF01C8, 0xF0311, 0xF0312, 0xF0313, 0xF0314, 0xF0315, 0xF0316});
  court[0xF01D9].conns = {{0xE, 0xD, 0xF0315}, {0xE, 0xD, 0xF0312}, {0xE, 0xD, 0xF0316}};
  CHECK_MSG(Run(Version::Usa102, kCourtyard, court, out) == Result::Patched, "1.02 Courtyard applies");
  CHECK_MSG(Has(out, 0xF01C8) && Has(out, 0xF0311) && Has(out, 0xF0316) && !Has(out, 0xF0312) &&
                !Has(out, 0xF0313) && !Has(out, 0xF0314) && !Has(out, 0xF0315),
            "Courtyard lock objects gone");
  CHECK_MSG(HasConn(out, 0xF01D9, 0xE, 0xD, 0xF0316) && !HasConn(out, 0xF01D9, 0xE, 0xD, 0xF0315) &&
                !HasConn(out, 0xF01D9, 0xE, 0xD, 0xF0312) && out[0xF01D9].conns.size() == 2,
            "Courtyard relay conns");
  CHECK_MSG(HasConn(out, 0xF01D9, 0xE, 1, 0xF01C8) && HasConn(out, 0xF01C8, 0, 4, 0xF01D9),
            "Courtyard 1.00 wiring added");
  CHECK_MSG(out[0xF01D9].conns.back().target == 0xF01C8, "added conn is appended");

  Room elite = Ids({0xD01F6, 0x40D0407});
  elite[0xD0340].conns = {{1, 0xD, 0xD0517}, {1, 0xD, 0xD0100}};
  elite[0xD0517];
  elite[0x40D01A4].conns = {{0xE, 4, 0x1}};
  CHECK_MSG(Run(Version::Usa102, kElite, elite, out) == Result::Patched, "1.02 Elite applies");
  CHECK_MSG(!Has(out, 0xD0517) && !HasConn(out, 0xD0340, 1, 0xD, 0xD0517) &&
                HasConn(out, 0xD0340, 1, 0xD, 0xD0100),
            "Elite relay and its conn go, the other conn stays");
  CHECK_MSG(HasConn(out, 0x40D01A4, 0xE, 0xD, 0x40D0407) && HasConn(out, 0x40D01A4, 0xE, 0xD, 0xD01F6) &&
                out[0x40D01A4].conns.size() == 3 && out[0x40D01A4].conns[0].target == 1,
            "Elite exit conns appended after the pirate's own");

  // Fail-safe: a missing REMCONN means nothing changes (all or nothing).
  {
    Room noRem = court;
    noRem[0xF01D9].conns = {{0xE, 0xD, 0xF0315}}; // the 0xF0312 conn is absent
    const std::vector< uint8_t > scly = Build(noRem);
    std::vector< uint8_t > bytes = {1, 2, 3};
    CHECK_MSG(PortRevisionScripts::Apply(Version::Usa102, kCourtyard, scly.data(), scly.size(), bytes,
                                         &misses) == Result::Missed && misses == 1 && bytes.empty(),
              "Courtyard missing remconn misses once, out empty");
    Room noRem2 = elite; // 1.00 / 1.01 / PAL layout: the relay conn isn't there
    noRem2[0xD0340].conns = {{1, 0xD, 0xD0100}};
    CHECK_MSG(Run(Version::Usa102, kElite, noRem2, out, &misses) == Result::Missed && misses == 1,
              "Elite missing remconn misses");
  }
  // A malformed script is a miss, never a crash.
  {
    const uint8_t junk[4] = {1, 2, 3, 4};
    std::vector< uint8_t > bytes;
    CHECK_MSG(PortRevisionScripts::Apply(Version::Usa102, kQuarry, junk, 4, bytes, &misses) ==
                      Result::Missed && bytes.empty(),
              "junk script misses");
  }

  CHECK_MSG(PortRevisionScripts::Describe(Version::Usa102, kElite)[0] != 0, "describe");

  if (port_test::Failures() == 0)
    std::puts("[revision-scripts] OK");
  return PORT_TEST_RESULT();
}
