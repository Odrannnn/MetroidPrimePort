// Room environments at run time: which areas have one, their cubes on the GPU, and the
// cube and ambient for a model. See port_room_env.h.
#include "port_room_env_internal.h"

namespace PortRoomEnv {
namespace EnvDetail {

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
Worker sWorker;

// Remastered keeps one world-wide virtual texture of baked lighting, its tiles keyed by world
// position. Every area registers its own copy of each tile it has when it loads
// (CBakedLightingProbeTexture, from InitializeRenderNodes 0x3fc2f4 through
// OnRegisterContextNode 0x1ca01c): the first copy stays mapped and later ones wait on standby,
// and when the mapped copy's area unloads (0x1ca650) the most recently registered standby copy
// takes over, or the tile is unmapped. sTiles is that table, sTileSets the tiles of each
// registered area, and sTileSeq the order areas have been asked to load in. The one world
// grid is composed from every mapped key (SubmitWorld, ResolveVolume), and composed again when
// a key changes owner or an area comes or goes.
PortRoomEnvTiles::TileTable sTiles;
std::unordered_map<uint32_t, std::shared_ptr<const PortRoomEnvTiles::TileSet>> sTileSets;
uint64_t sTileSeq = 0;

World sWorld;
File sNoFile; // the file of the world's job, which reads none

// The grids an area samples: the composed one of a version 21 file, else the file's own.
// Version 21 areas share the world grid (World), so it is not one of theirs.
const std::vector<Grid>& Grids(const Area& area) {
  static const std::vector<Grid> none;
  return area.file.tiles != nullptr ? none : area.file.grids;
}

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
float sHintLightmap[3] = {}; // SetLightmapHint; PortRoomGeo::HasLightmapLookup
int sEnabled = -1;
int sBloom = -1;
uint32_t sViewArea = 0;
int sGrade = -1;
GradeFade sGradeFade;
// The grade on screen: its area and index, to tell a grade turned off from one outranked.
uint32_t sGradeArea = 0;
int sGradeIndex = -1;
uint64_t sGradeOrder = 0;
BacklightFade sBacklight;
uint64_t sBacklightOrder = 0;
FogFade sFog;
uint64_t sFogOrder = 0;
uint64_t sRegionOrder = 0;
int sVolFog = -1;
// Console `roomenv fogregions on|off`: drop the fog regions to see what they add.
bool sFogRegions = true;
bool sPlayerFluid = false;
bool sCameraWater = false;
uint8_t sExposureSwap = 1;
uint64_t sItems = 0;
std::vector<WorldVolume> sWorldVolumes;
WorldVolumeState sWorldVolumeState;
float sCameraPoint[3] = {};
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
  if (area.pending.valid()) {
    area.pending.wait(); // the loader thread reads nothing of the area, but the future must not outlive it
  }
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
  const uint8_t* source = grid.points ? grid.points->data() : file.data.data() + grid.offset;
  std::vector<uint8_t> points(source, source + count * kPoint);
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

// A version 21 area's grid: its region's mapped tiles composed into points (ResolveGrid), then
// filled as FillVolume does. On the worker; false when cancelled.
bool ResolveVolume(const VolumeInput& input, const File* file, Result& out, const std::atomic<bool>& cancel) {
  const PortRoomEnvTiles::Lookup lookup = [&input](const PortRoomEnvTiles::TileKey& key) {
    const auto it = input.mapped.find(key);
    return it != input.mapped.end() ? it->second : std::shared_ptr<const PortRoomEnvTiles::TileSet>();
  };
  PortRoomEnvTiles::ResolvedGrid resolved;
  std::string note;
  out.resolved = true;
  bool composed;
  if (input.world) {
    std::vector<PortRoomEnvTiles::TileKey> keys;
    keys.reserve(input.mapped.size());
    for (const auto& entry : input.mapped) {
      keys.push_back(entry.first);
    }
    composed = PortRoomEnvTiles::ResolveWorld(input.worldToPoint, keys, lookup, kMaxGridPoints, resolved, note);
  } else {
    composed = PortRoomEnvTiles::ResolveGrid(*input.own, lookup, kMaxGridPoints, resolved, note);
  }
  if (!composed) {
    out.empty = true;
    out.grid = {};
    PortLog::Write("room env: %08X tiles: %s\n", out.area, note.c_str());
    return true;
  }
  if (cancel.load(std::memory_order_relaxed)) {
    return false;
  }
  std::copy(std::begin(resolved.worldToGrid), std::end(resolved.worldToGrid), out.grid.worldToGrid);
  std::copy(std::begin(resolved.size), std::end(resolved.size), out.grid.size);
  out.grid.average = resolved.average;
  out.grid.points = std::make_shared<const std::vector<uint8_t>>(std::move(resolved.points));
  return FillVolume(*file, out.grid, out.volume, cancel);
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

// What Remastered multiplies the room's radiance by before its tone curve: 2^(3 - EV). Before
// a frame is measured (and without auto exposure) EV is the tonemap's own (UpdateTonemapping
// 0xaffb18 with L < 0); UpdateFrame moves it from there.
float RoomExposure(const Area& area) {
  const float ev = area.file.tonemap[0];
  if (!std::isfinite(ev)) {
    return 0.f;
  }
  return std::exp2(3.f - ev);
}


// The frame's exposure, for radiance from `area`: one exposure covers the whole picture
// (see UpdateFrame), or the area's own before the frame has one.
float FrameExposure(const Area& area) {
  return sFrame.exposure > 0.f ? sFrame.exposure : area.exposure;
}

// The exposure baked light is read at: the frame's, or 1 with no tone data, where Remastered draws
// through STonemapParams::BuildLinear(3.0) (E = 1) and uses the baked values as they are.
float ProbeExposure(const Area& area) {
  const float exposure = FrameExposure(area);
  return exposure > 0.f ? exposure : 1.f;
}

// MP_ROOM_ENV_GAIN: scales the room's baked and sun light alike.
float LightGain() {
  static const float gain = port::EnvFloat("MP_ROOM_ENV_GAIN", 1.f);
  return gain;
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

}  // namespace EnvDetail

using namespace EnvDetail;

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
  if (!Enabled()) {
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

static ExposureAverage sAverage;

void ResetExposure() { sAverage.Reset(); }

void UpdateFrame() {
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
  using Clock = std::chrono::steady_clock;
  FinishLoads();
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
  Area& area = view->second;
  const File& file = area.file;
  const float* const t = file.tonemap;
  // The hint in force: a registered fluid hint, else the room's plain one.
  struct Hint {
    float exposure[2];
    float bias, sigma, staticLerp;
  } eff = {{file.exposure[0], file.exposure[1]}, file.exposureBias, file.exposureSigma, file.staticLerp};
  const int fluidPick = area.hints.Pick(file.fluidHints.size());
  if (fluidPick >= 0) {
    const FluidHint& h = file.fluidHints[size_t(fluidPick)];
    eff = {{h.exposure[0], h.exposure[1]}, h.bias, h.sigma, h.staticLerp};
  }
  const bool hint = eff.exposure[0] != 0.f || eff.exposure[1] != 0.f;
  // A hint that registered this frame is type 1: SetValue instead of the Gaussian's easing. The
  // types are cleared after every frame, wherever the view was.
  const bool hintSnap = fluidPick >= 0 && area.hintSnap;
  for (auto& entry : sAreas) {
    entry.second.hintSnap = false;
  }
  // CSceneTonemapParams' static exposure: a fixed point in the hint's range, or the
  // tonemap's own EV without one.
  const float staticEv = hint ? eff.exposure[0] + (eff.exposure[1] - eff.exposure[0]) * eff.staticLerp : t[0];
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
    f.last = now;
    f.moving = false;
    f.progress = 0.f;
    f.carry = 0.f;
  } else {
    std::memcpy(f.to, target, sizeof(target));
  }
  f.area = sViewArea;
  // TransitionTonemapParams 0xb005bc: idle at `from`; a target that differs starts a move at
  // progress 0, and every moving frame adds dt over the transition time (1 s up or down,
  // skTonemap_ParamTransition*Time) before it blends. Retargeting mid-move keeps `from` and
  // the progress; the move ends by making the target `from`.
  {
    ParamTransition tr;
    std::memcpy(tr.from, f.from, sizeof(tr.from));
    std::memcpy(tr.to, f.to, sizeof(tr.to));
    tr.progress = f.progress;
    tr.moving = f.moving;
    if (jump) {
      tr.Snap(target);
    } else {
      tr.Step(target, std::chrono::duration<float>(now - f.last).count());
    }
    std::memcpy(f.from, tr.from, sizeof(f.from));
    std::memcpy(f.to, tr.to, sizeof(f.to));
    std::memcpy(f.shown, tr.shown, sizeof(f.shown));
    f.progress = tr.progress;
    f.moving = tr.moving;
  }
  // UpdateTonemapping 0xaffb18 measures every frame in a room with a hint, whatever is drawn.
  f.measuring = hint && AutoExposure() && Enabled();
  if (hasRadiance && serial != sAverage.serial) {
    sAverage.serial = serial;
    if (f.measuring) {
      sAverage.Take(radiance);
    }
  }
  // The Gaussian steps every 60th of a second.
  f.carry += std::chrono::duration<float>(now - f.last).count();
  f.last = now;
  int steps = 0;
  if (!f.measuring || hintSnap) {
    f.carry = 0.f;
  } else {
    steps = int(f.carry * 60.f);
    f.carry -= float(steps) / 60.f;
  }
  f.ev = sAverage.Update(f.measuring, f.shown[0], eff.exposure[0], eff.exposure[1], eff.bias, eff.sigma, hintSnap,
                          std::min(steps, 30), &f.targetEv);
  const float exposure = std::exp2(3.f - f.ev);
  f.exposure = std::isfinite(exposure) && exposure > 0.f ? exposure : 0.f;
  f.hasTone = f.exposure > 0.f && f.shown[1] > 0.f && f.shown[1] < 1.f;
  if (f.hasTone) {
    BuildTone(f.shown[1], f.shown[2], f.shown[3], f.shown[4], f.tone);
  }
  static const bool sLog = port::EnvFlag("MP_ROOM_ENV_EXPOSURE_LOG");
  if (sLog) {
    PortLog::Write("room env exposure: area %08X jump %d shown %g %g %g %g %g %g ev %g target %g L %g fluid %d "
                   "snap %d swap %d\n",
                   sViewArea, int(jump), f.shown[0], f.shown[1], f.shown[2], f.shown[3], f.shown[4], f.shown[5], f.ev,
                   f.targetEv, sAverage.L, fluidPick, int(hintSnap), int(sExposureSwap));
  }
}

float MeasureExposure() { return sFrame.measuring ? sFrame.exposure : 0.f; }

float FrameExposure() { return sFrame.exposure; }

float ParticleInverseExposure(bool blended) {
  if (!sFrame.hasTone || !Enabled() || (blended && sFrame.exposure <= 0.f)) {
    return 1.f;
  }
  // The same frame/tonemap values used by FrameExposure and GlowScale, before material shading.
  const float inverse = blended ? 1.f / sFrame.exposure : std::exp2(sFrame.shown[5] - 3.f);
  return std::isfinite(inverse) && inverse > 0.f ? inverse : 1.f;
}

float GlowScale() {
  if (sStatic < 0) {
    sStatic = port::EnvFlag("MP_ROOM_ENV_STATIC_EXPOSURE", true) ? 1 : 0;
  }
  if (sStatic == 0 || !sFrame.hasTone || !Enabled()) {
    return 1.f;
  }
  // The light is divided by the static exposure and the frame then multiplied by its own:
  // 2^(3 - EV) / 2^(3 - static EV).
  const float scale = std::exp2(sFrame.shown[5] - sFrame.ev);
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
  if (GlowScale() <= 0.f || sStatic == 0 || !sFrame.hasTone || !Enabled()) {
    return 0.f;
  }
  // The shader multiplies an unlit surface by GlowScale, 2^(static EV - EV); this makes the
  // product Remastered's 2^(3 - EV) on a sky's HDR colour.
  const float gain = std::exp2(3.f - sFrame.shown[5]);
  return std::isfinite(gain) && gain > 0.f ? gain : 0.f;
}

float GlowGain(bool frameExposed) {
  // Without tone data Remastered draws through STonemapParams::BuildLinear(3.0) (0x211ab0):
  // exposure 1, so the glow is left as authored (kb topic/post-output.md "No tonemap data").
  constexpr float kFallback = 1.f;
  static const bool sOff = !port::EnvFlag("MP_REMASTERED_GLOW_EXPOSURE", true);
  if (sOff || !sFrame.hasTone || !Enabled()) {
    return kFallback;
  }
  // The shader multiplies the glow by GlowScale, 2^(static EV - EV), where the static
  // exposure is in use; this is the rest of Remastered's 2^(3 - EV).
  const float gain = frameExposed || SkyGain() <= 0.f ? sFrame.exposure : SkyGain();
  return std::isfinite(gain) && gain > 0.f ? gain : kFallback;
}

float ParticleLightGain(bool inverseExposed) {
  // MP_PARTICLE_LIGHT_SCALE multiplies it (debug).
  static const float sScale = std::max(port::EnvFloat("MP_PARTICLE_LIGHT_SCALE", 1.f), 0.f);
  const float invPi = sScale / 3.14159265f;
  const float exposure = Enabled() ? sFrame.exposure : 0.f;
  if (!(exposure > 0.f) || !std::isfinite(exposure)) {
    return invPi;
  }
  // GlowScale is E / E_static, or 1 where the static exposure is off (the frame's stands in).
  return (inverseExposed ? GlowScale() : exposure) * LightGain() * invPi;
}

float UnlitGain(bool frameExposed) {
  // Outside an exposed room Remastered's static default (exposure 1) applies: nothing to add.
  if (!sFrame.hasTone || !Enabled()) {
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
  if (!Enabled() || !BloomEnabled()) {
    return false;
  }
  // Bloom runs where the room (or its world) has a BloomEffect, tone curve or not
  // (NPostFX::get_bloom_data 0xb4b7f0; kb topic/bloom-no-bloomeffect.md). The importer pads a
  // BloomEffect's tints to five, so fewer means none.
  const auto view = sAreas.find(sViewArea);
  if (view == sAreas.end() || view->second.file.bloomTints.size() < 20) {
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

float EnvFxIntensity() {
  const auto view = sAreas.find(sViewArea);
  return view == sAreas.end() ? 1.f : view->second.file.envFxIntensity;
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
  // The room's grade, as CAreaPrioritizedGameHintManager picks it (HintOutranks): of the
  // requested ones whose layer is active, the room's own before the master's, then the
  // highest priority, then the one turned on last. A room
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
      if (HintOutranks(grade, order, pick, pickOrder)) {
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
    } else if (pick == nullptr) {
      // No grade requested: CColorGradeManager::OnHintChangedCallback (0x6b86ec) returns
      // without a transition when the new hint is null, so the last grade keeps its full
      // weight (Remastered never fades to identity).
    } else if (target != f.to) {
      // Mid-fade, the new fade starts from whichever grade showed more.
      const float shown = f.seconds > 0.f
          ? std::chrono::duration<float>(now - f.start).count() / f.seconds : 1.f;
      f.from = shown >= 0.5f ? f.to : f.from;
      f.to = target;
      f.seconds = off != nullptr ? off->fadeOut : pick->fadeIn;
      f.start = now;
    }
    if (pick != nullptr) {
      f.fadeOut = pick->fadeOut;
    }
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
  // As CAreaPrioritizedGameHintManager picks (HintOutranks, see ColorGrade).
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
    if (HintOutranks(hint, order, pick, pickOrder)) {
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
enum class SunWhy : char { Picked, Weaker, Off, Layer, Lit };
std::vector< SunWhy > sSunWhy;

// The room's Remastered lighting: the camera area's file, at the frame's exposure (0: none).
const Area* SunArea(float& exposure) {
  exposure = 0.f;
  if (!Enabled()) {
    return nullptr;
  }
  const auto view = sAreas.find(sViewArea);
  if (view == sAreas.end() || !view->second.hasFile) {
    return nullptr;
  }
  // Remastered's light units only mean something at its exposure.
  exposure = FrameExposure(view->second);
  return exposure > 0.f ? &view->second : nullptr;
}

// Whether a sun shines now: on (as the room geometry's script last set it, else as it starts) and, on a
// story layer, only for its cinematic. The intro's suns are its shots' lighting: the next shot hides them,
// but a skipped intro (randomprime's patches) never plays it, and they'd light the hangar from overhead.
SunWhy SunState(const SunLight& s, LayerActive layerActive, void* context, bool cinematic) {
  const int shown = s.group != PortRoomGeo::kNoGroup ? PortRoomGeo::GroupShown(sViewArea, s.group) : -1;
  if (!(shown >= 0 ? shown == 1 : s.on)) {
    return SunWhy::Off;
  }
  if (s.layer >= 0 && (!cinematic || (layerActive != nullptr && !layerActive(s.layer, context)))) {
    return SunWhy::Layer;
  }
  return SunWhy::Lit;
}
} // namespace

bool Sun(LayerActive layerActive, void* context, bool cinematic, float toSun[3], float color[3]) {
  sSunWhy.clear();
  float exposure = 0.f;
  const Area* area = SunArea(exposure);
  if (area == nullptr) {
    return false;
  }
  const std::vector< SunLight >& suns = area->file.suns;
  sSunWhy.assign(suns.size(), SunWhy::Lit);
  const SunLight* pick = nullptr;
  float pickStrength = 0.f;
  for (size_t i = 0; i < suns.size(); ++i) {
    const SunLight& s = suns[i];
    const SunWhy state = SunState(s, layerActive, context, cinematic);
    if (state != SunWhy::Lit) {
      sSunWhy[i] = state;
      continue;
    }
    // Every directional LightDynamic is a scene light (SunLights); only one with the cast bit builds
    // a shadow map (RebuildLightProxy), and the sun that casts is lit through the shadow's own slot.
    if (!s.casts) {
      continue;
    }
    sSunWhy[i] = SunWhy::Weaker;
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

bool SunOwnsLighting() {
  float exposure = 0.f;
  return SunArea(exposure) != nullptr;
}

void SunLights(LayerActive layerActive, void* context, bool cinematic, const float worldToView[3][4],
               std::vector<float>& out) {
  static const bool sOn = port::EnvFlag("MP_ROOM_LIGHTS", true);
  float exposure = 0.f;
  const Area* area = sOn ? SunArea(exposure) : nullptr;
  if (area == nullptr) {
    return;
  }
  const float scale = exposure * LightGain() / 3.14159265f;
  for (const SunLight& s : area->file.suns) {
    // A cast-bit sun is the shadow's (Sun).
    if (s.casts || SunState(s, layerActive, context, cinematic) != SunWhy::Lit) {
      continue;
    }
    float color[3];
    bool lit = false;
    for (int c = 0; c < 3; ++c) {
      color[c] = std::isfinite(s.color[c]) ? std::max(s.color[c], 0.f) * scale : 0.f;
      lit = lit || color[c] > 0.f;
    }
    // Kind 2 of GXPortSetRoomLights' records: a direction, no position or falloff.
    float axis[3];
    for (int r = 0; r < 3; ++r) {
      const float* m = worldToView[r];
      axis[r] = m[0] * s.toSun[0] + m[1] * s.toSun[1] + m[2] * s.toSun[2];
    }
    const float record[16] = {0.f,     0.f,     0.f,     0.f,   color[0], color[1], color[2], 0.f,
                              axis[0], axis[1], axis[2], 0.f,   0.f,      0.f,      2.f,      0.f};
    constexpr size_t kMaxRoomLights = 64;
    if (!lit || out.size() >= kMaxRoomLights * 16 ||
        !std::all_of(std::begin(record), std::end(record), [](float v) { return std::isfinite(v); })) {
      continue;
    }
    out.insert(out.end(), std::begin(record), std::end(record));
  }
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
  static const char* const kWhy[] = {"PICKED (casts)", "casts, weaker", "off", "story layer", "lit, no shadow"};
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

namespace {
// The walk both RoomLights forms take: a light that is active, on an active layer and not black, with its
// timeline evaluated. `fn(l, n)` gets the file's light and its state now (n.color is colour x intensity).
template <typename F>
void ForEachRoomLight(F&& fn) {
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
      bool lit = false;
      for (int c = 0; c < 3; ++c) {
        lit = lit || (std::isfinite(n.color[c]) && n.color[c] > 0.f);
      }
      if (!lit) {
        continue;
      }
      fn(l, n);
    }
  }
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
  const float exposure = FrameExposure(view->second);
  if (exposure <= 0.f) {
    return;
  }
  // As the sun's (see Sun).
  const float scale = exposure * LightGain() / 3.14159265f;
  ForEachRoomLight([&](const PointLight& l, const LightNow& n) {
    float color[3];
    for (int c = 0; c < 3; ++c) {
      color[c] = std::isfinite(n.color[c]) ? std::max(n.color[c], 0.f) * scale : 0.f;
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
    PortClusteredLights::ConeAttenuation(l.spot, n.cone[0], n.cone[1], coneScale, coneBias);
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
      return;
    }
    // The game's fullest room (the frigate hangar) has 16; the cap only bounds a hostile file's FIFO write and shader loop.
    constexpr size_t kMaxRoomLights = 64;
    if (out.size() >= kMaxRoomLights * 16) {
      return;
    }
    out.insert(out.end(), std::begin(record), std::end(record));
  });
}

void RoomLights(std::vector< PortClusteredLights::LightSource >& out) {
  static const bool sOn = port::EnvFlag("MP_ROOM_LIGHTS", true);
  if (!sOn || !Enabled()) {
    return;
  }
  const auto view = sAreas.find(sViewArea);
  if (view == sAreas.end() || !view->second.hasFile) {
    return;
  }
  // The same gate as the PBR form: without the room's exposure there is no Remastered lighting to gather
  // (the probe's AmbientAt is empty then too), and the port's own room lighting is what the effect sees.
  if (FrameExposure(view->second) <= 0.f) {
    return;
  }
  // The clustered gather's own cap; it reads the first kMaxLights records, so more would never be seen.
  ForEachRoomLight([&](const PointLight& l, const LightNow& n) {
    if (out.size() >= size_t(PortClusteredLights::kMaxLights)) {
      return;
    }
    PortClusteredLights::LightSource source;
    for (int c = 0; c < 3; ++c) {
      source.color[c] = std::isfinite(n.color[c]) ? std::max(n.color[c], 0.f) : 0.f;
    }
    std::memcpy(source.pos, l.pos, sizeof(source.pos));
    std::memcpy(source.toLight, l.toLight, sizeof(source.toLight));
    // FromValues: far <= near moves far just past near (as the PBR form and MakeLight do).
    source.nearD = std::max(n.nearFar[0], 0.f);
    source.farD = std::max(n.nearFar[1], 0.f);
    if (!(source.farD > source.nearD)) {
      source.farD = source.nearD + 0.001f;
    }
    source.spot = l.spot;
    source.innerDeg = n.cone[0];
    source.outerDeg = n.cone[1];
    source.falloff = n.falloff;
    out.push_back(source);
  });
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
    c.worldShift[i] = h.worldShift[i];
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
    o.worldShift[i] = b.worldShift[i];
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
  // As the backlight picks (HintOutranks). A room without a file has no hints.
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
      if (HintOutranks(hint, order, pick, pickOrder)) {
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
  // Remastered's m3 (minus the wind's travel) plus its world's origin, in its axes (-x, z, y).
  const float travel[3] = {c.worldShift[0] + c.wind[0] * sFog.time, c.worldShift[1] + c.wind[1] * sFog.time,
                           c.worldShift[2] + c.wind[2] * sFog.time};
  out.noiseOffset[0] = travel[0];
  out.noiseOffset[1] = -travel[2];
  out.noiseOffset[2] = -travel[1];
  for (int i = 0; i < 3; ++i) {
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
                  "%08X fog %zu: %s priority %d%s layer %d fade %g/%g range %g decay %g links %zu%s\n", sViewArea, i,
                  on ? "on " : "off", int(hint.priority), hint.master ? " master" : "", int(hint.layer), hint.fadeIn,
                  hint.fadeOut, hint.range, hint.decay, hint.links.size(), shown ? " (shown)" : "");
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
  if (!sWorldVolumes.empty()) {
    std::snprintf(line, sizeof(line), "world volumes: %zu, camera %.1f %.1f %.1f\n", sWorldVolumes.size(),
                  sCameraPoint[0], sCameraPoint[1], sCameraPoint[2]);
    out += line;
  }
  for (size_t i = 0; i < sWorldVolumes.size(); ++i) {
    const WorldVolume& v = sWorldVolumes[i];
    const bool inside = i < sWorldVolumeState.inside.size() && sWorldVolumeState.inside[i] != 0;
    const uint64_t entered = i < sWorldVolumeState.entered.size() ? sWorldVolumeState.entered[i] : 0;
    std::snprintf(line, sizeof(line), "world volume %zu: %s centre %.1f %.1f %.1f half %.1f %.1f %.1f entered #%llu\n",
                  i, inside ? "inside " : "outside", v.centre[0], v.centre[1], v.centre[2], v.half[0], v.half[1],
                  v.half[2], (unsigned long long)entered);
    out += line;
  }
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
  if (found->second.pending.valid()) {
    found->second.queued.emplace_back(sender, state);
    return;
  }
  DriveGrades(mrea, found->second, sender, state);
}

void SendScriptState(uint32_t sender, int state) {
  for (auto& [mrea, area] : sAreas) {
    if (area.pending.valid()) {
      area.queued.emplace_back(sender, state);
    } else {
      DriveGrades(mrea, area, sender, state);
    }
  }
}

void SetFluid(bool player, bool camera, uint64_t items) {
  sItems = items;
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
      if (camera) {
        DriveCameraWaterItems(mrea, area, items);
      }
      // The hints hear the camera's fluid state in every loaded area (once their file is in).
      if (area.hasFile && area.hints.Set(area.file.fluidHints, camera)) {
        area.hintSnap = true;
      }
    }
  }
}

void SetExposureState(bool unmorphed, bool thermal, bool cinematic) {
  const uint8_t state = ExposureHints::SwapState(unmorphed, thermal, cinematic);
  if (state == sExposureSwap) {
    return;
  }
  sExposureSwap = state;
  for (auto& [mrea, area] : sAreas) {
    if (area.hasFile && area.hints.SetPair(area.file.fluidHints, state)) {
      area.hintSnap = true;
    }
  }
}

void SetCameraPoint(const float pos[3]) {
  if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2])) {
    return;
  }
  std::memcpy(sCameraPoint, pos, sizeof(sCameraPoint));
  static std::vector<WorldVolumeState::Event> events;
  events.clear();
  sWorldVolumeState.Update(sWorldVolumes, pos, events);
  for (const WorldVolumeState::Event& e : events) {
    PortLog::Write("room env: camera %s world volume %u at %.1f %.1f %.1f\n", e.state == 0 ? "entered" : "left",
                   e.volume, pos[0], pos[1], pos[2]);
    for (auto& [mrea, area] : sAreas) {
      // One still loading hears the state so far when it finishes (ReplayWorldVolumes).
      if (!area.pending.valid()) {
        DriveGrades(mrea, area, kSenderWorldVolume + e.volume, e.state);
      }
    }
  }
}

void ResetGrades() {
  sPlayerFluid = false;
  sCameraWater = false;
  sWorldVolumeState.Clear();
  sGradeIndex = -1;
  sBacklight = {};
  sFog = {};
  for (auto& [mrea, area] : sAreas) {
    // A load still waiting for its file starts its grades when it finishes.
    if (!area.pending.valid()) {
      StartGrades(mrea, area, false, false);
    } else {
      area.pendingFluid = area.pendingWater = false;
      area.queued.clear();
    }
  }
}

void SetViewGradesOn(bool on) {
  const auto it = sAreas.find(sViewArea);
  if (it == sAreas.end()) {
    return;
  }
  for (auto& request : it->second.grades) {
    request.on = on;
    if (on) {
      request.order = ++sGradeOrder;
    }
  }
}

std::string GradeInfo() {
  std::string out;
  char line[256];
  if (sGradeFade.started) {
    const GradeFade& f = sGradeFade;
    const float t = f.seconds > 0.f
        ? std::min(1.f, std::chrono::duration<float>(std::chrono::steady_clock::now() - f.start).count() / f.seconds)
        : 1.f;
    std::snprintf(line, sizeof(line), "grade fade: %08X -> %08X, weight %.3f over %g s\n", f.from, f.to, t, f.seconds);
    out += line;
  }
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
      std::snprintf(line, sizeof(line),
                    "%08X backlight %zu: %s priority %d%s layer %d fade %g/%g top %g back %g links %zu%s\n", mrea, i,
                    on ? "on " : "off", int(hint.priority), hint.master ? " master" : "", int(hint.layer), hint.fadeIn,
                    hint.fadeOut, hint.top, hint.back, hint.links.size(), shown ? " (shown)" : "");
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
      std::snprintf(line, sizeof(line), "  %zu: %s priority %d%s layer %d fade %g/%g links %zu lut %08X%s\n", i,
                    on ? "on " : "off", int(grade.priority), grade.master ? " master" : "", int(grade.layer),
                    grade.fadeIn, grade.fadeOut, grade.links.size(), grade.id, shown ? " (shown)" : "");
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
    // It draws this frame: no waiting for its file past here.
    const auto found = sAreas.find(mrea);
    if (found != sAreas.end()) {
      Finish(mrea, found->second);
    }
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
  std::fill(std::begin(sHintLightmap), std::end(sHintLightmap), 0.f);
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
  if (area.file.tiles != nullptr) {
    return area.ownAverage > 0.f && sWorld.has();
  }
  bool lit = false;
  const std::vector<Grid>& grids = Grids(area);
  for (size_t i = 0; i < grids.size() && i < area.volumes.size(); ++i) {
    if (grids[i].average > 0.f) {
      if (area.volumes[i].id == 0) {
        return false;
      }
      lit = true;
    }
  }
  return lit;
}

namespace {

// What an area's lightmap is multiplied by. Like the volume's level (see Compose): the
// lightmap holds irradiance as the grid does, read at the frame's exposure (1 with no tone data).
float LightmapLevel(const Area& area) { return ProbeExposure(area) * LightGain(); }

} // namespace

bool HasLightmapOnly(uint32_t mrea) {
  if (!Enabled() || !VolumesEnabled()) {
    return false;
  }
  const auto found = sAreas.find(mrea);
  if (found == sAreas.end()) {
    return false;
  }
  const Area& area = found->second;
  return area.file.tiles == nullptr && area.file.grids.empty() && area.lightmap != 0 && LightmapLevel(area) > 0.f;
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
  sTiles.Clear();
  sTileSets.clear();
  sTileSeq = 0;
  sWorker.Cancel(0);
  if (sWorld.volume != 0) {
    GXDestroyPBRVolume(sWorld.volume);
  }
  sWorld.volume = 0;
  sWorld.grid = {};
  sWorld.dirty = false;
  ++sWorld.gen;
  FreeBlend();
  sFrame = {};
  sBrdfSent = false;
  Invalidate();
  sGradeIndex = -1;
  sBacklight = {};
  sFog = {};
  sWorldVolumes.clear();
  sWorldVolumeState.Clear();
}

void SetLoadedAreas(const uint32_t* mreas, size_t count) {
  bool changed = false;
  for (auto it = sAreas.begin(); it != sAreas.end();) {
    if (std::find(mreas, mreas + count, it->first) == mreas + count) {
      if (it->second.registered) {
        // Its mapped tiles go to the most recent copy another area holds (or unmap).
        if (!sTiles.Unregister(it->first).empty()) {
          sWorld.dirty = true;
        }
      }
      sTileSets.erase(it->first);
      Free(it->second);
      it = sAreas.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  if (sAreas.empty()) {
    sTileSeq = 0;
  }
  for (size_t i = 0; i < count; ++i) {
    if (sAreas.find(mreas[i]) == sAreas.end()) {
      Load(mreas[i], sAreas[mreas[i]]);
      changed = true;
    }
  }
  RegisterReady(); // also composes the world texture again when a tile changed hands
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
  if (VolumesEnabled() && !sHint && sWorld.has() && !sAreas.empty() && sAreas.begin()->second.file.tiles != nullptr) {
    // A draw with no hint area: Remastered samples the same world texture by position, so it
    // gets the world volume, with the first area's exposure.
    out.volumeArea = &sAreas.begin()->second;
    out.grid = &sWorld.grid;
    out.volume = sWorld.volume;
  } else if (sHint && VolumesEnabled()) {
    const auto found = sAreas.find(sHintArea);
    if (found != sAreas.end()) {
      Area& area = found->second;
      if (area.file.tiles != nullptr) {
        // One texture for the world, sampled by position alone (a tap past the mapped
        // tiles reads the transparent black border).
        if (sWorld.has()) {
          out.volumeArea = &area;
          out.grid = &sWorld.grid;
          out.volume = sWorld.volume;
        }
      } else {
        int pick = -1;
        float nearest = 0.f;
        const std::vector<Grid>& grids = Grids(area);
        for (size_t i = 0; i < grids.size() && i < area.volumes.size(); ++i) {
          const float distance = GridDistance(grids[i], pos);
          if (grids[i].average > 0.f && (pick < 0 || distance < nearest)) {
            pick = int(i);
            nearest = distance;
          }
        }
        if (pick >= 0) {
          const GpuVolume& gpu = area.volumes[pick];
          if (gpu.id != 0) {
            out.volumeArea = &area;
            out.grid = &grids[pick];
            out.volume = gpu.id;
          }
        }
      }
    }
  }
  if (AmbientScale() > 0.f) {
    // The first grid with light whose texture holds the point is the one. Remastered has
    // no fallback spot: outside a texture the sampler reads its transparent black border.
    for (auto& [mrea, area] : sAreas) {
      for (const Grid& grid : Grids(area)) {
        if (grid.average > 0.f && SampleGrid(area.file, grid, pos, out.sample)) {
          out.ambientArea = &area;
          out.average = grid.average;
          return;
        }
      }
    }
    // Version 21 areas: the world grid. Its exposure is the hint's area's, else the first's.
    if (sWorld.grid.average > 0.f && !sAreas.empty() && SampleGrid(sNoFile, sWorld.grid, pos, out.sample)) {
      const auto hint = sAreas.find(sHintArea);
      out.ambientArea = hint != sAreas.end() ? &hint->second : &sAreas.begin()->second;
      out.average = sWorld.grid.average;
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
  out = {};
  BlendSource sources[kMaxBlend];
  const size_t sourceCount = ProbeBlend() ? BlendSources(sources) : 0;
  if (sourceCount != 0) {
    // Remastered's blend around the camera: every model reflects the same cube, scaled by
    // the blended intensity (a lone probe's cube by its share of it, the rest being black).
    const Blend& blend = sBlend.blend;
    const BlendSource* heaviest = &sources[0];
    uint32_t mips = sources[0].gpu->mipCount;
    for (size_t i = 0; i < sourceCount; ++i) {
      heaviest = sources[i].weight > heaviest->weight ? &sources[i] : heaviest;
      mips = std::min(mips, sources[i].gpu->mipCount);
    }
    float share = 1.f;
    const ProbeBlendState& s = sBlend;
    if (sourceCount == 1) {
      out.cube = sources[0].gpu->id;
      share = sources[0].weight;
    } else if (s.shown >= 0) {
      // The last blend drawn, which is this one but for a frame where Aurora could not draw.
      out.cube = s.dst[s.shown];
    } else {
      out.cube = heaviest->gpu->id;
      mips = heaviest->gpu->mipCount;
    }
    const float exposure = ProbeExposure(*heaviest->area);
    out.params[0] = exposure * blend.intensity * share * gain;
    out.cubeIntensity = blend.intensity * share;
    out.params[1] = PortRoomEnvLod::CubeLod(mips, lod);
    out.params[2] = float(mips > 2 ? mips - 2 : 0);
    // Each probe's cube may be turned its own way; the blend takes the heaviest one's.
    std::memcpy(out.worldToCube, heaviest->probe->worldToCube, sizeof(out.worldToCube));
    if (blend.max > 1e-6f && std::fabs(blend.intensity) >= 1e-5f) {
      out.occlusionMin = blend.min / blend.intensity;
      out.occlusionInvMax = 1.f / blend.max;
    }
  } else if (located.cube != nullptr) {
    const Probe& probe = *located.probe;
    const GpuCube& gpu = *located.cube;
    // The cube keeps its level at the frame's exposure (1 with no tone data): a probe in a dark
    // corner reflects a dark corner.
    const float exposure = ProbeExposure(*located.cubeArea);
    out.cube = gpu.id;
    out.params[0] = exposure * probe.scale * gain;
    out.cubeIntensity = probe.scale;
    out.params[1] = PortRoomEnvLod::CubeLod(gpu.mipCount, lod);
    out.params[2] = float(gpu.mipCount > 2 ? gpu.mipCount - 2 : 0);
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
    // The baked light is the level, at the frame's exposure (1 with no tone data). The grid
    // holds irradiance, and a diffuse surface sends 1/pi of that back.
    out.volumeLevel = ProbeExposure(*located.volumeArea) * gain / 3.14159265f;
    out.volumeBias = volumeBias;
    out.volumeDiagnostic = static_cast<float>(VolumeView());
  }
  if (located.ambientArea != nullptr) {
    const Ambient& sample = located.sample;
    // The baked level itself, at the frame's exposure (1 with no tone data): Remastered adds no
    // ambient constant in a shipped room (kb topic/ambient-constant.md Round 3), so the retail
    // area ambient takes no part. The grid holds irradiance, of which a diffuse surface sends 1/pi
    // back, as the volume's level has it.
    const float exposure = ProbeExposure(*located.ambientArea) * ambient * gain / 3.14159265f;
    out.hasAmbient = true;
    out.ambientAbsolute = true;
    for (int i = 0; i < 3; ++i) {
      out.ambient[0][i] = (sample.mean[i] - sample.lobe[i]) * exposure;
      out.ambient[1][i] = 2.f * sample.lobe[i] * (1.f + sample.sharpness[i]) * exposure;
      out.ambient[2][i] = 1.f + 2.f * sample.sharpness[i];
      std::memcpy(out.ambient[3 + i], sample.direction[i], sizeof(sample.direction[i]));
    }
    if (out.volume == 0 && out.occlusionInvMax > 0.f) {
      // The shader occludes the reflection by the grid at each pixel; without the grid on
      // the GPU, the model's spot stands for all of it.
      const float brightest = std::max(std::max(sample.mean[0], sample.mean[1]), sample.mean[2]);
      const float seen = std::min(std::max(brightest * out.occlusionInvMax, 0.f), 1.f);
      // Not 0: the shader takes an exposure of 0 for a cube that is not HDR.
      const float factor = std::max(out.occlusionMin + (1.f - out.occlusionMin) * seen, 1e-4f);
      out.params[0] *= factor;
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
  if (!sHint || !PortRoomGeo::HasLightmapLookup(sHintLightmap) || !Enabled() || !VolumesEnabled()) {
    return false;
  }
  const auto found = sAreas.find(sHintArea);
  if (found == sAreas.end() || found->second.lightmap == 0) {
    return false;
  }
  const float level = LightmapLevel(found->second);
  static const float scale = port::EnvFloat("MP_ROOM_ENV_LIGHTMAP_SCALE", 1.f);
  // The shader takes a level of 0 for no lightmap (a scale of 0 is a one-texel lookup).
  if (!(level * scale > 0.f)) {
    return false;
  }
  out.lightmap = found->second.lightmap;
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

bool AmbientAt(const float pos[3], Ambient& out) {
  if (!Enabled() || AmbientScale() <= 0.f) {
    return false;
  }
  // One entry: a particle batch asks for one point, and the same point again on the next frame while it
  // lives, and Locate walks every loaded area's grids.
  static float sLastPos[3];
  static Ambient sLastAmbient;
  static bool sLastHit = false;
  if (sLastHit && std::memcmp(sLastPos, pos, sizeof(sLastPos)) == 0) {
    out = sLastAmbient;
    return true;
  }
  Located located;
  Locate(pos, located);
  std::memcpy(sLastPos, pos, sizeof(sLastPos));
  sLastHit = located.ambientArea != nullptr;
  if (!sLastHit) {
    return false;
  }
  sLastAmbient = located.sample;
  out = sLastAmbient;
  return true;
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
                  "frame: EV %g towards %g (L %g, %s), exposure %g, sigma %g, measurement %u%s, tone EV %g mid %g "
                  "contrast %g toe %g shoulder %g, static EV %g (glow x%g)\n",
                  f.ev, f.targetEv, sAverage.L, sAverage.created ? "Gaussian" : "tonemap EV", f.exposure,
                  sAverage.sigma, sAverage.serial,
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
  if (sWorld.grid.size[0] != 0) {
    Ambient ambient;
    const float distance = GridDistance(sWorld.grid, pos);
    const bool lit = distance == 0.f && SampleGrid(sNoFile, sWorld.grid, pos, ambient);
    char here[160] = "no light here";
    if (lit) {
      std::snprintf(here, sizeof(here), "lit here, mean %g %g %g", ambient.mean[0], ambient.mean[1], ambient.mean[2]);
    }
    std::snprintf(line, sizeof(line), "  world grid: %u x %u x %u (%.1f M points), average %g, volume %u, %s\n",
                  sWorld.grid.size[0], sWorld.grid.size[1], sWorld.grid.size[2],
                  double(sWorld.grid.size[0]) * sWorld.grid.size[1] * sWorld.grid.size[2] / 1e6, sWorld.grid.average,
                  sWorld.volume, here);
    out += line;
  }
  // The tile table: per registered area, its load order, tiles, and how many it owns (the
  // mapped copy of a key is the first loaded one; see Grids()).
  for (const auto& [mrea, area] : sAreas) {
    const auto set = sTileSets.find(mrea);
    if (!area.registered || set == sTileSets.end()) {
      continue;
    }
    size_t own = 0;
    for (const PortRoomEnvTiles::Tile& tile : set->second->tiles) {
      uint32_t node = 0;
      own += sTiles.Active(tile.key, &node) && node == mrea ? 1 : 0;
    }
    std::snprintf(line, sizeof(line), "  tiles %08X: load #%llu, %zu tiles, %zu mapped to it, %zu keys in the table\n",
                  mrea, static_cast<unsigned long long>(area.loadSeq), set->second->tiles.size(), own,
                  sTiles.KeyCount());
    out += line;
  }
  return out;
}

} // namespace PortRoomEnv
