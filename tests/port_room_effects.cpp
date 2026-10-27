#include "port_room_effects.h"
#include "port_test.h"

#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace {

using PortRoomEffects::Effect;
using PortRoomEffects::File;

Effect MakeEffect(uint32_t layer, uint32_t index) {
  Effect e;
  e.id = PortRoomEffects::AddOnId(layer, 0x1A, index);
  e.name = "Remastered effect " + std::to_string(index);
  e.pos[0] = -120.5f;
  e.pos[2] = 33.f;
  e.rot[1] = 90.f;
  e.scale[0] = 2.f;
  e.part = 0xFE123456;
  e.active = index % 2 == 0;
  e.noTimerUnlessAreaOccluded = true;
  e.thermalVisorVisible = false;
  e.rateCamDistRangeFarRate = 0.25f;
  return e;
}

void TestIds() {
  CHECK(PortRoomEffects::AddOnId(1, 0x1A, 0) == 0x041AF000);
  CHECK(PortRoomEffects::AddOnId(63, 0x3FF, PortRoomEffects::kMaxAddOns - 1) == 0xFFFFFFFF);
  CHECK(PortRoomEffects::IsAddOnId(0x041AF000));
  CHECK(PortRoomEffects::IsAddOnId(0x001AFFFE));
  CHECK(!PortRoomEffects::IsAddOnId(0x041AEFFF));
  CHECK(!PortRoomEffects::IsAddOnId(0x000001CA));
  CHECK(PortRoomEffects::kMaxAddOns == 0x1000);
}

void TestEffect() {
  const Effect in = MakeEffect(2, 7);
  std::vector<uint8_t> bytes;
  PortRoomEffects::AppendEffect(in, bytes);
  // Type, length, then the body (id, no connections, 24 properties).
  REQUIRE(bytes.size() > 17);
  CHECK(bytes[0] == 7);
  const uint32_t length = uint32_t(bytes[1]) << 24 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 8 | bytes[4];
  CHECK(length == bytes.size() - 5);
  CHECK(bytes[5] == 0x08 && bytes[6] == 0x1A && bytes[7] == 0xF0 && bytes[8] == 0x07);
  CHECK(bytes[16] == 24);
  Effect out;
  size_t used = 0;
  REQUIRE(PortRoomEffects::ReadEffect(bytes.data(), bytes.size(), out, used));
  CHECK(used == bytes.size());
  CHECK(out.id == in.id && out.name == in.name && out.part == in.part);
  CHECK(out.pos[0] == -120.5f && out.pos[2] == 33.f && out.rot[1] == 90.f && out.scale[0] == 2.f);
  CHECK(out.active == in.active && out.noTimerUnlessAreaOccluded && !out.thermalVisorVisible && out.xrayVisorVisible);
  CHECK(out.rateCamDistRangeFarRate == 0.25f && out.duration == 0.2f && out.rateCamDistRangeMax == 30.f);
  // 4 + 4 + 4 + name + 36 + 8 + 4 + 1 + 16 + 1 + 12 + 4 + 71 light bytes.
  CHECK(length == 12 + in.name.size() + 1 + 36 + 8 + 4 + 1 + 16 + 1 + 12 + 4 + 71);
  CHECK(!PortRoomEffects::ReadEffect(bytes.data(), bytes.size() - 1, out, used));
}

void TestFile() {
  File in;
  in.skips = {0x040001CA, 0x040001CB, 0x040001CC};
  in.connected = 5;
  for (uint32_t l : {0u, 1u}) {
    PortRoomEffects::Layer layer;
    layer.layer = l;
    for (uint32_t i = 0; i < 3; ++i) {
      PortRoomEffects::AppendEffect(MakeEffect(l, l * 3 + i), layer.objects);
      ++layer.count;
    }
    in.layers.push_back(layer);
  }
  const std::vector<uint8_t> data = PortRoomEffects::Write(in);
  File out;
  std::string error;
  REQUIRE_MSG(PortRoomEffects::Parse(data, out, error), error.c_str());
  CHECK(out.skips == in.skips && out.connected == 5 && out.layers.size() == 2);
  CHECK(out.barriers.empty() && out.overrides.empty());
  CHECK(out.layers[1].layer == 1 && out.layers[1].count == 3 && out.layers[1].objects == in.layers[1].objects);

  std::vector<uint8_t> bad = data;
  bad.pop_back();
  CHECK_MSG(!PortRoomEffects::Parse(bad, out, error), "truncated");
  bad = data;
  bad.push_back(0);
  CHECK_MSG(!PortRoomEffects::Parse(bad, out, error), "trailing bytes");
  bad = data;
  bad[4] = 9;
  CHECK_MSG(!PortRoomEffects::Parse(bad, out, error), "version");
  // Write writes version 4 with no barriers, placements or patterned records.
  CHECK(data[4] == 4 && data[5] == 0 && PortRoomEffects::kFileVersion == 9);

  // An object outside the add-on range, or on another layer than its own, is refused.
  File wrong;
  PortRoomEffects::Layer layer;
  Effect retail = MakeEffect(0, 0);
  retail.id = 0x001A01CA;
  PortRoomEffects::AppendEffect(retail, layer.objects);
  layer.count = 1;
  wrong.layers.push_back(layer);
  CHECK_MSG(!PortRoomEffects::Parse(PortRoomEffects::Write(wrong), out, error), "retail id refused");
  wrong.layers[0].objects.clear();
  PortRoomEffects::AppendEffect(MakeEffect(3, 0), wrong.layers[0].objects);
  CHECK_MSG(!PortRoomEffects::Parse(PortRoomEffects::Write(wrong), out, error), "layer mismatch refused");

  File empty;
  CHECK(PortRoomEffects::Parse(PortRoomEffects::Write(empty), out, error) && out.layers.empty());
}

PortRoomEffects::Variable MakeVariable(uint8_t tag) {
  PortRoomEffects::Variable v;
  for (size_t i = 0; i < v.guid.size(); ++i) {
    v.guid[i] = uint8_t(tag + i);
  }
  v.color[0] = 0.59f;
  v.color[3] = 0.25f;
  v.vec[2] = 0.5f;
  v.real = -0.0025f;
  v.integer = -3;
  return v;
}

// Version 2 carries each object's variables; a version 1 file (none) still loads.
void TestFileVariables() {
  File in;
  PortRoomEffects::Layer layer;
  layer.layer = 0;
  for (uint32_t i = 0; i < 3; ++i) {
    PortRoomEffects::AppendEffect(MakeEffect(0, i), layer.objects);
    ++layer.count;
  }
  layer.vars = {{MakeVariable(1), MakeVariable(2)}, {}, {MakeVariable(9)}};
  in.layers.push_back(layer);
  const std::vector<uint8_t> data = PortRoomEffects::Write(in);
  File out;
  std::string error;
  REQUIRE_MSG(PortRoomEffects::Parse(data, out, error), error.c_str());
  REQUIRE(out.layers.size() == 1 && out.layers[0].vars.size() == 3);
  const auto& vars = out.layers[0].vars;
  CHECK(vars[0].size() == 2 && vars[1].empty() && vars[2].size() == 1);
  const PortRoomEffects::Variable& v = vars[2][0];
  CHECK(v.guid == MakeVariable(9).guid && v.color[0] == 0.59f && v.color[1] == 1.f && v.color[3] == 0.25f);
  CHECK(v.vec[2] == 0.5f && v.real == -0.0025f && v.integer == -3);

  // Three u32 counts and three records follow the objects, then the override and barrier counts.
  const size_t tail = 3 * 4 + 3 * PortRoomEffects::kVariableBytes;
  std::vector<uint8_t> bad = data;
  bad.resize(bad.size() - tail + 4);  // the counts, no records
  CHECK_MSG(!PortRoomEffects::Parse(bad, out, error), "truncated variables");

  // The same file as version 1: the objects, no variable blocks, no overrides and no barriers.
  std::vector<uint8_t> v1(data.begin(), data.end() - std::ptrdiff_t(tail + 8));
  v1[4] = 1;
  REQUIRE_MSG(PortRoomEffects::Parse(v1, out, error), error.c_str());
  CHECK(out.layers.size() == 1 && out.layers[0].count == 3 && out.layers[0].vars.size() == 3);
  CHECK(out.layers[0].vars[0].empty() && out.layers[0].vars[2].empty() && out.overrides.empty());
}

// Version 3 carries the overrides: the PART and the particle variables a matched retail Effect
// object takes from its Remastered actor. They live outside the layers, so a file may have them
// alone.
void TestFileOverrides() {
  File in;
  in.skips = {0x040001CA};
  in.connected = 2;
  PortRoomEffects::Override over;
  over.id = 0x04000210;
  over.part = 0xFE123456;
  over.vars = {MakeVariable(4), MakeVariable(5)};
  in.overrides.push_back(over);
  in.overrides.push_back({0x04000211, 0xFFFFFFFF, {}});
  const std::vector<uint8_t> data = PortRoomEffects::Write(in);
  File out;
  std::string error;
  REQUIRE_MSG(PortRoomEffects::Parse(data, out, error), error.c_str());
  CHECK(out.layers.empty() && out.skips == in.skips && out.connected == 2);
  REQUIRE(out.overrides.size() == 2);
  CHECK(out.overrides[0].id == 0x04000210 && out.overrides[0].part == 0xFE123456);
  REQUIRE(out.overrides[0].vars.size() == 2);
  const PortRoomEffects::Variable& v = out.overrides[0].vars[1];
  CHECK(v.guid == MakeVariable(5).guid && v.color[0] == 0.59f && v.color[3] == 0.25f && v.vec[2] == 0.5f);
  CHECK(v.real == -0.0025f && v.integer == -3);
  CHECK(out.overrides[1].id == 0x04000211 && out.overrides[1].part == 0xFFFFFFFF && out.overrides[1].vars.empty());

  // The override block: its count, two ids, two parts, two counts and two records, then the
  // barrier count a version 4 file ends with.
  const size_t block = 4 + 2 * 12 + 2 * PortRoomEffects::kVariableBytes;
  std::vector<uint8_t> v2(data.begin(), data.end() - std::ptrdiff_t(block + 4));
  v2[4] = 2;
  REQUIRE_MSG(PortRoomEffects::Parse(v2, out, error), error.c_str());
  CHECK(out.overrides.empty() && out.skips == in.skips && out.layers.empty());

  for (size_t cut = 1; cut < block + 4; cut += 3) {
    const std::vector<uint8_t> bad(data.begin(), data.end() - std::ptrdiff_t(cut));
    CHECK_MSG(!PortRoomEffects::Parse(bad, out, error), "truncated overrides");
  }
  std::vector<uint8_t> many(data.begin(), data.end() - std::ptrdiff_t(block + 4));
  for (uint8_t b : {0xFF, 0x00, 0x00, 0x00}) {
    many.push_back(b);
  }
  CHECK_MSG(!PortRoomEffects::Parse(many, out, error), "override count");
}

void TestOverridePlacement() {
  File in;
  PortRoomEffects::Override jet{0x000E01FD, 0x3A617BEB, {}};
  jet.hasPlacement = true;
  jet.pos[0] = 332.578125f;
  jet.rot[1] = 180.f;
  jet.scale[2] = 2.f;
  in.overrides.push_back(jet);
  in.overrides.push_back({0x000E01FE, 0xFE123456, {}});
  const auto data = PortRoomEffects::Write(in);
  File out;
  std::string error;
  REQUIRE(data[4] == 5);
  REQUIRE_MSG(PortRoomEffects::Parse(data, out, error), error.c_str());
  REQUIRE(out.overrides.size() == 2);
  CHECK(out.overrides[0].hasPlacement && out.overrides[0].pos[0] == jet.pos[0] &&
        out.overrides[0].rot[1] == 180.f && out.overrides[0].scale[2] == 2.f);
  CHECK(!out.overrides[1].hasPlacement);
  for (size_t cut = 1; cut <= 44; ++cut) {
    auto bad = data;
    bad.resize(bad.size() - cut);
    CHECK(!PortRoomEffects::Parse(bad, out, error));
    CHECK(out.overrides.empty());
  }
  auto old = data;
  old.resize(old.size() - 44);
  old[4] = 4;
  REQUIRE_MSG(PortRoomEffects::Parse(old, out, error), error.c_str());
  CHECK(!out.overrides[0].hasPlacement && !out.overrides[1].hasPlacement);
  auto bad = data;
  // Reject a non-finite placement even if its framing is intact (NaN position.x).
  const size_t position = bad.size() - 40;
  bad[position + 2] = 0xC0;
  bad[position + 3] = 0x7F;
  CHECK(!PortRoomEffects::Parse(bad, out, error));
}

// Version 6 appends an enemy's death particles; without them the file stays version 5 or 4.
void TestFilePatterned() {
  File in;
  PortRoomEffects::PatternedEffect death;
  death.id = 0x041A0123;
  death.part = 0xFE00ABCD;
  death.hasOffset = true;
  death.offset[2] = 1.5f;
  in.patterned.push_back(death);
  PortRoomEffects::PatternedEffect ice;
  ice.id = 0x041A0123;
  ice.slot = 1;
  ice.part = 0x12345678;
  in.patterned.push_back(ice);
  in.overrides.push_back({0x000E01FE, 0xFE123456, {}});
  const auto data = PortRoomEffects::Write(in);
  File out;
  std::string error;
  REQUIRE(data[4] == 6);
  REQUIRE_MSG(PortRoomEffects::Parse(data, out, error), error.c_str());
  REQUIRE(out.patterned.size() == 2 && out.overrides.size() == 1);
  CHECK(out.patterned[0].id == death.id && out.patterned[0].slot == 0 && out.patterned[0].part == death.part &&
        out.patterned[0].hasOffset && out.patterned[0].offset[2] == 1.5f && out.patterned[0].offset[0] == 0.f);
  CHECK(out.patterned[1].slot == 1 && out.patterned[1].part == ice.part && !out.patterned[1].hasOffset);
  CHECK(!out.overrides[0].hasPlacement);
  // 16 + 12 bytes of offset + 16 bytes of the second record, the count, then the v5 flag.
  for (size_t cut = 1; cut <= 52; ++cut) {
    auto bad = data;
    bad.resize(bad.size() - cut);
    CHECK(!PortRoomEffects::Parse(bad, out, error));
    CHECK(out.patterned.empty());
  }
  auto old = data;
  old.resize(old.size() - 48);
  old[4] = 5;
  REQUIRE_MSG(PortRoomEffects::Parse(old, out, error), error.c_str());
  CHECK(out.patterned.empty() && out.overrides.size() == 1);
  auto badSlot = data;
  badSlot[badSlot.size() - 16 + 4] = 2;  // the second record's slot
  CHECK(!PortRoomEffects::Parse(badSlot, out, error));
  auto badOffset = data;
  const size_t z = data.size() - 16 - 4;  // the first record's offset.z
  badOffset[z + 2] = 0xC0;
  badOffset[z + 3] = 0x7F;
  CHECK(!PortRoomEffects::Parse(badOffset, out, error));
  in.patterned.clear();
  CHECK(PortRoomEffects::Write(in)[4] == 4);
}

// Version 7 appends PART property patches; without them the file stays version 6 or older.
void TestFilePatches() {
  File in;
  in.patches.push_back({0x0410002A, 252, 0x00494E00, 0xFE00ABCD});
  in.patches.push_back({0x0410002A, 268, 0xFFFFFFFF, 0x12345678});
  const auto data = PortRoomEffects::Write(in);
  File out;
  std::string error;
  REQUIRE(data[4] == 7);
  REQUIRE_MSG(PortRoomEffects::Parse(data, out, error), error.c_str());
  REQUIRE(out.patches.size() == 2);
  CHECK(out.patches[0].id == 0x0410002A && out.patches[0].offset == 252 && out.patches[0].retail == 0x00494E00 &&
        out.patches[0].part == 0xFE00ABCD);
  CHECK(out.patches[1].offset == 268 && out.patches[1].part == 0x12345678);
  for (size_t cut = 1; cut <= 36; ++cut) {
    auto bad = data;
    bad.resize(bad.size() - cut);
    CHECK(!PortRoomEffects::Parse(bad, out, error));
    CHECK(out.patches.empty());
  }
  auto far = data;
  far[far.size() - 12 + 2] = 0x02;  // the second patch's offset, past kMaxPatchOffset
  CHECK(!PortRoomEffects::Parse(far, out, error));
  in.patches.clear();
  CHECK(PortRoomEffects::Write(in)[4] == 4);

  // Applying: offsets count from after the name's NUL; a changed retail value, another id, a
  // PART the caller cannot build or a slot past the end leave the bytes alone.
  std::vector<uint8_t> props = {'w', 0, 0, 0, 0, 0, 0x00, 0x49, 0x4E, 0x00, 0xAA};
  const std::vector<PortRoomEffects::PropertyPatch> patches = {
      {0x0410002A, 4, 0x00494E00, 0xFE00ABCD},
      {0x0410002A, 0, 0x11111111, 0x22222222},  // retail value differs
      {0x0510002B, 4, 0x00494E00, 0x33333333},  // another object
      {0x0410002A, 6, 0, 0x44444444},            // past the end
  };
  // Layer bits don't take part: 0x0C10002A is the same object.
  CHECK(PortRoomEffects::ApplyPatches(patches, 0x0C10002A, props.data(), props.size(), nullptr) == 1);
  CHECK(props[6] == 0xFE && props[7] == 0x00 && props[8] == 0xAB && props[9] == 0xCD && props[10] == 0xAA);
  CHECK(props[2] == 0 && props[5] == 0);
  std::vector<uint8_t> again = {'w', 0, 0, 0, 0, 0, 0x00, 0x49, 0x4E, 0x00};
  CHECK(PortRoomEffects::ApplyPatches(patches, 0x0410002A, again.data(), again.size(),
                                      [](uint32_t) { return false; }) == 0);
  CHECK(again[6] == 0x00 && again[7] == 0x49);
  std::vector<uint8_t> noName = {'w', 'x'};
  CHECK(PortRoomEffects::ApplyPatches(patches, 0x0410002A, noName.data(), noName.size(), nullptr) == 0);
}

// Version 8 appends Remastered steam records (CScriptSteamMP1): a retail Steam trigger, its
// visor PART and parameters, two splines and the linked sources.
void TestFileSteams() {
  File in;
  PortRoomEffects::Steam s;
  s.id = 0x000901D9;
  s.part = 0xFE00ABCD;
  s.color[0] = 2.5f;
  s.a = 0.6f;
  s.h = 3.f;
  s.colorFlag = false;
  s.s1 = {1, 2, 3, 4, 5};
  s.sources.push_back({0x000901DA, {1.f, -2.f, 3.f}});
  s.sources.push_back({0, {4.f, 5.f, 6.f}});
  in.steams.push_back(s);
  in.steams.push_back(PortRoomEffects::Steam{});
  const auto data = PortRoomEffects::Write(in);
  File out;
  std::string error;
  REQUIRE(data[4] == 8);
  REQUIRE_MSG(PortRoomEffects::Parse(data, out, error), error.c_str());
  REQUIRE(out.steams.size() == 2);
  const PortRoomEffects::Steam& a = out.steams[0];
  CHECK(a.id == 0x000901D9 && a.part == 0xFE00ABCD && a.color[0] == 2.5f && a.color[3] == 1.f);
  CHECK(a.a == 0.6f && a.b == 0.25f && a.h == 3.f && !a.colorFlag);
  CHECK(a.s1 == s.s1 && a.s2.empty() && a.sources.size() == 2);
  CHECK(a.sources[0].id == 0x000901DA && a.sources[0].pos[1] == -2.f && a.sources[1].pos[2] == 6.f);
  CHECK(out.steams[1].colorFlag && out.steams[1].part == 0xFFFFFFFF && out.steams[1].sources.empty());
  for (size_t cut = 1; cut <= 40; ++cut) {
    auto bad = data;
    bad.resize(bad.size() - cut);
    CHECK(!PortRoomEffects::Parse(bad, out, error));
  }
  in.steams.clear();
  CHECK(PortRoomEffects::Write(in)[4] == 4);
}

// Version 9 appends actor slot effects: assets Remastered gives a retail object where retail
// has no property (the Metroid Prime relay's beam electric).
void TestFileActorSlots() {
  File in;
  in.actorSlots.push_back({0x0C1000A2, PortRoomEffects::kActorSlotExoBeamElectric, 0xFE001234});
  in.actorSlots.push_back({0x0C1000A2, PortRoomEffects::kActorSlotExoBeamElectric + 3, 0xFE005678});
  const auto data = PortRoomEffects::Write(in);
  File out;
  std::string error;
  REQUIRE(data[4] == 9);
  REQUIRE_MSG(PortRoomEffects::Parse(data, out, error), error.c_str());
  REQUIRE(out.actorSlots.size() == 2 && out.steams.empty() && out.patches.empty());
  CHECK(out.actorSlots[0].id == 0x0C1000A2 && out.actorSlots[0].slot == 0 && out.actorSlots[0].part == 0xFE001234);
  CHECK(out.actorSlots[1].slot == 3 && out.actorSlots[1].part == 0xFE005678);
  for (size_t cut = 1; cut <= 12; ++cut) {
    auto bad = data;
    bad.resize(bad.size() - cut);
    CHECK(!PortRoomEffects::Parse(bad, out, error));
  }
  auto bad = data;
  bad[bad.size() - 8] = uint8_t(PortRoomEffects::kActorSlots);  // the last record's slot
  CHECK(!PortRoomEffects::Parse(bad, out, error));
  in.actorSlots.clear();
  CHECK(PortRoomEffects::Write(in)[4] == 4);
}

// Version 4 carries a room's barrier: a controller retail Effect object, its duration, and five
// rings, each a platform (another retail object) and the two effects this file adds, with the
// three particle guids that drive the breakup. The codec checks what the file itself must hold
// and nothing about which room it is.
constexpr uint32_t kBarrierLayer = 1;
constexpr uint32_t kBarrierArea = 0x1A;
constexpr uint32_t kController = 0x041A01CA;

std::array<uint8_t, 16> Guid(uint8_t tag) {
  std::array<uint8_t, 16> g{};
  for (size_t i = 0; i < g.size(); ++i) {
    g[i] = uint8_t(tag + i);
  }
  return g;
}

PortRoomEffects::Effect BarrierEffect(uint32_t index) {
  PortRoomEffects::Effect e;
  e.id = PortRoomEffects::AddOnId(kBarrierLayer, kBarrierArea, index);
  e.name = "barrier " + std::to_string(index);
  e.part = 0xFE123456;
  e.active = false;  // a barrier's effects start hidden
  return e;
}

// A valid version 4 file: ten effects on layer 1, and a barrier whose controller and platforms
// are that room's retail objects.
File BarrierFile(const std::function<void(PortRoomEffects::Effect&)>& edit = {}) {
  File in;
  PortRoomEffects::Layer layer;
  layer.layer = kBarrierLayer;
  for (uint32_t i = 0; i < 10; ++i) {
    PortRoomEffects::Effect e = BarrierEffect(i);
    if (edit) {
      edit(e);
    }
    PortRoomEffects::AppendEffect(e, layer.objects);
    ++layer.count;
  }
  in.layers.push_back(layer);
  PortRoomEffects::BarrierGroup group;
  group.controller = kController;
  group.duration = 12.5f;
  for (uint32_t r = 0; r < PortRoomEffects::kBarrierRings; ++r) {
    PortRoomEffects::BarrierRing& ring = group.rings[r];
    ring.platform = 0x041A01D0 + r;
    ring.refraction = PortRoomEffects::AddOnId(kBarrierLayer, kBarrierArea, 2 * r);
    ring.glow = PortRoomEffects::AddOnId(kBarrierLayer, kBarrierArea, 2 * r + 1);
    ring.breakupRefraction = Guid(uint8_t(1 + 3 * r));
    ring.motion = Guid(uint8_t(2 + 3 * r));
    ring.breakupGlow = Guid(uint8_t(3 + 3 * r));
  }
  in.barriers.push_back(group);
  return in;
}

void TestFileBarriers() {
  CHECK(PortRoomEffects::kBarrierRings == 5 && PortRoomEffects::kMaxBarrierGroups == 1);
  CHECK(PortRoomEffects::kBarrierRingBytes == 60 && PortRoomEffects::kBarrierGroupBytes == 308);
  const File in = BarrierFile();
  const std::vector<uint8_t> data = PortRoomEffects::Write(in);
  File out;
  std::string error;
  REQUIRE_MSG(PortRoomEffects::Parse(data, out, error), error.c_str());
  REQUIRE(out.barriers.size() == 1);
  const PortRoomEffects::BarrierGroup& group = out.barriers[0];
  CHECK(group.controller == kController && group.duration == 12.5f);
  for (uint32_t r = 0; r < PortRoomEffects::kBarrierRings; ++r) {
    const PortRoomEffects::BarrierRing& ring = group.rings[r];
    CHECK(ring.platform == 0x041A01D0 + r);
    CHECK(ring.refraction == PortRoomEffects::AddOnId(kBarrierLayer, kBarrierArea, 2 * r));
    CHECK(ring.glow == PortRoomEffects::AddOnId(kBarrierLayer, kBarrierArea, 2 * r + 1));
    CHECK(ring.breakupRefraction == Guid(uint8_t(1 + 3 * r)));
    CHECK(ring.motion == Guid(uint8_t(2 + 3 * r)));
    CHECK(ring.breakupGlow == Guid(uint8_t(3 + 3 * r)));
  }
  // The wire: the group count, then the controller, the duration and five ring records.
  File none = in;
  none.barriers.clear();
  const std::vector<uint8_t> plain = PortRoomEffects::Write(none);
  CHECK_MSG(plain.size() + PortRoomEffects::kBarrierGroupBytes == data.size(), "barrier block size");
  CHECK(plain.size() >= 4 && plain[plain.size() - 4] == 0);  // a count of none, still written
  const size_t countAt = data.size() - PortRoomEffects::kBarrierGroupBytes - 4;
  CHECK(data[countAt] == 1 && data[countAt + 4] == 0xCA && data[countAt + 5] == 0x01);  // controller, LE
  // The guids are the Variable ones, byte for byte (after the count, the controller, the
  // duration and the first ring's three ids).
  const auto firstGuid = Guid(1);
  CHECK(std::memcmp(data.data() + countAt + 24, firstGuid.data(), 16) == 0);

  // Versions 1 to 3 have no barriers: the same effects load, with none.
  {
    std::vector<uint8_t> v3(plain.begin(), plain.end() - 4);
    v3[4] = 3;
    File older;
    REQUIRE_MSG(PortRoomEffects::Parse(v3, older, error), error.c_str());
    CHECK(older.barriers.empty() && older.layers.size() == 1 && older.layers[0].count == 10);
  }
  // Version 2 has no overrides either.
  {
    std::vector<uint8_t> v2(plain.begin(), plain.end() - std::ptrdiff_t(4 + 4));
    v2[4] = 2;
    File older;
    REQUIRE_MSG(PortRoomEffects::Parse(v2, older, error), error.c_str());
    CHECK(older.barriers.empty() && older.layers.size() == 1 && older.layers[0].count == 10);
  }
  // Version 1 has no variable blocks either (these objects have none, so four bytes each).
  std::vector<uint8_t> v1(plain.begin(), plain.end() - std::ptrdiff_t(4 + 4 + 10 * 4));
  v1[4] = 1;
  File oldest;
  REQUIRE_MSG(PortRoomEffects::Parse(v1, oldest, error), error.c_str());
  CHECK(oldest.barriers.empty() && oldest.overrides.empty() && oldest.layers.size() == 1 &&
        oldest.layers[0].count == 10);

  // Nothing of a refused file is kept, and the caller's File is left empty.
  File kept;
  REQUIRE_MSG(PortRoomEffects::Parse(data, kept, error), error.c_str());
  File two = in;
  two.barriers.push_back(in.barriers[0]);
  CHECK_MSG(!PortRoomEffects::Parse(PortRoomEffects::Write(two), kept, error), "two groups");
  CHECK(kept.layers.empty() && kept.barriers.empty() && kept.skips.empty() && kept.overrides.empty());
  CHECK(kept.connected == 0);

  // Every truncation of a valid file is refused.
  size_t accepted = 0;
  for (size_t cut = 1; cut < data.size(); ++cut) {
    const std::vector<uint8_t> shortFile(data.begin(), data.end() - std::ptrdiff_t(cut));
    File shortOut;
    if (PortRoomEffects::Parse(shortFile, shortOut, error)) {
      ++accepted;
    }
  }
  CHECK_MSG(accepted == 0, "every truncation refused");
  // A count of two with only one group behind it, and one byte after the last ring.
  std::vector<uint8_t> count2 = data;
  count2[countAt] = 2;
  CHECK_MSG(!PortRoomEffects::Parse(count2, out, error), "two groups claimed");
  std::vector<uint8_t> trailing = data;
  trailing.push_back(0);
  CHECK_MSG(!PortRoomEffects::Parse(trailing, out, error), "bytes after the barriers");

  // The duration is a positive, finite number of seconds, at most 60.
  for (float duration : {0.f, -0.5f, 60.5f, 1e9f, std::numeric_limits<float>::quiet_NaN(),
                         std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()}) {
    File f = in;
    f.barriers[0].duration = duration;
    const std::vector<uint8_t> wall = PortRoomEffects::Write(f);
    CHECK_MSG(!PortRoomEffects::Parse(wall, out, error), "duration out of range");
    CHECK_MSG(out.barriers.empty() && out.layers.empty(), "out of range duration keeps nothing");
  }
  File longest = in;
  longest.barriers[0].duration = 60.f;
  REQUIRE_MSG(PortRoomEffects::Parse(PortRoomEffects::Write(longest), out, error), "60 seconds");

  // The controller and the platforms are the room's own objects, all different.
  File ctrlAddon = in;
  ctrlAddon.barriers[0].controller = PortRoomEffects::AddOnId(kBarrierLayer, kBarrierArea, 3);
  CHECK_MSG(!PortRoomEffects::Parse(PortRoomEffects::Write(ctrlAddon), out, error), "an add-on controller");
  File platformAddon = in;
  platformAddon.barriers[0].rings[0].platform = PortRoomEffects::AddOnId(kBarrierLayer, kBarrierArea, 20);
  CHECK_MSG(!PortRoomEffects::Parse(PortRoomEffects::Write(platformAddon), out, error), "an add-on platform");
  File samePlatform = in;
  samePlatform.barriers[0].rings[1].platform = samePlatform.barriers[0].rings[0].platform;
  CHECK_MSG(!PortRoomEffects::Parse(PortRoomEffects::Write(samePlatform), out, error), "one platform, two rings");
  File ctrlPlatform = in;
  ctrlPlatform.barriers[0].rings[4].platform = kController;
  CHECK_MSG(!PortRoomEffects::Parse(PortRoomEffects::Write(ctrlPlatform), out, error), "the controller as a platform");

  // Every member is on the controller's layer, in its room.
  File otherLayer = in;
  otherLayer.barriers[0].rings[3].platform = 0x081A01D3;
  CHECK_MSG(!PortRoomEffects::Parse(PortRoomEffects::Write(otherLayer), out, error), "a platform on another layer");
  File otherArea = in;
  otherArea.barriers[0].rings[3].platform = 0x041B01D3;
  CHECK_MSG(!PortRoomEffects::Parse(PortRoomEffects::Write(otherArea), out, error), "a platform in another room");

  // Each ring shows two of this file's effects: exactly one each, whole ids, inactive, with a PART.
  File absent = in;
  absent.barriers[0].rings[2].glow = PortRoomEffects::AddOnId(kBarrierLayer, kBarrierArea, 40);
  CHECK_MSG(!PortRoomEffects::Parse(PortRoomEffects::Write(absent), out, error), "an absent effect");
  File normalizedRef = in;
  normalizedRef.barriers[0].rings[0].refraction = 0x001AF000;  // the object, without its layer bits
  CHECK_MSG(!PortRoomEffects::Parse(PortRoomEffects::Write(normalizedRef), out, error), "a normalized reference");
  File shared = in;
  shared.barriers[0].rings[1].refraction = shared.barriers[0].rings[0].glow;
  CHECK_MSG(!PortRoomEffects::Parse(PortRoomEffects::Write(shared), out, error), "one effect, two rings");
  File selfShared = in;
  selfShared.barriers[0].rings[0].glow = selfShared.barriers[0].rings[0].refraction;
  CHECK_MSG(!PortRoomEffects::Parse(PortRoomEffects::Write(selfShared), out, error), "one effect twice in a ring");
  const uint32_t activeId = PortRoomEffects::AddOnId(kBarrierLayer, kBarrierArea, 7);
  CHECK_MSG(!PortRoomEffects::Parse(
                PortRoomEffects::Write(BarrierFile([&](PortRoomEffects::Effect& e) {
                  e.active = e.id == activeId;
                })),
                out, error),
            "an active effect");
  CHECK_MSG(!PortRoomEffects::Parse(
                PortRoomEffects::Write(BarrierFile([&](PortRoomEffects::Effect& e) {
                  e.part = e.id == activeId ? 0xFFFFFFFF : 0xFE123456;
                })),
                out, error),
            "an effect with no PART");
  CHECK_MSG(!PortRoomEffects::Parse(
                PortRoomEffects::Write(BarrierFile([&](PortRoomEffects::Effect& e) {
                  e.part = e.id == activeId ? 0 : 0xFE123456;
                })),
                out, error),
            "an effect with a zero PART");

  // No member is one the file leaves out or rewrites, its layer bits ignored when comparing.
  File skipped = in;
  skipped.skips = {kController};
  CHECK_MSG(!PortRoomEffects::Parse(PortRoomEffects::Write(skipped), out, error), "a skipped controller");
  File skippedFar = in;
  skippedFar.skips = {0x081A01D3};  // the platform on layer 2: the same object to a room's script
  CHECK_MSG(!PortRoomEffects::Parse(PortRoomEffects::Write(skippedFar), out, error), "a platform skipped");
  File overridden = in;
  overridden.overrides.push_back({0x041A01D0, 0xFE123456, {}});
  CHECK_MSG(!PortRoomEffects::Parse(PortRoomEffects::Write(overridden), out, error), "an overridden platform");
  File overriddenEffect = in;
  overriddenEffect.overrides.push_back({activeId, 0xFFFFFFFF, {}});
  CHECK_MSG(!PortRoomEffects::Parse(PortRoomEffects::Write(overriddenEffect), out, error), "an overridden effect");

  // Fifteen guids, each set and none twice.
  File sameGuid = in;
  sameGuid.barriers[0].rings[2].motion = sameGuid.barriers[0].rings[0].breakupRefraction;
  CHECK_MSG(!PortRoomEffects::Parse(PortRoomEffects::Write(sameGuid), out, error), "one guid, two rings");
  File selfGuid = in;
  selfGuid.barriers[0].rings[0].breakupGlow = selfGuid.barriers[0].rings[0].motion;
  CHECK_MSG(!PortRoomEffects::Parse(PortRoomEffects::Write(selfGuid), out, error), "one guid twice in a ring");
  File noGuid = in;
  noGuid.barriers[0].rings[4].breakupGlow = {};
  CHECK_MSG(!PortRoomEffects::Parse(PortRoomEffects::Write(noGuid), out, error), "a zero guid");

  // With a barrier present the file's own ids must be unambiguous; without one they may repeat
  // across layers, as a room's script ids do.
  File twoLayers = in;
  PortRoomEffects::Layer second;
  second.layer = 2;
  PortRoomEffects::Effect twin = BarrierEffect(0);
  twin.id = PortRoomEffects::AddOnId(2, kBarrierArea, 0);
  PortRoomEffects::AppendEffect(twin, second.objects);
  second.count = 1;
  twoLayers.layers.push_back(second);
  CHECK_MSG(!PortRoomEffects::Parse(PortRoomEffects::Write(twoLayers), out, error), "two objects, one id");
  twoLayers.barriers.clear();
  File plainOut;
  REQUIRE_MSG(PortRoomEffects::Parse(PortRoomEffects::Write(twoLayers), plainOut, error), error.c_str());
  CHECK(plainOut.barriers.empty() && plainOut.layers.size() == 2);
}

void PutLE16(std::vector<uint8_t>& b, uint32_t v) {
  b.push_back(uint8_t(v));
  b.push_back(uint8_t(v >> 8));
}
void PutLE32(std::vector<uint8_t>& b, uint32_t v) {
  PutLE16(b, v & 0xffff);
  PutLE16(b, v >> 16);
}
void PutProp(std::vector<uint8_t>& b, uint32_t id, const std::vector<uint8_t>& data) {
  PutLE32(b, id);
  PutLE16(b, uint32_t(data.size()));
  b.insert(b.end(), data.begin(), data.end());
}
std::vector<uint8_t> Floats(std::initializer_list<float> fs) {
  std::vector<uint8_t> b;
  for (float f : fs) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    PutLE32(b, v);
  }
  return b;
}
// One entry of property 3def0de6, laid out as Landing Site's mist actors have them.
std::vector<uint8_t> Entry(uint8_t tag, uint32_t valueId, const std::vector<uint8_t>& value, bool guid = true) {
  std::vector<uint8_t> info;
  PutLE16(info, guid ? 1 : 0);
  if (guid) {
    const auto id = MakeVariable(tag).guid;
    PutProp(info, 0x82dde61c, std::vector<uint8_t>(id.begin(), id.end()));
  }
  std::vector<uint8_t> e;
  PutLE16(e, value.empty() ? 1 : 2);
  PutProp(e, 0xae893172, info);
  if (!value.empty()) {
    PutProp(e, valueId, value);
  }
  return e;
}

void TestExternalVariables() {
  std::vector<uint8_t> b;
  PutLE32(b, 5);
  for (const auto& e : {Entry(1, 0xfd41dc4b, Floats({0.65f})), Entry(2, 0x45eff31d, Floats({0.59f, 0.72f, 0.75f, 1.f})),
                        Entry(3, 0x282df928, Floats({0.f, 0.f, 0.25f})), Entry(4, 0xcd774f7a, {7, 0, 0, 0}),
                        Entry(5, 0, {})}) {
    b.insert(b.end(), e.begin(), e.end());
  }
  std::vector<PortRoomEffects::Variable> vars;
  REQUIRE(PortRoomEffects::ReadExternalVariables(b.data(), b.size(), vars));
  REQUIRE(vars.size() == 5);
  CHECK(vars[0].guid == MakeVariable(1).guid && vars[0].real == 0.65f && vars[0].color[0] == 1.f);
  CHECK(vars[1].color[0] == 0.59f && vars[1].color[3] == 1.f && vars[1].real == 0.f);
  CHECK(vars[2].vec[2] == 0.25f);
  CHECK(vars[3].integer == 7);
  // No value: the loader's defaults (white, zero).
  CHECK(vars[4].guid == MakeVariable(5).guid && vars[4].color[1] == 1.f && vars[4].real == 0.f && vars[4].integer == 0);

  for (size_t cut = 1; cut < b.size(); cut += 7) {
    CHECK_MSG(!PortRoomEffects::ReadExternalVariables(b.data(), b.size() - cut, vars), "truncated");
  }
  std::vector<uint8_t> noGuid;
  PutLE32(noGuid, 1);
  const auto e = Entry(1, 0xfd41dc4b, Floats({1.f}), false);
  noGuid.insert(noGuid.end(), e.begin(), e.end());
  CHECK_MSG(!PortRoomEffects::ReadExternalVariables(noGuid.data(), noGuid.size(), vars), "no guid");
  std::vector<uint8_t> none;
  PutLE32(none, 0);
  CHECK(PortRoomEffects::ReadExternalVariables(none.data(), none.size(), vars) && vars.empty());
}

}  // namespace

int main() {
  TestIds();
  TestEffect();
  TestFile();
  TestFileVariables();
  TestFileOverrides();
  TestOverridePlacement();
  TestFilePatterned();
  TestFilePatches();
  TestFileSteams();
  TestFileActorSlots();
  TestFileBarriers();
  TestExternalVariables();
  uint32_t id = 0;
  CHECK_MSG(PortRoomEffects::ParseFileName("1a2B3c4D.ROOMFX", id) && id == 0x1A2B3C4D, "file name");
  CHECK(!PortRoomEffects::ParseFileName("1A2B3C4D.roomgeo", id));
  CHECK(!PortRoomEffects::ParseFileName("1A2B3C4.roomfx", id));
  return PORT_TEST_RESULT();
}
