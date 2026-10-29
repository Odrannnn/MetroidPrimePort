#include "port_room_env.h"

#include "port_room_env_lod.h"
#include "port_room_geo.h"
#include "port_test.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

void Put32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(uint8_t(value >> (i * 8)));
  }
}

void PutFloat(std::vector<uint8_t>& out, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  Put32(out, bits);
}

// An axis-aligned box probe: centre and half extents.
void PutProbe(std::vector<uint8_t>& out, float cx, float cy, float cz, float hx, float hy, float hz, uint32_t cube) {
  const float centre[3] = {cx, cy, cz};
  const float half[3] = {hx, hy, hz};
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      PutFloat(out, row == col ? 1.f / half[row] : 0.f);
    }
    PutFloat(out, -centre[row] / half[row]);
  }
  for (int i = 0; i < 9; ++i) {
    PutFloat(out, i % 4 == 0 ? 1.f : 0.f);
  }
  Put32(out, 0);
  Put32(out, cube);
  PutFloat(out, 1.f);
  PutFloat(out, 0.05f);
}

void PutCube(std::vector<uint8_t>& out, uint32_t size, uint32_t mips, uint8_t fill) {
  const size_t bytes = PortRoomEnv::CubeBytes(size, mips);
  Put32(out, size);
  Put32(out, mips);
  Put32(out, 0);
  Put32(out, uint32_t(bytes));
  out.insert(out.end(), bytes, fill);
}

std::vector<uint8_t> MakeFile(uint32_t version = 1) {
  std::vector<uint8_t> out = {'M', 'P', 'E', 'V'};
  Put32(out, version);
  PutFloat(out, 4.f);
  PutFloat(out, 0.18f);
  PutFloat(out, 0.6f);
  PutFloat(out, 0.15f);
  Put32(out, 3);
  Put32(out, 2);
  PutProbe(out, 0.f, 0.f, 0.f, 10.f, 10.f, 10.f, 0);  // the room
  PutProbe(out, 2.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1);     // an alcove in it
  PutProbe(out, 40.f, 0.f, 0.f, 5.f, 5.f, 5.f, 0);    // next door
  PutCube(out, 8, 4, 0x11);
  PutCube(out, 4, 3, 0x22);
  return out;
}

// A grid point: grey light `level` (1.0 as a half is 0x3C00), with red arriving from +x of
// the grid, green from +y and blue from +z.
void PutPoint(std::vector<uint8_t>& out, uint16_t level) {
  for (int i = 0; i < 3; ++i) {
    out.push_back(uint8_t(level));
    out.push_back(uint8_t(level >> 8));
  }
  for (int i = 0; i < 3; ++i) {
    out.push_back(0x00);
    out.push_back(0x38); // 0.5
  }
  out.insert(out.end(), {255, 0, 51});
  for (int channel = 0; channel < 3; ++channel) {
    for (int axis = 0; axis < 3; ++axis) {
      out.push_back(channel == axis ? 255 : 128);
    }
  }
}

// Version 2: the same, then a 3 x 2 x 2 grid of 2 m cells whose point (0, 0, 0) is at world
// (10, 20, 30), with grid x along world y, y along world -x and z along world z. The
// points of the grid's x = 2 are empty, and those of x = 1 twice as bright as x = 0.
std::vector<uint8_t> MakeGridFile() {
  std::vector<uint8_t> out = MakeFile(2);
  Put32(out, 1);
  const float rows[12] = {0.f, 0.5f, 0.f, -10.f, -0.5f, 0.f, 0.f, 5.f, 0.f, 0.f, 0.5f, -15.f};
  for (float value : rows) {
    PutFloat(out, value);
  }
  Put32(out, 3);
  Put32(out, 2);
  Put32(out, 2);
  for (int i = 0; i < 12; ++i) {
    PutPoint(out, i % 3 == 0 ? 0x3C00 : i % 3 == 1 ? 0x4000 : 0);
  }
  return out;
}

bool Near(float a, float b) { return a > b - 0.01f && a < b + 0.01f; }

void TestGrid() {
  PortRoomEnv::File file;
  std::string error;
  const std::vector<uint8_t> good = MakeGridFile();
  CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(good), file, error), "grid: parse");
  CHECK_MSG(file.grids.size() == 1 && file.probes.size() == 3, "grid: counts");
  if (file.grids.size() != 1) {
    return;
  }
  const PortRoomEnv::Grid& grid = file.grids[0];
  CHECK_MSG(grid.size[0] == 3 && grid.size[1] == 2 && grid.size[2] == 2 && Near(grid.average, 1.414f), "grid: header");
  const size_t before = good.size() - 12 * 24 - 64;
  for (size_t length = before; length < good.size(); ++length) {
    CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(good.begin(), good.begin() + length), file, error), "grid: cut short");
  }
  std::vector<uint8_t> bad = good;
  bad[before + 4 + 48] = 0;
  CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(bad), file, error), "grid: size");
  bad = good;
  bad[before] = 200;
  CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(bad), file, error), "grid: count");
  CHECK_MSG(PortRoomEnv::Parse(MakeFile(), file, error) && file.grids.empty(), "grid: version 1 has none");
  CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(good), file, error), "grid: parse again");
  CHECK_MSG(file.exposure[0] == 0.f && file.exposure[1] == 0.f, "grid: version 2 has no exposure range");

  // Version 3 ends with the exposure range.
  std::vector<uint8_t> v3 = good;
  v3[4] = 3;
  CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(v3), file, error), "exposure: cut short");
  PutFloat(v3, 5.5f);
  PutFloat(v3, 6.f);
  CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v3), file, error) && file.exposure[0] == 5.5f && file.exposure[1] == 6.f,
            "exposure: range");
  PutFloat(v3, 0.f);
  std::memcpy(v3.data() + v3.size() - 8, v3.data() + v3.size() - 4, 4); // highest below lowest
  v3.resize(v3.size() - 4);
  CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v3), file, error) && file.exposure[0] == 0.f && file.exposure[1] == 0.f,
            "exposure: a backwards range is none");

  // Version 4 follows the range with the exposure bias and the curve's contrast.
  std::vector<uint8_t> v4 = v3;
  v4[4] = 4;
  CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(v4), file, error), "tone: cut short");
  PutFloat(v4, 1.5f);
  PutFloat(v4, 0.4f);
  CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v4), file, error) && file.exposureBias == 1.5f && file.contrast == 0.4f,
            "tone: bias and contrast");
  v4.resize(v4.size() - 4);
  PutFloat(v4, 7.f);
  CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v4), file, error) && file.contrast == 0.f,
            "tone: a contrast out of range is none");

  // Version 5 adds the bloom: a threshold and a count of RGBA tints.
  std::vector<uint8_t> v5 = v4;
  v5[4] = 5;
  CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(v5), file, error), "bloom: cut short");
  PutFloat(v5, 0.5f);
  Put32(v5, 2);
  CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(v5), file, error), "bloom: tints cut short");
  for (int i = 0; i < 8; ++i) {
    PutFloat(v5, float(i));
  }
  CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v5), file, error) && file.bloomThreshold == 0.5f &&
                file.bloomTints.size() == 8 && file.bloomTints[6] == 6.f,
            "bloom: threshold and tints");
  CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v4), file, error) && file.bloomTints.empty(),
            "bloom: version 4 has none");

  // Version 6 adds the colour grades: layer, fades, a 33^3 RGBA8 LUT each.
  std::vector<uint8_t> v6 = v5;
  v6[4] = 6;
  CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(v6), file, error), "grade: cut short");
  Put32(v6, 2);
  for (int g = 0; g < 2; ++g) {
    Put32(v6, g == 0 ? uint32_t(-1) : 3u);
    PutFloat(v6, g == 0 ? 1.f : 1000.f);
    PutFloat(v6, 0.5f);
    for (uint32_t z = 0; z < 33; ++z) {
      for (uint32_t y = 0; y < 33; ++y) {
        for (uint32_t x = 0; x < 33; ++x) {
          // The first is the identity, the second warms it.
          const uint8_t px[4] = {uint8_t(std::min(255u, x * 255 / 32 + (g == 1 ? 20u : 0u))), uint8_t(y * 255 / 32),
                                 uint8_t(z * 255 / 32), 255};
          v6.insert(v6.end(), px, px + 4);
        }
      }
    }
  }
  CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v6), file, error) && file.grades.size() == 2 &&
                file.grades[0].layer == -1 && file.grades[0].fadeIn == 1.f && file.grades[0].id == 0 &&
                file.grades[1].layer == 3 && file.grades[1].fadeIn == 0.f && file.grades[1].fadeOut == 0.5f &&
                file.grades[1].id != 0,
            "grade: layers, fades, the identity's id is 0");
  CHECK_MSG(file.exposureSigma == 32.f && file.staticLerp == 0.5f, "convergence: version 6 has the defaults");

  // Version 7 adds the exposure's convergence sigma and the static exposure's lerp.
  std::vector<uint8_t> v7 = v6;
  v7[4] = 7;
  CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(v7), file, error), "convergence: cut short");
  PutFloat(v7, 12.f);
  PutFloat(v7, 0.25f);
  CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v7), file, error) && file.exposureSigma == 12.f &&
                file.staticLerp == 0.25f,
            "convergence: sigma and lerp");
  v7.resize(v7.size() - 8);
  PutFloat(v7, -1.f);
  PutFloat(v7, 2.f);
  CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v7), file, error) && file.exposureSigma == 32.f &&
                file.staticLerp == 0.5f,
            "convergence: out of range values fall back");

  // Version 8 adds a probe's priority and intensity range after its padding.
  std::vector<uint8_t> v8 = v7;
  v8[4] = 8;
  for (int i = 2; i >= 0; --i) {
    std::vector<uint8_t> extra;
    Put32(extra, uint32_t(10 + i));
    PutFloat(extra, 0.25f);
    PutFloat(extra, 2.f);
    v8.insert(v8.begin() + 32 + 100 * (i + 1), extra.begin(), extra.end());
  }
  CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v8), file, error) && file.probes.size() == 3 &&
                file.probes[1].padding == 0.05f && file.probes[1].priority == 11 && file.probes[2].intensityMin == 0.25f &&
                file.probes[0].intensityMax == 2.f && file.exposureSigma == 32.f && file.grids.size() == 1,
            "probe: version 8 record");
  CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v7), file, error) && file.probes[1].padding == 1.f &&
                file.probes[1].priority == 0 && file.probes[1].intensityMin == 0.f && file.probes[1].intensityMax == 1.f,
            "probe: version 7 has the defaults");

  // Version 9 makes the layer field the retail script layer; before it, it is unused.
  for (size_t i = 0; i < 3; ++i) {
    const uint32_t layer = i == 1 ? 2u : uint32_t(-1);
    std::memcpy(v8.data() + 32 + 112 * i + 84, &layer, 4);
  }
  CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v8), file, error) && file.probes[1].layer == -1,
            "probe: version 8 ignores the layer");
  std::vector<uint8_t> v9 = v8;
  v9[4] = 9;
  CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v9), file, error) && file.probes[0].layer == -1 &&
                file.probes[1].layer == 2 && file.probes[1].priority == 11,
            "probe: version 9 layer");
  CHECK_MSG(PortRoomEnv::ProbeOn(file.probes[0], 0) && PortRoomEnv::ProbeOn(file.probes[1], 1u << 2) &&
                !PortRoomEnv::ProbeOn(file.probes[1], ~uint64_t(1u << 2)),
            "probe: on by its layer");
  CHECK_MSG(file.grades.size() == 2 && file.grades[1].on && file.grades[1].priority == 0 && file.grades[1].links.empty(),
            "grade: version 9 is on, priority 0, no links");

  // Version 10 adds a grade's start, priority and links after its fades.
  {
    std::vector<uint8_t> v10 = v9;
    v10[4] = 10;
    // The grades come after the bloom, which version 8 moved by three probes' 12 bytes.
    const size_t first = v5.size() + 4 + 36;
    const size_t record = 12 + PortRoomEnv::kGradeLutBytes;
    std::vector<uint8_t> extra1;
    extra1.push_back(0);
    extra1.insert(extra1.end(), 3, 0);
    Put32(extra1, 55);
    Put32(extra1, 2);
    Put32(extra1, PortRoomEnv::kSenderCameraWater);
    extra1.insert(extra1.end(), {0, 1, 0, 0});
    Put32(extra1, 0x04100022);
    extra1.insert(extra1.end(), {9, 2, 0, 0});
    std::vector<uint8_t> extra0 = {1, 1, 0, 0};  // on, the master's
    Put32(extra0, 50);
    Put32(extra0, 0);
    v10.insert(v10.begin() + std::ptrdiff_t(first + record + 12), extra1.begin(), extra1.end());
    v10.insert(v10.begin() + std::ptrdiff_t(first + 12), extra0.begin(), extra0.end());
    CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v10), file, error) && file.grades.size() == 2 &&
                  file.grades[0].on && file.grades[0].master && file.grades[0].priority == 50 &&
                  file.grades[0].links.empty() && !file.grades[1].on && !file.grades[1].master && file.grades[1].priority == 55 && file.grades[1].links.size() == 2 &&
                  file.grades[1].links[0].sender == PortRoomEnv::kSenderCameraWater && file.grades[1].links[0].action == 1 &&
                  file.grades[1].links[1].sender == 0x04100022 && file.grades[1].links[1].state == 9 &&
                  file.grades[1].fadeOut == 0.5f && file.grades[1].id != 0 && file.exposureSigma == 32.f,
              "grade: version 10 record");
    std::vector<uint8_t> bad = v10;
    bad[first + 12 + record + 12 + 9] = 2;  // grade 1's link count, now 514
    CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(bad), file, error), "grade: too many links");

    // Version 11 adds the backlight hints after the exposure floats.
    std::vector<uint8_t> v11 = v10;
    v11[4] = 11;
    CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(v11), file, error), "backlight: cut short");
    Put32(v11, 2);
    for (int i = 0; i < 2; ++i) {
      Put32(v11, i == 0 ? uint32_t(-1) : 3u);
      PutFloat(v11, i == 0 ? 1.f : 0.f);
      PutFloat(v11, 0.5f);
      v11.insert(v11.end(), {uint8_t(i == 0 ? 1 : 0), uint8_t(i == 1 ? 1 : 0), 0, 0});
      Put32(v11, i == 0 ? 50 : 70);
      PutFloat(v11, i == 0 ? 1.f : 3.f);
      PutFloat(v11, i == 0 ? 1.f : 0.25f);
      Put32(v11, i == 0 ? 0 : 1);
      if (i == 1) {
        Put32(v11, PortRoomEnv::kSenderPlayerFluid);
        v11.insert(v11.end(), {9, 1, 0, 0});
      }
    }
    CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v11), file, error) && file.backlights.size() == 2 &&
                  file.backlights[0].layer == -1 && file.backlights[0].on && file.backlights[0].priority == 50 &&
                  file.backlights[0].links.empty() && !file.backlights[0].master && file.backlights[1].master &&
                  file.backlights[1].layer == 3 && !file.backlights[1].on &&
                  file.backlights[1].fadeIn == 0.f && file.backlights[1].fadeOut == 0.5f &&
                  file.backlights[1].top == 3.f && file.backlights[1].back == 0.25f &&
                  file.backlights[1].links.size() == 1 &&
                  file.backlights[1].links[0].sender == PortRoomEnv::kSenderPlayerFluid &&
                  file.grades.size() == 2 && file.exposureSigma == 32.f,
              "backlight: version 11 record");
    CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v10), file, error) && file.backlights.empty(),
              "backlight: older versions have none");

    // Version 12 adds the volumetric fog hints (368 bytes and the links each).
    std::vector<uint8_t> v12 = v11;
    v12[4] = 12;
    CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(v12), file, error), "fog: cut short");
    Put32(v12, 1);
    Put32(v12, 5);
    PutFloat(v12, 2.f);
    PutFloat(v12, 3.f);
    v12.insert(v12.end(), {1, 0, 0, 0});
    Put32(v12, 60);
    for (int i = 0; i < 10; ++i) {
      PutFloat(v12, float(i + 1));  // range 1 .. lightCap 10
    }
    for (float w : {0.5f, 0.f, -0.5f}) {
      PutFloat(v12, w);
    }
    v12.insert(v12.end(), {1, 1, 0, 0});
    for (int i = 0; i < 8; ++i) {
      PutFloat(v12, 0.1f * float(i));
    }
    for (int i = 0; i < 64; ++i) {
      PutFloat(v12, float(i));
    }
    Put32(v12, 1);
    Put32(v12, 0x04100022);
    v12.insert(v12.end(), {9, 1, 0, 0});
    CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v12), file, error) && file.fogs.size() == 1 &&
                  file.fogs[0].layer == 5 && file.fogs[0].fadeIn == 2.f && file.fogs[0].fadeOut == 3.f &&
                  file.fogs[0].on && file.fogs[0].priority == 60 && file.fogs[0].range == 1.f &&
                  file.fogs[0].decay == 5.f && file.fogs[0].lightCap == 10.f && file.fogs[0].wind[2] == -0.5f &&
                  file.fogs[0].useScriptWind && file.fogs[0].noProbe && file.fogs[0].colorB[3] == 0.1f * 3 &&
                  file.fogs[0].colorA[0] == 0.1f * 4 && file.fogs[0].lut[63] == 63.f &&
                  file.fogs[0].links.size() == 1 && file.fogs[0].links[0].sender == 0x04100022 &&
                  file.fogs[0].links[0].state == 9 && file.backlights.size() == 2,
              "fog: version 12 record");
    CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v11), file, error) && file.fogs.empty(),
              "fog: older versions have none");
    CHECK_MSG(file.fogs.empty() && PortRoomEnv::Parse(std::vector<uint8_t>(v12), file, error) &&
                  file.fogs[0].linearFade && !file.fogs[0].hasFadeInSpline,
              "fog: version 12 fades linearly");

    // Version 13 adds each fog's fade splines after its links.
    std::vector<uint8_t> v13 = v12;
    v13[4] = 13;
    CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(v13), file, error), "fog: fades cut short");
    std::vector<uint8_t> spline;
    Put32(spline, 2);
    for (float t : {0.f, 2.f}) {
      PutFloat(spline, t);
      PutFloat(spline, t * 0.5f);
      spline.insert(spline.end(), {0, 0}); // linear tangents
    }
    PutFloat(spline, 0.f);
    PutFloat(spline, 1.f);
    spline.insert(spline.end(), {0, 0, 0}); // constant before and after, no clamp
    Put32(v13, uint32_t(spline.size()));
    v13.insert(v13.end(), spline.begin(), spline.end());
    v13.resize((v13.size() + 3) & ~size_t(3));
    Put32(v13, 0);
    CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v13), file, error) && file.fogs.size() == 1 &&
                  !file.fogs[0].linearFade && file.fogs[0].hasFadeInSpline && !file.fogs[0].hasFadeOutSpline &&
                  file.fogs[0].fadeInSpline.LastTime() == 2.f && file.fogs[0].fadeInSpline.Eval(1.f) == 0.5f &&
                  file.fogs[0].fadeInSpline.Eval(3.f) == 1.f && file.fogs[0].links.size() == 1,
              "fog: version 13 fade splines");

    // Version 14 adds the fog regions after the fogs.
    {
      std::vector<uint8_t> v14 = v13;
      v14[4] = 14;
      CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(v14), file, error), "fog region: count cut short");
      Put32(v14, 2);
      const auto putRegion = [&v14](float density, uint8_t fluid) {
        Put32(v14, uint32_t(-1));
        v14.insert(v14.end(), {1, fluid, 1, 0});
        for (int i = 0; i < 12; ++i) {
          PutFloat(v14, float(i));
        }
        for (int i = 0; i < 3; ++i) {
          PutFloat(v14, -2.f);
        }
        PutFloat(v14, 0.f); // mult: override
        for (int i = 0; i < 3; ++i) {
          PutFloat(v14, 3.f);
        }
        PutFloat(v14, 7.f); // cap
        for (int i = 0; i < 4; ++i) {
          PutFloat(v14, 0.25f * float(i + 1));
        }
        PutFloat(v14, density);
        for (int i = 0; i < 6; ++i) {
          PutFloat(v14, i < 3 ? -1.f : 1.f);
        }
      };
      putRegion(0.5f, 1);
      Put32(v14, 1);
      Put32(v14, 0x00100005);
      v14.insert(v14.end(), {9, 2, 0, 0});
      std::vector<uint8_t> cut = v14;
      putRegion(std::numeric_limits<float>::quiet_NaN(), 3);
      Put32(v14, 0);
      CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(cut), file, error), "fog region: records cut short");
      CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v14), file, error) && file.regions.size() == 2,
                "fog region: version 14 parses");
      if (file.regions.size() == 2) {
        const PortRoomEnv::FogRegion& r = file.regions[0];
        CHECK_MSG(r.layer == -1 && r.on && r.fluid == 1 && r.hasColor && !r.hasCap && r.m[11] == 11.f &&
                      r.edgeScale[2] == -2.f && r.mult == 0.f && r.edgeBias[0] == 3.f && r.cap == 7.f &&
                      r.color[3] == 1.f && r.density == 0.5f && r.box[0] == -1.f && r.box[5] == 1.f &&
                      r.links.size() == 1 && r.links[0].sender == 0x00100005 && r.links[0].state == 9 &&
                      r.links[0].action == 2,
                  "fog region: version 14 record");
        const PortRoomEnv::FogRegion& bad = file.regions[1];
        CHECK_MSG(bad.fluid == 0 && bad.density == 0.f && bad.box[0] > bad.box[3] && bad.links.empty(),
                  "fog region: a NaN region never shows");
      }
      CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v13), file, error) && file.regions.empty(),
                "fog region: older versions have none");
    }
    // Version 15 adds each region's distance, transmittance and mode, then the transitions.
    {
      std::vector<uint8_t> v15 = v13;
      v15[4] = 15;
      Put32(v15, 1);
      Put32(v15, uint32_t(-1));
      v15.insert(v15.end(), {1, 0, 0, 0});
      for (int i = 0; i < 12 + 3 + 1 + 3 + 1 + 4 + 1 + 6; ++i) {
        PutFloat(v15, 0.f);
      }
      Put32(v15, 0); // links
      PutFloat(v15, 40.f);
      PutFloat(v15, 0.5f);
      v15.insert(v15.end(), {1, 0, 0, 0}); // subtracting
      CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(v15), file, error), "fog transition: count cut short");
      Put32(v15, 2);
      const auto putTransition = [&v15, &spline](uint32_t region, bool withSpline) {
        Put32(v15, region);
        Put32(v15, 3);
        v15.insert(v15.end(), {1, 1, 0, 0x1d});
        PutFloat(v15, 100.f);
        PutFloat(v15, 0.25f);
        for (int i = 0; i < 4; ++i) {
          PutFloat(v15, 0.5f);
        }
        PutFloat(v15, 2.f);
        Put32(v15, withSpline ? uint32_t(spline.size()) : 0);
        if (withSpline) {
          v15.insert(v15.end(), spline.begin(), spline.end());
          v15.resize((v15.size() + 3) & ~size_t(3));
        }
        Put32(v15, 1);
        Put32(v15, 0x00100007);
        v15.insert(v15.end(), {4, PortRoomEnv::kTransitionRestart, 0, 0});
      };
      putTransition(0, true);
      std::vector<uint8_t> cut = v15;
      cut.resize(cut.size() - 4);
      putTransition(5, false);
      CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(cut), file, error), "fog transition: records cut short");
      CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v15), file, error) && file.regions.size() == 1 &&
                    file.transitions.size() == 2,
                "fog transition: version 15 parses");
      // Version 18 adds the lightmap after the suns: width 0 = none, else BC6H blocks.
      {
        std::vector<uint8_t> v18 = v15;
        v18[4] = 18;
        Put32(v18, 0); // suns
        std::vector<uint8_t> none = v18;
        Put32(none, 0);
        CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(none), file, error) && file.lightmap.width == 0,
                  "lightmap: none");
        std::vector<uint8_t> lit = v18;
        Put32(lit, 8);  // width
        Put32(lit, 4);  // height
        Put32(lit, 4);  // layers
        Put32(lit, 0);  // signed
        Put32(lit, 4 * 2 * 16);
        lit.insert(lit.end(), 4 * 2 * 16, 0x5a);
        CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(lit), file, error) && file.lightmap.width == 8 &&
                      file.lightmap.height == 4 && file.lightmap.layers == 4 && !file.lightmap.isSigned &&
                      file.lightmap.length == 128 && file.lightmap.offset + file.lightmap.length == file.data.size(),
                  "lightmap: record parses");
        std::vector<uint8_t> cutLm(lit.begin(), lit.end() - 1);
        CHECK_MSG(!PortRoomEnv::Parse(std::move(cutLm), file, error), "lightmap: cut short");
        std::vector<uint8_t> badLen = lit;
        badLen[badLen.size() - 128 - 4] = 64;
        CHECK_MSG(!PortRoomEnv::Parse(std::move(badLen), file, error), "lightmap: wrong length");
      }
      if (file.regions.size() == 1 && file.transitions.size() == 2) {
        const PortRoomEnv::FogRegion& r = file.regions[0];
        CHECK_MSG(r.distance == 40.f && r.transmittance == 0.5f && r.subtract, "fog transition: region tail");
        const PortRoomEnv::FogTransition& t = file.transitions[0];
        CHECK_MSG(t.region == 0 && t.layer == 3 && t.on && t.autoStart && !t.loop && t.select == 0xd &&
                      t.distance == 100.f && t.transmittance == 0.25f && t.color[3] == 0.5f && t.cap == 2.f &&
                      t.phase.LastTime() > 0.f && t.links.size() == 1 && t.links[0].sender == 0x00100007 &&
                      t.links[0].state == 4 && t.links[0].action == PortRoomEnv::kTransitionRestart,
                  "fog transition: version 15 record");
        const PortRoomEnv::FogTransition& bad = file.transitions[1];
        CHECK_MSG(bad.region == PortRoomEnv::kNoFogRegion && bad.select == 0, "fog transition: no region moves nothing");
      }
    }
    v13[v13.size() - 4 - ((spline.size() + 3) & ~size_t(3))] = 0xff; // a key count that runs off the end
    CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(v13), file, error), "fog: bad fade spline");
  }

  {
    PortRoomEnv::Convergence c;
    c.SetSigma(32.f);
    c.SetValue(4.f);
    for (int i = 0; i < 100; ++i) {
      c.Step(4.f);
    }
    CHECK_MSG(Near(c.value, 4.f), "convergence: a constant stays put");
    // The recursive filter's step response is an S curve with a small ripple near the top
    // (~0.004 in 6, around frame 130), never past the target.
    float prev = c.value;
    bool smooth = true;
    float atSigma = 0.f;
    for (int i = 0; i < 600; ++i) {
      c.Step(10.f);
      smooth = smooth && c.value >= prev - 0.01f && c.value <= 10.f + 1e-3f;
      prev = c.value;
      if (i == 31) {
        atSigma = c.value;
      }
    }
    CHECK_MSG(smooth && atSigma > 5.f && atSigma < 9.f && Near(c.value, 10.f), "convergence: a step settles on the target");
    c.SetValue(-2.f);
    CHECK_MSG(c.value == -2.f, "convergence: SetValue snaps");
    c.Step(-2.f);
    CHECK_MSG(Near(c.value, -2.f), "convergence: snapped history");
  }
  v6.pop_back();
  CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(v6), file, error), "grade: LUT cut short");
  CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(good), file, error), "grid: parse once more");

  PortRoomEnv::Ambient a;
  const float atPoint[3] = {10.f, 20.f, 30.f};
  CHECK_MSG(PortRoomEnv::SampleGrid(file, file.grids[0], atPoint, a), "grid: at a point");
  CHECK_MSG(Near(a.mean[0], 1.f) && Near(a.lobe[1], 0.5f) && Near(a.sharpness[0], 1.f) && Near(a.sharpness[1], 0.f) &&
                Near(a.sharpness[2], 0.2f),
            "grid: the point's values");
  // Red comes from grid +x, which is world +y; green from grid +y, world -x.
  CHECK_MSG(Near(a.direction[0][0], 0.f) && Near(a.direction[0][1], 1.f) && Near(a.direction[0][2], 0.f), "grid: red's direction");
  CHECK_MSG(Near(a.direction[1][0], -1.f) && Near(a.direction[1][1], 0.f), "grid: green's direction");
  CHECK_MSG(Near(a.direction[2][2], 1.f), "grid: blue's direction");
  const float halfway[3] = {9.f, 21.f, 31.f}; // grid (0.5, 0.5, 0.5)
  CHECK_MSG(PortRoomEnv::SampleGrid(file, file.grids[0], halfway, a) && Near(a.mean[0], 1.5f), "grid: between points");
  const float byEmpty[3] = {10.f, 23.5f, 30.f}; // grid x 1.75: the empty point counts as stored (0)
  CHECK_MSG(PortRoomEnv::SampleGrid(file, file.grids[0], byEmpty, a) && Near(a.mean[0], 0.5f), "grid: next to an empty point");
  const float inEmpty[3] = {10.f, 24.f, 30.f};
  CHECK_MSG(PortRoomEnv::SampleGrid(file, file.grids[0], inEmpty, a) && Near(a.mean[0], 0.f), "grid: an empty point");
  const float edge[3] = {10.f, 19.5f, 29.5f}; // a quarter cell outside two faces: the border texels read 0
  CHECK_MSG(PortRoomEnv::SampleGrid(file, file.grids[0], edge, a) && Near(a.mean[0], 0.5625f), "grid: just outside");
  const float outside[3] = {10.f, 18.f, 30.f};
  CHECK_MSG(!PortRoomEnv::SampleGrid(file, file.grids[0], outside, a), "grid: outside");
}

void TestNames() {
  uint32_t id = 0;
  CHECK_MSG(PortRoomEnv::ParseFileName("d1241219.RoomEnv", id) && id == 0xD1241219, "file name");
  CHECK_MSG(!PortRoomEnv::ParseFileName("D1241219.roomenvs", id), "file name: longer suffix");
  CHECK_MSG(!PortRoomEnv::ParseFileName("D124121G.roomenv", id), "file name: bad hex");
  CHECK_MSG(!PortRoomEnv::ParseFileName("D1241219.TXTR", id), "file name: a resource");
  CHECK_MSG(!PortRoomEnv::ParseFileName("", id), "file name: empty");
}

void TestParse() {
  CHECK_MSG(PortRoomEnv::CubeBytes(8, 4) == size_t(4 + 1 + 1 + 1) * 16 * 6, "cube bytes");
  PortRoomEnv::File file;
  std::string error;
  const std::vector<uint8_t> good = MakeFile();
  CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(good), file, error), "parse");
  CHECK_MSG(file.probes.size() == 3 && file.cubes.size() == 2, "parse: counts");
  CHECK_MSG(file.tonemap[1] == 0.18f, "parse: tonemap");
  CHECK_MSG(file.probes[1].cube == 1 && file.probes[1].padding == 1.f, "parse: probe");
  CHECK_MSG(file.cubes[1].size == 4 && file.cubes[1].mipCount == 3 && file.data[file.cubes[1].offset] == 0x22 &&
                file.data[file.cubes[0].offset + file.cubes[0].length - 1] == 0x11,
            "parse: cubes");

  // Every way of cutting the file short fails, and none reads past the end.
  for (size_t length = 0; length < good.size(); length += length < 400 ? 1 : 97) {
    CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(good.begin(), good.begin() + length), file, error), "parse: cut short");
  }
  std::vector<uint8_t> bad = good;
  bad[4] = 6;
  CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(bad), file, error), "parse: version");
  bad = good;
  bad[32 + 88] = 2;
  CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(bad), file, error), "parse: cube index");
  // A probe without a cube (its REFL texture id invalid) is kept, with no cube.
  bad = good;
  for (int i = 0; i < 4; ++i) {
    bad[32 + 88 + i] = 0xFF;
  }
  CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(bad), file, error) && !file.probes.empty() &&
                file.probes[0].cube == PortRoomEnv::kProbeNoCube,
            "parse: probe without a cube");
  bad = good;
  bad[32 + 300] = 6; // the first cube's size
  CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(bad), file, error), "parse: cube size");
  bad = good;
  bad[32 + 2] = 0xC0;
  bad[32 + 3] = 0x7F; // NaN
  CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(bad), file, error), "parse: not finite");
  bad = good;
  bad[24] = 0xFF;
  bad[25] = 0xFF;
  bad[26] = 0xFF;
  bad[27] = 0xFF;
  CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(bad), file, error), "parse: probe count");
}

void TestPick() {
  PortRoomEnv::File file;
  std::string error;
  CHECK_MSG(PortRoomEnv::Parse(MakeFile(), file, error), "pick: parse");
  const float inRoom[3] = {-5.f, 0.f, 0.f};
  const float inAlcove[3] = {2.5f, 0.5f, 0.f};
  const float between[3] = {30.f, 0.f, 0.f}; // 20 from the room, 5 from next door
  const float nearRoom[3] = {12.f, 0.f, 0.f};
  PortRoomEnv::Pick pick = PortRoomEnv::PickProbe(file, inRoom);
  CHECK_MSG(pick.probe == 0 && pick.inside, "pick: inside");
  pick = PortRoomEnv::PickProbe(file, inAlcove);
  CHECK_MSG(pick.probe == 1 && pick.inside && pick.score > 7.99f && pick.score < 8.01f, "pick: the smaller of two boxes");
  pick = PortRoomEnv::PickProbe(file, between);
  CHECK_MSG(pick.probe == 2 && !pick.inside && pick.score > 4.99f && pick.score < 5.01f, "pick: the nearest box");
  pick = PortRoomEnv::PickProbe(file, nearRoom);
  CHECK_MSG(pick.probe == 0 && !pick.inside, "pick: outside, nearest");
  file.probes[1].layer = 3;
  pick = PortRoomEnv::PickProbe(file, inAlcove, ~(uint64_t(1) << 3));
  CHECK_MSG(pick.probe == 0 && pick.inside, "pick: a probe whose layer is off is left out");
  pick = PortRoomEnv::PickProbe(file, inAlcove, uint64_t(1) << 3);
  CHECK_MSG(pick.probe == 1, "pick: its layer on");
  file.probes[1].layer = -1;

  PortRoomEnv::Pick none;
  CHECK_MSG(!none.Better(pick) && pick.Better(none), "pick: against none");
  CHECK_MSG(PortRoomEnv::PickProbe(PortRoomEnv::File(), inRoom).probe < 0, "pick: no probes");
}

// PickProbe as it was before the half extents were worked out at parse time: the same pick,
// bit for bit, is what the stored extents must give.
PortRoomEnv::Pick PickProbeReference(const PortRoomEnv::File& file, const float pos[3]) {
  PortRoomEnv::Pick best;
  for (size_t i = 0; i < file.probes.size(); ++i) {
    const float* m = file.probes[i].worldToBox;
    PortRoomEnv::Pick pick;
    pick.probe = int(i);
    pick.inside = true;
    float volume = 8.f;
    float distance2 = 0.f;
    for (int row = 0; row < 3; ++row) {
      const float* r = m + row * 4;
      const float u = r[0] * pos[0] + r[1] * pos[1] + r[2] * pos[2] + r[3];
      const float scale = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
      const float half = scale > 1e-12f ? 1.f / scale : 0.f;
      volume *= half;
      const float over = std::fabs(u) - 1.f;
      if (!(over <= 0.f)) {
        pick.inside = false;
        distance2 += over * half * over * half;
      }
    }
    pick.score = pick.inside ? volume : std::sqrt(distance2);
    if (pick.Better(best)) {
      best = pick;
    }
  }
  return best;
}

void TestPickExact() {
  // Turned, stretched and overlapping boxes (and one squashed to nothing along a row), each
  // written out as a file and parsed, then picked from at points in and around them.
  uint32_t seed = 12345;
  const auto random = [&seed](float lo, float hi) {
    seed = seed * 1664525u + 1013904223u;
    return lo + (hi - lo) * float(seed >> 8) / float(1u << 24);
  };
  constexpr uint32_t kProbes = 24;
  std::vector<uint8_t> data = {'M', 'P', 'E', 'V'};
  Put32(data, 1);
  for (int i = 0; i < 4; ++i) {
    PutFloat(data, 0.5f);
  }
  Put32(data, kProbes);
  Put32(data, 1);
  for (uint32_t probe = 0; probe < kProbes; ++probe) {
    const float angle = random(0.f, 6.2831853f);
    const float c = std::cos(angle), s = std::sin(angle);
    const float half[3] = {random(0.3f, 30.f), random(0.3f, 30.f), probe == 5 ? 0.f : random(0.3f, 30.f)};
    const float centre[3] = {random(-50.f, 50.f), random(-50.f, 50.f), random(-20.f, 20.f)};
    const float axes[3][3] = {{c, s, 0.f}, {-s, c, 0.f}, {0.f, 0.f, 1.f}};
    for (int row = 0; row < 3; ++row) {
      const float inverse = half[row] > 0.f ? 1.f / half[row] : 0.f;
      float offset = 0.f;
      for (int col = 0; col < 3; ++col) {
        PutFloat(data, axes[row][col] * inverse);
        offset -= axes[row][col] * inverse * centre[col];
      }
      PutFloat(data, offset);
    }
    for (int i = 0; i < 9; ++i) {
      PutFloat(data, i % 4 == 0 ? 1.f : 0.f);
    }
    Put32(data, 0);
    Put32(data, 0);
    PutFloat(data, 1.f);
    PutFloat(data, 0.05f);
  }
  PutCube(data, 4, 3, 0x11);
  PortRoomEnv::File file;
  std::string error;
  CHECK_MSG(PortRoomEnv::Parse(std::move(data), file, error), "pick exact: parse");
  int mismatches = 0;
  int inside = 0;
  for (int i = 0; i < 20000; ++i) {
    const float pos[3] = {random(-90.f, 90.f), random(-90.f, 90.f), random(-40.f, 40.f)};
    const PortRoomEnv::Pick a = PortRoomEnv::PickProbe(file, pos);
    const PortRoomEnv::Pick b = PickProbeReference(file, pos);
    if (a.probe != b.probe || a.inside != b.inside || std::memcmp(&a.score, &b.score, sizeof(a.score)) != 0) {
      ++mismatches;
    }
    inside += a.inside ? 1 : 0;
  }
  CHECK_MSG(mismatches == 0, "pick exact: same picks as before");
  CHECK_MSG(inside > 100 && inside < 19900, "pick exact: points both in and out of boxes");
  CHECK_MSG(file.probes[5].half[2] == 0.f && file.probes[5].volume == 0.f, "pick exact: a flat box has no volume");
}

// An axis-aligned probe along x: centre, half extent (all three axes).
PortRoomEnv::Probe BoxProbe(float cx, float half, int32_t priority, float scale, float padding) {
  PortRoomEnv::Probe p{};
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 4; ++col) {
      p.worldToBox[row * 4 + col] = row == col ? 1.f / half : 0.f;
    }
  }
  p.worldToBox[3] = -cx / half;
  p.scale = scale;
  p.padding = padding;
  p.priority = priority;
  p.intensityMin = 0.1f * scale;
  p.intensityMax = 2.f * scale;
  return p;
}

void TestBlend() {
  bool inside = false;
  const PortRoomEnv::Probe room = BoxProbe(0.f, 10.f, 0, 2.f, 4.f);
  const float centre[3] = {0.f, 0.f, 0.f};
  const float out1[3] = {11.f, 0.f, 0.f};
  const float corner[3] = {11.f, 13.f, 0.f}; // 1 m out in x, 3 m in y: the farthest counts
  const float far[3] = {20.f, 0.f, 0.f};
  CHECK_MSG(PortRoomEnv::ProbeFade(room, centre, inside) == 1.f && inside, "fade: inside");
  CHECK_MSG(Near(PortRoomEnv::ProbeFade(room, out1, inside), 0.75f) && !inside, "fade: in the padding");
  CHECK_MSG(Near(PortRoomEnv::ProbeFade(room, corner, inside), 0.25f), "fade: farthest axis");
  CHECK_MSG(PortRoomEnv::ProbeFade(room, far, inside) == 0.f, "fade: past the padding");
  PortRoomEnv::Probe tight = room;
  tight.padding = 0.f;
  CHECK_MSG(PortRoomEnv::ProbeFade(tight, out1, inside) == 0.f && PortRoomEnv::ProbeFade(tight, centre, inside) == 1.f,
            "fade: no padding");

  // A small high-priority probe inside the room, and a room next door.
  const PortRoomEnv::Probe alcove = BoxProbe(5.f, 1.f, 1, 1.f, 2.f);
  const PortRoomEnv::Probe nextDoor = BoxProbe(22.f, 10.f, 0, 4.f, 4.f);
  const PortRoomEnv::BlendCandidate all[3] = {{1, &room}, {2, &alcove}, {3, &nextDoor}};
  PortRoomEnv::Blend blend;
  PortRoomEnv::UpdateBlend(all, 3, centre, blend);
  CHECK_MSG(blend.entries.size() == 1 && blend.entries[0].key == 1 && blend.entries[0].weight == 1.f &&
                blend.intensity == 2.f && Near(blend.min, 0.2f) && blend.max == 4.f,
            "blend: one probe keeps its own values");

  // 1 m outside the alcove: it takes half, the room (which holds the point) the rest.
  const float byAlcove[3] = {7.f, 0.f, 0.f};
  PortRoomEnv::UpdateBlend(all, 3, byAlcove, blend);
  CHECK_MSG(blend.entries.size() == 2 && blend.entries[0].key == 2 && blend.entries[1].key == 1, "blend: priority first");
  // Raw weights 0.5 and 0.5; intensity 0.5 * 1 + 0.5 * 2.
  CHECK_MSG(Near(blend.intensity, 1.5f) && Near(blend.entries[0].weight, 1.f / 3.f) &&
                Near(blend.entries[1].weight, 2.f / 3.f) && Near(blend.min, 0.15f) && Near(blend.max, 3.f),
            "blend: weights by share and intensity");

  // Between the rooms (both 1 m outside, padding 4): 0.75 of the first, 0.25 * 0.75 of the
  // second. The room was listed last frame, so it stays first.
  const float between[3] = {11.f, 0.f, 0.f};
  PortRoomEnv::UpdateBlend(all, 3, between, blend);
  CHECK_MSG(blend.entries.size() == 2 && blend.entries[0].key == 1 && blend.entries[1].key == 3, "blend: old first on ties");
  const float sum = 0.75f * 2.f + 0.1875f * 4.f;
  CHECK_MSG(Near(blend.intensity, sum) && Near(blend.entries[0].weight, 1.5f / sum), "blend: fading out of two rooms");

  // Swapped order with no history: the candidates' order decides.
  PortRoomEnv::Blend fresh;
  const PortRoomEnv::BlendCandidate swapped[2] = {{3, &nextDoor}, {1, &room}};
  PortRoomEnv::UpdateBlend(swapped, 2, between, fresh);
  CHECK_MSG(fresh.entries.size() == 2 && fresh.entries[0].key == 3, "blend: new ones in the candidates' order");

  // A probe that left the candidates (its area unloaded) drops out.
  PortRoomEnv::UpdateBlend(all + 2, 1, between, blend);
  CHECK_MSG(blend.entries.size() == 1 && blend.entries[0].key == 3 && blend.intensity == 4.f, "blend: unloaded probe");
  PortRoomEnv::UpdateBlend(all, 3, far, blend);
  CHECK_MSG(blend.entries.size() == 1 && blend.entries[0].key == 3, "blend: inside next door");
  const float nowhere[3] = {100.f, 0.f, 0.f};
  PortRoomEnv::UpdateBlend(all, 3, nowhere, blend);
  CHECK_MSG(blend.entries.empty() && blend.intensity == 1.f && blend.min == 0.f && blend.max == 1.f, "blend: none");

  // At most four, by priority.
  std::vector<PortRoomEnv::Probe> many;
  for (int i = 0; i < 6; ++i) {
    many.push_back(BoxProbe(0.f, 10.f + float(i), i, 1.f, 10.f));
  }
  std::vector<PortRoomEnv::BlendCandidate> manyCandidates;
  for (int i = 0; i < 6; ++i) {
    manyCandidates.push_back({uint64_t(100 + i), &many[i]});
  }
  PortRoomEnv::Blend capped;
  // Outside every box; the smallest is out of reach, the next four of five fade 0.5..0.2.
  const float edge[3] = {20.f, 0.f, 0.f};
  PortRoomEnv::UpdateBlend(manyCandidates.data(), manyCandidates.size(), edge, capped);
  CHECK_MSG(capped.entries.size() == 4 && capped.entries[0].key == 105 && capped.entries[3].key == 102,
            "blend: four, by priority");
  // The point inside the top probe: it takes everything.
  const float edgeIn[3] = {14.f, 0.f, 0.f};
  PortRoomEnv::UpdateBlend(manyCandidates.data(), manyCandidates.size(), edgeIn, capped);
  CHECK_MSG(capped.entries.size() == 1 && capped.entries[0].key == 105, "blend: the top probe holds the point");
}

// The mip a reflection is read from: the cube's own top one, as Remastered reads it (roughness
// times the probe cube's mip count minus one). A cap only lowers it, and nothing makes it
// negative or past the cube. A locally imported 128^2 cube has 8 mips, so it reads at 7.
void TestLod() {
  const float none = PortRoomEnvLod::kNoCap;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  CHECK_MSG(PortRoomEnvLod::CubeLod(8, none) == 7.f, "lod: 8 mips, no cap");
  CHECK_MSG(PortRoomEnvLod::CubeLod(1, none) == 0.f, "lod: 1 mip, no cap");
  CHECK_MSG(PortRoomEnvLod::CubeLod(0, none) == 0.f, "lod: no mips, no cap");
  CHECK_MSG(PortRoomEnvLod::CubeLod(8, 5.f) == 5.f, "lod: capped to 5");
  CHECK_MSG(PortRoomEnvLod::CubeLod(8, 0.f) == 0.f, "lod: capped to 0");
  CHECK_MSG(PortRoomEnvLod::CubeLod(8, 99.f) == 7.f, "lod: a cap past the range does not raise it");
  CHECK_MSG(PortRoomEnvLod::CubeLod(3, 99.f) == 2.f, "lod: a cap past a short cube's range");
  CHECK_MSG(PortRoomEnvLod::CubeLod(8, 7.f) == 7.f, "lod: a cap at the range is that range");
  CHECK_MSG(PortRoomEnvLod::CubeLod(2, 1.f) == 1.f, "lod: capped, 2 mips");
  CHECK_MSG(PortRoomEnvLod::CubeLod(2, 3.f) == 1.f, "lod: a cap past 2 mips");
  // A fraction of a mip is fine: the sampler interpolates between the two.
  CHECK_MSG(PortRoomEnvLod::CubeLod(8, 2.5f) == 2.5f, "lod: a fractional cap");
  CHECK_MSG(PortRoomEnvLod::CubeLod(2, 0.25f) == 0.25f, "lod: a fractional cap under the range");
  // A negative or nonfinite value is not a mip, so the cube's own range stands.
  CHECK_MSG(PortRoomEnvLod::CubeLod(8, -1.f) == 7.f, "lod: a negative cap is ignored");
  CHECK_MSG(PortRoomEnvLod::CubeLod(8, -0.5f) == 7.f, "lod: a small negative cap is ignored");
  CHECK_MSG(PortRoomEnvLod::CubeLod(8, nan) == 7.f, "lod: a NaN cap is ignored");
  CHECK_MSG(PortRoomEnvLod::CubeLod(8, inf) == 7.f, "lod: an infinite cap is ignored");
  CHECK_MSG(PortRoomEnvLod::CubeLod(8, -inf) == 7.f, "lod: a negative infinite cap is ignored");
  // Whatever the cap, a cube of one mip or none is read at 0, never below.
  CHECK_MSG(PortRoomEnvLod::CubeLod(1, 0.f) == 0.f && PortRoomEnvLod::CubeLod(0, 0.f) == 0.f &&
                PortRoomEnvLod::CubeLod(0, none) == 0.f && PortRoomEnvLod::CubeLod(1, -3.f) == 0.f,
            "lod: one mip or none stays at 0");
}

void TestBrdfLut() {
  std::string error;
  std::vector<uint8_t> table(PortRoomEnv::kBrdfLutSize, 0x80);
  CHECK_MSG(PortRoomEnv::kBrdfLutSize == 256, "brdf: 16x8 RG8 is 256 bytes");
  CHECK_MSG(PortRoomEnv::ValidBrdfLut(table, error), "brdf: a table of the right size is valid");
  table.pop_back();
  CHECK_MSG(!PortRoomEnv::ValidBrdfLut(table, error) && !error.empty(), "brdf: one byte short is rejected with a reason");
  table.assign(PortRoomEnv::kBrdfLutSize + 1, 0);
  CHECK_MSG(!PortRoomEnv::ValidBrdfLut(table, error), "brdf: one byte long is rejected");
  table.clear();
  CHECK_MSG(!PortRoomEnv::ValidBrdfLut(table, error), "brdf: an empty file is rejected");
}

void TestPowerBomb() {
  const auto isColor = [](const float rgb[3], float r, float g, float b) {
    return std::fabs(rgb[0] - r) < 1e-4f && std::fabs(rgb[1] - g) < 1e-4f && std::fabs(rgb[2] - b) < 1e-4f;
  };
  float rgb[3];
  PortRoomEnv::PowerBombBakedLight(-1.f, rgb);
  CHECK_MSG(isColor(rgb, 1.f, 1.f, 1.f), "bomb: no bomb is white");
  PortRoomEnv::PowerBombBakedLight(1.7f, rgb);
  CHECK_MSG(isColor(rgb, 1.f, 1.f, 1.f), "bomb: white before 1.75 s");
  PortRoomEnv::PowerBombBakedLight(2.625f, rgb);
  CHECK_MSG(isColor(rgb, 18.f, 0.5f + 0.643f * 17.5f, 0.5f + 0.298f * 17.5f), "bomb: halfway at 2.625 s");
  PortRoomEnv::PowerBombBakedLight(3.75f, rgb);
  CHECK_MSG(isColor(rgb, 35.f, 0.643f * 35.f, 0.298f * 35.f), "bomb: full orange from 3.5 to 4 s");
  PortRoomEnv::PowerBombBakedLight(4.25f, rgb);
  CHECK_MSG(isColor(rgb, 18.f, 0.5f + 0.643f * 17.5f, 0.5f + 0.298f * 17.5f), "bomb: half back at 4.25 s");
  PortRoomEnv::PowerBombBakedLight(4.5f, rgb);
  CHECK_MSG(isColor(rgb, 1.f, 1.f, 1.f), "bomb: white again from 4.5 s");
}
void TestExposureHints() {
  using PortRoomEnv::ExposureHints;
  using PortRoomEnv::FluidHint;
  using PortRoomEnv::ParamTransition;
  std::vector<FluidHint> hints(2);
  hints[0].mode = 1;
  hints[1].mode = 2;
  ExposureHints h;
  // Camera outside at load: mode 1 idle, mode 2 inactive until the first exit.
  CHECK_MSG(!h.Load(hints, false), "hints: nothing registers at load outside a fluid");
  CHECK_MSG(h.Pick(2) == -1, "hints: the plain hint outside");
  CHECK_MSG(h.Set(hints, true), "hints: entering registers mode 1 (snap)");
  CHECK_MSG(h.Pick(2) == 0, "hints: mode 1 in force inside");
  CHECK_MSG(!h.Set(hints, true), "hints: no change, no snap");
  CHECK_MSG(h.Set(hints, false), "hints: leaving registers mode 2 (snap)");
  CHECK_MSG(h.Pick(2) == 1, "hints: mode 2 in force after the exit");
  CHECK_MSG(h.Set(hints, true) && h.Pick(2) == 0, "hints: re-entering swaps back to mode 1");
  // Camera inside at load: mode 1 registers (snap), mode 2 stays out.
  CHECK_MSG(h.Load(hints, true) && h.Pick(2) == 0, "hints: inside at load starts mode 1");
  // Mode 1 alone: leaving unregisters without a snap.
  std::vector<FluidHint> one(1);
  ExposureHints g;
  g.Load(one, false);
  g.Set(one, true);
  CHECK_MSG(!g.Set(one, false) && g.Pick(1) == -1, "hints: leaving only unregisters mode 1");

  // Shipped registration is priority descending, insertion-before-equal. A
  // priority of 1 or 2 is not a fluid mode (SLdrAutoExposureHint::Load 0x538b54).
  {
    std::vector<FluidHint> ranked(4);
    for (auto& hint : ranked) {
      hint.mode = 0;
    }
    ranked[0].priority = 6;
    ranked[1].priority = 1;
    ranked[2].priority = 6;
    ranked[3].priority = 10;
    ranked[3].on = false;
    ExposureHints r;
    CHECK(!r.Load(ranked, false));
    CHECK_MSG(r.Pick(4) == 2, "highest active priority, newest first on ties");
    CHECK_MSG(!r.Set(ranked, true) && r.Pick(4) == 2, "plain priority-6 hints do not change underwater");
    ranked[2].mode = 1;
    r.Load(ranked, false);
    CHECK(r.Pick(4) == 0);
    CHECK(r.Set(ranked, true) && r.Pick(4) == 2);
    ranked[2].priority = -1;
    r.Load(ranked, false);
    CHECK_MSG(!r.Set(ranked, true) && r.Pick(4) == 0, "a newer lower-priority fluid hint cannot override or snap");
  }

  // CAutoExposureHintMP1: pair A (role 1) and B (role 2), plus an ordinary mode 1 hint.
  {
    using Swap = ExposureHints;
    std::vector<FluidHint> pair(3);
    pair[0].mode = 0;
    pair[0].role = 1;
    pair[1].mode = 0;
    pair[1].role = 2;
    pair[2].mode = 1;
    ExposureHints p;
    CHECK_MSG(!p.Load(pair, false, 1) && p.Pick(3) == 0, "pair: A registers at load, without a snap");
    CHECK_MSG(!p.SetPair(pair, Swap::SwapState(true, false, false)), "pair: no change, no snap");
    CHECK_MSG(p.SetPair(pair, Swap::SwapState(true, true, false)) && p.Pick(3) == 1, "pair: thermal registers B (snap)");
    CHECK_MSG(!p.SetPair(pair, Swap::SwapState(true, true, false)), "pair: same state, no snap");
    CHECK_MSG(p.SetPair(pair, Swap::SwapState(true, false, false)) && p.Pick(3) == 0, "pair: back to A (snap)");
    CHECK_MSG(!p.SetPair(pair, Swap::SwapState(false, true, false)), "pair: morphed thermal stays on A");
    CHECK_MSG(p.SetPair(pair, Swap::SwapState(true, true, true)) == false && p.Pick(3) == 0,
              "pair: thermal in a cinematic stays on A");
    CHECK_MSG(p.SetPair(pair, Swap::SwapState(true, true, false)) && p.Pick(3) == 1, "pair: thermal again picks B");
    // The newest registered hint wins: a fluid hint registered after B outranks it.
    CHECK_MSG(p.Set(pair, true) && p.Pick(3) == 2, "pair: the fluid hint registered last wins");
    CHECK_MSG(p.SetPair(pair, 1) && p.Pick(3) == 0, "pair: A registers after the fluid hint and wins");
    // Load straight into thermal: B, no snap.
    ExposureHints q;
    CHECK_MSG(!q.Load(pair, false, 0) && q.Pick(3) == 1, "pair: load in thermal starts B");
    // Fluid-mode pair hint: eligible only inside the fluid while enabled.
    std::vector<FluidHint> fl(2);
    fl[0].mode = 1;
    fl[0].role = 1;
    fl[1].mode = 1;
    fl[1].role = 2;
    ExposureHints r;
    r.Load(fl, false, 1);
    CHECK_MSG(r.Pick(2) == -1, "pair: fluid-mode hints idle outside");
    CHECK_MSG(r.Set(fl, true) && r.Pick(2) == 0, "pair: inside registers A only");
    CHECK_MSG(r.SetPair(fl, 0) && r.Pick(2) == 1, "pair: swap inside registers B");
    // Script modes never register.
    std::vector<FluidHint> sc(2);
    sc[0].mode = 5;
    sc[0].role = 1;
    sc[1].mode = 5;
    sc[1].role = 2;
    ExposureHints s5;
    s5.Load(sc, true, 1);
    CHECK_MSG(!s5.SetPair(sc, 0) && !s5.Set(sc, false) && s5.Pick(2) == -1, "pair: mode 5 never registers");
  }

  const float a[6] = {1, 2, 3, 4, 5, 6};
  const float b[6] = {11, 12, 13, 14, 15, 16};
  const float c[6] = {21, 22, 23, 24, 25, 26};
  ParamTransition t;
  t.Snap(a);
  t.Step(a, 0.1f);
  CHECK_MSG(!t.moving && t.shown[0] == 1.f, "transition: idle at the target");
  t.Step(b, 0.f);
  CHECK_MSG(t.moving && t.progress == 0.f && t.shown[0] == 1.f, "transition: a new target starts at progress 0");
  t.Step(b, 0.5f);
  CHECK_MSG(std::fabs(t.shown[0] - 6.f) < 1e-4f && std::fabs(t.progress - 0.5f) < 1e-6f, "transition: half way");
  t.Step(c, 0.f);
  CHECK_MSG(t.from[0] == 1.f && std::fabs(t.progress - 0.5f) < 1e-6f, "transition: retarget keeps from and progress");
  CHECK_MSG(std::fabs(t.shown[0] - 1.f) > 0.f && t.to[0] == 21.f, "transition: the retarget aims at the new target");
  t.Step(c, 0.5f);
  CHECK_MSG(!t.moving && t.from[0] == 21.f && t.shown[0] == 21.f, "transition: ends on the target");
  t.Snap(b);
  CHECK_MSG(!t.moving && t.shown[3] == 14.f && t.from[3] == 14.f, "transition: snap");
}
// A version 21 tile-set section: identity transform, `box`, one 1x1x1 layer-0 tile.
std::vector<uint8_t> MakeTileSection(const float matrix0, const int32_t box[6], uint32_t count = 1) {
  std::vector<uint8_t> out;
  for (int i = 0; i < 12; ++i) {
    PutFloat(out, i == 0 ? matrix0 : 0.f);
  }
  for (int i = 0; i < 6; ++i) {
    Put32(out, uint32_t(box[i]));
  }
  Put32(out, count);
  Put32(out, count != 0 ? 6 : 0);
  for (uint32_t i = 0; i < count; ++i) {
    for (uint32_t v : {0u, i, 0u, 0u, 0u, 0u, 0u, 1u, 1u, 1u, 0u}) {
      Put32(out, v);
    }
  }
  out.insert(out.end(), count != 0 ? 6 : 0, 0);
  return out;
}

void TestTileSection() {
  const int32_t good[6] = {0, 0, 0, 10, 10, 10};
  const auto parse = [](const std::vector<uint8_t>& d) {
    PortRoomEnvTiles::TileSet set;
    std::string error;
    size_t at = 0;
    return PortRoomEnv::ParseTileSet(d, at, set, error);
  };
  CHECK_MSG(parse(MakeTileSection(1.f, good)), "tiles: well formed");
  const int32_t inverted[6] = {5, 0, 0, 4, 10, 10};
  CHECK_MSG(!parse(MakeTileSection(1.f, inverted)), "tiles: inverted box");
  const int32_t huge[6] = {-2000000000, 0, 0, 2000000000, 10, 10};
  CHECK_MSG(!parse(MakeTileSection(1.f, huge)), "tiles: absurd box");
  const int32_t wide[6] = {0, 0, 0, 100000, 10, 10};
  CHECK_MSG(!parse(MakeTileSection(1.f, wide)), "tiles: box wider than a room");
  CHECK_MSG(!parse(MakeTileSection(std::numeric_limits<float>::infinity(), good)), "tiles: non-finite transform");
  std::vector<uint8_t> many = MakeTileSection(1.f, good);
  many[72] = 0xFF;
  many[73] = 0xFF;
  many[74] = 0xFF;
  many[75] = 0x7F;
  CHECK_MSG(!parse(many), "tiles: absurd tile count");
  std::vector<uint8_t> cut = MakeTileSection(1.f, good);
  cut.pop_back();
  CHECK_MSG(!parse(cut), "tiles: cut short");
}

// A version 22 file with nothing in it but `volumes` world volumes.
std::vector<uint8_t> MakeVolumeFile(const std::vector<PortRoomEnv::WorldVolume>& volumes) {
  std::vector<uint8_t> out = {'M', 'P', 'E', 'V'};
  Put32(out, 22);
  for (float t : {4.f, 0.18f, 0.6f, 0.15f}) {
    PutFloat(out, t);
  }
  Put32(out, 0); // probes
  Put32(out, 0); // cubes
  Put32(out, 0); // no tiles
  for (float f : {0.f, 0.f, 0.f, 0.f, 0.9f}) {
    PutFloat(out, f); // exposure range, bias, contrast, bloom threshold
  }
  Put32(out, 0); // tints
  Put32(out, 0); // grades
  PutFloat(out, 32.f);
  PutFloat(out, 0.5f);
  for (int section = 0; section < 8; ++section) {
    Put32(out, 0); // backlights, fogs, regions, transitions, suns, lightmap, lights, hints
  }
  Put32(out, uint32_t(volumes.size()));
  for (const PortRoomEnv::WorldVolume& v : volumes) {
    for (float f : v.centre) PutFloat(out, f);
    for (float f : v.half) PutFloat(out, f);
    for (float f : v.axes) PutFloat(out, f);
  }
  return out;
}

// CAreaPrioritizedGameHintManager::Update (0x5e4de4): the view area's own hint, by priority
// then the last turned on, wins over any of the master's; the master's shows only when the
// room has none on.
void TestHintOutranks() {
  PortRoomEnv::Grade master, room, room2;
  master.master = true;
  master.priority = 90;
  room.priority = 10;
  room2.priority = 10;
  CHECK_MSG(PortRoomEnv::HintOutranks(master, 0, static_cast<const PortRoomEnv::Grade*>(nullptr), 0),
            "hint: anything beats none");
  CHECK_MSG(PortRoomEnv::HintOutranks(room, 1, &master, 0) && !PortRoomEnv::HintOutranks(master, 1, &room, 0),
            "hint: the room's beats a higher-priority master's");
  PortRoomEnv::Grade master2 = master;
  master2.priority = 95;
  CHECK_MSG(PortRoomEnv::HintOutranks(master2, 1, &master, 0) && !PortRoomEnv::HintOutranks(master, 0, &master2, 1),
            "hint: the master's by priority among themselves");
  CHECK_MSG(PortRoomEnv::HintOutranks(room2, 2, &room, 1) && !PortRoomEnv::HintOutranks(room, 1, &room2, 2),
            "hint: equal priorities, the last turned on");
  // The same rule for the fogs and backlights, and as a scan over a room's list.
  std::vector<PortRoomEnv::FogHint> fogs(3);
  fogs[0].master = true;
  fogs[0].priority = 10;
  fogs[1].priority = 5;
  fogs[2].master = true;
  fogs[2].priority = 20;
  const PortRoomEnv::FogHint* pick = nullptr;
  uint64_t pickOrder = 0;
  for (size_t i = 0; i < fogs.size(); ++i) {
    if (PortRoomEnv::HintOutranks(fogs[i], i, pick, pickOrder)) {
      pick = &fogs[i];
      pickOrder = i;
    }
  }
  CHECK_MSG(pick == &fogs[1], "hint: fog, the room's over the master's");
  PortRoomEnv::BacklightHint b0, b1;
  b0.master = true;
  b0.priority = 90;
  b1.priority = 50;
  CHECK_MSG(PortRoomEnv::HintOutranks(b1, 0, &b0, 1), "hint: backlight, the room's over the master's");
}

void TestWorldVolumes() {
  using PortRoomEnv::WorldVolume;
  using Event = PortRoomEnv::WorldVolumeState::Event;
  WorldVolume a;
  a.half[0] = a.half[1] = a.half[2] = 1.f;
  // At x = 10, turned a quarter about z: 2 m along world y, 1 m along world x.
  WorldVolume b;
  b.centre[0] = 10.f;
  b.half[0] = 2.f;
  b.half[1] = b.half[2] = 1.f;
  const float axes[9] = {0.f, 1.f, 0.f, -1.f, 0.f, 0.f, 0.f, 0.f, 1.f};
  std::copy(axes, axes + 9, b.axes);
  PortRoomEnv::File file;
  std::string error;
  const std::vector<uint8_t> good = MakeVolumeFile({a, b});
  {
    auto v27 = MakeVolumeFile({});
    v27[4] = 27;
    v27.resize(v27.size() - 8); // replace the empty hint and volume lists
    Put32(v27, 2);
    for (int i = 0; i < 2; ++i) {
      v27.insert(v27.end(), {0, 0, 0, uint8_t(i == 0)}); // plain, ordinary, active/inactive
      for (float f : {5.75f, 6.f, 0.f, 32.f, 0.5f}) PutFloat(v27, f);
      Put32(v27, uint32_t(i == 0 ? -2 : 6));
    }
    Put32(v27, 0); // world volumes
    for (float f : {0.f, 0.f, 0.f, 1.f}) PutFloat(v27, f);
    CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v27), file, error), "hint v27 record stride");
    CHECK(file.fluidHints.size() == 2);
    CHECK(file.fluidHints[0].mode == 0 && file.fluidHints[0].on && file.fluidHints[0].priority == -2);
    CHECK(!file.fluidHints[1].on && file.fluidHints[1].priority == 6);
    CHECK(file.fluidHints[1].exposure[0] == 5.75f && file.envFxIntensity == 1.f);
    v27.resize(v27.size() - 20); // truncate inside the second priority
    CHECK(!PortRoomEnv::Parse(std::move(v27), file, error));
  }
  {
    // Version 28: each hint ends with the script links that (de)activate its entity.
    auto v28 = MakeVolumeFile({});
    v28[4] = 28;
    v28.resize(v28.size() - 8);
    Put32(v28, 2);
    for (int i = 0; i < 2; ++i) {
      v28.insert(v28.end(), {0, 0, 0, 0}); // both start inactive
      for (float f : {5.75f, 6.f, 0.f, 32.f, 0.5f}) PutFloat(v28, f);
      Put32(v28, uint32_t(i + 1));
      Put32(v28, 1);
      Put32(v28, 0x172);
      v28.insert(v28.end(), {0, uint8_t(i == 0 ? PortRoomGeo::kShow : PortRoomGeo::kToggle), 0, 0});
    }
    Put32(v28, 0);
    for (float f : {0.f, 0.f, 0.f, 1.f}) PutFloat(v28, f);
    CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v28), file, error), "hint v28 links");
    CHECK(file.fluidHints.size() == 2 && file.fluidHints[1].priority == 2);
    CHECK(file.fluidHints[0].links.size() == 1 && file.fluidHints[0].links[0].sender == 0x172 &&
          file.fluidHints[0].links[0].state == 0 && file.fluidHints[0].links[0].action == PortRoomGeo::kShow);
    PortRoomEnv::ExposureHints hints;
    hints.Load(file.fluidHints, false);
    CHECK_MSG(hints.Pick(2) < 0, "hint links: nothing on before the sender fires");
    CHECK_MSG(!hints.Drive(file.fluidHints, 0x172, 1), "hint links: another state drives nothing");
    CHECK_MSG(hints.Drive(file.fluidHints, 0x172, 0) && hints.Pick(2) == 1, "hint links: show and toggle on");
    CHECK_MSG(hints.Drive(file.fluidHints, 0x172, 0) && hints.Pick(2) == 0, "hint links: toggle off again");
    v28.resize(v28.size() - 24); // inside the second hint's link
    CHECK(!PortRoomEnv::Parse(std::move(v28), file, error));
  }
  CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(good), file, error), "volumes: parse");
  CHECK_MSG(file.worldVolumes.size() == 2 && PortRoomEnv::SameWorldVolumes(file.worldVolumes, {a, b}),
            "volumes: read back");
  CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(good.begin(), good.end() - 4), file, error),
            "volumes: cut short");
  CHECK_MSG(file.worldShift[0] == 0.f && file.worldShift[2] == 0.f, "volumes: version 22 has no world shift");
  // Version 23 adds the world shift at the end.
  std::vector<uint8_t> v23 = good;
  v23[4] = 23;
  CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(v23), file, error), "shift: cut short");
  for (float f : {1.5f, -2.f, 30.f}) {
    PutFloat(v23, f);
  }
  CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v23), file, error) && file.worldShift[0] == 1.5f &&
                file.worldShift[1] == -2.f && file.worldShift[2] == 30.f && file.worldVolumes.size() == 2,
            "shift: version 23 read back");
  // Version 26 adds the EnvironmentFX colour intensity after the shift; before it, 1.
  CHECK_MSG(file.envFxIntensity == 1.f, "envfx: version 23 has the default intensity");
  std::vector<uint8_t> v26 = v23;
  v26[4] = 26;
  CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(v26), file, error), "envfx: cut short");
  PutFloat(v26, 5.f);
  CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v26), file, error) && file.envFxIntensity == 5.f &&
                file.worldShift[2] == 30.f,
            "envfx: version 26 read back");
  v26.resize(v26.size() - 4);
  PutFloat(v26, std::numeric_limits<float>::infinity());
  CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v26), file, error) && file.envFxIntensity == 1.f,
            "envfx: a bad intensity is the default");
  {
    // Version 30 adds the room's own trigger volumes at the end.
    CHECK_MSG(file.roomVolumes.empty(), "room volumes: none before version 30");
    std::vector<uint8_t> v30 = v26;
    v30[4] = 30;
    CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(v30), file, error), "room volumes: cut short");
    Put32(v30, 2);
    for (int k = 0; k < 2; ++k) {
      // b's box (turned a quarter about z), then one that reads wrong.
      for (float f : b.centre) PutFloat(v30, f);
      for (float f : b.half) PutFloat(v30, k == 0 ? f : std::numeric_limits<float>::quiet_NaN());
      for (float f : b.axes) PutFloat(v30, f);
      v30.insert(v30.end(), {uint8_t(k == 0 ? 3 : 1), 0, 0, 0});
    }
    CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(v30.begin(), v30.end() - 4), file, error),
              "room volumes: entry cut short");
    CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v30), file, error) && file.roomVolumes.size() == 2 &&
                  file.envFxIntensity == 1.f && file.worldVolumes.size() == 2,
              "room volumes: version 30 read back");
    using PortRoomEnv::RoomVolume;
    const RoomVolume& r = file.roomVolumes[0];
    CHECK_MSG(r.detect == (PortRoomEnv::kRoomVolumePlayer | PortRoomEnv::kRoomVolumeCamera) && r.centre[0] == 10.f &&
                  r.half[0] == 2.f && r.axes[1] == 1.f,
              "room volumes: fields");
    const float in[3] = {10.f, 1.5f, 0.f}, out[3] = {11.5f, 0.f, 0.f}, far[3] = {50.f, 50.f, 50.f};
    // The box spans x 9..11, y -2..2, z -1..1 in the area's axes.
    const float touchLo[3] = {10.8f, -0.5f, -0.5f}, touchHi[3] = {12.f, 0.5f, 0.5f};
    const float awayLo[3] = {11.2f, -0.5f, -0.5f}, awayHi[3] = {12.f, 0.5f, 0.5f};
    CHECK_MSG(PortRoomEnv::InsideRoomVolume(r, in, nullptr, nullptr, false), "room volumes: camera inside");
    CHECK_MSG(!PortRoomEnv::InsideRoomVolume(r, out, nullptr, nullptr, false), "room volumes: camera outside");
    CHECK_MSG(PortRoomEnv::InsideRoomVolume(r, far, touchLo, touchHi, true), "room volumes: player bounds overlap");
    CHECK_MSG(!PortRoomEnv::InsideRoomVolume(r, far, awayLo, awayHi, true), "room volumes: player bounds apart");
    CHECK_MSG(!PortRoomEnv::InsideRoomVolume(r, far, touchLo, touchHi, false), "room volumes: no player");
    RoomVolume cameraOnly = r;
    cameraOnly.detect = PortRoomEnv::kRoomVolumeCamera;
    CHECK_MSG(!PortRoomEnv::InsideRoomVolume(cameraOnly, far, touchLo, touchHi, true),
              "room volumes: a camera trigger ignores the player");
    RoomVolume playerOnly = r;
    playerOnly.detect = PortRoomEnv::kRoomVolumePlayer;
    CHECK_MSG(!PortRoomEnv::InsideRoomVolume(playerOnly, in, awayLo, awayHi, true),
              "room volumes: a player trigger ignores the camera");
    const RoomVolume& bad = file.roomVolumes[1];
    CHECK_MSG(bad.half[0] < 0.f && !PortRoomEnv::InsideRoomVolume(bad, in, touchLo, touchHi, true),
              "room volumes: a bad box detects nothing");
    CHECK_MSG(file.worldVolumes[0].detect == PortRoomEnv::kRoomVolumeCamera && file.envVars.empty() &&
                  file.envOps.empty() && file.envBranches.empty(),
              "env vars: before version 31 world volumes see the camera, and no variables");

    // Version 31: what each world volume detects, then the world's variables, ops and branches.
    std::vector<uint8_t> v31 = v30;
    v31[4] = 31;
    CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(v31), file, error), "env vars: cut short");
    Put32(v31, 2);
    v31.insert(v31.end(), {PortRoomEnv::kRoomVolumePlayer, 0xff});
    Put32(v31, 1);
    std::array<uint8_t, 16> guid{};
    for (size_t i = 0; i < guid.size(); ++i) guid[i] = uint8_t(0x35 + i);
    v31.insert(v31.end(), guid.begin(), guid.end());
    const std::string name = "ZIM_Fog";
    Put32(v31, uint32_t(name.size()));
    v31.insert(v31.end(), name.begin(), name.end());
    for (uint32_t f : {0u, 3u, 0u, 3u}) Put32(v31, f); // value, flags, min, max
    Put32(v31, 1);
    Put32(v31, PortRoomEnv::kSenderWorldVolume + 1);
    v31.insert(v31.end(), {1, PortRoomEnv::kEnvAdd, 0, 0});
    Put32(v31, uint32_t(-2));
    v31.insert(v31.end(), guid.begin(), guid.end());
    Put32(v31, 3);
    for (int k = 0; k < 3; ++k) {
      v31.insert(v31.end(), guid.begin(), guid.end());
      v31.insert(v31.end(), {uint8_t(k), uint8_t(PortRoomEnv::kEnvOnLoad | PortRoomEnv::kEnvSkipUnchanged), 0, 0});
      Put32(v31, uint32_t(k));
      Put32(v31, 39);
    }
    CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(v31.begin(), v31.end() - 4), file, error),
              "env vars: branch cut short");
    CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v31), file, error) && file.roomVolumes.size() == 2 &&
                  file.worldVolumes.size() == 2,
              "env vars: version 31 read back");
    CHECK_MSG(file.worldVolumes[0].detect == PortRoomEnv::kRoomVolumePlayer &&
                  file.worldVolumes[1].detect == (PortRoomEnv::kRoomVolumePlayer | PortRoomEnv::kRoomVolumeCamera),
              "env vars: world volume detect, unknown bits dropped");
    CHECK_MSG(file.envVars.size() == 1 && file.envVars[0].guid == guid && file.envVars[0].name == name &&
                  file.envVars[0].value == 0 && file.envVars[0].flags == 3 && file.envVars[0].max == 3,
              "env vars: variable");
    CHECK_MSG(file.envOps.size() == 1 && file.envOps[0].sender == PortRoomEnv::kSenderWorldVolume + 1 &&
                  file.envOps[0].state == 1 && file.envOps[0].op == PortRoomEnv::kEnvAdd &&
                  file.envOps[0].value == -2 && file.envOps[0].var == guid,
              "env vars: op");
    CHECK_MSG(file.envBranches.size() == 3 && file.envBranches[2].relop == 2 && file.envBranches[2].value == 2 &&
                  file.envBranches[2].query == 39 && file.envBranches[1].var == guid &&
                  file.envBranches[0].flags == (PortRoomEnv::kEnvOnLoad | PortRoomEnv::kEnvSkipUnchanged),
              "env vars: branches");
    std::vector<uint8_t> wrongCount = v30;
    wrongCount[4] = 31;
    Put32(wrongCount, 1); // not the file's two world volumes
    wrongCount.push_back(1);
    for (int k = 0; k < 3; ++k) Put32(wrongCount, 0);
    CHECK_MSG(!PortRoomEnv::Parse(std::move(wrongCount), file, error), "env vars: detect count must match");
    // Version 32 adds to each volume whether the trigger is active at load, and the script
    // that switches it (its own links); the version 31 section follows the volumes.
    std::vector<uint8_t> v32 = v26;
    v32[4] = 32;
    Put32(v32, 2);
    for (int k = 0; k < 2; ++k) {
      for (float f : b.centre) PutFloat(v32, f);
      for (float f : b.half) PutFloat(v32, k == 0 ? f : std::numeric_limits<float>::quiet_NaN());
      for (float f : b.axes) PutFloat(v32, f);
      v32.insert(v32.end(), {uint8_t(k == 0 ? 3 : 1), 0, 0, 0});
      // The first starts off, switched on by 0x172 state 2 and toggled by 0x173 state 0; the
      // second is on at load with nothing switching it.
      v32.insert(v32.end(), {uint8_t(k == 0 ? 0 : 1), 0, 0, 0});
      Put32(v32, k == 0 ? 2 : 0);
      if (k == 0) {
        Put32(v32, 0x172);
        v32.insert(v32.end(), {2, uint8_t(PortRoomGeo::kShow), 0, 0});
        Put32(v32, 0x173);
        v32.insert(v32.end(), {0, uint8_t(PortRoomGeo::kToggle), 0, 0});
      }
    }
    Put32(v32, 2);                      // version 31's section: what the two world volumes detect
    v32.insert(v32.end(), {1, 1});      // then no variables, ops or branches
    for (uint32_t empty : {0u, 0u, 0u}) Put32(v32, empty);
    const size_t inTail = v26.size() + 4 + 64 + 4;  // inside the first volume's version 32 tail
    CHECK_MSG(!PortRoomEnv::Parse(std::vector<uint8_t>(v32.begin(), v32.begin() + inTail), file, error),
              "room volumes: version 32 tail cut short");
    CHECK_MSG(PortRoomEnv::Parse(std::vector<uint8_t>(v32), file, error) && file.roomVolumes.size() == 2 &&
                  file.envBranches.empty(),
              "room volumes: version 32 read back");
    const RoomVolume& off = file.roomVolumes[0];
    CHECK_MSG(off.detect == (PortRoomEnv::kRoomVolumePlayer | PortRoomEnv::kRoomVolumeCamera) && !off.on &&
                  off.links.size() == 2 && off.links[0].sender == 0x172 && off.links[0].state == 2 &&
                  off.links[0].action == PortRoomGeo::kShow && off.links[1].sender == 0x173 &&
                  off.links[1].state == 0 && off.links[1].action == PortRoomGeo::kToggle,
              "room volumes: version 32 off at load with the links that switch it");
    CHECK_MSG(file.roomVolumes[1].on && file.roomVolumes[1].links.empty(),
              "room volumes: version 32 a trigger on at load has no links");
  }
  const float inB[3] = {10.f, 1.5f, 0.f}, outB[3] = {11.5f, 0.f, 0.f};
  CHECK_MSG(PortRoomEnv::InsideWorldVolume(b, inB) && !PortRoomEnv::InsideWorldVolume(b, outB), "volumes: turned box");

  const std::vector<WorldVolume> table{a, b};
  PortRoomEnv::WorldVolumeState state;
  std::vector<Event> events;
  const auto same = [](const std::vector<Event>& got, std::initializer_list<Event> want) {
    return got.size() == want.size() && std::equal(got.begin(), got.end(), want.begin(), [](const Event& x, const Event& y) {
             return x.volume == y.volume && x.state == y.state;
           });
  };
  const float atA[3] = {0.f, 0.f, 0.f}, atB[3] = {10.f, 0.f, 0.f};
  state.Update(table, atA, events);
  CHECK_MSG(same(events, {{0, 0}}), "volumes: starting inside enters");
  events.clear();
  state.Update(table, atA, events);
  CHECK_MSG(events.empty(), "volumes: staying");
  state.Update(table, atB, events);
  CHECK_MSG(same(events, {{0, 1}, {1, 0}}), "volumes: crossing");
  events.clear();
  state.Update(table, atA, events);
  CHECK_MSG(same(events, {{0, 0}, {1, 1}}), "volumes: back");
  events.clear();
  // b entered (3) and left (5), a entered last (4): a late area hears them in that order.
  state.Replay(events);
  CHECK_MSG(same(events, {{1, 0}, {0, 0}, {1, 1}}), "volumes: replay order");
  events.clear();
  state.Update({a}, atA, events);
  CHECK_MSG(same(events, {{0, 0}}), "volumes: a new table starts outside");
  events.clear();
  state.Clear();
  state.Replay(events);
  CHECK_MSG(events.empty(), "volumes: cleared");
}
// CScriptEnvironmentVarQuery / CScriptEnvironmentVarModifier: the criteria relations, and the
// modifier's set/add/sub clamped by the variable's range.
void TestEnvVars() {
  using PortRoomEnv::EnvBranch;
  using PortRoomEnv::EnvVar;
  const auto holds = [](uint8_t relop, int32_t criteria, int32_t value) {
    EnvBranch b;
    b.relop = relop;
    b.value = criteria;
    return PortRoomEnv::EnvBranchHolds(b, value);
  };
  CHECK_MSG(holds(0, 1, 1) && !holds(0, 1, 2), "env vars: relop 0 ==");
  CHECK_MSG(holds(1, 1, 2) && !holds(1, 1, 1), "env vars: relop 1 !=");
  CHECK_MSG(holds(2, 1, 2) && !holds(2, 1, 1) && !holds(2, 2, 1), "env vars: relop 2 criteria < var");
  CHECK_MSG(holds(3, 1, 1) && holds(3, 1, 2) && !holds(3, 2, 1), "env vars: relop 3 criteria <= var");
  CHECK_MSG(holds(4, 2, 1) && !holds(4, 1, 1) && !holds(4, 1, 2), "env vars: relop 4 var < criteria");
  CHECK_MSG(holds(5, 1, 1) && holds(5, 2, 1) && !holds(5, 1, 2), "env vars: relop 5 var <= criteria");
  CHECK_MSG(!holds(6, 1, 1), "env vars: an unknown relop never holds");

  EnvVar free;
  CHECK_MSG(PortRoomEnv::ApplyEnvOp(free, 5, PortRoomEnv::kEnvSet, -3) == -3, "env vars: set");
  CHECK_MSG(PortRoomEnv::ApplyEnvOp(free, 5, PortRoomEnv::kEnvAdd, 2) == 7, "env vars: add");
  CHECK_MSG(PortRoomEnv::ApplyEnvOp(free, 5, PortRoomEnv::kEnvSub, 7) == -2, "env vars: sub, unclamped");
  CHECK_MSG(PortRoomEnv::ApplyEnvOp(free, INT32_MAX, PortRoomEnv::kEnvAdd, 1) == INT32_MAX,
            "env vars: no wrap past int32");
  CHECK_MSG(PortRoomEnv::ApplyEnvOp(free, 5, 9, 1) == 5, "env vars: an unknown op keeps the value");
  EnvVar fog; // Intro's ZIM_Fog: 0..3
  fog.flags = PortRoomEnv::kEnvVarMin | PortRoomEnv::kEnvVarMax;
  fog.max = 3;
  CHECK_MSG(PortRoomEnv::ApplyEnvOp(fog, 2, PortRoomEnv::kEnvAdd, 5) == 3, "env vars: clamped to max");
  CHECK_MSG(PortRoomEnv::ApplyEnvOp(fog, 1, PortRoomEnv::kEnvSub, 4) == 0, "env vars: clamped to min");
  EnvVar low = fog;
  low.flags = PortRoomEnv::kEnvVarMin; // min only
  CHECK_MSG(PortRoomEnv::ApplyEnvOp(low, 1, PortRoomEnv::kEnvAdd, 9) == 10 &&
                PortRoomEnv::ApplyEnvOp(low, 1, PortRoomEnv::kEnvSub, 9) == 0,
            "env vars: one bound only");

  // Tallon Intro's ZIM_Fog query: one criteria per value, all onLoad. At load the variable is
  // 0, so only the value-0 branches hold.
  std::vector<EnvBranch> intro;
  for (int32_t value : {0, 0, 0, 1, 1, 1, 2, 2}) {
    EnvBranch b;
    b.value = value;
    b.flags = PortRoomEnv::kEnvOnLoad | PortRoomEnv::kEnvOnChange | PortRoomEnv::kEnvSkipUnchanged;
    intro.push_back(b);
  }
  size_t held = 0;
  for (size_t i = 0; i < intro.size(); ++i) {
    if ((intro[i].flags & PortRoomEnv::kEnvOnLoad) != 0 && PortRoomEnv::EnvBranchHolds(intro[i], 0)) {
      CHECK_MSG(intro[i].value == 0, "env vars: onLoad at 0 holds only value 0");
      ++held;
    }
  }
  CHECK_MSG(held == 3, "env vars: onLoad at 0 fires the three value-0 branches");
}

// CPostFXManager::UpdateTonemapping 0xaffb18: L from the readback, and the start.
void TestExposureAverage() {
  constexpr float kGrey = 0.2158605f;
  PortRoomEnv::ExposureAverage e;
  float target = 0.f;
  CHECK_MSG(e.L == -1.f && !e.created, "exposure average: starts without a measurement");
  CHECK_MSG(e.Update(true, 1.5f, -4.f, 8.f, 0.f, 32.f, false, 1, &target) == 1.5f && target == 1.5f && !e.created,
            "exposure average: the tonemap EV before the first L");
  const float rgb[3] = {0.1f, 4.f * kGrey, 0.3f};
  e.Take(rgb);
  CHECK_MSG(e.L == 4.f * kGrey, "exposure average: L is the largest channel");
  CHECK_MSG(e.Update(false, 1.5f, -4.f, 8.f, 0.f, 32.f, false, 1, &target) == 1.5f && !e.created,
            "exposure average: no hint, the tonemap EV and no Gaussian");
  // log2(4) + 3 + 0.5 = 5.5
  CHECK_MSG(Near(e.Update(true, 1.5f, -4.f, 8.f, 0.5f, 32.f, false, 1, &target), 5.5f) && Near(target, 5.5f) &&
                e.created,
            "exposure average: the Gaussian starts at the first target");
  const float bright[3] = {64.f * kGrey, 0.f, 0.f}; // log2(64) + 3 = 9, past hi
  e.Take(bright);
  const float before = e.ev.value;
  const float stepped = e.Update(true, 1.5f, -4.f, 8.f, 0.f, 32.f, false, 10, &target);
  CHECK_MSG(target == 8.f && stepped > before && stepped < 8.f, "exposure average: clamped to hi, steps towards it");
  CHECK_MSG(e.Update(true, 1.5f, -4.f, 8.f, 0.f, 32.f, true, 0, &target) == 8.f, "exposure average: a snap sets it");
  const float black[3] = {0.f, 0.f, 0.f}; // log2(0) = -inf, clamped to lo
  e.Take(black);
  CHECK_MSG(e.Update(true, 1.5f, -4.f, 8.f, 0.f, 32.f, true, 0, &target) == -4.f && target == -4.f,
            "exposure average: L = 0 clamps to lo");
  const float nan[3] = {std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f};
  e.Take(nan);
  CHECK_MSG(e.Update(true, 1.5f, -4.f, 8.f, 0.f, 32.f, false, 1, &target) == 1.5f,
            "exposure average: a NaN L fails L >= 0");
  e.Reset();
  CHECK_MSG(!e.created && e.L == -1.f, "exposure average: reset");
  e.Take(rgb);
  CHECK_MSG(Near(e.Update(true, 1.5f, -4.f, 8.f, 0.f, 32.f, false, 5, &target), 5.f) && e.created,
            "exposure average: after a reset the Gaussian starts again at L");
}
} // namespace

// What the camera's blend overrides a material's cube with (CMaterialMgr::ResolveReflectionProbe):
// no probe around is a black cube, a lone probe without a cube no override at all.
void TestCameraCube() {
  using PortRoomEnv::CameraCube;
  PortRoomEnv::Probe withCube;
  withCube.cube = 0;
  PortRoomEnv::Probe noCube;
  noCube.cube = PortRoomEnv::kProbeNoCube;
  PortRoomEnv::Blend blend;
  CHECK_MSG(PortRoomEnv::CameraCubeState(blend) == CameraCube::Black, "camera cube: empty is black");
  blend.entries.push_back({1, &withCube, 1.f});
  CHECK_MSG(PortRoomEnv::CameraCubeState(blend) == CameraCube::Probe, "camera cube: lone probe");
  blend.entries[0].probe = &noCube;
  CHECK_MSG(PortRoomEnv::CameraCubeState(blend) == CameraCube::NoOverride, "camera cube: lone probe without a cube");
  blend.entries[0].probe = nullptr;
  CHECK_MSG(PortRoomEnv::CameraCubeState(blend) == CameraCube::NoOverride, "camera cube: null result");
  blend.entries[0].probe = &noCube;
  blend.entries.push_back({2, &withCube, 0.5f});
  CHECK_MSG(PortRoomEnv::CameraCubeState(blend) == CameraCube::Probe, "camera cube: two probes");
}

int main() {
  TestCameraCube();
  TestWorldVolumes();
  TestEnvVars();
  TestTileSection();
  TestBrdfLut();
  TestPowerBomb();
  TestNames();
  TestParse();
  TestPick();
  TestPickExact();
  TestBlend();
  TestGrid();
  TestLod();
  TestExposureHints();
  TestHintOutranks();
  TestExposureAverage();
  if (port_test::Failures() == 0) {
    std::puts("port_room_env tests passed");
  }
  return PORT_TEST_RESULT();
}
