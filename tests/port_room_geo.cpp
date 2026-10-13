#include "port_room_geo.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int sFailures = 0;

void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++sFailures;
  }
}

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
  Check(PortRoomGeo::Parse(file, out, error), "v3 file parses");
  Check(out.size() == 3, "three instances");
  if (out.size() != 3) {
    return;
  }
  Check(out[0].layer == PortRoomGeo::kEveryLayer && out[0].active && out[0].links.empty(), "plain instance");
  Check(out[1].model == 0x22222222 && out[1].transform[3] == -2.5f, "model and transform");
  Check(out[1].layer == 3 && !out[1].active, "layer and active");
  Check(out[0].platform == 0 && out[1].platform == 0x041a0044 && out[1].platformStart[0] == -142.f &&
            out[1].platformStart[1] == 0.f && out[1].platformStart[2] == 3.25f,
        "platform");
  Check(out[1].links.size() == 2, "two links");
  if (out[1].links.size() == 2) {
    Check(out[1].links[0].sender == 0x0c1a0042 && out[1].links[0].state == 9 &&
              out[1].links[0].action == PortRoomGeo::kShow && out[1].links[0].delay == 0.f,
          "first link");
    Check(out[1].links[1].action == PortRoomGeo::kToggle && std::fabs(out[1].links[1].delay - 5.52f) < 0.006f,
          "second link, with its delay");
  }
  Check(out[2].model == 0x33333333, "instance after links");

  for (size_t cut = 12; cut < file.size(); ++cut) {
    const std::vector<uint8_t> part(file.begin(), file.begin() + cut);
    if (PortRoomGeo::Parse(part, out, error)) {
      std::fprintf(stderr, "FAIL: truncated at %zu parses\n", cut);
      ++sFailures;
    }
  }
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
  Check(PortRoomGeo::Parse(file, out, error), "v1 file parses");
  Check(out.size() == 2 && out[1].model == 0xabc00001, "v1 instances");
  Check(out.size() == 2 && out[1].active && out[1].layer == PortRoomGeo::kEveryLayer, "v1 defaults");

  file[4] = 6;
  Check(!PortRoomGeo::Parse(file, out, error), "unknown version rejected");
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
    Check(PortRoomGeo::Parse(file, out, error, &back), "anim file parses");
    Check(out.size() == 3 && out[0].anim.empty() && out[2].anim.empty() && out[1].anim.size() == 1 &&
              out[1].anim[0].fps == 30.f && out[1].anim[0].loop && !out[1].animOnShow &&
              out[1].anim[0].keys == in[1].anim[0].keys,
          "anim");
    Check(out.size() == 3 && out[0].glows == in[0].glows && out[2].group == in[2].group, "sections before the anim");

    const size_t plain = file.size() - 8 - 8 - 12 - 3 * 28;
    for (size_t cut = plain + 1; cut < file.size(); ++cut) {
      const std::vector<uint8_t> part(file.begin(), file.begin() + cut);
      if (PortRoomGeo::Parse(part, out, error, &back)) {
        std::fprintf(stderr, "FAIL: anim truncated at %zu parses\n", cut);
        ++sFailures;
      }
    }
    std::vector<uint8_t> bad = file;
    bad[plain + 8] = 3; // no such instance
    Check(!PortRoomGeo::Parse(bad, out, error, &back), "anim of a missing instance rejected");
    bad = file;
    bad[4] = 4;
    Check(!PortRoomGeo::Parse(bad, out, error, &back), "anim in a version 4 file rejected");

    PortRoomGeo::Instance extra = in[1];
    std::vector<PortRoomGeo::Instance> two = in;
    two.push_back(extra);
    std::vector<uint8_t> dup = PortRoomGeo::Write(two);
    // The second entry's index (entries of 8 + 12 + 84 bytes after the 8 byte head) becomes 1.
    const size_t secondAt = dup.size() - (8 + 12 + 3 * 28);
    dup[secondAt] = 1;
    Check(!PortRoomGeo::Parse(dup, out, error, &back), "duplicate anim instance rejected");

    auto mutated = [&](const char* what, auto change) {
      std::vector<PortRoomGeo::Instance> t = in;
      change(t[1]);
      Check(!PortRoomGeo::Parse(PortRoomGeo::Write(t), out, error, &back), what);
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
  Check(file.size() > 8 && file[4] == 10, "version 10 written");
  Check(PortRoomGeo::Parse(file, out, error), "clips file parses");
  Check(out.size() == 2 && out[0].anim.empty() && !out[0].animOnShow && out[1].animOnShow &&
            out[1].anim.size() == 2 && out[1].anim[0].fps == 24.f && !out[1].anim[0].loop &&
            out[1].anim[0].keys == in[1].anim[0].keys && out[1].anim[1].fps == 12.f && out[1].anim[1].loop &&
            out[1].anim[1].keys == in[1].anim[1].keys,
        "clips round trip");

  // The ANIM entry starts after the 8 byte head, 8 + (12 + 84) + (12 + 56) bytes from the end.
  const size_t entry = 8 + 12 + 3 * 28 + 12 + 2 * 28;
  const size_t at = file.size() - entry;
  std::vector<uint8_t> bad = file;
  bad[at + 4] = 2; // unknown flag
  Check(!PortRoomGeo::Parse(bad, out, error), "unknown anim flag rejected");
  bad = file;
  bad[at + 5] = 0;
  Check(!PortRoomGeo::Parse(bad, out, error), "zero clips rejected");
  bad = file;
  bad[at + 5] = 3; // a third clip that is not there
  Check(!PortRoomGeo::Parse(bad, out, error), "missing clip rejected");
  bad = file;
  bad[at + 8 + 8] = 2; // loop flag
  Check(!PortRoomGeo::Parse(bad, out, error), "bad loop flag rejected");
  for (size_t cut = at - 8 + 1; cut < file.size(); ++cut) {
    const std::vector<uint8_t> part(file.begin(), file.begin() + cut);
    if (PortRoomGeo::Parse(part, out, error)) {
      std::fprintf(stderr, "FAIL: clips truncated at %zu parses\n", cut);
      ++sFailures;
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
    Check(PortRoomGeo::Parse(old, out, error), "old anim entry parses");
    Check(out.size() == 2 && out[1].anim.size() == 1 && out[1].anim[0].fps == 30.f && out[1].anim[0].loop &&
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
  Check(PortRoomGeo::Parse(file, out, error, &back) && out.size() == 1 && out[0].sky && back.hidden == script.hidden,
        "hidden objects round-trip");
  Check(PortRoomGeo::Write(in).size() == file.size() - 24, "no hidden section without hidden objects");
  for (size_t cut = file.size() - 23; cut < file.size(); ++cut) {
    const std::vector<uint8_t> part(file.begin(), file.begin() + cut);
    if (PortRoomGeo::Parse(part, out, error, &back)) {
      std::fprintf(stderr, "FAIL: hidden objects truncated at %zu parse\n", cut);
      ++sFailures;
    }
  }
  std::vector<uint8_t> bad = file;
  bad[file.size() - 20] = 0; // count 0
  bad.resize(file.size() - 16);
  Check(!PortRoomGeo::Parse(bad, out, error, &back), "empty hidden section rejected");
  bad = file;
  bad[file.size() - 4] = 1; // instance 1 of 1
  Check(!PortRoomGeo::Parse(bad, out, error, &back), "hidden object of a missing instance rejected");
  bad = file;
  bad[4] = 8;
  Check(!PortRoomGeo::Parse(bad, out, error, &back), "hidden section in a version 8 file rejected");
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
  Check(file.size() == plain + 8 + 24, "lightmap: section written only with a scale");
  std::vector<PortRoomGeo::Instance> out;
  std::string error;
  Check(PortRoomGeo::Parse(file, out, error) && out.size() == 2 && out[0].lightmap[2] == 0.f &&
            out[1].lightmap[0] == 0.25f && out[1].lightmap[1] == 0.5f && out[1].lightmap[2] == 0.125f,
        "lightmap: round trip");
  for (size_t cut = plain + 1; cut < file.size(); ++cut) {
    const std::vector<uint8_t> part(file.begin(), file.begin() + cut);
    if (PortRoomGeo::Parse(part, out, error)) {
      std::fprintf(stderr, "FAIL: lightmap truncated at %zu parses\n", cut);
      ++sFailures;
    }
  }
  std::vector<uint8_t> bad = file;
  bad[4] = 9;
  Check(!PortRoomGeo::Parse(bad, out, error), "lightmap: section in a version 9 file rejected");

  // A scale of 0 with an offset is a one-texel lookup (Remastered's props), kept on its own.
  in[1].lightmap[2] = 0.f;
  const std::vector<uint8_t> texel = PortRoomGeo::Write(in);
  Check(texel.size() == plain + 8 + 24 && PortRoomGeo::Parse(texel, out, error) && out.size() == 2 &&
            out[1].lightmap[0] == 0.25f && out[1].lightmap[1] == 0.5f && out[1].lightmap[2] == 0.f &&
            PortRoomGeo::HasLightmapLookup(out[1].lightmap) && !PortRoomGeo::HasLightmapLookup(out[0].lightmap),
        "lightmap: one-texel lookup round trip");
  const float negative[3] = {0.25f, 0.5f, -1.f};
  const float nan[3] = {0.25f, std::nanf(""), 0.125f};
  Check(!PortRoomGeo::HasLightmapLookup(negative) && !PortRoomGeo::HasLightmapLookup(nan),
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
  Check(PortRoomGeo::Parse(file, out, error, &back), "sky file parses");
  Check(out.size() == 2 && !out[0].sky && out[1].sky && out[0].anim.size() == 1 && out[0].anim[0].keys == in[0].anim[0].keys, "sky");
  Check(out.size() == 2 && out[1].skyRadiance[0] == 0.5f && out[1].skyRadiance[1] == 2.f &&
            out[1].skyRadiance[2] == 0.f,
        "sky radiance");
  // Version 6: the index alone, and no radiance.
  // Its animation entry is the version 5 one: no flags or loop, a single clip.
  const size_t animAt = file.size() - 24 - 84; // the sky section is 8 + 16 bytes, the animation's 8 + 76
  std::vector<uint8_t> old(file.begin(), file.begin() + animAt + 8);
  old.insert(old.end(), file.begin() + animAt + 8, file.begin() + animAt + 12);
  old.insert(old.end(), file.begin() + animAt + 16, file.begin() + animAt + 24);
  old.insert(old.end(), file.begin() + animAt + 28, file.begin() + animAt + 84);
  old.insert(old.end(), file.end() - 24, file.end() - 12);
  old[4] = 6;
  Check(PortRoomGeo::Parse(old, out, error, &back) && out.size() == 2 && out[1].sky &&
            out[1].skyRadiance[0] == 0.f && out[1].skyRadiance[1] == 0.f,
        "version 6 sky parses");
  // A negative or non-finite radiance reads as not known.
  std::vector<uint8_t> odd = file;
  const float nan = std::nanf("");
  const float negative = -1.f;
  std::memcpy(odd.data() + file.size() - 12, &negative, 4);
  std::memcpy(odd.data() + file.size() - 8, &nan, 4);
  Check(PortRoomGeo::Parse(odd, out, error, &back) && out.size() == 2 && out[1].skyRadiance[0] == 0.f &&
            out[1].skyRadiance[1] == 0.f,
        "odd sky radiance reads as 0");
  for (size_t cut = file.size() - 23; cut < file.size(); ++cut) {
    const std::vector<uint8_t> part(file.begin(), file.begin() + cut);
    if (PortRoomGeo::Parse(part, out, error, &back)) {
      std::fprintf(stderr, "FAIL: sky truncated at %zu parses\n", cut);
      ++sFailures;
    }
  }
  std::vector<uint8_t> bad = file;
  bad[file.size() - 16] = 2; // no such instance
  Check(!PortRoomGeo::Parse(bad, out, error, &back), "sky of a missing instance rejected");
  bad = file;
  bad[4] = 5;
  Check(!PortRoomGeo::Parse(bad, out, error, &back), "sky in a version 5 file rejected");
  bad = file;
  bad[file.size() - 20] = 2; // count 2
  Put32(bad, 1);
  for (int i = 0; i < 3; ++i) {
    Put32(bad, 0);
  }
  Check(!PortRoomGeo::Parse(bad, out, error, &back), "duplicate sky instance rejected");
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
    Check(PortRoomGeo::Parse(file, out, error, &back), "glow file parses");
    Check(out.size() == 3 && !out[0].glows && !out[2].glows && out[1].glows && out[1].glow[0] == 0.f &&
              out[1].glow[1] == 2.f && out[1].glow[2] == 1.326f,
          "glow");
    Check(out.size() == 3 && out[2].group == in[2].group, "groups before the glow");
    const size_t plain = file.size() - 8 - 16;
    for (size_t cut = plain + 1; cut < file.size(); ++cut) {
      const std::vector<uint8_t> part(file.begin(), file.begin() + cut);
      if (PortRoomGeo::Parse(part, out, error, &back)) {
        std::fprintf(stderr, "FAIL: glow truncated at %zu parses\n", cut);
        ++sFailures;
      }
    }
    std::vector<uint8_t> bad = file;
    bad[plain + 8] = 3; // no such instance
    Check(!PortRoomGeo::Parse(bad, out, error, &back), "glow of a missing instance rejected");
    bad = file;
    bad[4] = 3;
    Check(!PortRoomGeo::Parse(bad, out, error, &back), "glow in a version 3 file rejected");
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
  Check(PortRoomGeo::Parse(file, out, error), "v2 file parses");
  Check(out.size() == 2 && out[1].model == 0xabc00001 && out[1].layer == 1 && !out[1].active, "v2 instances");
  Check(out.size() == 2 && out[1].links.size() == 1 && out[1].links[0].sender == 0x00100021, "v2 links");
  Check(out.size() == 2 && out[1].platform == 0, "v2 has no platform");
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
  Check(PortRoomGeo::Parse(file, out, error, &back), "script file parses");
  Check(out.size() == 2 && out[0].group == PortRoomGeo::kNoGroup && out[1].group == 7, "groups");
  Check(back.nodes.size() == 2 && back.edges.size() == 3, "script sizes");
  if (back.nodes.size() == 2 && back.edges.size() == 3) {
    Check(back.nodes[0].kind == PortRoomGeo::kCameraVolume && back.nodes[0].centre[0] == 10.f &&
              back.nodes[0].half[1] == 2.5f && back.nodes[0].axes[0] == 0.f && back.nodes[0].axes[1] == 1.f &&
              back.nodes[0].active,
          "volume node");
    Check(back.nodes[1].kind == PortRoomGeo::kCounter && back.nodes[1].max == 50 && !back.nodes[1].active,
          "counter node");
    Check(!back.edges[1].retail && back.edges[1].event == 1 && back.edges[1].action == PortRoomGeo::kGroupHide &&
              back.edges[1].from == 1 && back.edges[1].to == 7,
          "group edge");
    Check(back.edges[2].retail && back.edges[2].event == 9 && back.edges[2].from == 0x0c1a0042, "retail edge");
  }

  // Without groups or a script the section is left out.
  in[1].group = PortRoomGeo::kNoGroup;
  Check(PortRoomGeo::Write(in).size() + 12 + 2 * 68 + 3 * 12 + 2 * 4 == PortRoomGeo::Write(in, &back).size(),
        "no empty section");

  for (size_t cut = 13; cut < file.size(); ++cut) {
    const std::vector<uint8_t> part(file.begin(), file.begin() + cut);
    if (PortRoomGeo::Parse(part, out, error, &back) && cut != PortRoomGeo::Write(in).size()) {
      std::fprintf(stderr, "FAIL: script truncated at %zu parses\n", cut);
      ++sFailures;
    }
  }

  std::vector<uint8_t> bad = file;
  bad[bad.size() - 8 - 3 * 12 + 4] = 9; // the first edge's from: no such node
  Check(!PortRoomGeo::Parse(bad, out, error, &back), "edge from a missing node rejected");
  bad = PortRoomGeo::Write(in);
  bad.push_back(1);
  Check(!PortRoomGeo::Parse(bad, out, error, &back), "trailing bytes rejected");
}

void TestLods() {
  std::vector<PortRoomGeo::Lods> in(2);
  in[0].model = 0x11111111;
  in[0].levels = {{100.f, 0x11111112}, {400.f, 0x11111113}};
  in[1].model = 0x22222222;
  in[1].levels = {{2500.f, 0x22222223}};
  const std::vector<uint8_t> file = PortRoomGeo::WriteLods(in);
  Check(file.size() == 12 + 2 * 8 + 3 * 8, "lods size");
  std::vector<PortRoomGeo::Lods> out;
  std::string error;
  Check(PortRoomGeo::ParseLods(file, out, error), "lods parse");
  Check(out.size() == 2 && out[0].model == 0x11111111 && out[0].levels.size() == 2 &&
            out[0].levels[1].distanceSq == 400.f && out[0].levels[1].model == 0x11111113,
        "first model's levels");
  Check(out.size() == 2 && out[1].levels.size() == 1 && out[1].levels[0].model == 0x22222223, "second model");
  for (size_t cut = 0; cut < file.size(); ++cut) {
    const std::vector<uint8_t> part(file.begin(), file.begin() + cut);
    if (PortRoomGeo::ParseLods(part, out, error)) {
      std::fprintf(stderr, "FAIL: lods truncated at %zu parses\n", cut);
      ++sFailures;
    }
  }
  std::vector<PortRoomGeo::Lods> bad = in;
  std::swap(bad[0].levels[0], bad[0].levels[1]);
  Check(!PortRoomGeo::ParseLods(PortRoomGeo::WriteLods(bad), out, error), "out-of-order distances rejected");
  bad = in;
  bad[1].levels.clear();
  Check(!PortRoomGeo::ParseLods(PortRoomGeo::WriteLods(bad), out, error), "model without levels rejected");
}
} // namespace

int main() {
  TestRoundTrip();
  TestVersion1();
  TestVersion2();
  TestScript();
  TestGlow();
  TestAnim();
  TestClips();
  TestSky();
  TestHide();
  TestLightmap();
  TestLods();
  uint32_t id = 0;
  Check(PortRoomGeo::ParseFileName("1a2B3c4D.ROOMGEO", id) && id == 0x1A2B3C4D, "file name");
  if (sFailures == 0) {
    std::printf("port_room_geo_tests: ok\n");
  }
  return sFailures == 0 ? 0 : 1;
}
