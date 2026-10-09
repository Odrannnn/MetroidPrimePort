// Room environments at run time: which areas have one, their cubes on the GPU, and the
// cube and ambient for a model. See port_room_env.h.
#include "port_debug.h"
#include "port_env.h"
#include "port_room_env.h"
#include "port_strings.h"

#include "port_gci.h"
#include "port_log.h"
#include "port_mods.h"
#include "port_remastered_txtr.h"
#include "port_room_env_lod.h"
#include "port_room_geo.h"

#include <Kyoto/CRandom16.hpp>
#include <dolphin/gx/GXExtra.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace PortRoomEnv {
namespace {

struct GpuCube {
  uint32_t id = 0;      // 0: not made yet (the worker is decoding it), or it failed
  bool failed = false;
  float average = 0.f;  // luminance over every direction, as stored
  float peak = 0.f;     // the largest channel of the colour over every direction
  uint32_t mipCount = 0;
};

struct GpuVolume {
  uint32_t id = 0; // 0: not made yet (the worker is filling it in)
};

struct Area {
  File file;
  bool hasFile = false; // the file was read
  uint32_t serial = 0;  // of this load, which the worker's jobs for it carry
  std::vector<GpuCube> cubes;
  std::vector<GpuVolume> volumes; // one a grid
  uint32_t lightmap = 0; // GXCreatePBRLightmap's id; 0 until the worker has made it, or none
  float exposure = 0.f; // what takes the room's radiance to the display's range; 0: unknown
  float tone[3][4] = {}; // its tone curve
  bool hasGeo = false;   // the mod replaces its geometry
  uint64_t layers = ~uint64_t(0); // its active script layers (SetAreaLayers)
  // Its grades' requests (Remastered's CAreaPrioritizedGameHintManager): on or off, and
  // when they were last turned on, which decides between equal priorities.
  struct GradeRequest {
    bool on = false;
    uint64_t order = 0;
  };
  std::vector<GradeRequest> grades;
  std::vector<GradeRequest> backlights; // likewise
  std::vector<GradeRequest> fogs;       // likewise
  std::vector<GradeRequest> regions;    // the fog regions' activation (only `on` matters)
  std::vector<FogRegion> live;          // the fog regions as their transitions leave them
  // A fog region transition's playback (CVolumetricFogRegionTransitionGOC), and the region's
  // state when it last started.
  struct Transition {
    bool active = false;
    bool playing = false;
    bool dead = false;
    double time = 0.;
    bool hasColor = false;
    bool hasCap = false;
    bool hasRegion = false;
    float distance = 0.f;
    float transmittance = 0.f;
    float color[4] = {};
    float cap = 0.f;
  };
  std::vector<Transition> transitions;
  // A point or spot light's state (CLightDynamicGOC): active, and its timeline.
  struct Light {
    bool active = false;
    bool playing = false;
    bool forward = true;
    double time = 0.;
    int group = -1; // what its script group last said (PortRoomGeo::GroupShown)
  };
  std::vector<Light> lights;
};

// The frame's exposure and tone curve, as CPostFXManager::UpdateTonemapping moves them.
struct FrameState {
  bool started = false;
  uint32_t area = 0; // the camera's room last frame
  std::vector<uint32_t> loaded; // the areas in memory last frame
  Convergence ev;
  float sigma = -1.f;
  float targetEv = 0.f;
  bool measuring = false; // this frame is measured (MeasureExposure)
  bool measured = false;  // targetEv is a measurement
  float measuredEv = 0.f;
  uint32_t serial = 0;      // of the last measurement seen
  uint32_t ignoreUntil = 0; // measurements up to here are of before a jump
  // The tonemap's EV, mid, contrast, toe and shoulder, and the static EV (see GlowScale),
  // moving linearly from `from` to `to`.
  float from[6] = {};
  float to[6] = {};
  float shown[6] = {};
  std::chrono::steady_clock::time_point start;
  std::chrono::steady_clock::time_point last;
  float carry = 0.f; // seconds not stepped yet
  float exposure = 0.f; // 2^(3 - EV); 0: none yet
  bool hasTone = false;
  float tone[3][4] = {};
};
FrameState sFrame;
std::vector<uint32_t> sLoadedSpare; // FrameState::loaded of the frame before last
int sAuto = -1;
int sStatic = -1;
int sAreaLights = -1;

// Areas in memory; one without a file has an empty File.
std::unordered_map<uint32_t, Area> sAreas;
uint32_t sNextSerial = 1;

// Work kept off the render thread: decoding a cube's BC6H into the RGBA16F that
// GXCreatePBRCube takes, and filling a grid's empty points in for GXCreatePBRVolume. Load
// queues every cube and volume of an area as it reads the file; one thread works through
// them in order, and UpdateFrame hands what is done to the GPU, one a frame. Last comes
// Keep: a copy of the parts of the file still read once the cubes are made, which takes
// the file's place, so the cubes' blocks (most of it) are freed. A job reads its area's
// File, which stays as it is until the job is done: Free cancels the area's jobs, and
// waits for the one running to stop, before the file goes, and Keep is the area's last.
// Defined after sAreas so that it is destroyed, and its thread joined, first.
enum class JobKind { Cube, Volume, Lightmap, Keep };

struct Job {
  JobKind kind = JobKind::Cube;
  uint32_t area = 0;
  uint32_t serial = 0; // Area::serial
  size_t index = 0;    // of the cube or grid in the file
  const File* file = nullptr;
  bool raw = false; // Lightmap: the device takes its BC6H blocks as they are
};

struct Result {
  JobKind kind = JobKind::Cube;
  uint32_t area = 0;
  uint32_t serial = 0;
  size_t index = 0;
  std::vector<uint16_t> cube;  // RGBA16F, as GXCreatePBRCube takes it
  std::vector<uint8_t> volume; // as GXCreatePBRVolume takes it, or Keep's bytes
  std::vector<size_t> offsets; // Keep: where each grid's points, then each grade's LUT, are
  // Lightmap: its size, and whether `volume` holds its BC6H blocks or `cube` its RGBA16F.
  uint32_t width = 0;
  uint32_t height = 0;
  bool raw = false;
};

struct Worker {
  // Decoded cubes waiting for the GPU; the thread waits while this many are.
  static constexpr size_t kMaxResults = 2;

  ~Worker();
  void Submit(const Job& job);
  // Drops the jobs and results of a load, and waits for its job that is running to stop.
  void Cancel(uint32_t serial);
  // The oldest result, if there is one; a cube or volume only when `gpu`.
  bool Take(Result& out, bool gpu);

  void Run();
  // False when cancelled.
  bool Do(const Job& job, Result& out);

  std::mutex mutex;
  std::condition_variable wake; // the thread: a job, room for its result, or stop
  std::condition_variable done; // Cancel: the running job ended
  std::deque<Job> jobs;
  std::deque<Result> results;
  bool running = false;
  uint32_t runningSerial = 0;
  std::atomic<bool> cancel{false};
  bool stop = false;
  bool noThread = false; // the thread could not start: jobs run in Submit
  std::thread thread;
};
Worker sWorker;

uint32_t sNextCube = 1;
uint32_t sNextVolume = 1;
int sVolumes = -1;
int sBombTint = -1;             // MP_REMASTERED_BOMB_TINT
float sPowerBombTime = -1.f;    // SetPowerBombTime
float sBakedLight[3] = {1.f, 1.f, 1.f}; // BakedLightModulation
float sAmbientScale = -1.f;
float sVolumeView = -1.f;
bool sHint = false;
bool sBrdfSent = false; // the mods' brdf.lut, or the lack of one, is with Aurora
uint32_t sHintArea = 0;
float sHintCentre[3];
float sHintLightmap[3] = {}; // SetLightmapHint; scale 0: none
int sEnabled = -1;
int sExposure = -1;
int sBloom = -1;
uint32_t sViewArea = 0;
int sGrade = -1;
// The colour grade on screen: fading from one LUT to another (0 is the identity).
struct GradeFade {
  bool started = false;
  uint32_t from = 0;
  uint32_t to = 0;
  float seconds = 0.f; // how long the fade to `to` takes
  float fadeOut = 0.f; // of `to`, for when the next room has no grade
  std::chrono::steady_clock::time_point start;
};
GradeFade sGradeFade;
// The grade on screen: its area and index, to tell a grade turned off from one outranked.
uint32_t sGradeArea = 0;
int sGradeIndex = -1;
uint64_t sGradeOrder = 0;
// The character backlight's strengths (NRenderDebugDefaults::skBacklightTop and Back when no
// hint is picked), moving linearly from `from` to `to` over `seconds`.
constexpr float kBacklightTop = 2.f;
constexpr float kBacklightBack = 4.f;
struct BacklightFade {
  bool started = false;
  bool hinted = false; // a hint is picked
  uint32_t area = 0;   // of the hint, and its index
  int index = -1;
  float fadeOut = 0.f; // of the hint
  float from[2] = {kBacklightTop, kBacklightBack};
  float to[2] = {kBacklightTop, kBacklightBack};
  float seconds = 0.f;
  std::chrono::steady_clock::time_point start;
};
BacklightFade sBacklight;
uint64_t sBacklightOrder = 0;
// The volumetric fog (Remastered's CVolumetricFogManager::UpdateSceneNode). The node shows
// `out`; a hint change starts an interpolation from it to the target, whose phase is the
// fade's spline at the seconds since the change (SVolumetricFogDynamicData::
// interpolate_by_phase). The defaults are SVolumetricFogDynamicData's constructor's.
struct FogCore {
  float range = 0.f, scatter = 0.f, absorb = 0.f, m1z = 0.f, decay = 0.f;
  float attenSlope = 0.f, attenBias = 1.f;
  float noiseFreq = 0.f, noiseStrength = 0.f, lightCap = 65535.f;
  float wind[3] = {};
  float colorB[4] = {1.f, 1.f, 1.f, 1.f}, colorA[3] = {1.f, 1.f, 1.f};
  bool noProbe = false;
  float lut[64] = {};
};
struct FogFade {
  bool hinted = false; // a hint is picked
  uint32_t area = 0;   // of the hint, and its index
  int index = -1;
  // The picked hint's fade-out, kept for when it goes away.
  bool hasFadeOut = false;
  bool linearFadeOut = false;
  float fadeOutSeconds = 0.f;
  PortMayaSpline fadeOut;
  // The running interpolation, if any: a spline, or (older files) linear over `seconds`.
  bool fading = false;
  bool linear = false;
  float seconds = 0.f;
  PortMayaSpline spline;
  float elapsed = 0.f;
  FogCore from, out;
  bool active = false; // a hint is picked or an interpolation runs
  float time = 0.f;    // since the node was made; the noise moves by wind * time
  uint32_t frames = 0; // updates since the state manager was made
};
FogFade sFog;
uint64_t sFogOrder = 0;
uint64_t sRegionOrder = 0;
int sVolFog = -1;
// Console `roomenv fogregions on|off`: drop the fog regions to see what they add.
bool sFogRegions = true;
bool sPlayerFluid = false;
bool sCameraWater = false;
// LUTs handed to Aurora already; they are kept there for the run.
std::unordered_set<uint32_t> sGradeLuts;
// Model draws come in runs at one position; the last answer is kept until the frame or a
// setting moves on.
bool sLastValid = false;
bool sLastFound = false;
Selection sLast;
// Counts the changes to what Select finds for a point (see Invalidate).
uint32_t sEpoch = 1;

// The areas, their cubes or volumes on the GPU, or a setting that picks among them changed:
// every point Select has looked from is looked at again.
void Invalidate() {
  sLastValid = false;
  ++sEpoch;
}

// The reflection probes blended around the camera (UpdateBlend), and the cube Aurora draws
// them into. That cube is bound by its id, and a draw only binds a new one when the id
// changes, so a new blend goes into the other of two ids.
struct ProbeBlendState {
  int enabled = -1;
  bool hasView = false;
  float view[3] = {};
  Blend blend;
  uint32_t dst[2] = {};
  int shown = -1; // which dst holds `keys`/`weights`; -1: none
  uint64_t keys[kMaxBlend] = {};
  float weights[kMaxBlend] = {};
  size_t count = 0;
};
ProbeBlendState sBlend;

float HalfToFloat(uint16_t h) {
  const int exponent = (h >> 10) & 0x1F;
  const int mantissa = h & 0x3FF;
  float value;
  if (exponent == 0) {
    value = std::ldexp(float(mantissa), -24);
  } else if (exponent == 31) {
    value = mantissa == 0 ? 65504.f : 0.f; // infinity is clamped, a NaN is dropped
  } else {
    value = std::ldexp(float(mantissa | 0x400), exponent - 25);
  }
  return (h & 0x8000) != 0 ? -value : value;
}

void Free(Area& area) {
  // Before the file goes: the worker may be reading it.
  sWorker.Cancel(area.serial);
  for (GpuCube& cube : area.cubes) {
    if (cube.id != 0) {
      GXDestroyPBRCube(cube.id);
    }
  }
  area.cubes.clear();
  for (GpuVolume& volume : area.volumes) {
    if (volume.id != 0) {
      GXDestroyPBRVolume(volume.id);
    }
  }
  area.volumes.clear();
  if (area.lightmap != 0) {
    GXDestroyPBRLightmap(area.lightmap);
    area.lightmap = 0;
  }
}

uint16_t FloatToHalf(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  const uint16_t sign = uint16_t((bits >> 16) & 0x8000);
  const int exponent = int((bits >> 23) & 0xFF) - 127 + 15;
  const uint32_t mantissa = bits & 0x7FFFFF;
  if (exponent <= 0) {
    return exponent < -10 ? sign : uint16_t(sign | ((mantissa | 0x800000) >> (14 - exponent)));
  }
  if (exponent >= 31) {
    return uint16_t(sign | 0x7BFF); // the largest half, for anything past it
  }
  return uint16_t(sign | (exponent << 10) | (mantissa >> 13));
}

// MP_ROOM_ENV_LIGHTMAP: the rooms' baked lightmaps, which light room geometry in place of
// the coarser volume.
bool Lightmaps() {
  static const bool on = port::EnvFlag("MP_ROOM_ENV_LIGHTMAP", true);
  return on;
}


// A grid as the textures of GXCreatePBRVolume. Remastered samples the grid as it is: the
// empty points (inside walls, or away from the bake) read 0 and the filter blends them in,
// with no fill and no validity flag. MP_ROOM_ENV_FILL_LAYERS=n (for comparisons) lets the
// empty points take the light of their lit neighbours first, n layers deep.
// On the worker; false when cancelled.
bool FillVolume(const File& file, const Grid& grid, std::vector<uint8_t>& texels, const std::atomic<bool>& cancel) {
  constexpr size_t kPoint = 24;
  static const int kLayers = int(port::EnvFloat("MP_ROOM_ENV_FILL_LAYERS", 0.f));
  const size_t sx = grid.size[0], sy = grid.size[1], sz = grid.size[2];
  const size_t count = sx * sy * sz;
  std::vector<uint8_t> points(file.data.begin() + grid.offset, file.data.begin() + grid.offset + count * kPoint);
  const auto lit = [&points](size_t index) {
    const uint8_t* p = points.data() + index * kPoint;
    // Means are not negative, so any set bit but the sign is light.
    return ((p[0] | p[2] | p[4]) != 0) || (((p[1] | p[3] | p[5]) & 0x7F) != 0);
  };
  std::vector<uint8_t> state(count); // 1: lit, 2: filled in this layer
  for (size_t i = 0; i < count; ++i) {
    state[i] = lit(i) ? 1 : 0;
  }
  const size_t step[3] = {1, sx, sx * sy};
  const size_t size[3] = {sx, sy, sz};
  for (int layer = 0; layer < kLayers; ++layer) {
    size_t filled = 0;
    size_t index = 0;
    for (size_t z = 0; z < sz; ++z) {
      if (cancel.load(std::memory_order_relaxed)) {
        return false;
      }
      for (size_t y = 0; y < sy; ++y) {
        for (size_t x = 0; x < sx; ++x, ++index) {
          if (state[index] != 0) {
            continue;
          }
          const size_t at[3] = {x, y, z};
          float halves[6] = {};
          float bytes[12] = {};
          int total = 0;
          for (int axis = 0; axis < 3; ++axis) {
            for (int side = 0; side < 2; ++side) {
              if (side == 0 ? at[axis] == 0 : at[axis] + 1 == size[axis]) {
                continue;
              }
              const size_t other = side == 0 ? index - step[axis] : index + step[axis];
              if (state[other] != 1) {
                continue;
              }
              const uint8_t* p = points.data() + other * kPoint;
              for (int i = 0; i < 6; ++i) {
                halves[i] += HalfToFloat(uint16_t(p[i * 2] | (p[i * 2 + 1] << 8)));
              }
              for (int i = 0; i < 12; ++i) {
                bytes[i] += float(p[12 + i]);
              }
              ++total;
            }
          }
          if (total == 0) {
            continue;
          }
          uint8_t* p = points.data() + index * kPoint;
          for (int i = 0; i < 6; ++i) {
            const uint16_t half = FloatToHalf(halves[i] / float(total));
            p[i * 2] = uint8_t(half);
            p[i * 2 + 1] = uint8_t(half >> 8);
          }
          for (int i = 0; i < 12; ++i) {
            p[12 + i] = uint8_t(bytes[i] / float(total) + 0.5f);
          }
          state[index] = 2;
          ++filled;
        }
      }
    }
    if (filled == 0) {
      break;
    }
    for (uint8_t& value : state) {
      value = value != 0 ? 1 : 0;
    }
  }
  texels.assign(count * 28, 0);
  uint8_t* mean = texels.data();
  uint8_t* lobe = mean + count * 8;
  uint8_t* direction = lobe + count * 8;
  for (size_t i = 0; i < count; ++i) {
    const uint8_t* p = points.data() + i * kPoint;
    std::memcpy(mean + i * 8, p, 6);
    std::memcpy(lobe + i * 8, p + 6, 6);
    mean[i * 8 + 7] = lobe[i * 8 + 7] = 0x3C; // alpha 1.0
    for (int channel = 0; channel < 3; ++channel) {
      uint8_t* out = direction + (count * channel + i) * 4;
      std::memcpy(out, p + 15 + channel * 3, 3);
      out[3] = p[12 + channel];
    }
  }
  return true;
}

// How far outside a grid a point is, in metres; 0 inside.
float GridDistance(const Grid& grid, const float pos[3]) {
  const float* m = grid.worldToGrid;
  const float scale = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
  float sum = 0.f;
  for (int row = 0; row < 3; ++row) {
    const float* r = m + row * 4;
    const float at = r[0] * pos[0] + r[1] * pos[1] + r[2] * pos[2] + r[3];
    const float out = std::max(std::max(-0.5f - at, at - (float(grid.size[row]) - 0.5f)), 0.f);
    sum += out * out;
  }
  return scale > 1e-12f ? std::sqrt(sum) / scale : 3.4e38f;
}

// A cube's average luminance, from its last mip (a texel or four a face), and the largest
// channel of its average colour.
float CubeAverage(const File& file, const Cube& cube, float& peak) {
  const uint32_t edge = std::max(cube.size >> (cube.mipCount - 1), 1u);
  const size_t blockBytes = size_t((edge + 3) / 4) * ((edge + 3) / 4) * 16;
  const uint8_t* blocks = file.data.data() + cube.offset + cube.length - blockBytes * 6;
  std::vector<uint16_t> texels(size_t(edge) * edge * 4);
  double sum[3] = {};
  for (uint32_t face = 0; face < 6; ++face) {
    PortRemastered::DecodeBc6hFace(blocks + blockBytes * face, edge, cube.isSigned, texels.data());
    for (size_t i = 0; i < size_t(edge) * edge; ++i) {
      for (int c = 0; c < 3; ++c) {
        sum[c] += HalfToFloat(texels[i * 4 + c]);
      }
    }
  }
  const double count = 6.0 * edge * edge;
  peak = float(std::max(std::max(sum[0], sum[1]), sum[2]) / count);
  return float((0.2126 * sum[0] + 0.7152 * sum[1] + 0.0722 * sum[2]) / count);
}

// What Remastered multiplies the room's radiance by before its tone curve: 2^(3 - EV).
// Without auto exposure EV is the Tonemap's own. With it (CPostFXManager::
// UpdateTonemapping), EV = log2(L / grey) + 3 + bias, held to the hint's range, where L is
// the largest channel of the last frame's average colour and grey is sRGB 128. The port
// has no HDR frame to average, so L is the middle one of the room's probes instead; over
// the 275 rooms that lands inside the hint's range for most.
float RoomExposure(const Area& area) {
  constexpr float kGrey = 0.2158605f; // sRGB 128, linear
  const File& file = area.file;
  float ev = file.tonemap[0];
  if (file.exposure[0] != 0.f || file.exposure[1] != 0.f) {
    std::vector<float> levels;
    for (const Probe& probe : file.probes) {
      const float level = area.cubes[probe.cube].peak * probe.scale;
      if (level > 0.f && std::isfinite(level)) {
        levels.push_back(level);
      }
    }
    float level = 0.f;
    if (!levels.empty()) {
      std::nth_element(levels.begin(), levels.begin() + levels.size() / 2, levels.end());
      level = levels[levels.size() / 2];
    } else {
      // The grid's points are the same radiance.
      for (const Grid& grid : file.grids) {
        level = std::max(level, grid.average);
      }
    }
    if (!(level > 0.f)) {
      return 0.f;
    }
    ev = std::log2(level / kGrey) + 3.f + file.exposureBias;
    ev = std::min(std::max(ev, file.exposure[0]), file.exposure[1]);
  }
  if (!std::isfinite(ev)) {
    return 0.f;
  }
  return std::exp2(3.f - ev);
}

// The whole file, in one read when its size is known. The stream is left as a read
// through istreambuf_iterator leaves it: failed only when the file did not open.
using port::ReadAll;

// What `sender` drives on `state` among a kind of hints (grades or backlights).
template <class Hint>
void DriveHints(uint32_t mrea, const char* what, const std::vector<Hint>& hints,
                std::vector<Area::GradeRequest>& requests, uint64_t& orderCounter, uint32_t sender, int state) {
  for (size_t i = 0; i < requests.size() && i < hints.size(); ++i) {
    for (const GradeLink& link : hints[i].links) {
      if (link.sender != sender || link.state != state) {
        continue;
      }
      Area::GradeRequest& request = requests[i];
      const bool on = link.action == PortRoomGeo::kShow ? true
                      : link.action == PortRoomGeo::kHide ? false
                      : link.action == PortRoomGeo::kToggle ? !request.on
                                                             : request.on;
      if (on && !request.on) {
        // Above every file-order start (StartGrades).
        request.order = (uint64_t(1) << 32) + ++orderCounter;
      }
      if (on != request.on) {
        PortLog::Write("room env: %08X %s %zu %s by %08X state %d\n", mrea, what, i, on ? "on" : "off", sender, state);
      }
      request.on = on;
    }
  }
}

// OnAction_Start: plays on from where it is, from the region's state now (GetTransitionState:
// distance and transmittance always, colour and cap unsigned and only when the region has them).
void StartTransition(Area& area, size_t i) {
  Area::Transition& t = area.transitions[i];
  const FogTransition& file = area.file.transitions[i];
  t.playing = true;
  t.hasRegion = file.region < area.live.size();
  t.hasColor = false;
  t.hasCap = false;
  if (!t.hasRegion) {
    return;
  }
  const FogRegion& region = area.live[file.region];
  t.distance = region.distance;
  t.transmittance = region.transmittance;
  t.hasColor = region.hasColor;
  for (int c = 0; c < 4; ++c) {
    t.color[c] = std::fabs(region.color[c]);
  }
  t.hasCap = region.hasCap;
  t.cap = std::fabs(region.cap);
}

// The transitions `sender` drives on `state` (AcceptScriptMsg).
void DriveTransitions(uint32_t mrea, Area& area, uint32_t sender, int state) {
  for (size_t i = 0; i < area.transitions.size() && i < area.file.transitions.size(); ++i) {
    Area::Transition& t = area.transitions[i];
    for (const GradeLink& link : area.file.transitions[i].links) {
      if (link.sender != sender || link.state != state || t.dead) {
        continue;
      }
      switch (link.action) {
      case PortRoomGeo::kShow:
      case PortRoomGeo::kHide:
        t.active = link.action == PortRoomGeo::kShow;
        break;
      case kTransitionStart:
        if (t.active) {
          StartTransition(area, i);
        }
        break;
      case kTransitionRestart:
        if (t.active) {
          t.time = 0.;
          StartTransition(area, i);
        }
        break;
      case kTransitionStop:
        if (t.active) {
          t.playing = false;
        }
        break;
      case kTransitionDelete:
        t.dead = true;
        t.playing = false;
        break;
      default:
        continue;
      }
      PortLog::Write("room env: %08X fog transition %zu action %d by %08X state %d\n", mrea, i, int(link.action),
                     sender, state);
    }
  }
}

// CTimePlaybackManager's time step: to 1/60000 s.
double QuantiseTime(double t) { return std::floor(t * 60000. + 0.5) / 60000.; }

// Think: one frame of every playing transition, which leaves its region in the new state.
void StepTransitions(Area& area, float dt) {
  for (size_t i = 0; i < area.transitions.size() && i < area.file.transitions.size(); ++i) {
    Area::Transition& t = area.transitions[i];
    const FogTransition& file = area.file.transitions[i];
    if (!t.playing || !t.active || t.dead || file.region >= area.live.size()) {
      continue;
    }
    FogRegion& region = area.live[file.region];
    if (region.layer >= 0 && region.layer < 64 && (area.layers >> region.layer & 1) == 0) {
      continue;
    }
    const double first = file.phase.FirstTime();
    const double last = file.phase.LastTime();
    if (file.loop) {
      t.time += dt;
      if (t.time > last) {
        t.time -= last - first;
      }
      t.time = QuantiseTime(t.time);
    } else {
      t.time = QuantiseTime(t.time + dt);
      if (t.time >= last) {
        t.time = last;
        t.playing = false;
      }
    }
    const float p = std::clamp(file.phase.Eval(float(t.time)), 0.f, 1.f);
    const auto lerp = [p](float a, float b) { return a + (b - a) * p; };
    // A field the start and the target both have blends; one only either has is that one.
    const bool targetD = (file.select & 1) != 0;
    const bool targetT = (file.select & 2) != 0;
    const bool targetColor = (file.select & 4) != 0;
    const bool targetCap = (file.select & 8) != 0;
    const bool hasD = t.hasRegion || targetD;
    const bool hasT = t.hasRegion || targetT;
    const float d = t.hasRegion && targetD ? lerp(t.distance, file.distance) : targetD ? file.distance : t.distance;
    const float tr = t.hasRegion && targetT ? lerp(t.transmittance, file.transmittance)
                     : targetT                 ? file.transmittance
                                               : t.transmittance;
    // SetTransitionState: the signs come back from the region's mode.
    const float s = region.subtract ? -1.f : 1.f;
    // (A transmittance of 0 or a distance of 0 would make it infinite: kept as it was.)
    if (hasD && hasT && std::isfinite(-std::log(tr) / d)) {
      region.distance = d;
      region.transmittance = tr;
      region.density = s * (-std::log(tr) / d);
    }
    if (t.hasColor || targetColor) {
      for (int c = 0; c < 4; ++c) {
        const float v = t.hasColor && targetColor ? lerp(t.color[c], file.color[c])
                        : targetColor             ? file.color[c]
                                                  : t.color[c];
        region.color[c] = c < 3 ? v * s : v;
      }
      region.hasColor = true;
    }
    if (t.hasCap || targetCap) {
      const float cap = t.hasCap && targetCap ? lerp(t.cap, file.cap) : targetCap ? file.cap : t.cap;
      region.cap = s * cap;
      region.hasCap = true;
    }
  }
}

// The lights `sender` drives on `state` (AcceptScriptMsg).
void DriveLights(uint32_t mrea, Area& area, uint32_t sender, int state) {
  for (size_t i = 0; i < area.lights.size() && i < area.file.lights.size(); ++i) {
    Area::Light& l = area.lights[i];
    for (const GradeLink& link : area.file.lights[i].links) {
      if (link.sender != sender || link.state != state) {
        continue;
      }
      switch (link.action) {
      case PortRoomGeo::kShow:
      case PortRoomGeo::kHide:
        l.active = link.action == PortRoomGeo::kShow;
        break;
      case PortRoomGeo::kToggle:
        l.active = !l.active;
        break;
      case kLightStart:
      case kLightStop:
        if (l.active) {
          l.playing = link.action == kLightStart;
        }
        break;
      case kLightReset:
        if (l.active) {
          l.time = 0.;
        }
        break;
      case kLightForward:
      case kLightBackward:
        if (l.active) {
          l.forward = link.action == kLightForward;
        }
        break;
      case kLightReverse:
        if (l.active) {
          l.forward = !l.forward;
        }
        break;
      default:
        continue;
      }
      PortLog::Write("room env: %08X light %zu action %d by %08X state %d\n", mrea, i, int(link.action), sender,
                     state);
    }
  }
}

// Think: the script groups' word on each light, then one frame of every playing timeline
// (CTimePlaybackManager::Update). One that ends without a loop stops there, and switches its
// light off when its finished event is linked to do so.
void StepLights(uint32_t mrea, Area& area, float dt) {
  for (size_t i = 0; i < area.lights.size() && i < area.file.lights.size(); ++i) {
    Area::Light& l = area.lights[i];
    const PointLight& file = area.file.lights[i];
    if (file.group != PortRoomGeo::kNoGroup) {
      const int shown = PortRoomGeo::GroupShown(mrea, file.group);
      if (shown >= 0 && shown != l.group) {
        l.active = shown == 1;
      }
      l.group = shown;
    }
    if (!file.animated || !l.playing || !l.active ||
        (file.layer >= 0 && file.layer < 64 && (area.layers >> file.layer & 1) == 0)) {
      continue;
    }
    const double length = file.length;
    bool ended = false;
    if (l.forward) {
      l.time = QuantiseTime(l.time + dt);
      if (l.time > length) {
        if (file.loop && length > 0.) {
          l.time = QuantiseTime(std::fmod(l.time, length));
        } else {
          l.time = length;
          ended = true;
        }
      } else if (l.time == length && !file.loop) {
        ended = true;
      }
    } else {
      l.time = QuantiseTime(l.time - dt);
      if (l.time < 0.) {
        if (file.loop && length > 0.) {
          l.time = QuantiseTime(length - std::fmod(-l.time, length));
        } else {
          l.time = 0.;
          ended = true;
        }
      } else if (l.time == 0. && !file.loop) {
        ended = true;
      }
    }
    if (ended) {
      l.playing = false;
      // Only the backward end (t = 0) sends the event these links answer; the forward end's is unlinked.
      if (file.offAtStart && !l.forward) {
        l.active = false;
      }
    }
  }
}

// The grades and backlights `sender` drives on `state`.
void DriveGrades(uint32_t mrea, Area& area, uint32_t sender, int state) {
  DriveHints(mrea, "grade", area.file.grades, area.grades, sGradeOrder, sender, state);
  DriveHints(mrea, "backlight", area.file.backlights, area.backlights, sBacklightOrder, sender, state);
  DriveHints(mrea, "fog", area.file.fogs, area.fogs, sFogOrder, sender, state);
  DriveHints(mrea, "fog region", area.file.regions, area.regions, sRegionOrder, sender, state);
  DriveTransitions(mrea, area, sender, state);
  DriveLights(mrea, area, sender, state);
}

// Every grade and backlight as it starts, then the fluids the player and camera are in already.
void StartGrades(uint32_t mrea, Area& area) {
  area.grades.assign(area.file.grades.size(), {});
  for (size_t i = 0; i < area.grades.size(); ++i) {
    area.grades[i].on = area.file.grades[i].on;
    // In file order, below anything turned on later: the last of equals wins, as before.
    area.grades[i].order = i;
  }
  area.backlights.assign(area.file.backlights.size(), {});
  for (size_t i = 0; i < area.backlights.size(); ++i) {
    area.backlights[i].on = area.file.backlights[i].on;
    area.backlights[i].order = i;
  }
  area.fogs.assign(area.file.fogs.size(), {});
  for (size_t i = 0; i < area.fogs.size(); ++i) {
    area.fogs[i].on = area.file.fogs[i].on;
    area.fogs[i].order = i;
  }
  area.regions.assign(area.file.regions.size(), {});
  for (size_t i = 0; i < area.regions.size(); ++i) {
    area.regions[i].on = area.file.regions[i].on;
  }
  area.live = area.file.regions;
  area.lights.assign(area.file.lights.size(), {});
  for (size_t i = 0; i < area.lights.size(); ++i) {
    area.lights[i].active = area.file.lights[i].on;
    area.lights[i].playing = area.file.lights[i].playing;
  }
  area.transitions.assign(area.file.transitions.size(), {});
  for (size_t i = 0; i < area.transitions.size(); ++i) {
    area.transitions[i].active = area.file.transitions[i].on;
    if (area.file.transitions[i].autoStart) {
      StartTransition(area, i);
    }
  }
  if (sPlayerFluid) {
    DriveGrades(mrea, area, kSenderPlayerFluid, 0);
  }
  if (sCameraWater) {
    DriveGrades(mrea, area, kSenderCameraWater, 0);
  }
}

void Load(uint32_t mrea, Area& area) {
  area.hasGeo = !PortMods::RoomGeoPath(mrea).empty();
  const std::string path = PortMods::RoomEnvPath(mrea);
  if (path.empty()) {
    return;
  }
  std::ifstream in(PortGci::PathFromString(path), std::ios::binary);
  std::vector<uint8_t> data = ReadAll(in);
  std::string error;
  if (!in || !Parse(std::move(data), area.file, error)) {
    PortLog::Write("room env: %s: %s\n", path.c_str(), error.empty() ? "cannot read" : error.c_str());
    area.file = {};
    return;
  }
  area.cubes.resize(area.file.cubes.size());
  for (size_t i = 0; i < area.cubes.size(); ++i) {
    area.cubes[i].average = CubeAverage(area.file, area.file.cubes[i], area.cubes[i].peak);
    // Black: nothing to reflect, and no exposure to set by it.
    area.cubes[i].failed = !(area.cubes[i].average > 1e-6f);
  }
  area.volumes.resize(area.file.grids.size());
  StartGrades(mrea, area);
  area.exposure = RoomExposure(area);
  const float* const t = area.file.tonemap;
  if (area.exposure > 0.f && t[1] > 0.f && t[1] < 1.f) {
    BuildTone(t[1], area.file.contrast, t[2], t[3], area.tone);
  }
  PortLog::Write("room env: %08X exposure %g (EV %g, hint %g..%g bias %g), tone mid %g contrast %g toe %g shoulder %g\n",
                 mrea, area.exposure, area.exposure > 0.f ? 3.f - std::log2(area.exposure) : 0.f,
                 area.file.exposure[0], area.file.exposure[1], area.file.exposureBias, t[1], area.file.contrast, t[2],
                 t[3]);
  area.hasFile = true;
  area.serial = sNextSerial++;
  if (sNextSerial == 0) {
    sNextSerial = 1;
  }
  // The worker makes them all now, so no draw waits for one. The volumes first: until its
  // area's are made, room geometry is lit as without volumes (see HasVolume).
  for (size_t i = 0; i < area.file.grids.size(); ++i) {
    if (area.file.grids[i].average > 0.f) {
      sWorker.Submit({JobKind::Volume, mrea, area.serial, i, &area.file});
    }
  }
  for (size_t i = 0; i < area.cubes.size(); ++i) {
    if (!area.cubes[i].failed) {
      sWorker.Submit({JobKind::Cube, mrea, area.serial, i, &area.file});
    }
  }
  const Lightmap& lightmap = area.file.lightmap;
  const bool lightmapFits = lightmap.width != 0 && lightmap.height != 0 && lightmap.layers >= 4 &&
                            lightmap.offset <= area.file.data.size() &&
                            lightmap.length <= area.file.data.size() - lightmap.offset;
  if (lightmapFits && Lightmaps()) {
    // BC6H wants whole blocks; a device without it gets the colours.
    const bool raw = GXPBRLightmapBC6HSupported() && lightmap.width % 4 == 0 && lightmap.height % 4 == 0;
    sWorker.Submit({JobKind::Lightmap, mrea, area.serial, 0, &area.file, raw});
  }
  // Drops the cubes' and lightmap's blocks once they are made.
  if (!area.file.cubes.empty() || lightmapFits) {
    sWorker.Submit({JobKind::Keep, mrea, area.serial, 0, &area.file});
  }
}

// The frame's exposure, for radiance from `area`: one exposure covers the whole picture
// (see UpdateFrame), or the area's own before the frame has one.
float FrameExposure(const Area& area) {
  return sFrame.exposure > 0.f ? sFrame.exposure : area.exposure;
}

// MP_ROOM_ENV_GAIN: scales the room's baked and sun light alike.
float LightGain() {
  static const float gain = port::EnvFloat("MP_ROOM_ENV_GAIN", 1.f);
  return gain;
}

// What an area keeps of its file once its cubes and lightmap are made: the grids' points, which
// SampleGrid reads for models every frame, and the grades' LUTs, handed to Aurora when one
// is first shown; with where each starts in the copy. The cubes' and lightmap's blocks are left behind.
void KeepData(const File& file, std::vector<uint8_t>& kept, std::vector<size_t>& offsets) {
  constexpr size_t kPoint = 24;
  size_t bytes = 0;
  for (const Grid& grid : file.grids) {
    bytes += size_t(grid.size[0]) * grid.size[1] * grid.size[2] * kPoint;
  }
  bytes += file.grades.size() * kGradeLutBytes;
  kept.clear();
  kept.reserve(bytes);
  offsets.clear();
  const auto keep = [&](size_t offset, size_t length) {
    offsets.push_back(kept.size());
    kept.insert(kept.end(), file.data.begin() + offset, file.data.begin() + offset + length);
  };
  for (const Grid& grid : file.grids) {
    keep(grid.offset, size_t(grid.size[0]) * grid.size[1] * grid.size[2] * kPoint);
  }
  for (const Grade& grade : file.grades) {
    keep(grade.offset, kGradeLutBytes);
  }
}

// Decodes a cube into RGBA16Float, every mip of face 0, then face 1 (GXCreatePBRCube); the
// file has every face of mip 0, then mip 1. On the worker; false when cancelled.
bool DecodeCube(const File& file, const Cube& cube, std::vector<uint16_t>& texels, const std::atomic<bool>& cancel) {
  std::vector<size_t> mipOffset(cube.mipCount);
  size_t perFace = 0;
  for (uint32_t mip = 0; mip < cube.mipCount; ++mip) {
    const size_t edge = std::max(cube.size >> mip, 1u);
    mipOffset[mip] = perFace;
    perFace += edge * edge * 8;
  }
  texels.assign(perFace * 6 / 2, 0);
  const uint8_t* blocks = file.data.data() + cube.offset;
  for (uint32_t mip = 0; mip < cube.mipCount; ++mip) {
    const uint32_t edge = std::max(cube.size >> mip, 1u);
    const size_t blockBytes = size_t((edge + 3) / 4) * ((edge + 3) / 4) * 16;
    for (uint32_t face = 0; face < 6; ++face) {
      if (cancel.load(std::memory_order_relaxed)) {
        return false;
      }
      uint16_t* out = texels.data() + (perFace * face + mipOffset[mip]) / 2;
      PortRemastered::DecodeBc6hFace(blocks, edge, cube.isSigned, out);
      blocks += blockBytes;
    }
  }
  return true;
}

// A room's lightmap as GXCreatePBRLightmap takes it: its blocks as they are when `raw`, else
// decoded to RGBA16F and halved until a layer has at most kMaxLightmapTexels (a 2888 square
// of four layers is 267 MB at full size). On the worker; false when cancelled.
constexpr size_t kMaxLightmapTexels = size_t(1) << 21;

bool MakeLightmap(const File& file, bool raw, Result& out, const std::atomic<bool>& cancel) {
  const Lightmap& map = file.lightmap;
  const uint8_t* const blocks = file.data.data() + map.offset;
  out.raw = raw;
  out.width = map.width;
  out.height = map.height;
  if (raw) {
    out.volume.assign(blocks, blocks + map.length);
    return true;
  }
  uint32_t shift = 0;
  while (size_t(std::max(map.width >> shift, 1u)) * std::max(map.height >> shift, 1u) > kMaxLightmapTexels) {
    ++shift;
  }
  // Rounded up, so the edge texels count too (the box loop clamps).
  const uint32_t width = std::max((map.width + (1u << shift) - 1) >> shift, 1u);
  const uint32_t height = std::max((map.height + (1u << shift) - 1) >> shift, 1u);
  const size_t layerBytes = map.length / map.layers;
  std::vector<uint16_t> full(size_t(map.width) * map.height * 4);
  out.width = width;
  out.height = height;
  out.cube.assign(size_t(width) * height * 4 * map.layers, 0);
  for (uint32_t layer = 0; layer < map.layers; ++layer) {
    if (cancel.load(std::memory_order_relaxed)) {
      return false;
    }
    PortRemastered::DecodeBc6hImage(blocks + layerBytes * layer, map.width, map.height, map.isSigned, full.data());
    uint16_t* const dst = out.cube.data() + size_t(width) * height * 4 * layer;
    if (shift == 0) {
      std::copy(full.begin(), full.end(), dst);
      continue;
    }
    // Each texel the average of those under it at full size.
    const uint32_t step = 1u << shift;
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        float sum[4] = {};
        uint32_t count = 0;
        for (uint32_t sy = y * step; sy < std::min((y + 1) * step, map.height); ++sy) {
          for (uint32_t sx = x * step; sx < std::min((x + 1) * step, map.width); ++sx) {
            const uint16_t* t = full.data() + (size_t(sy) * map.width + sx) * 4;
            for (int c = 0; c < 4; ++c) {
              sum[c] += HalfToFloat(t[c]);
            }
            ++count;
          }
        }
        for (int c = 0; c < 4; ++c) {
          dst[(size_t(y) * width + x) * 4 + c] = FloatToHalf(sum[c] / float(count));
        }
      }
    }
  }
  return true;
}

Worker::~Worker() {
  {
    std::lock_guard<std::mutex> lock(mutex);
    stop = true;
    cancel = true;
  }
  wake.notify_all();
  if (thread.joinable()) {
    thread.join();
  }
}

void Worker::Submit(const Job& job) {
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!thread.joinable() && !noThread) {
      try {
        thread = std::thread(&Worker::Run, this);
      } catch (...) {
        noThread = true;
        PortLog::Write("room env: no worker thread; cubes and volumes are made as areas load\n");
      }
    }
    if (!noThread) {
      jobs.push_back(job);
      wake.notify_one();
      return;
    }
  }
  Result result;
  if (Do(job, result)) {
    std::lock_guard<std::mutex> lock(mutex);
    results.push_back(std::move(result));
  }
}

void Worker::Cancel(uint32_t serial) {
  std::unique_lock<std::mutex> lock(mutex);
  jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [serial](const Job& job) { return job.serial == serial; }),
             jobs.end());
  results.erase(std::remove_if(results.begin(), results.end(),
                               [serial](const Result& result) { return result.serial == serial; }),
                results.end());
  if (running && runningSerial == serial) {
    cancel = true;
    done.wait(lock, [this, serial] { return !running || runningSerial != serial; });
  }
  wake.notify_all();
}

bool Worker::Take(Result& out, bool gpu) {
  std::lock_guard<std::mutex> lock(mutex);
  if (results.empty() || (!gpu && results.front().kind != JobKind::Keep)) {
    return false;
  }
  out = std::move(results.front());
  results.pop_front();
  wake.notify_all();
  return true;
}

void Worker::Run() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex);
      wake.wait(lock, [this] { return stop || (!jobs.empty() && results.size() < kMaxResults); });
      if (stop) {
        return;
      }
      job = jobs.front();
      jobs.pop_front();
      running = true;
      runningSerial = job.serial;
      cancel = false;
    }
    Result result;
    const bool finished = Do(job, result);
    {
      std::lock_guard<std::mutex> lock(mutex);
      running = false;
      if (finished && !cancel) {
        results.push_back(std::move(result));
      }
    }
    done.notify_all();
  }
}

bool Worker::Do(const Job& job, Result& out) {
  out.kind = job.kind;
  out.area = job.area;
  out.serial = job.serial;
  out.index = job.index;
  switch (job.kind) {
  case JobKind::Cube:
    return DecodeCube(*job.file, job.file->cubes[job.index], out.cube, cancel);
  case JobKind::Volume:
    return FillVolume(*job.file, job.file->grids[job.index], out.volume, cancel);
  case JobKind::Lightmap:
    return MakeLightmap(*job.file, job.raw, out, cancel);
  case JobKind::Keep:
    KeepData(*job.file, out.volume, out.offsets);
    return true;
  }
  return false;
}

// Hands the worker's decoded cubes and filled volumes to the GPU, one a frame: each is
// megabytes for Aurora to copy and upload, and an area brings several at once. While the
// room environment is off they wait (nothing would use them), and so does the worker.
void TakeResults() {
  bool gpu = Enabled();
  Result result;
  while (sWorker.Take(result, gpu)) {
    const auto found = sAreas.find(result.area);
    if (found == sAreas.end() || found->second.serial != result.serial) {
      continue; // Free drops an area's results, so this does not happen
    }
    Area& area = found->second;
    File& file = area.file;
    if (result.kind == JobKind::Keep) {
      // The worker has finished with the old bytes: Keep is the area's last job.
      file.data.swap(result.volume);
      for (size_t i = 0; i < file.grids.size(); ++i) {
        file.grids[i].offset = result.offsets[i];
      }
      for (size_t i = 0; i < file.grades.size(); ++i) {
        file.grades[i].offset = result.offsets[file.grids.size() + i];
      }
      for (Cube& cube : file.cubes) {
        cube.offset = cube.length = 0; // its blocks are gone
      }
      continue;
    }
    if (result.kind == JobKind::Lightmap) {
      const Lightmap& lightmap = file.lightmap;
      const GXPBRLightmapFormat format = !result.raw         ? GX_PBR_LIGHTMAP_RGBA16F
                                         : lightmap.isSigned ? GX_PBR_LIGHTMAP_BC6H_SFLOAT
                                                             : GX_PBR_LIGHTMAP_BC6H_UFLOAT;
      const void* data = result.raw ? static_cast<const void*>(result.volume.data()) : result.cube.data();
      const size_t bytes = result.raw ? result.volume.size() : result.cube.size() * 2;
      if (area.lightmap != 0) {
        GXDestroyPBRLightmap(area.lightmap);
      }
      area.lightmap = GXCreatePBRLightmap(result.width, result.height, lightmap.layers, format, data, uint32_t(bytes));
      PortLog::Write("room env: %08X lightmap %u %ux%u x %u layers%s\n", result.area, area.lightmap, result.width,
                     result.height, lightmap.layers, result.raw ? " (BC6H)" : " (RGBA16F)");
      Invalidate();
      gpu = false;
      continue;
    }
    if (result.kind == JobKind::Cube) {
      const Cube& cube = file.cubes[result.index];
      GpuCube& gpuCube = area.cubes[result.index];
      gpuCube.id = sNextCube++;
      if (sNextCube == 0) {
        sNextCube = 1;
      }
      gpuCube.mipCount = cube.mipCount;
      GXCreatePBRCube(gpuCube.id, cube.size, cube.mipCount, result.cube.data(), uint32_t(result.cube.size() * 2));
    } else {
      const Grid& grid = file.grids[result.index];
      GpuVolume& gpuVolume = area.volumes[result.index];
      gpuVolume.id = sNextVolume++;
      if (sNextVolume == 0) {
        sNextVolume = 1;
      }
      GXCreatePBRVolume(gpuVolume.id, grid.size[0], grid.size[1], grid.size[2], result.volume.data(),
                        uint32_t(result.volume.size()));
      PortLog::Write("room env: %08X volume %u %ux%ux%u average %g\n", result.area, gpuVolume.id, grid.size[0],
                     grid.size[1], grid.size[2], grid.average);
    }
    // What Select finds changes with what is on the GPU.
    Invalidate();
    gpu = false;
  }
}

// A probe's GPU cube; null when it is black or the worker has not made it yet.
GpuCube* ProbeCube(Area& area, const Probe& probe) {
  GpuCube& gpu = area.cubes[probe.cube];
  return gpu.id != 0 ? &gpu : nullptr;
}

// The blend's keys are an area's MREA (high half) and the probe's index in its file.
uint64_t ProbeKey(uint32_t mrea, size_t index) { return (uint64_t(mrea) << 32) | uint64_t(index); }

bool FindProbe(uint64_t key, Area*& area, const Probe*& probe) {
  const auto found = sAreas.find(uint32_t(key >> 32));
  const size_t index = size_t(key & 0xFFFFFFFFu);
  if (found == sAreas.end() || index >= found->second.file.probes.size()) {
    return false;
  }
  area = &found->second;
  probe = &found->second.file.probes[index];
  return true;
}

// The blend's probes that have a cube: a black one adds nothing to the blended cube.
struct BlendSource {
  uint64_t key;
  Area* area;
  const Probe* probe;
  GpuCube* gpu;
  float weight;
};
size_t BlendSources(BlendSource out[kMaxBlend]) {
  size_t count = 0;
  for (const BlendEntry& entry : sBlend.blend.entries) {
    Area* area = nullptr;
    const Probe* probe = nullptr;
    if (count == kMaxBlend || !FindProbe(entry.key, area, probe)) {
      continue;
    }
    GpuCube* const gpu = ProbeCube(*area, *probe);
    if (gpu != nullptr) {
      out[count++] = {entry.key, area, probe, gpu, entry.weight};
    }
  }
  return count;
}

void FreeBlend() {
  for (uint32_t& id : sBlend.dst) {
    if (id != 0) {
      GXDestroyPBRCube(id);
      id = 0;
    }
  }
  sBlend.shown = -1;
  sBlend.count = 0;
  sBlend.blend = {};
}

// Moves the blend to the camera, and has Aurora draw a new blended cube when its probes or
// their weights changed.
void UpdateProbeBlend() {
  ProbeBlendState& s = sBlend;
  if (!Enabled() || !ProbeBlend() || !s.hasView) {
    s.blend = {};
    return;
  }
  std::vector<BlendCandidate> candidates;
  for (auto& [mrea, area] : sAreas) {
    for (size_t i = 0; i < area.file.probes.size(); ++i) {
      if (ProbeOn(area.file.probes[i], area.layers)) {
        candidates.push_back({ProbeKey(mrea, i), &area.file.probes[i]});
      }
    }
  }
  UpdateBlend(candidates.data(), candidates.size(), s.view, s.blend);
  sLastValid = false;
  BlendSource sources[kMaxBlend];
  const size_t count = BlendSources(sources);
  if (count < 2) {
    return; // a lone cube is bound as it is
  }
  bool same = s.shown >= 0 && s.count == count;
  for (size_t i = 0; same && i < count; ++i) {
    same = s.keys[i] == sources[i].key && std::fabs(s.weights[i] - sources[i].weight) < 1e-3f;
  }
  if (same) {
    return;
  }
  const int next = s.shown == 0 ? 1 : 0;
  if (s.dst[next] == 0) {
    s.dst[next] = sNextCube++;
    if (sNextCube == 0) {
      sNextCube = 1;
    }
  }
  uint32_t ids[kMaxBlend];
  float weights[kMaxBlend];
  for (size_t i = 0; i < count; ++i) {
    ids[i] = sources[i].gpu->id;
    weights[i] = sources[i].weight;
  }
  // It fails outside a frame's drawing; the next frame tries again.
  if (GXPortBlendPBRCube(s.dst[next], ids, weights, uint32_t(count))) {
    s.shown = next;
    s.count = count;
    for (size_t i = 0; i < count; ++i) {
      s.keys[i] = sources[i].key;
      s.weights[i] = sources[i].weight;
    }
  }
}

} // namespace

void BuildTone(float mid, float contrast, float toe, float shoulder, float rows[3][4]) {
  const float a25 = std::atan2(0.25f, mid);
  const float a75 = std::atan2(0.75f, mid);
  const float slope = std::tan(a25 + contrast * (a75 - a25));
  const float lineEnd = mid + 0.75f * shoulder / slope;
  // The toe: a cubic through the origin that meets the line at `mid`, in value and slope.
  const float half = 0.5f * (0.75f / mid - slope);
  const float c = (half - slope >= 0.f ? slope : half) * (1.f - toe);
  rows[0][0] = (slope + c) / (mid * mid) - 0.5f / (mid * mid * mid);
  rows[0][1] = 0.75f / (mid * mid) - (slope + 2.f * c) / mid;
  rows[0][2] = c;
  rows[0][3] = 0.f;
  rows[1][0] = slope;
  rows[1][1] = 0.25f - slope * mid;
  rows[1][2] = mid;
  rows[1][3] = lineEnd;
  // The shoulder: from the line's end towards 1.
  const float top = 0.25f + 0.75f * shoulder;
  const float k = 1.f - top > 0.f ? slope / (1.f - top) : 0.f;
  rows[2][0] = 1.f - top;
  rows[2][1] = k;
  rows[2][2] = -lineEnd * k;
  rows[2][3] = top;
}

bool Tone(float rows[3][4]) {
  if (!Enabled() || !RoomExposed()) {
    return false;
  }
  if (!sFrame.hasTone) {
    return false;
  }
  std::memcpy(rows, sFrame.tone, sizeof(sFrame.tone));
  return true;
}

// Hands Aurora the first mod's roomenv/brdf.lut, or tells it to use the analytic fit.
void SendBrdfLut() {
  sBrdfSent = true;
  std::vector<uint8_t> data;
  const std::string path = PortMods::BrdfLutPath();
  if (!path.empty()) {
    std::ifstream in(PortGci::PathFromString(path), std::ios::binary);
    data = ReadAll(in);
    std::string error;
    if (!in || !ValidBrdfLut(data, error)) {
      PortLog::Write("room env: %s: %s\n", path.c_str(), error.empty() ? "cannot read" : error.c_str());
      data.clear();
    } else {
      PortLog::Write("room env: environment BRDF table from %s\n", path.c_str());
    }
  }
  GXSetPBRBrdfLut(data.empty() ? nullptr : data.data(), uint32_t(data.size()));
}

// Remastered's X-ray visor post (kb topic/xray-post-shader-00089f0.md); MP_XRAY_POST=0 keeps the
// retail visor, as does Original experience.
static float sXRayTime = 0.f;
static bool sXRayActive = false;

static bool XRayPostEnabled() {
  static const bool sEnv = port::EnvFlag("MP_XRAY_POST", true);
  return sEnv && !PortDebug::OriginalExperience();
}

void UpdateFrame(bool roomGeoDrawing) {
  if (!sBrdfSent) {
    SendBrdfLut();
  }
  if (sBombTint < 0) {
    sBombTint = port::EnvFlag("MP_REMASTERED_BOMB_TINT", true) ? 1 : 0;
  }
  PowerBombBakedLight(sBombTint != 0 ? sPowerBombTime : -1.f, sBakedLight);
  if (sXRayActive && XRayPostEnabled()) {
    // init_frame 0xfb34b0 sets CBakedLightingManager's modulation override (0.6, 0.6, 0.6); it
    // replaces the colour entirely, power-bomb flash included (UpdateLightModulationColor 0x1c6f4c).
    constexpr float kXRayShadowMod = 0.6f;
    sBakedLight[0] = sBakedLight[1] = sBakedLight[2] = kXRayShadowMod;
  }
  GXSetPBRBakedLightModulation(sBakedLight);
  constexpr float kGrey = 0.2158605f; // sRGB 128, linear
  constexpr uint32_t kInFlight = 3;   // readbacks Aurora may have queued
  using Clock = std::chrono::steady_clock;
  TakeResults();
  // The exposure moves every frame, so a model's last answer is stale.
  sLastValid = false;
  FrameState& f = sFrame;
  const auto now = Clock::now();
  // Built in last frame's spare list and swapped in, so no frame allocates.
  std::vector<uint32_t>& loaded = sLoadedSpare;
  loaded.clear();
  for (const auto& entry : sAreas) {
    loaded.push_back(entry.first);
  }
  const auto view = sAreas.find(sViewArea);
  if (view == sAreas.end() || !view->second.hasFile) {
    // A room without an environment keeps the frame as it was, unless the one it came from
    // is gone too.
    if (f.started && f.area != sViewArea && sAreas.find(f.area) == sAreas.end()) {
      f = {};
    }
    f.last = now;
    f.loaded.swap(loaded);
    return;
  }
  const Area& area = view->second;
  const File& file = area.file;
  const float* const t = file.tonemap;
  const bool hint = file.exposure[0] != 0.f || file.exposure[1] != 0.f;
  // CSceneTonemapParams' static exposure: a fixed point in the hint's range, or the
  // tonemap's own EV without one.
  const float staticEv = hint ? file.exposure[0] + (file.exposure[1] - file.exposure[0]) * file.staticLerp : t[0];
  const float target[6] = {t[0], t[1], file.contrast, t[2], t[3], staticEv};
  float radiance[3] = {};
  uint32_t serial = 0;
  const bool hasRadiance = GXPortFrameRadiance(radiance, &serial);
  // Walking into a room that was already loaded eases, even when the room left is gone at
  // once; arriving in one that wasn't (a world load, a warp) starts there.
  const bool jump = !f.started || (f.area != sViewArea &&
                                   std::find(f.loaded.begin(), f.loaded.end(), sViewArea) == f.loaded.end());
  f.loaded.swap(loaded);
  if (jump) {
    f.started = true;
    std::memcpy(f.from, target, sizeof(target));
    std::memcpy(f.to, target, sizeof(target));
    f.start = f.last = now;
    f.carry = 0.f;
    f.measured = false;
    f.ignoreUntil = serial + kInFlight;
  } else if (std::memcmp(target, f.to, sizeof(target)) != 0) {
    std::memcpy(f.from, f.shown, sizeof(f.shown));
    std::memcpy(f.to, target, sizeof(target));
    f.start = now;
  }
  f.area = sViewArea;
  const float moved = std::min(std::chrono::duration<float>(now - f.start).count(), 1.f);
  for (int i = 0; i < 6; ++i) {
    f.shown[i] = f.from[i] + (f.to[i] - f.from[i]) * moved;
  }
  if (f.sigma != file.exposureSigma) {
    f.sigma = file.exposureSigma;
    f.ev.SetSigma(f.sigma);
  }
  // Measuring settles only where the picture follows the exposure: room geometry, lit by
  // the room's own light. The retail world keeps its level whatever the exposure, and
  // measuring it would push the exposure to an end of the hint's range.
  f.measuring = hint && roomGeoDrawing && area.hasGeo && AutoExposure() && Enabled() && RoomExposed();
  if (hasRadiance && serial != f.serial) {
    f.serial = serial;
    const float peak = std::max(std::max(radiance[0], radiance[1]), radiance[2]);
    if (f.measuring && int32_t(serial - f.ignoreUntil) > 0 && peak > 0.f && std::isfinite(peak)) {
      const float ev = std::log2(peak / kGrey) + 3.f + file.exposureBias;
      f.measuredEv = std::min(std::max(ev, file.exposure[0]), file.exposure[1]);
      f.measured = true;
    }
  }
  if (!f.measuring) {
    f.measured = false;
  }
  if (!hint) {
    f.targetEv = f.shown[0];
  } else if (f.measured) {
    f.targetEv = f.measuredEv;
  } else {
    f.targetEv = area.exposure > 0.f ? 3.f - std::log2(area.exposure) : f.shown[0];
  }
  // Without a hint the exposure is the tonemap's, moving linearly with it; with one it eases
  // through the Gaussian, a step every 60th of a second.
  f.carry += std::chrono::duration<float>(now - f.last).count();
  f.last = now;
  if (jump || !hint) {
    f.ev.SetValue(f.targetEv);
    f.carry = 0.f;
  } else {
    const int steps = int(f.carry * 60.f);
    f.carry -= float(steps) / 60.f;
    for (int i = std::min(steps, 30); i > 0; --i) {
      f.ev.Step(f.targetEv);
    }
  }
  const float exposure = std::exp2(3.f - f.ev.value);
  f.exposure = std::isfinite(exposure) && exposure > 0.f ? exposure : 0.f;
  f.hasTone = f.exposure > 0.f && f.shown[1] > 0.f && f.shown[1] < 1.f;
  if (f.hasTone) {
    BuildTone(f.shown[1], f.shown[2], f.shown[3], f.shown[4], f.tone);
  }
}

float MeasureExposure() { return sFrame.measuring ? sFrame.exposure : 0.f; }

float FrameExposure() { return sFrame.exposure; }

float GlowScale() {
  if (sStatic < 0) {
    sStatic = port::EnvFlag("MP_ROOM_ENV_STATIC_EXPOSURE", true) ? 1 : 0;
  }
  if (sStatic == 0 || !sFrame.hasTone || !Enabled() || !RoomExposed()) {
    return 1.f;
  }
  // The light is divided by the static exposure and the frame then multiplied by its own:
  // 2^(3 - EV) / 2^(3 - static EV).
  const float scale = std::exp2(sFrame.shown[5] - sFrame.ev.value);
  return std::isfinite(scale) && scale > 0.f ? scale : 1.f;
}

// Remastered's thermal visor post (kb topic/thermal-post.md); MP_THERMAL_POST=0 keeps the retail
// blend, as does Original experience or a mod without the imported gradient.
static float sThermalTime = 0.f;
static bool sThermalLutSent = false;

static bool ThermalPostEnabled() {
  static const bool sEnv = port::EnvFlag("MP_THERMAL_POST", true);
  return sEnv && !PortDebug::OriginalExperience();
}

void ThermalTick(float dt, bool thermalActive) {
  sThermalTime = thermalActive ? sThermalTime + dt : 0.f;
}

bool ThermalPass(bool hot, float areaHeat) {
  if (!ThermalPostEnabled()) {
    return false;
  }
  if (!sThermalLutSent) {
    const std::string path = PortMods::ThermalLutPath();
    if (path.empty()) {
      return false;
    }
    sThermalLutSent = true;
    std::ifstream in(PortGci::PathFromString(path), std::ios::binary);
    const std::vector<uint8_t> data = ReadAll(in);
    if (!in || !GXPortSetThermalLut(data.data(), uint32_t(data.size()))) {
      PortLog::Write("room env: %s: cannot read the thermal gradient\n", path.c_str());
      return false;
    }
  }
  float tone[3][4];
  if (!(FrameExposure() > 0.f) || !Tone(tone)) {
    return false;
  }
  // The cold pass's noise offset: two draws of Remastered's CRandom16 (seed 0x54524d4c), one
  // generator kept across calls.
  static CRandom16 sRandom(0x54524d4c);
  float r[4] = {0.f, 0.f, 0.f, 0.f};
  if (!hot) {
    r[0] = sRandom.Float();
    r[1] = sRandom.Float();
  }
  // The ghost: the previous frame's target over this one, past 0.1 s of thermal time.
  const float v[4] = {hot ? 1.f : 0.f, areaHeat, sThermalTime, sThermalTime > 0.1f ? 1.f : 0.f};
  return GXPortThermalPass(tone, v, r);
}

void XRayTick(float dt, bool xrayActive) {
  sXRayActive = xrayActive;
  sXRayTime = xrayActive ? sXRayTime + dt : 0.f;
}

bool XRayPass(bool distortion) {
  if (!XRayPostEnabled()) {
    return false;
  }
  const float exposure = FrameExposure();
  float tone[3][4];
  if (!(exposure > 0.f) || !Tone(tone)) {
    return false;
  }
  // Shader 00089f0's constants (kb topic/xray-post-shader-00089f0.md): the tweak values of
  // init_frame 0xfb34b0; the BSS words are never written.
  const float p[8][4] = {
      {0.28986f, 1.3f, distortion ? 1.f : 0.f, 1.f / exposure},
      {0.f, 0.f, 0.01f, -1.5f},
      {0.f, 0.f, 0.f, 0.9865f},
      {0.65f, 0.9f, 0.9f, 0.999f},
      {0.95f, 1.5f, 3.f, 0.76f},
      {3.5e-7f, 1.3125e-8f, 2.f, sXRayTime},
      {0.18f, 0.22f, 0.44f, 0.7f},
      {0.f, 15.f, 0.f, 0.f},
  };
  // SetupViewForDraw's depth range.
  // skXRayResolution: the scale of the target the world is drawn into (init_frame 0xfb34b0).
  const float depthRange[3] = {0.125f, 1.f, distortion ? 0.7f : 1.f};
  return GXPortXRayPass(p, tone, depthRange);
}

float SkyGain() {
  if (GlowScale() <= 0.f || sStatic == 0 || !sFrame.hasTone || !Enabled() || !RoomExposed()) {
    return 0.f;
  }
  // The shader multiplies an unlit surface by GlowScale, 2^(static EV - EV); this makes the
  // product Remastered's 2^(3 - EV) on a sky's HDR colour.
  const float gain = std::exp2(3.f - sFrame.shown[5]);
  return std::isfinite(gain) && gain > 0.f ? gain : 0.f;
}

float GlowGain(bool frameExposed) {
  // Before Remastered's exposure was applied the converter folded 0.10 into the glow.
  constexpr float kFallback = 0.10f;
  static const bool sOff = !port::EnvFlag("MP_REMASTERED_GLOW_EXPOSURE", true);
  if (sOff || !sFrame.hasTone || !Enabled() || !RoomExposed()) {
    return kFallback;
  }
  // The shader multiplies the glow by GlowScale, 2^(static EV - EV), where the static
  // exposure is in use; this is the rest of Remastered's 2^(3 - EV).
  const float gain = frameExposed || SkyGain() <= 0.f ? sFrame.exposure : SkyGain();
  return std::isfinite(gain) && gain > 0.f ? gain : kFallback;
}

float UnlitGain(bool frameExposed) {
  // Outside an exposed room Remastered's static default (exposure 1) applies: nothing to add.
  if (!sFrame.hasTone || !Enabled() || !RoomExposed()) {
    return 1.f;
  }
  const float sky = SkyGain();
  const float gain = frameExposed || sky <= 0.f ? sFrame.exposure : sky;
  return std::isfinite(gain) && gain > 0.f ? gain : 1.f;
}

void SetStaticExposure(bool on) { sStatic = on ? 1 : 0; }

bool StaticExposure() {
  GlowScale();
  return sStatic != 0;
}

bool AreaLights() {
  if (sAreaLights < 0) {
    sAreaLights = port::EnvFlag("MP_ROOM_ENV_AREA_LIGHTS") ? 1 : 0;
  }
  return sAreaLights != 0;
}

void SetAreaLights(bool on) { sAreaLights = on ? 1 : 0; }

bool AutoExposure() {
  if (sAuto < 0) {
    sAuto = port::EnvFlag("MP_ROOM_ENV_AUTO_EXPOSURE", true) ? 1 : 0;
  }
  return sAuto != 0;
}

void SetAutoExposure(bool on) { sAuto = on ? 1 : 0; }

bool BloomEnabled() {
  if (sBloom < 0) {
    sBloom = port::EnvFlag("MP_BLOOM", true) ? 1 : 0;
  }
  return sBloom != 0;
}

void SetBloomEnabled(bool on) { sBloom = on ? 1 : 0; }

bool Bloom(float& threshold, float tints[5][3]) {
  if (!Enabled() || !RoomExposed() || !BloomEnabled()) {
    return false;
  }
  const auto view = sAreas.find(sViewArea);
  if (view == sAreas.end() || !(view->second.tone[1][0] > 0.f) || view->second.file.bloomTints.size() < 20) {
    return false;
  }
  const std::vector<float>& rgba = view->second.file.bloomTints;
  for (int i = 0; i < 5; ++i) {
    for (int c = 0; c < 3; ++c) {
      tints[i][c] = rgba[i * 4 + c];
    }
  }
  threshold = view->second.file.bloomThreshold;
  return true;
}

bool ColorGradeEnabled() {
  if (sGrade < 0) {
    sGrade = port::EnvFlag("MP_COLOR_GRADE", true) ? 1 : 0;
  }
  return sGrade != 0;
}

void SetColorGradeEnabled(bool on) { sGrade = on ? 1 : 0; }

bool ColorGrade(LayerActive layerActive, void* context, uint32_t& a, uint32_t& b, float& weight) {
  a = b = 0;
  weight = 0.f;
  if (!Enabled() || !ColorGradeEnabled()) {
    sGradeFade.started = false;
    return false;
  }
  // The room's grade, as CAreaPrioritizedGameHintManager picks it: of the requested ones
  // whose layer is active, the highest priority, then the one turned on last. A room
  // without a file keeps whatever the frame had, as the doors between two rooms do.
  const auto view = sAreas.find(sViewArea);
  if (view != sAreas.end() && view->second.hasFile) {
    const Area& area = view->second;
    const File& file = area.file;
    const Grade* pick = nullptr;
    int pickIndex = -1;
    uint64_t pickOrder = 0;
    for (size_t i = 0; i < file.grades.size(); ++i) {
      const Grade& grade = file.grades[i];
      const bool on = i < area.grades.size() ? area.grades[i].on : grade.on;
      const uint64_t order = i < area.grades.size() ? area.grades[i].order : i;
      if (!on || (grade.layer >= 0 && layerActive != nullptr && !layerActive(grade.layer, context))) {
        continue;
      }
      if (pick == nullptr || grade.priority > pick->priority ||
          (grade.priority == pick->priority && order >= pickOrder)) {
        pick = &grade;
        pickIndex = int(i);
        pickOrder = order;
      }
    }
    // The grade shown so far, when it is this room's and has been turned off: the fade is
    // its fade-out, not the new one's fade-in.
    const Grade* off = nullptr;
    if (sGradeArea == sViewArea && sGradeIndex >= 0 && sGradeIndex != pickIndex &&
        size_t(sGradeIndex) < file.grades.size() &&
        !(size_t(sGradeIndex) < area.grades.size() ? area.grades[sGradeIndex].on : file.grades[sGradeIndex].on)) {
      off = &file.grades[sGradeIndex];
    }
    sGradeArea = sViewArea;
    sGradeIndex = pickIndex;
    const uint32_t target = pick != nullptr ? pick->id : 0;
    if (target != 0 && sGradeLuts.insert(target).second) {
      GXPortColorGradeLut(target, file.data.data() + pick->offset);
    }
    GradeFade& f = sGradeFade;
    const auto now = std::chrono::steady_clock::now();
    if (!f.started) {
      f = {true, target, target, 0.f, 0.f, now};
    } else if (target != f.to) {
      // Mid-fade, the new fade starts from whichever grade showed more.
      const float shown = f.seconds > 0.f
          ? std::chrono::duration<float>(now - f.start).count() / f.seconds : 1.f;
      f.from = shown >= 0.5f ? f.to : f.from;
      f.to = target;
      f.seconds = off != nullptr ? off->fadeOut : pick != nullptr ? pick->fadeIn : f.fadeOut;
      f.start = now;
    }
    f.fadeOut = pick != nullptr ? pick->fadeOut : 0.f;
  }
  GradeFade& f = sGradeFade;
  if (!f.started) {
    return false;
  }
  float t = 1.f;
  if (f.seconds > 0.f) {
    t = std::chrono::duration<float>(std::chrono::steady_clock::now() - f.start).count() / f.seconds;
  }
  if (t >= 1.f) {
    f.from = f.to;
    t = 1.f;
  }
  a = f.from;
  b = f.to;
  weight = t;
  return a != 0 || b != 0;
}

namespace {

// The backlight's strengths now: top, then back.
void BacklightNow(float out[2]) {
  const BacklightFade& f = sBacklight;
  float t = 1.f;
  if (f.seconds > 0.f) {
    t = std::clamp(std::chrono::duration<float>(std::chrono::steady_clock::now() - f.start).count() / f.seconds, 0.f, 1.f);
  }
  for (int i = 0; i < 2; ++i) {
    out[i] = f.from[i] + (f.to[i] - f.from[i]) * t;
  }
}

} // namespace

void UpdateBacklight(LayerActive layerActive, void* context) {
  BacklightFade& f = sBacklight;
  if (!Enabled()) {
    f = {};
    return;
  }
  // A room without a file keeps whatever the frame had, as the grade does.
  const auto view = sAreas.find(sViewArea);
  if (view == sAreas.end() || !view->second.hasFile) {
    return;
  }
  const Area& area = view->second;
  const File& file = area.file;
  // As CAreaPrioritizedGameHintManager picks (see ColorGrade): of the requested hints whose
  // layer is active, the highest priority, then the one turned on last.
  const BacklightHint* pick = nullptr;
  int pickIndex = -1;
  uint64_t pickOrder = 0;
  for (size_t i = 0; i < file.backlights.size(); ++i) {
    const BacklightHint& hint = file.backlights[i];
    const bool on = i < area.backlights.size() ? area.backlights[i].on : hint.on;
    const uint64_t order = i < area.backlights.size() ? area.backlights[i].order : i;
    if (!on || (hint.layer >= 0 && layerActive != nullptr && !layerActive(hint.layer, context))) {
      continue;
    }
    if (pick == nullptr || hint.priority > pick->priority || (hint.priority == pick->priority && order >= pickOrder)) {
      pick = &hint;
      pickIndex = int(i);
      pickOrder = order;
    }
  }
  const float target[2] = {pick != nullptr ? pick->top : kBacklightTop, pick != nullptr ? pick->back : kBacklightBack};
  float now[2];
  BacklightNow(now);
  const bool hinted = pick != nullptr;
  if (!f.started) {
    f.started = true;
    f.seconds = 0.f;
    f.from[0] = f.to[0] = target[0];
    f.from[1] = f.to[1] = target[1];
  } else if (hinted != f.hinted || (hinted && (f.area != sViewArea || f.index != pickIndex))) {
    // No hint before: the first one is set at once. Hint to hint: over the new one's fade-in;
    // hint to none: over the old one's fade-out, back to the defaults.
    f.seconds = !hinted ? f.fadeOut : f.hinted ? pick->fadeIn : 0.f;
    for (int i = 0; i < 2; ++i) {
      f.from[i] = now[i];
      f.to[i] = target[i];
    }
    f.start = std::chrono::steady_clock::now();
  }
  f.hinted = hinted;
  f.area = sViewArea;
  f.index = pickIndex;
  if (hinted) {
    f.fadeOut = pick->fadeOut;
  }
}

namespace {
// What the last Sun call made of each of the camera area's suns, for SunInfo.
enum class SunWhy : char { Picked, Weaker, Off, Below, Layer };
std::vector< SunWhy > sSunWhy;
}

bool Sun(LayerActive layerActive, void* context, bool cinematic, float toSun[3], float color[3]) {
  sSunWhy.clear();
  if (!Enabled()) {
    return false;
  }
  const auto view = sAreas.find(sViewArea);
  if (view == sAreas.end() || !view->second.hasFile) {
    return false;
  }
  // Remastered's light units only mean something at its exposure.
  const float exposure = RoomExposed() ? FrameExposure(view->second) : 0.f;
  if (exposure <= 0.f) {
    return false;
  }
  const std::vector< SunLight >& suns = view->second.file.suns;
  sSunWhy.assign(suns.size(), SunWhy::Weaker);
  const SunLight* pick = nullptr;
  float pickStrength = 0.f;
  for (size_t i = 0; i < suns.size(); ++i) {
    const SunLight& s = suns[i];
    // The room geometry's script switches the hangar's suns as it does its geometry.
    const int shown = s.group != PortRoomGeo::kNoGroup ? PortRoomGeo::GroupShown(sViewArea, s.group) : -1;
    // A light from below the floor is a bounce fill (the hangar has warm ones), not a sun.
    // The intro's suns are its shots' lighting: the next shot hides them, but a skipped intro
    // (randomprime's patches) never plays it, and they'd light the hangar from overhead.
    if (!(shown >= 0 ? shown == 1 : s.on)) {
      sSunWhy[i] = SunWhy::Off;
      continue;
    }
    if (s.toSun[2] <= 0.f) {
      sSunWhy[i] = SunWhy::Below;
      continue;
    }
    if (s.layer >= 0 && (!cinematic || (layerActive != nullptr && !layerActive(s.layer, context)))) {
      sSunWhy[i] = SunWhy::Layer;
      continue;
    }
    const float strength = s.color[0] + s.color[1] + s.color[2];
    if (std::isfinite(strength) && strength > pickStrength) {
      pick = &s;
      pickStrength = strength;
    }
  }
  if (pick == nullptr) {
    return false;
  }
  sSunWhy[size_t(pick - suns.data())] = SunWhy::Picked;
  // Remastered builds the light from colour times intensity alone (NLightLoaders::build_light)
  // and its Lambert term has the 1/pi, which the PBR shader's lights leave out; the exposure,
  // applied in Remastered's tonemap pass, is in the port's light colours, as the baked light's.
  const float scale = exposure * LightGain() / 3.14159265f;
  for (int i = 0; i < 3; ++i) {
    toSun[i] = pick->toSun[i];
    color[i] = pick->color[i] * scale;
  }
  return true;
}

std::string SunInfo() {
  const auto view = sAreas.find(sViewArea);
  if (view == sAreas.end() || !view->second.hasFile) {
    return "no room file for the camera area";
  }
  const std::vector< SunLight >& suns = view->second.file.suns;
  if (suns.empty()) {
    return "the camera area's room file has no suns";
  }
  static const char* const kWhy[] = {"PICKED", "weaker", "off", "from below", "story layer"};
  std::string out;
  for (size_t i = 0; i < suns.size(); ++i) {
    const SunLight& s = suns[i];
    char line[256];
    std::snprintf(line, sizeof(line), "sun %zu: to sun %.3f %.3f %.3f colour %.3f %.3f %.3f layer %d group %s%s: %s\n",
                  i, s.toSun[0], s.toSun[1], s.toSun[2], s.color[0], s.color[1], s.color[2], int(s.layer),
                  s.group != PortRoomGeo::kNoGroup ? std::to_string(s.group).c_str() : "-", s.on ? "" : " (starts off)",
                  i < sSunWhy.size() ? kWhy[int(sSunWhy[i])] : "not looked at");
    out += line;
  }
  out.pop_back();
  return out;
}

namespace {
// A light now, in world space, as build_light makes it (kb func/light-dynamic.md).
struct LightNow {
  float color[3];
  float nearFar[2];
  float cone[2];
  uint8_t falloff;
};
LightNow EvalLight(const PointLight& l, const Area::Light& state) {
  LightNow n{{l.color[0], l.color[1], l.color[2]}, {l.nearFar[0], l.nearFar[1]}, {l.cone[0], l.cone[1]}, l.falloff};
  if (!l.animated) {
    return n;
  }
  const float t = float(state.time);
  const auto at = [&](LightSpline s, float fallback) {
    return l.hasSpline[s] ? l.splines[s].Eval(t) : fallback;
  };
  const float intensity = at(kLightSplineIntensity, l.intensity);
  float rgb[3] = {l.rgb[0], l.rgb[1], l.rgb[2]};
  if (l.hasSpline[kLightSplineGradient] && l.hasSpline[kLightSplineGradient + 1] &&
      l.hasSpline[kLightSplineGradient + 2]) {
    const float x = std::clamp(at(kLightSplineColor, 0.f), 0.f, 1.f);
    for (int c = 0; c < 3; ++c) {
      rgb[c] = l.splines[kLightSplineGradient + c].Eval(x);
    }
  }
  for (int c = 0; c < 3; ++c) {
    n.color[c] = rgb[c] * intensity;
  }
  // The stored near and far already have the entity's scale; the splines' don't.
  if (l.hasSpline[kLightSplineNear] && l.hasSpline[kLightSplineFar]) {
    n.nearFar[0] = l.scale[0] * l.splines[kLightSplineNear].Eval(t);
    n.nearFar[1] = l.scale[1] * l.splines[kLightSplineFar].Eval(t);
    n.falloff = l.animFalloff;
  }
  if (l.hasSpline[kLightSplineInner] && l.hasSpline[kLightSplineOuter]) {
    n.cone[0] = l.splines[kLightSplineInner].Eval(t);
    n.cone[1] = l.splines[kLightSplineOuter].Eval(t);
  }
  return n;
}
} // namespace

void RoomLights(const float worldToView[3][4], std::vector<float>& out) {
  static const bool sOn = port::EnvFlag("MP_ROOM_LIGHTS", true);
  if (!sOn || !Enabled()) {
    return;
  }
  const auto view = sAreas.find(sViewArea);
  if (view == sAreas.end() || !view->second.hasFile) {
    return;
  }
  const float exposure = RoomExposed() ? FrameExposure(view->second) : 0.f;
  if (exposure <= 0.f) {
    return;
  }
  // As the sun's (see Sun).
  const float scale = exposure * LightGain() / 3.14159265f;
  constexpr float kDegrees = 3.14159265f / 180.f;
  for (const auto& [mrea, area] : sAreas) {
    if (!area.hasFile) {
      continue;
    }
    for (size_t i = 0; i < area.lights.size() && i < area.file.lights.size(); ++i) {
      const PointLight& l = area.file.lights[i];
      if (!area.lights[i].active || (l.layer >= 0 && l.layer < 64 && (area.layers >> l.layer & 1) == 0)) {
        continue;
      }
      const LightNow n = EvalLight(l, area.lights[i]);
      float color[3];
      bool lit = false;
      for (int c = 0; c < 3; ++c) {
        color[c] = std::isfinite(n.color[c]) ? std::max(n.color[c], 0.f) * scale : 0.f;
        lit = lit || color[c] > 0.f;
      }
      if (!lit) {
        continue;
      }
      // FromValues: far <= near moves far just past near.
      const float near = std::max(n.nearFar[0], 0.f);
      float far = std::max(n.nearFar[1], 0.f);
      if (!(far > near)) {
        far = near + 0.001f;
      }
      const float a = 1.f / (far - near);
      const float b = -near * a;
      float coneScale = 0.f;
      float coneBias = 1.f;
      if (l.spot) {
        const float cosIn = std::cos(n.cone[0] * 0.5f * kDegrees);
        const float cosOut = std::cos(n.cone[1] * 0.5f * kDegrees);
        const float span = cosIn - cosOut;
        if (std::fabs(span) > 1e-6f) {
          coneScale = 1.f / span;
          coneBias = -cosOut / span;
        } else {
          coneScale = 0.f;
          coneBias = 0.f;
        }
      }
      float pos[3];
      float axis[3];
      for (int r = 0; r < 3; ++r) {
        const float* m = worldToView[r];
        pos[r] = m[0] * l.pos[0] + m[1] * l.pos[1] + m[2] * l.pos[2] + m[3];
        axis[r] = m[0] * l.toLight[0] + m[1] * l.toLight[1] + m[2] * l.toLight[2];
      }
      const float record[16] = {pos[0],  pos[1],  pos[2],  a,         color[0],       color[1],
                                color[2], b,      axis[0], axis[1],   axis[2],        coneScale,
                                coneBias, float(n.falloff), l.spot ? 1.f : 0.f, 0.f};
      if (!std::all_of(std::begin(record), std::end(record), [](float v) { return std::isfinite(v); })) {
        continue;
      }
      // The game's fullest room (the frigate hangar) has 16; the cap only bounds a hostile file's FIFO write and shader loop.
      constexpr size_t kMaxRoomLights = 64;
      if (out.size() >= kMaxRoomLights * 16) {
        return;
      }
      out.insert(out.end(), std::begin(record), std::end(record));
    }
  }
}

std::string RoomLightInfo() {
  std::string out;
  for (const auto& [mrea, area] : sAreas) {
    for (size_t i = 0; i < area.lights.size() && i < area.file.lights.size(); ++i) {
      const PointLight& l = area.file.lights[i];
      const Area::Light& state = area.lights[i];
      const LightNow n = EvalLight(l, state);
      char line[320];
      std::snprintf(line, sizeof(line),
                    "%08X light %zu: %s at %.2f %.2f %.2f colour %.2f %.2f %.2f range %.2f..%.2f kind %d layer %d%s "
                    "group %s%s%s\n",
                    mrea, i, l.spot ? "spot" : "point", l.pos[0], l.pos[1], l.pos[2], n.color[0], n.color[1],
                    n.color[2], n.nearFar[0], n.nearFar[1], int(n.falloff), int(l.layer),
                    l.layer >= 0 && l.layer < 64 && (area.layers >> l.layer & 1) == 0 ? " (off)" : "",
                    l.group != PortRoomGeo::kNoGroup ? std::to_string(l.group).c_str() : "-",
                    state.active ? "" : ", inactive",
                    l.animated ? (std::string(", t ") + std::to_string(state.time) + "/" + std::to_string(l.length) +
                                  (state.playing ? " playing" : " stopped") + (state.forward ? "" : " backward"))
                                     .c_str()
                               : "");
      out += line;
    }
  }
  if (out.empty()) {
    return "no Remastered point or spot lights in the loaded areas";
  }
  out.pop_back();
  return out;
}

bool Backlight(float& top, float& back) {
  top = kBacklightTop;
  back = kBacklightBack;
  if (!Enabled() || !sBacklight.started) {
    return false;
  }
  float now[2];
  BacklightNow(now);
  top = now[0];
  back = now[1];
  return true;
}

namespace {

FogCore FogOf(const FogHint& h) {
  FogCore c;
  c.range = h.range;
  c.scatter = h.scatter;
  c.absorb = h.absorb;
  c.m1z = h.m1z;
  c.decay = h.decay;
  c.attenSlope = h.attenSlope;
  c.attenBias = h.attenBias;
  c.noiseFreq = h.noiseFreq;
  c.noiseStrength = h.noiseStrength;
  c.lightCap = h.lightCap;
  for (int i = 0; i < 3; ++i) {
    c.wind[i] = h.useScriptWind ? h.wind[i] : 0.f;
    c.colorB[i] = h.colorB[i];
    c.colorA[i] = h.colorA[i];
  }
  c.colorB[3] = h.colorB[3];
  c.noProbe = h.noProbe;
  std::copy(h.lut, h.lut + 64, c.lut);
  return c;
}

// A fog's LUT at distance `d`: entry i is at (i/63)^2 * range, so the index is sqrt(d / range) * 63.
float SampleFogLut(const FogCore& c, float d) {
  if (!(c.range > 1.2e-7f)) {
    return c.lut[0];
  }
  const float x = std::clamp(std::sqrt(std::max(d, 0.f) / c.range) * 63.f, 0.f, 63.f);
  const int i = std::min(int(x), 62);
  return c.lut[i] + (c.lut[i + 1] - c.lut[i]) * (x - float(i));
}

FogCore BlendFog(const FogCore& a, const FogCore& b, float t) {
  const auto mix = [t](float x, float y) { return x + (y - x) * t; };
  FogCore o;
  o.range = mix(a.range, b.range);
  o.scatter = mix(a.scatter, b.scatter);
  o.absorb = mix(a.absorb, b.absorb);
  o.m1z = mix(a.m1z, b.m1z);
  o.decay = mix(a.decay, b.decay);
  o.attenSlope = mix(a.attenSlope, b.attenSlope);
  o.attenBias = mix(a.attenBias, b.attenBias);
  o.noiseFreq = mix(a.noiseFreq, b.noiseFreq);
  o.noiseStrength = mix(a.noiseStrength, b.noiseStrength);
  o.lightCap = mix(a.lightCap, b.lightCap);
  for (int i = 0; i < 3; ++i) {
    o.wind[i] = b.wind[i]; // snaps
    o.colorB[i] = mix(a.colorB[i], b.colorB[i]);
    o.colorA[i] = mix(a.colorA[i], b.colorA[i]);
  }
  o.colorB[3] = mix(a.colorB[3], b.colorB[3]);
  o.noProbe = t > 0.5f ? b.noProbe : a.noProbe;
  for (int i = 0; i < 64; ++i) {
    const float x = float(i) / 63.f;
    const float d = x * x * o.range;
    o.lut[i] = mix(SampleFogLut(a, d), SampleFogLut(b, d));
  }
  return o;
}

} // namespace

bool VolFogEnabled() {
  if (sVolFog < 0) {
    sVolFog = port::EnvFlag("MP_VOLFOG", true) ? 1 : 0;
  }
  return sVolFog != 0;
}

void SetVolFogEnabled(bool on) { sVolFog = on ? 1 : 0; }

bool FogOwnsRoom() {
  if (!Enabled() || !VolFogEnabled()) {
    return false;
  }
  const auto view = sAreas.find(sViewArea);
  return view != sAreas.end() && view->second.hasFile && view->second.file.version >= 12;
}

void SetFogRegionsEnabled(bool on) { sFogRegions = on; }

void UpdateFog(LayerActive layerActive, void* context, float dt) {
  FogFade& f = sFog;
  if (!Enabled()) {
    f = {};
    return;
  }
  dt = std::isfinite(dt) ? std::clamp(dt, 0.f, 1.f) : 0.f;
  for (auto& [mrea, area] : sAreas) {
    StepTransitions(area, dt);
    StepLights(mrea, area, dt);
  }
  // As the backlight picks: of the requested hints whose layer is active, the highest
  // priority, then the one turned on last. A room without a file has no hints.
  const FogHint* pick = nullptr;
  int pickIndex = -1;
  const auto view = sAreas.find(sViewArea);
  if (view != sAreas.end() && view->second.hasFile) {
    const Area& area = view->second;
    const File& file = area.file;
    uint64_t pickOrder = 0;
    for (size_t i = 0; i < file.fogs.size(); ++i) {
      const FogHint& hint = file.fogs[i];
      const bool on = i < area.fogs.size() ? area.fogs[i].on : hint.on;
      const uint64_t order = i < area.fogs.size() ? area.fogs[i].order : i;
      if (!on || (hint.layer >= 0 && layerActive != nullptr && !layerActive(hint.layer, context))) {
        continue;
      }
      if (pick == nullptr || hint.priority > pick->priority || (hint.priority == pick->priority && order >= pickOrder)) {
        pick = &hint;
        pickIndex = int(i);
        pickOrder = order;
      }
    }
  }
  const bool hinted = pick != nullptr;
  if (hinted != f.hinted || (hinted && (f.area != sViewArea || f.index != pickIndex))) {
    // OnHintChangedCallback: from what is on screen, with the new hint's fade-in, or the old
    // one's fade-out when none is left.
    f.from = f.out;
    f.elapsed = 0.f;
    if (hinted) {
      f.fading = pick->hasFadeInSpline || (pick->linearFade && pick->fadeIn > 0.f);
      f.linear = pick->linearFade;
      f.seconds = pick->fadeIn;
      f.spline = pick->fadeInSpline;
    } else {
      f.fading = f.hasFadeOut || (f.linearFadeOut && f.fadeOutSeconds > 0.f);
      f.linear = f.linearFadeOut;
      f.seconds = f.fadeOutSeconds;
      f.spline = f.fadeOut;
    }
    // A change before the state manager's first EndFrame (its update count is 0: the world
    // has just loaded) snaps, with nothing on screen yet to fade from.
    if (f.frames == 0) {
      f.fading = false;
    }
  }
  f.hinted = hinted;
  f.area = sViewArea;
  f.index = pickIndex;
  if (hinted) {
    f.hasFadeOut = pick->hasFadeOutSpline;
    f.linearFadeOut = pick->linearFade;
    f.fadeOutSeconds = pick->fadeOut;
    f.fadeOut = pick->fadeOutSpline;
  }
  // The target: the hint's fog, or with none what is shown without density.
  FogCore target = f.out;
  if (hinted) {
    target = FogOf(*pick);
  } else {
    target.decay = 0.f;
  }
  f.active = hinted || f.fading;
  if (f.fading) {
    // Time interpolation: the phase is not clamped, and the frame that reaches the last key
    // still shows the blend.
    f.elapsed += dt;
    const float phase = f.linear ? f.elapsed / f.seconds : f.spline.Eval(f.elapsed);
    f.out = BlendFog(f.from, target, phase);
    if (f.elapsed >= (f.linear ? f.seconds : f.spline.LastTime())) {
      f.fading = false;
    }
  } else {
    f.out = target;
  }
  f.time += dt;
  ++f.frames;
}

void FogRegions(std::vector<const FogRegion*>& out) {
  out.clear();
  if (!Enabled() || !VolFogEnabled() || !sFogRegions) {
    return;
  }
  // The proxies' add order: the areas in the order they loaded, each in file order.
  std::vector<const Area*> areas;
  for (const auto& [mrea, area] : sAreas) {
    if (area.hasFile && !area.live.empty()) {
      areas.push_back(&area);
    }
  }
  std::sort(areas.begin(), areas.end(), [](const Area* a, const Area* b) { return a->serial < b->serial; });
  for (const Area* area : areas) {
    for (size_t i = 0; i < area->live.size(); ++i) {
      const FogRegion& region = area->live[i];
      const bool on = i < area->regions.size() ? area->regions[i].on : region.on;
      const bool layer = region.layer < 0 || region.layer >= 64 || (area->layers >> region.layer & 1) != 0;
      // UpdateRenderState's fluid gate.
      const bool fluid = region.fluid == 1 ? sCameraWater : region.fluid == 2 ? !sCameraWater : true;
      if (on && layer && fluid) {
        out.push_back(&region);
      }
    }
  }
}

bool VolumetricFog(Fog& out) {
  if (!Enabled() || !VolFogEnabled() || !sFog.active) {
    return false;
  }
  // With no density anywhere the pass would leave the frame as it is.
  const FogCore& c = sFog.out;
  if (!(c.decay > 1e-9f)) {
    static std::vector<const FogRegion*> regions;
    FogRegions(regions);
    bool adds = false;
    for (const FogRegion* region : regions) {
      adds = adds || region->density > 0.f;
    }
    if (!adds) {
      return false;
    }
  }
  out.range = c.range;
  out.scatter = c.scatter;
  out.absorb = c.absorb;
  out.density = c.decay;
  out.attenSlope = c.attenSlope;
  out.attenBias = c.attenBias;
  out.noiseFreq = c.noiseFreq;
  out.noiseStrength = c.noiseStrength;
  out.lightCap = c.lightCap;
  for (int i = 0; i < 3; ++i) {
    out.noiseOffset[i] = c.wind[i] * sFog.time;
    out.colorB[i] = c.colorB[i];
    out.colorA[i] = c.colorA[i];
  }
  out.colorB[3] = c.colorB[3];
  out.noProbe = c.noProbe;
  std::copy(c.lut, c.lut + 64, out.lut);
  return true;
}

std::string FogInfo() {
  std::string out;
  char line[512];
  Fog fog;
  if (VolumetricFog(fog)) {
    std::snprintf(line, sizeof(line),
                  "fog range %g density %g scatter %g absorb %g atten %g/%g noise %g x%g cap %g offset %g %g %g noprobe %d "
                  "A %g %g %g B %g %g %g %g lut %g %g %g\n",
                  fog.range, fog.density, fog.scatter, fog.absorb, fog.attenSlope, fog.attenBias, fog.noiseFreq,
                  fog.noiseStrength, fog.lightCap, fog.noiseOffset[0], fog.noiseOffset[1], fog.noiseOffset[2],
                  int(fog.noProbe), fog.colorA[0], fog.colorA[1], fog.colorA[2], fog.colorB[0], fog.colorB[1],
                  fog.colorB[2], fog.colorB[3], fog.lut[0], fog.lut[32], fog.lut[63]);
  } else {
    std::snprintf(line, sizeof(line), "fog none%s\n", VolFogEnabled() ? "" : " (MP_VOLFOG=0)");
  }
  out += line;
  out += FogOwnsRoom() ? "retail distance fog off (Remastered's fog owns the room)\n"
                       : "retail distance fog on\n";
  const auto view = sAreas.find(sViewArea);
  if (view == sAreas.end()) {
    return out;
  }
  const Area& area = view->second;
  for (size_t i = 0; i < area.file.fogs.size(); ++i) {
    const FogHint& hint = area.file.fogs[i];
    const bool on = i < area.fogs.size() ? area.fogs[i].on : hint.on;
    const bool shown = sFog.hinted && sViewArea == sFog.area && int(i) == sFog.index;
    std::snprintf(line, sizeof(line),
                  "%08X fog %zu: %s priority %d layer %d fade %g/%g range %g decay %g links %zu%s\n", sViewArea, i,
                  on ? "on " : "off", int(hint.priority), int(hint.layer), hint.fadeIn, hint.fadeOut, hint.range,
                  hint.decay, hint.links.size(), shown ? " (shown)" : "");
    out += line;
  }
  std::vector<const FogRegion*> on;
  FogRegions(on);
  for (size_t i = 0; i < area.live.size(); ++i) {
    const FogRegion& r = area.live[i];
    const bool shown = std::find(on.begin(), on.end(), &r) != on.end();
    std::snprintf(line, sizeof(line),
                  "%08X region %zu: %s layer %d fluid %d mult %g density %g colour %s%g %g %g cap %s%g box %g %g %g .. "
                  "%g %g %g links %zu\n",
                  sViewArea, i, shown ? "on " : "off", int(r.layer), int(r.fluid), r.mult, r.density,
                  r.hasColor ? "" : "(none) ", r.color[0], r.color[1], r.color[2], r.hasCap ? "" : "(none) ", r.cap,
                  r.box[0], r.box[1], r.box[2], r.box[3], r.box[4], r.box[5], r.links.size());
    out += line;
  }
  for (size_t i = 0; i < area.file.transitions.size() && i < area.transitions.size(); ++i) {
    const FogTransition& f = area.file.transitions[i];
    const Area::Transition& t = area.transitions[i];
    std::snprintf(line, sizeof(line),
                  "%08X transition %zu: region %u %s%s%s time %g of %g..%g%s select %x distance %g transmittance %g "
                  "colour %g %g %g %g cap %g links %zu\n",
                  sViewArea, i, f.region, t.active ? "active" : "inactive", t.playing ? " playing" : "",
                  t.dead ? " deleted" : "", t.time, f.phase.FirstTime(), f.phase.LastTime(), f.loop ? " loop" : "",
                  unsigned(f.select), f.distance, f.transmittance, f.color[0], f.color[1], f.color[2], f.color[3], f.cap,
                  f.links.size());
    out += line;
  }
  std::snprintf(line, sizeof(line), "regions on in loaded areas: %zu\n", on.size());
  out += line;
  return out;
}

void OnScriptState(uint32_t mrea, uint32_t sender, int state) {
  auto found = sAreas.find(mrea);
  if (found == sAreas.end()) {
    // Script objects send states before the next SetLoadedAreas, which keeps the area.
    found = sAreas.emplace(mrea, Area()).first;
    Load(mrea, found->second);
    Invalidate();
  }
  DriveGrades(mrea, found->second, sender, state);
}

void SendScriptState(uint32_t sender, int state) {
  for (auto& [mrea, area] : sAreas) {
    DriveGrades(mrea, area, sender, state);
  }
}

void SetFluid(bool player, bool camera) {
  // State 0 as the fluid is entered, 1 as it is left (the writer's convention).
  if (player != sPlayerFluid) {
    sPlayerFluid = player;
    for (auto& [mrea, area] : sAreas) {
      DriveGrades(mrea, area, kSenderPlayerFluid, player ? 0 : 1);
    }
  }
  if (camera != sCameraWater) {
    sCameraWater = camera;
    for (auto& [mrea, area] : sAreas) {
      DriveGrades(mrea, area, kSenderCameraWater, camera ? 0 : 1);
    }
  }
}

void ResetGrades() {
  sPlayerFluid = false;
  sCameraWater = false;
  sGradeIndex = -1;
  sBacklight = {};
  sFog = {};
  for (auto& [mrea, area] : sAreas) {
    StartGrades(mrea, area);
  }
}

std::string GradeInfo() {
  std::string out;
  char line[256];
  std::snprintf(line, sizeof(line), "player in fluid %d, camera in water %d\n", int(sPlayerFluid), int(sCameraWater));
  out += line;
  float top, back;
  const bool hasBacklight = Backlight(top, back);
  std::snprintf(line, sizeof(line), "backlight top %g back %g%s\n", top, back, hasBacklight ? "" : " (defaults)");
  out += line;
  for (const auto& [mrea, area] : sAreas) {
    const File& file = area.file;
    for (size_t i = 0; i < file.suns.size(); ++i) {
      const SunLight& s = file.suns[i];
      const int shown = s.group != PortRoomGeo::kNoGroup ? PortRoomGeo::GroupShown(mrea, s.group) : -1;
      std::snprintf(line, sizeof(line), "%08X sun %zu: to (%g %g %g) colour (%g %g %g) layer %d group %d %s\n", mrea, i,
                    s.toSun[0], s.toSun[1], s.toSun[2], s.color[0], s.color[1], s.color[2], s.layer, int(s.group),
                    shown >= 0 ? (shown ? "shown" : "hidden") : (s.on ? "on" : "off"));
      out += line;
    }
    for (size_t i = 0; i < file.backlights.size(); ++i) {
      const BacklightHint& hint = file.backlights[i];
      const bool on = i < area.backlights.size() ? area.backlights[i].on : hint.on;
      const bool shown = sBacklight.hinted && mrea == sBacklight.area && int(i) == sBacklight.index;
      std::snprintf(line, sizeof(line), "%08X backlight %zu: %s priority %d layer %d fade %g/%g top %g back %g links %zu%s\n",
                    mrea, i, on ? "on " : "off", int(hint.priority), int(hint.layer), hint.fadeIn, hint.fadeOut,
                    hint.top, hint.back, hint.links.size(), shown ? " (shown)" : "");
      out += line;
    }
    if (file.grades.empty()) {
      continue;
    }
    std::snprintf(line, sizeof(line), "%08X%s: %zu grades\n", mrea, mrea == sViewArea ? " (view)" : "",
                  file.grades.size());
    out += line;
    for (size_t i = 0; i < file.grades.size(); ++i) {
      const Grade& grade = file.grades[i];
      const bool on = i < area.grades.size() ? area.grades[i].on : grade.on;
      const bool shown = mrea == sGradeArea && int(i) == sGradeIndex;
      std::snprintf(line, sizeof(line), "  %zu: %s priority %d layer %d fade %g/%g links %zu lut %08X%s\n", i,
                    on ? "on " : "off", int(grade.priority), int(grade.layer), grade.fadeIn, grade.fadeOut,
                    grade.links.size(), grade.id, shown ? " (shown)" : "");
      out += line;
      for (const GradeLink& link : grade.links) {
        std::snprintf(line, sizeof(line), "     by %08X state %d action %d\n", link.sender, int(link.state),
                      int(link.action));
        out += line;
      }
    }
  }
  return out;
}

void SetViewArea(uint32_t mrea) {
  if (sViewArea != mrea) {
    sViewArea = mrea;
    sLastValid = false;
  }
}

bool ProbeBlend() {
  if (sBlend.enabled < 0) {
    sBlend.enabled = port::EnvFlag("MP_ROOM_ENV_BLEND", true) ? 1 : 0;
  }
  return sBlend.enabled != 0;
}

void SetProbeBlend(bool on) {
  sBlend.enabled = on ? 1 : 0;
  sBlend.blend = {};
  sLastValid = false;
}

void SetViewPoint(const float pos[3]) {
  std::memcpy(sBlend.view, pos, sizeof(sBlend.view));
  sBlend.hasView = true;
  UpdateProbeBlend();
}

void SetPowerBombTime(float seconds) { sPowerBombTime = seconds; }

void BakedLightModulation(float rgb[3]) {
  for (int i = 0; i < 3; ++i) {
    rgb[i] = sBakedLight[i];
  }
}

bool VolumesEnabled() {
  if (sVolumes < 0) {
    sVolumes = port::EnvFlag("MP_ROOM_ENV_VOLUME", true) ? 1 : 0;
  }
  return sVolumes != 0;
}

void SetVolumesEnabled(bool on) {
  sVolumes = on ? 1 : 0;
  Invalidate();
}

float AmbientScale() {
  if (sAmbientScale < 0.f) {
    sAmbientScale = std::max(port::EnvFloat("MP_ROOM_ENV_AMBIENT", 1.f), 0.f);
  }
  return sAmbientScale;
}

void SetAmbientScale(float scale) {
  sAmbientScale = std::max(scale, 0.f);
  Invalidate();
}

int VolumeView() {
  if (sVolumeView < 0.f) {
    sVolumeView = std::max(port::EnvFloat("MP_ROOM_ENV_VOLUME_SHOW", 0.f), 0.f);
  }
  return static_cast<int>(sVolumeView);
}

void SetVolumeView(int view) {
  sVolumeView = static_cast<float>(std::max(view, 0));
  sLastValid = false;
}

bool Enabled() {
  if (sEnabled < 0) {
    sEnabled = port::EnvFlag("MP_ROOM_ENV", true) ? 1 : 0;
  }
  return sEnabled != 0;
}

bool RoomExposed() {
  if (sExposure < 0) {
    sExposure = port::EnvFlag("MP_ROOM_ENV_EXPOSURE", true) ? 1 : 0;
  }
  return sExposure != 0;
}

void SetRoomExposed(bool on) {
  sExposure = on ? 1 : 0;
  sLastValid = false;
}

void SetEnabled(bool enabled) {
  sEnabled = enabled ? 1 : 0;
  Invalidate();
}

void SetVolumeHint(uint32_t mrea, const float centre[3]) {
  sHint = true;
  sHintArea = mrea;
  std::memcpy(sHintCentre, centre, sizeof(sHintCentre));
}

void SetLightmapHint(const float lookup[3]) { std::memcpy(sHintLightmap, lookup, sizeof(sHintLightmap)); }

void ClearVolumeHint() {
  sHint = false;
  sHintLightmap[2] = 0.f;
}

static bool sProbeOverride = false;
static float sProbeOverridePos[3] = {};

void SetProbeOverride(const float pos[3]) {
  sProbeOverride = true;
  std::memcpy(sProbeOverridePos, pos, sizeof(sProbeOverridePos));
}

void ClearProbeOverride() { sProbeOverride = false; }

bool ProbeOverride(float pos[3]) {
  if (sProbeOverride) {
    std::memcpy(pos, sProbeOverridePos, sizeof(sProbeOverridePos));
  }
  return sProbeOverride;
}

bool HasVolume(uint32_t mrea) {
  if (!Enabled() || !VolumesEnabled()) {
    return false;
  }
  const auto found = sAreas.find(mrea);
  if (found == sAreas.end()) {
    return false;
  }
  // Only once the worker has made every volume the area has: until then its models are lit
  // as they are without volumes.
  const Area& area = found->second;
  bool lit = false;
  for (size_t i = 0; i < area.file.grids.size(); ++i) {
    if (area.file.grids[i].average > 0.f) {
      if (area.volumes[i].id == 0) {
        return false;
      }
      lit = true;
    }
  }
  return lit;
}

namespace {

// The material cubes made so far, by file id; 0 for a file that could not be read.
struct MaterialCubeGpu {
  uint32_t id = 0;
  uint32_t mips = 0;
};
std::unordered_map<uint32_t, MaterialCubeGpu> sMaterialCubes;
uint32_t sNextMaterialCube = 0;

} // namespace

uint32_t MaterialCube(uint32_t fileId, float params[4]) {
  auto [it, added] = sMaterialCubes.try_emplace(fileId);
  MaterialCubeGpu& gpu = it->second;
  if (added) {
    const std::string path = PortMods::MaterialCubePath(fileId);
    if (path.empty()) {
      return 0;
    }
    std::ifstream in(PortGci::PathFromString(path), std::ios::binary);
    const std::vector<uint8_t> data = ReadAll(in);
    const auto le32 = [&](size_t at) {
      return uint32_t(data[at]) | uint32_t(data[at + 1]) << 8 | uint32_t(data[at + 2]) << 16 |
             uint32_t(data[at + 3]) << 24;
    };
    const uint32_t edge = data.size() >= 12 ? le32(4) : 0;
    const uint32_t mips = data.size() >= 12 ? le32(8) : 0;
    size_t texels = 0;
    for (uint32_t mip = 0; mip < mips && mip < 16; ++mip) {
      texels += size_t(std::max(edge >> mip, 1u)) * std::max(edge >> mip, 1u);
    }
    if (data.size() < 12 || std::memcmp(data.data(), "MPCB", 4) != 0 || edge == 0 || edge > 4096 || mips == 0 || mips > 16 || data.size() != 12 + texels * 6 * 8) {
      PortLog::Write("room env: %s: not a material cube\n", path.c_str());
      return 0;
    }
    gpu.id = 0x80000000u | (sNextMaterialCube++ & 0x7FFFFFFFu);
    gpu.mips = mips;
    GXCreatePBRCube(gpu.id, edge, mips, data.data() + 12, uint32_t(data.size() - 12));
  }
  if (gpu.id != 0) {
    params[0] = 1.f;
    params[1] = float(gpu.mips - 1);
    params[2] = 0.f;
    params[3] = 0.f;
  }
  return gpu.id;
}

void Reset() {
  for (const auto& [fileId, gpu] : sMaterialCubes) {
    if (gpu.id != 0) {
      GXDestroyPBRCube(gpu.id);
    }
  }
  sMaterialCubes.clear();
  for (auto& [mrea, area] : sAreas) {
    Free(area);
  }
  sAreas.clear();
  FreeBlend();
  sFrame = {};
  sBrdfSent = false;
  Invalidate();
  sGradeIndex = -1;
  sBacklight = {};
  sFog = {};
}

void SetLoadedAreas(const uint32_t* mreas, size_t count) {
  bool changed = false;
  for (auto it = sAreas.begin(); it != sAreas.end();) {
    if (std::find(mreas, mreas + count, it->first) == mreas + count) {
      Free(it->second);
      it = sAreas.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  for (size_t i = 0; i < count; ++i) {
    if (sAreas.find(mreas[i]) == sAreas.end()) {
      Load(mreas[i], sAreas[mreas[i]]);
      changed = true;
    }
  }
  if (changed) {
    Invalidate();
  }
}

void SetAreaLayers(uint32_t mrea, uint64_t active) {
  const auto found = sAreas.find(mrea);
  if (found != sAreas.end() && found->second.layers != active) {
    found->second.layers = active;
    Invalidate(); // the probes Locate may pick
  }
}

namespace {

// Where a model's cube, volume and ambient come from, before any exposure: what the point
// and the hint pick among the loaded areas. It stays good while the areas, their cubes and
// volumes on the GPU and the settings that pick (see Invalidate) stay as they are.
struct Located {
  const Area* cubeArea = nullptr; // the probe's area, when its cube is on the GPU
  const Probe* probe = nullptr;
  const GpuCube* cube = nullptr;
  const Area* volumeArea = nullptr; // the hint's area, when its nearest grid has a volume
  const Grid* grid = nullptr;
  uint32_t volume = 0;
  const Area* ambientArea = nullptr; // whose grid gave `sample`, when one did
  float average = 0.f;               // that grid's
  Ambient sample;
};

// What else a Located depends on: the point Select looks from, and the volume hint.
struct SelectKey {
  uint32_t pos[3] = {}; // the point's bits
  uint32_t hintArea = 0; // 0 without a hint
  bool hint = false;
  bool operator==(const SelectKey& other) const {
    return std::memcmp(pos, other.pos, sizeof(pos)) == 0 && hintArea == other.hintArea && hint == other.hint;
  }
};

struct SelectKeyHash {
  size_t operator()(const SelectKey& key) const {
    uint64_t hash = 1469598103934665603ull;
    for (const uint32_t value : {key.pos[0], key.pos[1], key.pos[2], key.hintArea, uint32_t(key.hint)}) {
      hash = (hash ^ value) * 1099511628211ull;
      hash ^= hash >> 29;
    }
    return size_t(hash);
  }
};

// Every point Select has looked from since the last Invalidate, so a model that stays put
// (room geometry, most of a room's actors) is found again without a search. When `sLocated`
// fills it becomes `sLocatedOld`, whose entries move back as they are asked for again.
constexpr size_t kMaxLocated = 4096;
std::unordered_map<SelectKey, Located, SelectKeyHash> sLocated;
std::unordered_map<SelectKey, Located, SelectKeyHash> sLocatedOld;
uint32_t sLocatedEpoch = 0;
SelectKey sLastKey;

// The probe, volume and ambient for a point. A cube or volume the worker has not made yet
// is left out, as a black cube is.
void Locate(const float pos[3], Located& out) {
  out = {};
  Area* bestArea = nullptr;
  Pick best;
  for (auto& [mrea, area] : sAreas) {
    const Pick pick = PickProbe(area.file, pos, area.layers);
    if (pick.Better(best)) {
      best = pick;
      bestArea = &area;
    }
  }
  if (bestArea != nullptr) {
    const Probe& probe = bestArea->file.probes[best.probe];
    const GpuCube& gpu = bestArea->cubes[probe.cube];
    if (gpu.id != 0) {
      out.cubeArea = bestArea;
      out.probe = &probe;
      out.cube = &gpu;
    }
  }
  if (sHint && VolumesEnabled()) {
    const auto found = sAreas.find(sHintArea);
    if (found != sAreas.end()) {
      Area& area = found->second;
      int pick = -1;
      float nearest = 0.f;
      for (size_t i = 0; i < area.file.grids.size(); ++i) {
        const float distance = GridDistance(area.file.grids[i], pos);
        if (area.file.grids[i].average > 0.f && (pick < 0 || distance < nearest)) {
          pick = int(i);
          nearest = distance;
        }
      }
      if (pick >= 0) {
        const GpuVolume& gpu = area.volumes[pick];
        if (gpu.id != 0) {
          out.volumeArea = &area;
          out.grid = &area.file.grids[pick];
          out.volume = gpu.id;
        }
      }
    }
  }
  if (AmbientScale() > 0.f) {
    // The first grid with light whose texture holds the point is the one. Remastered has
    // no fallback spot: outside a texture the sampler reads its transparent black border.
    for (auto& [mrea, area] : sAreas) {
      for (const Grid& grid : area.file.grids) {
        if (grid.average > 0.f && SampleGrid(area.file, grid, pos, out.sample)) {
          out.ambientArea = &area;
          out.average = grid.average;
          return;
        }
      }
    }
  }
}

// The Selection for what Locate found, at the frame's exposure and the settings now.
bool Compose(const Located& located, Selection& out) {
  const float gain = LightGain();
  // The mip a reflection is read from is the cube's own top one, as Remastered's is; the
  // variable only lowers it (PortRoomEnvLod::CubeLod).
  static const float lod = port::EnvFloat("MP_ROOM_ENV_LOD", PortRoomEnvLod::kNoCap);
  static const float volumeBias = port::EnvFloat("MP_ROOM_ENV_VOLUME_BIAS", 0.f);
  const float ambient = AmbientScale();
  const float grey = 0.18f * gain;
  out = {};
  BlendSource sources[kMaxBlend];
  const size_t sourceCount = ProbeBlend() ? BlendSources(sources) : 0;
  if (sourceCount != 0) {
    // Remastered's blend around the camera: every model reflects the same cube, scaled by
    // the blended intensity (a lone probe's cube by its share of it, the rest being black).
    const Blend& blend = sBlend.blend;
    const BlendSource* heaviest = &sources[0];
    float average = 0.f;
    uint32_t mips = sources[0].gpu->mipCount;
    for (size_t i = 0; i < sourceCount; ++i) {
      heaviest = sources[i].weight > heaviest->weight ? &sources[i] : heaviest;
      average += sources[i].weight * sources[i].gpu->average;
      mips = std::min(mips, sources[i].gpu->mipCount);
    }
    float share = 1.f;
    const ProbeBlendState& s = sBlend;
    if (sourceCount == 1) {
      out.cube = sources[0].gpu->id;
      share = sources[0].weight;
      average = sources[0].gpu->average;
    } else if (s.shown >= 0) {
      // The last blend drawn, which is this one but for a frame where Aurora could not draw.
      out.cube = s.dst[s.shown];
    } else {
      out.cube = heaviest->gpu->id;
      average = heaviest->gpu->average;
      mips = heaviest->gpu->mipCount;
    }
    const float exposure = RoomExposed() ? FrameExposure(*heaviest->area) : 0.f;
    const bool room = exposure > 0.f;
    out.params[0] = room ? exposure * blend.intensity * share * gain : grey / average;
    out.cubeIntensity = room ? blend.intensity * share : 1.f;
    out.params[1] = PortRoomEnvLod::CubeLod(mips, lod);
    out.params[2] = float(mips > 2 ? mips - 2 : 0);
    out.params[3] = ambient > 0.f ? 1.f / (room ? average * out.params[0] : grey) : 0.f;
    // Each probe's cube may be turned its own way; the blend takes the heaviest one's.
    std::memcpy(out.worldToCube, heaviest->probe->worldToCube, sizeof(out.worldToCube));
    if (room && blend.max > 1e-6f && std::fabs(blend.intensity) >= 1e-5f) {
      out.occlusionMin = blend.min / blend.intensity;
      out.occlusionInvMax = 1.f / blend.max;
    }
  } else if (located.cube != nullptr) {
    const Probe& probe = *located.probe;
    const GpuCube& gpu = *located.cube;
    // The cube is exposed so that its average direction is middle grey, which is what
    // Remastered's auto exposure aims for (its Tonemap's key is 0.18 too); the lamps in
    // it then come out many times brighter than white, as they should.
    // With the room's exposure the cube keeps its level instead: a probe in a dark
    // corner reflects a dark corner.
    const float exposure = RoomExposed() ? FrameExposure(*located.cubeArea) : 0.f;
    const bool room = exposure > 0.f;
    out.cube = gpu.id;
    out.params[0] = room ? exposure * probe.scale * gain : grey / gpu.average;
    out.cubeIntensity = room ? probe.scale : 1.f;
    out.params[1] = PortRoomEnvLod::CubeLod(gpu.mipCount, lod);
    out.params[2] = float(gpu.mipCount > 2 ? gpu.mipCount - 2 : 0);
    out.params[3] = ambient > 0.f ? 1.f / (room ? gpu.average * out.params[0] : grey) : 0.f;
    std::memcpy(out.worldToCube, probe.worldToCube, sizeof(out.worldToCube));
  }
  if (located.volume != 0) {
    const Grid& grid = *located.grid;
    const float* m = grid.worldToGrid;
    const float scale = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    out.volume = located.volume;
    for (int row = 0; row < 3; ++row) {
      // Point i is the middle of texel i.
      const float size = float(grid.size[row]);
      for (int col = 0; col < 4; ++col) {
        out.worldToVolume[row * 4 + col] = (m[row * 4 + col] + (col == 3 ? 0.5f : 0.f)) / size;
      }
      for (int col = 0; col < 3; ++col) {
        out.worldToAxes[row * 3 + col] = m[row * 4 + col] / scale;
      }
    }
    // The baked light is the level, at the frame's exposure; without one, the grid's
    // average comes out at the key. The grid holds irradiance, and a diffuse surface
    // sends 1/pi of that back.
    const float exposure = RoomExposed() ? FrameExposure(*located.volumeArea) : 0.f;
    out.volumeLevel = (exposure > 0.f ? exposure * gain : 0.18f / grid.average) / 3.14159265f;
    out.volumeBias = volumeBias;
    out.volumeDiagnostic = static_cast<float>(VolumeView());
  }
  if (located.ambientArea != nullptr) {
    const Ambient& sample = located.sample;
    const float average = located.average;
    const float roomExposure = FrameExposure(*located.ambientArea);
    out.hasAmbient = true;
    // The grid gives the light's colour and direction; how bright it is stays the game's
    // ambient, which the shader multiplies in. The baked levels are HDR that Remastered
    // exposes by what is on screen (one room spans 0.0001 to 100), and the world around
    // the model is still lit the retail way. Of the level only this is kept: a spot
    // darker or brighter than its room is, within a factor of two.
    const float luminance = 0.2126f * sample.mean[0] + 0.7152f * sample.mean[1] + 0.0722f * sample.mean[2];
    const float level = std::min(std::max(std::sqrt(luminance / average), 0.5f), 2.f);
    float exposure = luminance > 0.f ? level / luminance * ambient : 0.f;
    if (RoomExposed() && roomExposure > 0.f) {
      // Or the baked level itself, at the room's exposure: the game's ambient is left out.
      // The grid holds irradiance, of which a diffuse surface sends 1/pi back, as the
      // volume's level has it.
      exposure = roomExposure * ambient * gain / 3.14159265f;
      out.ambientAbsolute = true;
    }
    for (int i = 0; i < 3; ++i) {
      out.ambient[0][i] = (sample.mean[i] - sample.lobe[i]) * exposure;
      out.ambient[1][i] = 2.f * sample.lobe[i] * (1.f + sample.sharpness[i]) * exposure;
      out.ambient[2][i] = 1.f + 2.f * sample.sharpness[i];
      std::memcpy(out.ambient[3 + i], sample.direction[i], sizeof(sample.direction[i]));
    }
    if (out.volume == 0 && out.occlusionInvMax > 0.f) {
      // The shader occludes the reflection by the grid at each pixel; without the grid on
      // the GPU, the model's spot stands for all of it. The ambient the cube shapes stays.
      const float brightest = std::max(std::max(sample.mean[0], sample.mean[1]), sample.mean[2]);
      const float seen = std::min(std::max(brightest * out.occlusionInvMax, 0.f), 1.f);
      // Not 0: the shader takes an exposure of 0 for a cube that is not HDR.
      const float factor = std::max(out.occlusionMin + (1.f - out.occlusionMin) * seen, 1e-4f);
      out.params[0] *= factor;
      out.params[3] /= factor;
      out.occlusionMin = 0.f;
      out.occlusionInvMax = 0.f;
    }
  }
  return out.cube != 0 || out.hasAmbient || out.volume != 0;
}

// The hinted instance's lookup into its area's lightmap, once that is on the GPU; outside the
// cache, which is by position, since instances that share a middle need not share a lookup.
bool AddLightmap(Selection& out) {
  out.lightmap = 0;
  if (!sHint || !(sHintLightmap[2] > 0.f) || !Enabled() || !VolumesEnabled()) {
    return false;
  }
  const auto found = sAreas.find(sHintArea);
  if (found == sAreas.end() || found->second.lightmap == 0) {
    return false;
  }
  const Area& area = found->second;
  // Like the volume's level (see Compose): the lightmap holds irradiance as the grid does.
  // Without the room's exposure, the grid's average comes out at the key.
  float level;
  const float exposure = RoomExposed() ? FrameExposure(area) : 0.f;
  if (exposure > 0.f) {
    level = exposure * LightGain();
  } else if (!area.file.grids.empty() && area.file.grids[0].average > 0.f) {
    level = 0.18f / area.file.grids[0].average;
  } else {
    return false;
  }
  static const float scale = port::EnvFloat("MP_ROOM_ENV_LIGHTMAP_SCALE", 1.f);
  out.lightmap = area.lightmap;
  out.lightmapRect[0] = sHintLightmap[0];
  out.lightmapRect[1] = sHintLightmap[1];
  out.lightmapRect[2] = sHintLightmap[2];
  out.lightmapRect[3] = level * scale / 3.14159265f;
  // Remastered's axes, as the room's grid has them: world (x, y, z) -> (-x, z, y).
  static const float kAxes[9] = {-1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 1.f, 0.f};
  std::memcpy(out.worldToLightmap, kAxes, sizeof(kAxes));
  return true;
}

} // namespace

bool Select(const float origin[3], Selection& out) {
  const float* const pos = sHint ? sHintCentre : origin;
  SelectKey key;
  std::memcpy(key.pos, pos, sizeof(key.pos));
  key.hint = sHint;
  key.hintArea = sHint ? sHintArea : 0;
  if (sLastValid && key == sLastKey) {
    out = sLast;
    return AddLightmap(out) || sLastFound;
  }
  sLastKey = key;
  sLastValid = true;
  sLastFound = false;
  if (!Enabled()) {
    return false;
  }
  if (sLocatedEpoch != sEpoch) {
    sLocated.clear();
    sLocatedOld.clear();
    sLocatedEpoch = sEpoch;
  }
  const Located* located = nullptr;
  const auto found = sLocated.find(key);
  if (found != sLocated.end()) {
    located = &found->second;
  } else {
    if (sLocated.size() >= kMaxLocated) {
      sLocatedOld.swap(sLocated);
      sLocated.clear();
    }
    const auto old = sLocatedOld.find(key);
    if (old != sLocatedOld.end()) {
      located = &sLocated.emplace(key, old->second).first->second;
    } else {
      Located fresh;
      Locate(pos, fresh);
      located = &sLocated.emplace(key, fresh).first->second;
    }
  }
  sLastFound = Compose(*located, sLast);
  out = sLast;
  return AddLightmap(out) || sLastFound;
}

void Stats(int& areas, int& probes, int& cubes, int& grids) {
  areas = probes = cubes = grids = 0;
  for (const auto& [mrea, area] : sAreas) {
    if (!area.file.probes.empty() || !area.file.grids.empty()) {
      ++areas;
      probes += int(area.file.probes.size());
      grids += int(area.file.grids.size());
    }
    for (const GpuCube& cube : area.cubes) {
      cubes += cube.id != 0 ? 1 : 0;
    }
  }
}

std::string Info(const float pos[3]) {
  std::string out;
  char line[320];
  if (sFrame.started) {
    const FrameState& f = sFrame;
    std::snprintf(line, sizeof(line),
                  "frame: EV %g towards %g (%s), exposure %g, sigma %g, measurement %u%s, tone EV %g mid %g "
                  "contrast %g toe %g shoulder %g, static EV %g (glow x%g)\n",
                  f.ev.value, f.targetEv, f.measured ? "measured" : "probes", f.exposure, f.sigma, f.serial,
                  f.measuring ? " (measuring)" : "", f.shown[0], f.shown[1], f.shown[2], f.shown[3], f.shown[4],
                  f.shown[5], GlowScale());
    out += line;
  }
  if (ProbeBlend()) {
    const ProbeBlendState& s = sBlend;
    BlendSource sources[kMaxBlend];
    const size_t count = BlendSources(sources);
    char cube[48] = "";
    if (count >= 2 && s.shown >= 0) {
      std::snprintf(cube, sizeof(cube), ", blended cube %u", s.dst[s.shown]);
    }
    std::snprintf(line, sizeof(line), "probe blend at %.2f %.2f %.2f: intensity %g, occlusion %g..%g%s%s\n",
                  s.view[0], s.view[1], s.view[2], s.blend.intensity, s.blend.min, s.blend.max, cube,
                  s.blend.entries.empty() ? " (no probe: each model picks)" : "");
    out += line;
    for (const BlendEntry& entry : s.blend.entries) {
      Area* area = nullptr;
      const Probe* probe = nullptr;
      if (FindProbe(entry.key, area, probe)) {
        bool inside = false;
        const float fade = ProbeFade(*probe, s.view, inside);
        std::snprintf(line, sizeof(line), "  %08X probe %u: weight %g, fade %g%s, priority %d\n",
                      uint32_t(entry.key >> 32), uint32_t(entry.key), entry.weight, fade, inside ? " (inside)" : "",
                      probe->priority);
        out += line;
      }
    }
  } else {
    out += "probe blend off: each model picks a probe\n";
  }
  for (const auto& [mrea, area] : sAreas) {
    const File& file = area.file;
    if (file.probes.empty() && file.grids.empty()) {
      continue;
    }
    const float* const t = file.tonemap;
    std::snprintf(line, sizeof(line),
                  "%08X%s: exposure %g (EV %g, hint %g..%g, bias %g), tone EV %g mid %g contrast %g toe %g "
                  "shoulder %g, %zu probe(s), %zu cube(s), %zu grid(s)\n",
                  mrea, mrea == sViewArea ? " (camera)" : "", area.exposure,
                  area.exposure > 0.f ? 3.f - std::log2(area.exposure) : 0.f, file.exposure[0], file.exposure[1],
                  file.exposureBias, t[0], t[1], file.contrast, t[2], t[3], file.probes.size(), file.cubes.size(),
                  file.grids.size());
    out += line;
    for (size_t i = 0; i < file.grades.size(); ++i) {
      const Grade& grade = file.grades[i];
      std::snprintf(line, sizeof(line), "  grade %zu: layer %d, fade in %g out %g, LUT %08X%s\n", i, grade.layer,
                    grade.fadeIn, grade.fadeOut, grade.id, grade.id == 0 ? " (identity)" : "");
      out += line;
    }
    const Pick pick = PickProbe(file, pos, area.layers);
    for (size_t i = 0; i < file.probes.size(); ++i) {
      const Probe& probe = file.probes[i];
      const GpuCube& cube = area.cubes[probe.cube];
      // The box's centre solves worldToBox * p = 0.
      const float* const m = probe.worldToBox;
      const float a[9] = {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
      const float det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) +
                        a[2] * (a[3] * a[7] - a[4] * a[6]);
      float centre[3] = {};
      if (std::fabs(det) > 1e-12f) {
        const float b[3] = {-m[3], -m[7], -m[11]};
        centre[0] = (b[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (b[1] * a[8] - a[5] * b[2]) +
                     a[2] * (b[1] * a[7] - a[4] * b[2])) / det;
        centre[1] = (a[0] * (b[1] * a[8] - a[5] * b[2]) - b[0] * (a[3] * a[8] - a[5] * a[6]) +
                     a[2] * (a[3] * b[2] - b[1] * a[6])) / det;
        centre[2] = (a[0] * (a[4] * b[2] - b[1] * a[7]) - a[1] * (a[3] * b[2] - b[1] * a[6]) +
                     b[0] * (a[3] * a[7] - a[4] * a[6])) / det;
      }
      bool inside = false;
      const float fade = ProbeFade(probe, pos, inside);
      std::snprintf(line, sizeof(line),
                    "  probe %zu%s: layer %d%s, centre %.1f %.1f %.1f, fade %g, cube %u, scale %g, padding %g, "
                    "priority %d, intensity %g..%g, average %g, peak %g%s\n",
                    i, int(i) == pick.probe ? " (picked)" : "", probe.layer,
                    ProbeOn(probe, area.layers) ? "" : " (off)", centre[0], centre[1], centre[2],
                    fade, probe.cube, probe.scale, probe.padding, probe.priority,
                    probe.intensityMin, probe.intensityMax, cube.average * probe.scale, cube.peak * probe.scale,
                    cube.failed ? ", black" : "");
      out += line;
    }
    for (size_t i = 0; i < file.grids.size(); ++i) {
      const Grid& grid = file.grids[i];
      const float distance = GridDistance(grid, pos);
      Ambient ambient;
      const bool lit = distance == 0.f && SampleGrid(file, grid, pos, ambient);
      int used = std::snprintf(line, sizeof(line), "  grid %zu: %u x %u x %u, average %g, ", i, grid.size[0],
                               grid.size[1], grid.size[2], grid.average);
      if (lit) {
        std::snprintf(line + used, sizeof(line) - used, "here mean %g %g %g, lobe %g %g %g, sharpness %g %g %g, up %g %g %g\n",
                      ambient.mean[0], ambient.mean[1], ambient.mean[2], ambient.lobe[0], ambient.lobe[1],
                      ambient.lobe[2], ambient.sharpness[0], ambient.sharpness[1], ambient.sharpness[2],
                      ambient.direction[1][0], ambient.direction[1][1], ambient.direction[1][2]);
      } else if (distance == 0.f) {
        std::snprintf(line + used, sizeof(line) - used, "no lit point here\n");
      } else {
        std::snprintf(line + used, sizeof(line) - used, "%.1f m away\n", distance);
      }
      out += line;
    }
  }
  return out;
}

} // namespace PortRoomEnv
