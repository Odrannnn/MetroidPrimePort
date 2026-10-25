#include "port_room_geo.h"
#include "port_maya_spline.h"
#include "port_test.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

void Put32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(uint8_t(value >> (i * 8)));
  }
}

PortRoomGeo::Instance MakeInstance(uint32_t model, float x) {
  PortRoomGeo::Instance instance;
  instance.model = model;
  instance.transform[0] = instance.transform[5] = instance.transform[10] = 1.f;
  instance.transform[3] = x;
  return instance;
}

void TestRoundTrip() {
  std::vector<PortRoomGeo::Instance> in;
  in.push_back(MakeInstance(0x11111111, 1.f));
  PortRoomGeo::Instance gated = MakeInstance(0x22222222, -2.5f);
  gated.layer = 3;
  gated.active = false;
  gated.links.push_back({0x0c1a0042, 9, PortRoomGeo::kShow});
  gated.links.push_back({0x001a0043, 10, PortRoomGeo::kToggle, 5.52f});
  gated.platform = 0x041a0044;
  gated.platformStart[0] = -142.f;
  gated.platformStart[2] = 3.25f;
  in.push_back(gated);
  in.push_back(MakeInstance(0x33333333, 0.f));

  std::vector<PortRoomGeo::Instance> out;
  std::string error;
  const std::vector<uint8_t> file = PortRoomGeo::Write(in);
  CHECK_MSG(PortRoomGeo::Parse(file, out, error), "v3 file parses");
  CHECK_MSG(out.size() == 3, "three instances");
  if (out.size() != 3) {
    return;
  }
  CHECK_MSG(out[0].layer == PortRoomGeo::kEveryLayer && out[0].active && out[0].links.empty(), "plain instance");
  CHECK_MSG(out[1].model == 0x22222222 && out[1].transform[3] == -2.5f, "model and transform");
  CHECK_MSG(out[1].layer == 3 && !out[1].active, "layer and active");
  CHECK_MSG(out[0].platform == 0 && out[1].platform == 0x041a0044 && out[1].platformStart[0] == -142.f &&
                out[1].platformStart[1] == 0.f && out[1].platformStart[2] == 3.25f,
            "platform");
  CHECK_MSG(out[1].links.size() == 2, "two links");
  if (out[1].links.size() == 2) {
    CHECK_MSG(out[1].links[0].sender == 0x0c1a0042 && out[1].links[0].state == 9 &&
                  out[1].links[0].action == PortRoomGeo::kShow && out[1].links[0].delay == 0.f,
              "first link");
    CHECK_MSG(out[1].links[1].action == PortRoomGeo::kToggle && std::fabs(out[1].links[1].delay - 5.52f) < 0.006f,
              "second link, with its delay");
  }
  CHECK_MSG(out[2].model == 0x33333333, "instance after links");

  for (size_t cut = 12; cut < file.size(); ++cut) {
    const std::vector<uint8_t> part(file.begin(), file.begin() + cut);
    if (PortRoomGeo::Parse(part, out, error)) {
      std::fprintf(stderr, "FAIL: truncated at %zu parses\n", cut);
      port_test::CountFailure();
    }
  }
}

void TestAlphaModulate() {
  using namespace PortRoomGeo;
  // Shipped Dynamo transition: opaque endpoint, depth-backed sorted fade,
  // then ShouldRender=false. No endpoint may leave an unmodulated colour pass.
  auto plan = PlanAlphaRender(1.f, false);
  CHECK(plan.visible && plan.unsorted && !plan.sortedOpaque && !plan.depthOpaque);
  plan = PlanAlphaRender(0.5f, true);
  CHECK(plan.visible && !plan.unsorted && plan.sortedOpaque && plan.depthOpaque);
  plan = PlanAlphaRender(0.f, true);
  CHECK(!plan.visible && !plan.unsorted && !plan.sortedOpaque && !plan.depthOpaque);
  plan = PlanAlphaRender(0.5f, false);
  CHECK(plan.visible && !plan.unsorted && plan.sortedOpaque && !plan.depthOpaque);
  AlphaModulateConfig c{1.f, 0.f, 1.2f, 0.f, 12 | 512};
  AlphaModulateState s;
  CHECK(!s.Registered(c, true) && !s.enabled);
  c.flags |= 0x1000;
  CHECK(!s.Registered(c, false) && !s.enabled);
  CHECK(s.Registered(c, true) && s.enabled && !s.toA && s.alpha == c.a);
  CHECK(!s.Tick(c, 0.6f) && std::fabs(s.alpha - 0.5f) < 1e-6f);
  s = {};
  c.flags &= ~0x1000u;
  CHECK(s.Message(c, false) && s.alpha == 1.f && !s.toA);
  CHECK(!s.Tick(c, 0.6f) && std::fabs(s.alpha - 0.5f) < 1e-6f);
  CHECK(!s.Tick(c, 0.6f) && s.alpha == 0.f && s.enabled);
  CHECK(s.Tick(c, 0.01f) && !s.enabled && !s.restore && s.alpha == 0.f);
  CHECK(s.Message(c, true) && s.alpha == 0.f && s.toA);
  CHECK(!s.Tick(c, 0.f) && s.alpha == 1.f && s.enabled);
  CHECK(s.Tick(c, 0.01f) && !s.enabled && s.alpha == 1.f);
  c.flags |= 2;
  s.Message(c, false);
  CHECK(s.Tick(c, 2.f) && s.restore); // restore INITIAL flags, not a clamped endpoint
  c = {1.f, 0.f, 4.f, 2.f, 13};
  s = {};
  s.Message(c, true);
  s.Tick(c, 0.5f);
  CHECK(!s.Message(c, false) && s.time == 3.f && !s.toA); // shipped interruption formula
  s.Tick(c, 1.1f);
  CHECK(s.enabled && s.reversing && s.toA && s.time == 0.f);
  CHECK(!s.Message(c, true) && !s.reversing && !s.toA);

  Script script;
  ScriptNode node;
  node.kind = kAlphaModulate;
  node.max = 12 | 512 | 0x1000;
  node.centre[0] = 1.f;
  node.centre[1] = 0.f;
  node.half[0] = 1.2f;
  script.nodes.push_back(node);
  script.edges.push_back({false, 3, kGroupModulate, 0, 0});
  auto instance = MakeInstance(0x12345678, 0.f);
  instance.group = 0;
  auto file = Write({instance}, &script);
  Script parsed;
  std::vector<Instance> instances;
  std::string error;
  CHECK(Parse(file, instances, error, &parsed) && parsed.nodes.size() == 1 &&
        parsed.nodes[0].AlphaConfig().timeA2B == 1.2f && parsed.nodes[0].max == node.max &&
        parsed.edges[0].action == kGroupModulate);
  file[4] = 14;
  CHECK(!Parse(file, instances, error, &parsed));
}

void TestActorTransform() {
  auto instance = MakeInstance(0x12345678, 2.f);
  instance.links.push_back({0x000600B9, 0, PortRoomGeo::kActorTransform});
  std::vector<PortRoomGeo::Instance> out;
  std::string error;
  auto file = PortRoomGeo::Write({instance});
  CHECK_MSG(PortRoomGeo::Parse(file, out, error) && out.size() == 1 && out[0].links.size() == 1 &&
                out[0].links[0].action == PortRoomGeo::kActorTransform &&
                out[0].links[0].sender == 0x000600B9 && out[0].transform[3] == 2.f,
            "actor-local attachment and full-transform binding survive the file");
  file[4] = 13;
  CHECK_MSG(!PortRoomGeo::Parse(file, out, error), "legacy files cannot reinterpret an actor-local transform");
  instance.links[0].sender = 0;
  CHECK_MSG(!PortRoomGeo::Parse(PortRoomGeo::Write({instance}), out, error), "attachment requires a source actor");
}

void TestVersion1() {
  std::vector<uint8_t> file;
  Put32(file, 0x4752504D);
  Put32(file, 1);
  Put32(file, 2);
  for (uint32_t i = 0; i < 2; ++i) {
    Put32(file, 0xabc00000 + i);
    for (int j = 0; j < 12; ++j) {
      Put32(file, 0);
    }
  }
  std::vector<PortRoomGeo::Instance> out;
  std::string error;
  CHECK_MSG(PortRoomGeo::Parse(file, out, error), "v1 file parses");
  CHECK_MSG(out.size() == 2 && out[1].model == 0xabc00001, "v1 instances");
  CHECK_MSG(out.size() == 2 && out[1].active && out[1].layer == PortRoomGeo::kEveryLayer, "v1 defaults");

  file[4] = 6;
  CHECK_MSG(!PortRoomGeo::Parse(file, out, error), "unknown version rejected");
}

// A scaling joint's clip keeps its stretch through the file and rejects a truncated one.
void TestAnimStretch() {
  std::vector<PortRoomGeo::Instance> in;
  in.push_back(MakeInstance(0x11111111, 1.f));
  in[0].anim.emplace_back();
  in[0].anim[0].fps = 30.f;
  in[0].anim[0].keys = {0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1};
  in[0].anim[0].stretch = {2, 0, 0, 0, 1, 0, 0, 0, 1, 1.5f, 0, 0.25f, 0, 0.5f, 0, 0, 0, 1};
  std::vector<PortRoomGeo::Instance> out;
  std::string error;
  const std::vector<uint8_t> file = PortRoomGeo::Write(in);
  CHECK_MSG(PortRoomGeo::Parse(file, out, error) && out.size() == 1 && out[0].anim.size() == 1 &&
                out[0].anim[0].stretch == in[0].anim[0].stretch && out[0].anim[0].keys == in[0].anim[0].keys,
            "stretch round trip");
  std::vector<uint8_t> cut(file.begin(), file.end() - 4);
  CHECK_MSG(!PortRoomGeo::Parse(cut, out, error), "truncated stretch rejected");
}

// The animation section, alone and after the script and glow sections.
void TestAnim() {
  std::vector<PortRoomGeo::Instance> in;
  in.push_back(MakeInstance(0x11111111, 1.f));
  in.push_back(MakeInstance(0x22222222, 2.f));
  in.push_back(MakeInstance(0x33333333, 3.f));
  in[1].anim.emplace_back();
  in[1].anim[0].fps = 30.f;
  in[1].anim[0].keys = {0, 0, 0, 1, 0, 0, 0, 0, 0, 0.7071068f, 0.7071068f, 0, 0, 1.5f, 0, 0, 1, 0, 0, 0, 2.f};
  std::vector<PortRoomGeo::Instance> out;
  PortRoomGeo::Script back;
  std::string error;
  for (int extras = 0; extras < 2; ++extras) {
    in[0].glows = extras != 0;
    in[0].glow[1] = 1.f;
    in[2].group = extras != 0 ? 4 : PortRoomGeo::kNoGroup;
    const std::vector<uint8_t> file = PortRoomGeo::Write(in);
    CHECK_MSG(PortRoomGeo::Parse(file, out, error, &back), "anim file parses");
    CHECK_MSG(out.size() == 3 && out[0].anim.empty() && out[2].anim.empty() && out[1].anim.size() == 1 &&
                  out[1].anim[0].fps == 30.f && out[1].anim[0].loop && !out[1].animOnShow &&
                  out[1].anim[0].keys == in[1].anim[0].keys,
              "anim");
    CHECK_MSG(out.size() == 3 && out[0].glows == in[0].glows && out[2].group == in[2].group, "sections before the anim");

    const size_t plain = file.size() - 8 - 8 - 12 - 3 * 28;
    for (size_t cut = plain + 1; cut < file.size(); ++cut) {
      const std::vector<uint8_t> part(file.begin(), file.begin() + cut);
      if (PortRoomGeo::Parse(part, out, error, &back)) {
        std::fprintf(stderr, "FAIL: anim truncated at %zu parses\n", cut);
        port_test::CountFailure();
      }
    }
    std::vector<uint8_t> bad = file;
    bad[plain + 8] = 3; // no such instance
    CHECK_MSG(!PortRoomGeo::Parse(bad, out, error, &back), "anim of a missing instance rejected");
    bad = file;
    bad[4] = 4;
    CHECK_MSG(!PortRoomGeo::Parse(bad, out, error, &back), "anim in a version 4 file rejected");

    PortRoomGeo::Instance extra = in[1];
    std::vector<PortRoomGeo::Instance> two = in;
    two.push_back(extra);
    std::vector<uint8_t> dup = PortRoomGeo::Write(two);
    // The second entry's index (entries of 8 + 12 + 84 bytes after the 8 byte head) becomes 1.
    const size_t secondAt = dup.size() - (8 + 12 + 3 * 28);
    dup[secondAt] = 1;
    CHECK_MSG(!PortRoomGeo::Parse(dup, out, error, &back), "duplicate anim instance rejected");

    auto mutated = [&](const char* what, auto change) {
      std::vector<PortRoomGeo::Instance> t = in;
      change(t[1]);
      CHECK_MSG(!PortRoomGeo::Parse(PortRoomGeo::Write(t), out, error, &back), what);
    };
    mutated("one frame rejected", [](PortRoomGeo::Instance& i) { i.anim[0].keys.resize(7); });
    mutated("zero fps rejected", [](PortRoomGeo::Instance& i) { i.anim[0].fps = 0.f; });
    mutated("negative fps rejected", [](PortRoomGeo::Instance& i) { i.anim[0].fps = -1.f; });
    mutated("nan fps rejected", [](PortRoomGeo::Instance& i) { i.anim[0].fps = std::nanf(""); });
    mutated("non-unit quaternion rejected", [](PortRoomGeo::Instance& i) { i.anim[0].keys[10] = 2.f; });
    mutated("non-finite key rejected", [](PortRoomGeo::Instance& i) { i.anim[0].keys[5] = INFINITY; });
  }
}

// Version 8's clip list: two clips, a flag, and the old layouts.
void TestClips() {
  std::vector<PortRoomGeo::Instance> in;
  in.push_back(MakeInstance(0x11111111, 1.f));
  in.push_back(MakeInstance(0x22222222, 2.f));
  in[1].animOnShow = true;
  in[1].anim.resize(2);
  in[1].anim[0].fps = 24.f;
  in[1].anim[0].loop = false;
  in[1].anim[0].keys = {0, 0, 0, 1, 0, 0, 0, 0, 0, 0.7071068f, 0.7071068f, 0, 0, 1.5f, 0, 0, 1, 0, 0, 0, 2.f};
  in[1].anim[1].fps = 12.f;
  in[1].anim[1].keys = {0, 0, 0, 1, 1, 2, 3, 0, 0, 0, 1, 4, 5, 6};
  std::vector<PortRoomGeo::Instance> out;
  std::string error;
  const std::vector<uint8_t> file = PortRoomGeo::Write(in);
  CHECK_MSG(file.size() > 8 && file[4] == 15, "version 15 written");
  CHECK_MSG(PortRoomGeo::Parse(file, out, error), "clips file parses");
  CHECK_MSG(out.size() == 2 && out[0].anim.empty() && !out[0].animOnShow && out[1].animOnShow &&
                out[1].anim.size() == 2 && out[1].anim[0].fps == 24.f && !out[1].anim[0].loop &&
                out[1].anim[0].keys == in[1].anim[0].keys && out[1].anim[1].fps == 12.f && out[1].anim[1].loop &&
                out[1].anim[1].keys == in[1].anim[1].keys,
            "clips round trip");

  // The ANIM entry starts after the 8 byte head, 8 + (12 + 84) + (12 + 56) bytes from the end.
  const size_t entry = 8 + 12 + 3 * 28 + 12 + 2 * 28;
  const size_t at = file.size() - entry;
  std::vector<uint8_t> bad = file;
  bad[at + 4] = 2; // unknown flag
  CHECK_MSG(!PortRoomGeo::Parse(bad, out, error), "unknown anim flag rejected");
  bad = file;
  bad[at + 5] = 0;
  CHECK_MSG(!PortRoomGeo::Parse(bad, out, error), "zero clips rejected");
  bad = file;
  bad[at + 5] = 3; // a third clip that is not there
  CHECK_MSG(!PortRoomGeo::Parse(bad, out, error), "missing clip rejected");
  bad = file;
  bad[at + 8 + 8] = 2; // loop flag
  CHECK_MSG(!PortRoomGeo::Parse(bad, out, error), "bad loop flag rejected");
  for (size_t cut = at - 8 + 1; cut < file.size(); ++cut) {
    const std::vector<uint8_t> part(file.begin(), file.begin() + cut);
    if (PortRoomGeo::Parse(part, out, error)) {
      std::fprintf(stderr, "FAIL: clips truncated at %zu parses\n", cut);
      port_test::CountFailure();
    }
  }

  // An entry of version 5 to 7 is one looping clip that starts at the load.
  for (uint8_t version = 5; version <= 7; ++version) {
    std::vector<uint8_t> old(file.begin(), file.begin() + (at - 8));
    old[4] = version;
    Put32(old, 0x4D494E41);
    Put32(old, 1);
    Put32(old, 1);
    float fps = 30.f;
    uint32_t bits;
    std::memcpy(&bits, &fps, 4);
    Put32(old, bits);
    Put32(old, 2);
    const float keys[14] = {0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1};
    for (float key : keys) {
      std::memcpy(&bits, &key, 4);
      Put32(old, bits);
    }
    CHECK_MSG(PortRoomGeo::Parse(old, out, error), "old anim entry parses");
    CHECK_MSG(out.size() == 2 && out[1].anim.size() == 1 && out[1].anim[0].fps == 30.f && out[1].anim[0].loop &&
                  !out[1].animOnShow && out[1].anim[0].keys.size() == 14 && out[1].anim[0].keys[13] == 1.f,
              "old anim entry is one looping clip");
  }
}

// The hidden objects, after the sky section.
void TestHide() {
  std::vector<PortRoomGeo::Instance> in;
  in.push_back(MakeInstance(0x11111111, 1.f));
  in[0].sky = true;
  PortRoomGeo::Script script;
  script.hidden = {{0x0001000C, 0}, {0x0001005F, 0}};
  std::vector<PortRoomGeo::Instance> out;
  PortRoomGeo::Script back;
  std::string error;
  const std::vector<uint8_t> file = PortRoomGeo::Write(in, &script);
  CHECK_MSG(PortRoomGeo::Parse(file, out, error, &back) && out.size() == 1 && out[0].sky && back.hidden == script.hidden,
            "hidden objects round-trip");
  CHECK_MSG(PortRoomGeo::Write(in).size() == file.size() - 24, "no hidden section without hidden objects");
  for (size_t cut = file.size() - 23; cut < file.size(); ++cut) {
    const std::vector<uint8_t> part(file.begin(), file.begin() + cut);
    if (PortRoomGeo::Parse(part, out, error, &back)) {
      std::fprintf(stderr, "FAIL: hidden objects truncated at %zu parse\n", cut);
      port_test::CountFailure();
    }
  }
  std::vector<uint8_t> bad = file;
  bad[file.size() - 20] = 0; // count 0
  bad.resize(file.size() - 16);
  CHECK_MSG(!PortRoomGeo::Parse(bad, out, error, &back), "empty hidden section rejected");
  bad = file;
  bad[file.size() - 4] = 1; // instance 1 of 1
  CHECK_MSG(!PortRoomGeo::Parse(bad, out, error, &back), "hidden object of a missing instance rejected");
  bad = file;
  bad[4] = 8;
  CHECK_MSG(!PortRoomGeo::Parse(bad, out, error, &back), "hidden section in a version 8 file rejected");
}

// The lightmap lookup section, after the hide section.
void TestLightmap() {
  std::vector<PortRoomGeo::Instance> in;
  in.push_back(MakeInstance(0x11111111, 1.f));
  in.push_back(MakeInstance(0x22222222, 2.f));
  const size_t plain = PortRoomGeo::Write(in).size();
  in[1].lightmap[0] = 0.25f;
  in[1].lightmap[1] = 0.5f;
  in[1].lightmap[2] = 0.125f;
  const std::vector<uint8_t> file = PortRoomGeo::Write(in);
  CHECK_MSG(file.size() == plain + 8 + 24, "lightmap: section written only with a scale");
  std::vector<PortRoomGeo::Instance> out;
  std::string error;
  CHECK_MSG(PortRoomGeo::Parse(file, out, error) && out.size() == 2 && out[0].lightmap[2] == 0.f &&
                out[1].lightmap[0] == 0.25f && out[1].lightmap[1] == 0.5f && out[1].lightmap[2] == 0.125f,
            "lightmap: round trip");
  for (size_t cut = plain + 1; cut < file.size(); ++cut) {
    const std::vector<uint8_t> part(file.begin(), file.begin() + cut);
    if (PortRoomGeo::Parse(part, out, error)) {
      std::fprintf(stderr, "FAIL: lightmap truncated at %zu parses\n", cut);
      port_test::CountFailure();
    }
  }
  std::vector<uint8_t> bad = file;
  bad[4] = 9;
  CHECK_MSG(!PortRoomGeo::Parse(bad, out, error), "lightmap: section in a version 9 file rejected");

  // A scale of 0 with an offset is a one-texel lookup (Remastered's props), kept on its own.
  in[1].lightmap[2] = 0.f;
  const std::vector<uint8_t> texel = PortRoomGeo::Write(in);
  CHECK_MSG(texel.size() == plain + 8 + 24 && PortRoomGeo::Parse(texel, out, error) && out.size() == 2 &&
                out[1].lightmap[0] == 0.25f && out[1].lightmap[1] == 0.5f && out[1].lightmap[2] == 0.f &&
                PortRoomGeo::HasLightmapLookup(out[1].lightmap) && !PortRoomGeo::HasLightmapLookup(out[0].lightmap),
            "lightmap: one-texel lookup round trip");
  const float negative[3] = {0.25f, 0.5f, -1.f};
  const float nan[3] = {0.25f, std::nanf(""), 0.125f};
  CHECK_MSG(!PortRoomGeo::HasLightmapLookup(negative) && !PortRoomGeo::HasLightmapLookup(nan),
            "lightmap: negative or non-finite lookups rejected");
}

// The glow section, alone and after the script section.
// The sky section, after the anim section.
void TestSky() {
  std::vector<PortRoomGeo::Instance> in;
  in.push_back(MakeInstance(0x11111111, 1.f));
  in.push_back(MakeInstance(0x22222222, 2.f));
  in[1].sky = true;
  in[1].skyRadiance[0] = 0.5f;
  in[1].skyRadiance[1] = 2.f;
  in[1].skyRadiance[2] = 0.f;
  in[0].anim.emplace_back();
  in[0].anim[0].fps = 30.f;
  in[0].anim[0].keys = {0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0};
  std::vector<PortRoomGeo::Instance> out;
  PortRoomGeo::Script back;
  std::string error;
  const std::vector<uint8_t> file = PortRoomGeo::Write(in);
  CHECK_MSG(PortRoomGeo::Parse(file, out, error, &back), "sky file parses");
  CHECK_MSG(out.size() == 2 && !out[0].sky && out[1].sky && out[0].anim.size() == 1 && out[0].anim[0].keys == in[0].anim[0].keys, "sky");
  CHECK_MSG(out.size() == 2 && out[1].skyRadiance[0] == 0.5f && out[1].skyRadiance[1] == 2.f &&
                out[1].skyRadiance[2] == 0.f,
            "sky radiance");
  CHECK_MSG(out.size() == 2 && !out[1].worldSky, "not a world sky");
  // Version 13's flags: bit 0, the world's sky.
  std::vector<uint8_t> world = file;
  world[file.size() - 4] = 1;
  CHECK_MSG(PortRoomGeo::Parse(world, out, error, &back) && out.size() == 2 && out[1].sky && out[1].worldSky,
            "world sky flag");
  in[1].worldSky = true;
  CHECK_MSG(PortRoomGeo::Write(in) == world, "world sky written");
  in[1].worldSky = false;
  // Version 12: radiance and no flags.
  std::vector<uint8_t> v12(file.begin(), file.end() - 4);
  v12[4] = 12;
  CHECK_MSG(PortRoomGeo::Parse(v12, out, error, &back) && out.size() == 2 && out[1].sky && !out[1].worldSky &&
                out[1].skyRadiance[1] == 2.f,
            "version 12 sky parses");
  // Version 6: the index alone, and no radiance.
  // Its animation entry is the version 5 one: no flags or loop, a single clip.
  const size_t animAt = file.size() - 28 - 84; // the sky section is 8 + 20 bytes, the animation's 8 + 76
  std::vector<uint8_t> old(file.begin(), file.begin() + animAt + 8);
  old.insert(old.end(), file.begin() + animAt + 8, file.begin() + animAt + 12);
  old.insert(old.end(), file.begin() + animAt + 16, file.begin() + animAt + 24);
  old.insert(old.end(), file.begin() + animAt + 28, file.begin() + animAt + 84);
  old.insert(old.end(), file.end() - 28, file.end() - 16);
  old[4] = 6;
  CHECK_MSG(PortRoomGeo::Parse(old, out, error, &back) && out.size() == 2 && out[1].sky &&
                out[1].skyRadiance[0] == 0.f && out[1].skyRadiance[1] == 0.f,
            "version 6 sky parses");
  // A negative or non-finite radiance reads as not known.
  std::vector<uint8_t> odd = file;
  const float nan = std::nanf("");
  const float negative = -1.f;
  std::memcpy(odd.data() + file.size() - 16, &negative, 4);
  std::memcpy(odd.data() + file.size() - 12, &nan, 4);
  CHECK_MSG(PortRoomGeo::Parse(odd, out, error, &back) && out.size() == 2 && out[1].skyRadiance[0] == 0.f &&
                out[1].skyRadiance[1] == 0.f,
            "odd sky radiance reads as 0");
  for (size_t cut = file.size() - 27; cut < file.size(); ++cut) {
    const std::vector<uint8_t> part(file.begin(), file.begin() + cut);
    if (PortRoomGeo::Parse(part, out, error, &back)) {
      std::fprintf(stderr, "FAIL: sky truncated at %zu parses\n", cut);
      port_test::CountFailure();
    }
  }
  std::vector<uint8_t> bad = file;
  bad[file.size() - 20] = 2; // no such instance
  CHECK_MSG(!PortRoomGeo::Parse(bad, out, error, &back), "sky of a missing instance rejected");
  bad = file;
  bad[4] = 5;
  CHECK_MSG(!PortRoomGeo::Parse(bad, out, error, &back), "sky in a version 5 file rejected");
  bad = file;
  bad[file.size() - 24] = 2; // count 2
  Put32(bad, 1);
  for (int i = 0; i < 4; ++i) {
    Put32(bad, 0);
  }
  CHECK_MSG(!PortRoomGeo::Parse(bad, out, error, &back), "duplicate sky instance rejected");
}

void TestGlow() {
  std::vector<PortRoomGeo::Instance> in;
  in.push_back(MakeInstance(0x11111111, 1.f));
  in.push_back(MakeInstance(0x22222222, 2.f));
  in.push_back(MakeInstance(0x33333333, 3.f));
  in[1].glows = true;
  in[1].glow[0] = 0.f;
  in[1].glow[1] = 2.f;
  in[1].glow[2] = 1.326f;
  std::vector<PortRoomGeo::Instance> out;
  PortRoomGeo::Script back;
  std::string error;
  for (int grouped = 0; grouped < 2; ++grouped) {
    in[2].group = grouped != 0 ? 4 : PortRoomGeo::kNoGroup;
    const std::vector<uint8_t> file = PortRoomGeo::Write(in);
    CHECK_MSG(PortRoomGeo::Parse(file, out, error, &back), "glow file parses");
    CHECK_MSG(out.size() == 3 && !out[0].glows && !out[2].glows && out[1].glows && out[1].glow[0] == 0.f &&
                  out[1].glow[1] == 2.f && out[1].glow[2] == 1.326f,
              "glow");
    CHECK_MSG(out.size() == 3 && out[2].group == in[2].group, "groups before the glow");
    const size_t plain = file.size() - 8 - 16;
    for (size_t cut = plain + 1; cut < file.size(); ++cut) {
      const std::vector<uint8_t> part(file.begin(), file.begin() + cut);
      if (PortRoomGeo::Parse(part, out, error, &back)) {
        std::fprintf(stderr, "FAIL: glow truncated at %zu parses\n", cut);
        port_test::CountFailure();
      }
    }
    std::vector<uint8_t> bad = file;
    bad[plain + 8] = 3; // no such instance
    CHECK_MSG(!PortRoomGeo::Parse(bad, out, error, &back), "glow of a missing instance rejected");
    bad = file;
    bad[4] = 3;
    CHECK_MSG(!PortRoomGeo::Parse(bad, out, error, &back), "glow in a version 3 file rejected");
  }
}

// Version 2: the platform fields are absent, the links follow the fixed part.
void TestVersion2() {
  std::vector<uint8_t> file;
  Put32(file, 0x4752504D);
  Put32(file, 2);
  Put32(file, 2);
  for (uint32_t i = 0; i < 2; ++i) {
    Put32(file, 0xabc00000 + i);
    for (int j = 0; j < 12; ++j) {
      Put32(file, 0);
    }
    file.push_back(1);
    file.push_back(0);
    file.push_back(1);
    file.push_back(0);
    Put32(file, 0x00100020 + i);
    file.push_back(9);
    file.push_back(PortRoomGeo::kShow);
    file.push_back(0);
    file.push_back(0);
  }
  std::vector<PortRoomGeo::Instance> out;
  std::string error;
  CHECK_MSG(PortRoomGeo::Parse(file, out, error), "v2 file parses");
  CHECK_MSG(out.size() == 2 && out[1].model == 0xabc00001 && out[1].layer == 1 && !out[1].active, "v2 instances");
  CHECK_MSG(out.size() == 2 && out[1].links.size() == 1 && out[1].links[0].sender == 0x00100021, "v2 links");
  CHECK_MSG(out.size() == 2 && out[1].platform == 0, "v2 has no platform");
}
// The script section: nodes, edges and each instance's group.
void TestScript() {
  std::vector<PortRoomGeo::Instance> in;
  in.push_back(MakeInstance(0x11111111, 1.f));
  in.push_back(MakeInstance(0x22222222, 2.f));
  in[1].group = 7;
  in[1].active = false;

  PortRoomGeo::Script script;
  PortRoomGeo::ScriptNode volume;
  volume.kind = PortRoomGeo::kCameraVolume;
  volume.centre[0] = 10.f;
  volume.half[1] = 2.5f;
  volume.axes[0] = 0.f;
  volume.axes[1] = 1.f;
  PortRoomGeo::ScriptNode counter;
  counter.kind = PortRoomGeo::kCounter;
  counter.max = 50;
  counter.active = false;
  script.nodes = {volume, counter};
  script.edges.push_back({false, 0, PortRoomGeo::kIncrement, 0, 1});
  script.edges.push_back({false, 1, PortRoomGeo::kGroupHide, 1, 7});
  script.edges.push_back({true, 9, PortRoomGeo::kNodeActivate, 0x0c1a0042, 1});

  const std::vector<uint8_t> file = PortRoomGeo::Write(in, &script);
  std::vector<PortRoomGeo::Instance> out;
  PortRoomGeo::Script back;
  std::string error;
  CHECK_MSG(PortRoomGeo::Parse(file, out, error, &back), "script file parses");
  CHECK_MSG(out.size() == 2 && out[0].group == PortRoomGeo::kNoGroup && out[1].group == 7, "groups");
  CHECK_MSG(back.nodes.size() == 2 && back.edges.size() == 3, "script sizes");
  if (back.nodes.size() == 2 && back.edges.size() == 3) {
    CHECK_MSG(back.nodes[0].kind == PortRoomGeo::kCameraVolume && back.nodes[0].centre[0] == 10.f &&
                  back.nodes[0].half[1] == 2.5f && back.nodes[0].axes[0] == 0.f && back.nodes[0].axes[1] == 1.f &&
                  back.nodes[0].active,
              "volume node");
    CHECK_MSG(back.nodes[1].kind == PortRoomGeo::kCounter && back.nodes[1].max == 50 && !back.nodes[1].active,
              "counter node");
    CHECK_MSG(!back.edges[1].retail && back.edges[1].event == 1 && back.edges[1].action == PortRoomGeo::kGroupHide &&
                  back.edges[1].from == 1 && back.edges[1].to == 7,
              "group edge");
    CHECK_MSG(back.edges[2].retail && back.edges[2].event == 9 && back.edges[2].from == 0x0c1a0042, "retail edge");
  }

  // Without groups or a script the section is left out.
  in[1].group = PortRoomGeo::kNoGroup;
  CHECK_MSG(PortRoomGeo::Write(in).size() + 12 + 2 * 68 + 3 * 12 + 2 * 4 == PortRoomGeo::Write(in, &back).size(),
            "no empty section");

  for (size_t cut = 13; cut < file.size(); ++cut) {
    const std::vector<uint8_t> part(file.begin(), file.begin() + cut);
    if (PortRoomGeo::Parse(part, out, error, &back) && cut != PortRoomGeo::Write(in).size()) {
      std::fprintf(stderr, "FAIL: script truncated at %zu parses\n", cut);
      port_test::CountFailure();
    }
  }

  std::vector<uint8_t> bad = file;
  bad[bad.size() - 8 - 3 * 12 + 4] = 9; // the first edge's from: no such node
  CHECK_MSG(!PortRoomGeo::Parse(bad, out, error, &back), "edge from a missing node rejected");
  bad = PortRoomGeo::Write(in);
  bad.push_back(1);
  CHECK_MSG(!PortRoomGeo::Parse(bad, out, error, &back), "trailing bytes rejected");
}

void TestLods() {
  std::vector<PortRoomGeo::Lods> in(2);
  in[0].model = 0x11111111;
  in[0].levels = {{100.f, 0x11111112}, {400.f, 0x11111113}};
  in[1].model = 0x22222222;
  in[1].levels = {{2500.f, 0x22222223}};
  const std::vector<uint8_t> file = PortRoomGeo::WriteLods(in);
  CHECK_MSG(file.size() == 12 + 2 * 8 + 3 * 8, "lods size");
  std::vector<PortRoomGeo::Lods> out;
  std::string error;
  CHECK_MSG(PortRoomGeo::ParseLods(file, out, error), "lods parse");
  CHECK_MSG(out.size() == 2 && out[0].model == 0x11111111 && out[0].levels.size() == 2 &&
                out[0].levels[1].distanceSq == 400.f && out[0].levels[1].model == 0x11111113,
            "first model's levels");
  CHECK_MSG(out.size() == 2 && out[1].levels.size() == 1 && out[1].levels[0].model == 0x22222223, "second model");
  for (size_t cut = 0; cut < file.size(); ++cut) {
    const std::vector<uint8_t> part(file.begin(), file.begin() + cut);
    if (PortRoomGeo::ParseLods(part, out, error)) {
      std::fprintf(stderr, "FAIL: lods truncated at %zu parses\n", cut);
      port_test::CountFailure();
    }
  }
  std::vector<PortRoomGeo::Lods> bad = in;
  std::swap(bad[0].levels[0], bad[0].levels[1]);
  CHECK_MSG(!PortRoomGeo::ParseLods(PortRoomGeo::WriteLods(bad), out, error), "out-of-order distances rejected");
  bad = in;
  bad[1].levels.clear();
  CHECK_MSG(!PortRoomGeo::ParseLods(PortRoomGeo::WriteLods(bad), out, error), "model without levels rejected");
}
void PutFloat(std::vector<uint8_t>& out, float value) {
  uint32_t u;
  std::memcpy(&u, &value, 4);
  Put32(out, u);
}

// Ice Shorelines' waterfall spline: (0, -2) linear, (2, -0.13) and (12, -0.13) flat, (16.5, -2)
// linear; constant before and after.
std::vector<uint8_t> WaterfallSpline() {
  const float knots[4][2] = {{0.f, -2.f}, {2.f, -0.13f}, {12.f, -0.13f}, {16.5f, -2.f}};
  const uint8_t type[4] = {0, 1, 1, 0};
  std::vector<uint8_t> out;
  Put32(out, 4);
  for (int i = 0; i < 4; ++i) {
    PutFloat(out, knots[i][0]);
    PutFloat(out, knots[i][1]);
    out.push_back(type[i]);
    out.push_back(type[i]);
  }
  PutFloat(out, -2.f);
  PutFloat(out, 0.f);
  out.push_back(0);
  out.push_back(0);
  out.push_back(0);
  return out;
}

void TestSpline() {
  const std::vector<uint8_t> bytes = WaterfallSpline();
  PortMayaSpline spline;
  size_t used = 0;
  CHECK_MSG(spline.Load(bytes.data(), bytes.size(), &used) && used == bytes.size(), "spline loads");
  const struct {
    float t, v;
  } expected[] = {{0.f, -2.f}, {1.f, -0.8312f}, {2.f, -0.13f}, {12.f, -0.13f}, {14.25f, -0.8312f}, {16.5f, -2.f}, {20.f, -2.f}};
  for (const auto& e : expected) {
    const float v = spline.Eval(e.t);
    if (std::fabs(v - e.v) > 0.005f) {
      std::fprintf(stderr, "t=%g: %g, expected %g\n", e.t, v, e.v);
    }
    CHECK_MSG(std::fabs(v - e.v) <= 0.005f, "spline value");
  }
}

// A driver on an instance, written and read back; files without one, and the older ones, load.
void TestDrivers() {
  std::vector<PortRoomGeo::Instance> in;
  in.push_back(MakeInstance(0x11111111, 1.f));
  PortRoomGeo::Instance driven = MakeInstance(0x22222222, 2.f);
  driven.active = false;
  PortRoomGeo::Instance::Driver d;
  d.slot = 3;
  d.sender = 0x0c1a01b0;
  d.state = 9;
  d.delay = 1.25f;
  d.spline = WaterfallSpline();
  driven.drivers.push_back(d);
  in.push_back(driven);
  in.push_back(MakeInstance(0x33333333, 3.f));

  std::vector<PortRoomGeo::Instance> out;
  std::string error;
  CHECK_MSG(PortRoomGeo::Parse(PortRoomGeo::Write(in), out, error), "driver file parses");
  CHECK_MSG(out.size() == 3 && out[0].drivers.empty() && out[2].drivers.empty() && out[1].drivers.size() == 1,
            "one driven instance");
  if (out.size() == 3 && out[1].drivers.size() == 1) {
    const PortRoomGeo::Instance::Driver& r = out[1].drivers[0];
    CHECK_MSG(r.slot == 3 && r.sender == d.sender && r.state == 9 && r.delay == 1.25f && r.spline == d.spline,
              "driver fields");
    CHECK_MSG(!out[1].active && out[1].model == 0x22222222, "driven instance fields");
  }

  in[1].drivers.clear();
  const std::vector<uint8_t> without = PortRoomGeo::Write(in);
  CHECK_MSG(PortRoomGeo::Parse(without, out, error) && out.size() == 3 && out[1].drivers.empty(), "no drivers");
  std::vector<uint8_t> with = PortRoomGeo::Write({in[0], driven});
  CHECK_MSG(with.size() > PortRoomGeo::Write({in[0], in[1]}).size(), "the section is only written for drivers");

  // A delay past the 16 bit field is clamped on write, not wrapped.
  in[1].drivers.push_back(d);
  in[1].drivers[0].delay = 1000.f;
  CHECK_MSG(PortRoomGeo::Parse(PortRoomGeo::Write(in), out, error) && out[1].drivers.size() == 1 &&
                std::fabs(out[1].drivers[0].delay - PortRoomGeo::kMaxDriverDelay) < 0.01f,
            "an over-long delay clamps");
  in[1].drivers.clear();

  // A shown driven instance with no start (a save state reset the scripts) starts now; a started
  // one keeps its start; a hidden one waits.
  CHECK_MSG(PortRoomGeo::DriverStartAt(true, 1e300, 42.0) == 42.0, "shown without a start fades from now");
  CHECK_MSG(PortRoomGeo::DriverStartAt(true, 40.0, 42.0) == 40.0, "a started driver keeps its start");
  CHECK_MSG(PortRoomGeo::DriverStartAt(false, 1e300, 42.0) == 1e300, "a hidden one waits");

  // A version 10 file (the version word 10, no WDRV) still loads.
  std::vector<uint8_t> old = without;
  old[4] = 10;
  CHECK_MSG(PortRoomGeo::Parse(old, out, error) && out.size() == 3 && out[1].drivers.empty(), "version 10 loads");

  // A damaged section is refused.
  std::vector<uint8_t> bad = PortRoomGeo::Write({driven});
  bad.resize(bad.size() - 3);
  CHECK_MSG(!PortRoomGeo::Parse(bad, out, error), "truncated driver refused");
}
} // namespace

int main() {
  TestRoundTrip();
  TestActorTransform();
  TestAlphaModulate();
  TestVersion1();
  TestVersion2();
  TestScript();
  TestGlow();
  TestAnim();
  TestAnimStretch();
  TestClips();
  TestSky();
  TestHide();
  TestLightmap();
  TestLods();
  TestSpline();
  TestDrivers();
  uint32_t id = 0;
  CHECK_MSG(PortRoomGeo::ParseFileName("1a2B3c4D.ROOMGEO", id) && id == 0x1A2B3C4D, "file name");
  if (port_test::Failures() == 0) {
    std::printf("port_room_geo_tests: ok\n");
  }
  return PORT_TEST_RESULT();
}
