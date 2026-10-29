// Internal to the room environment (port_room_env.h): its state and types, in
// PortRoomEnv::EnvDetail, shared by the parts it is split into:
//
//   port_room_env.cpp          the game side: the frame, probes, grades, fog and lights
//   port_room_env_script.cpp   what the area's scripts drive: fog transitions, lights, grades
//   port_room_env_load.cpp     loading a room's file, and the worker that decodes it
//
// The state itself is defined in port_room_env.cpp, in the order its destructors need.
// Nothing outside those files should include this.

#pragma once

#include "port_debug.h"
#include "port_env.h"
#include "port_room_env.h"
#include "port_strings.h"

#include "port_gci.h"
#include "port_log.h"
#include "port_mods.h"
#include "port_remastered_txtr.h"
#include "port_room_env_lod.h"
#include "port_room_env_tiles.h"
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
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace PortRoomEnv {
namespace EnvDetail {

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

// A room's file as the loader thread leaves it.
struct Loaded {
  File file;
  std::string error; // set when the file could not be read or parsed
  bool ok = false;
  std::vector<float> average; // per cube (CubeAverage)
  std::vector<float> peak;
};

struct Area {
  File file;
  bool hasFile = false; // the file was read
  // Load reads and parses the file on another thread; until Finish takes the result the area
  // looks like one without a file, and the script states it is sent wait in `queued`.
  std::future<Loaded> pending;
  uint32_t pendingFrames = 0;
  bool pendingFluid = false; // sPlayerFluid and sCameraWater when Load was called
  bool pendingWater = false;
  std::vector<std::pair<uint32_t, int>> queued; // sender, state
  std::vector<uint8_t> roomInside; // by file.roomVolumes entry: the player or camera is in it
  std::vector<uint8_t> roomActive; // by file.roomVolumes entry: its trigger is switched on
  uint32_t serial = 0;  // of this load, which the worker's jobs for it carry
  std::vector<GpuCube> cubes;
  std::vector<GpuVolume> volumes; // one a grid
  // Version 21 files (file.tiles) have no grid of their own: the world's one (World).
  float ownAverage = 0.f; // of the area's own tiles, for the exposure before the grid exists
  uint64_t loadSeq = 0;  // the order of Load calls, which is the order tiles are registered in
  bool registered = false; // its tiles are in sTiles
  bool settled = false;    // Finish ran (or the load failed): the next area may register
  uint32_t lightmap = 0; // GXCreatePBRLightmap's id; 0 until the worker has made it, or none
  float exposure = 0.f; // what takes the room's radiance to the display's range; 0: unknown
  float tone[3][4] = {}; // its tone curve
  ExposureHints hints;   // the fluid-mode exposure hints' state
  bool hintSnap = false; // a fluid hint registered this frame: the exposure snaps
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
  // A light's state (CLightDynamicGOC): active, and its timeline.
  struct Light {
    bool active = false;
    bool playing = false;
    bool forward = true;
    double time = 0.;
    int group = -1; // what its script group last said (PortRoomGeo::GroupShown)
  };
  std::vector<Light> lights;
  std::vector<Light> suns;
};

// The frame's exposure and tone curve, as CPostFXManager::UpdateTonemapping moves them.
struct FrameState {
  bool started = false;
  uint32_t area = 0; // the camera's room last frame
  std::vector<uint32_t> loaded; // the areas in memory last frame
  float ev = 0.f; // the frame's EV
  float targetEv = 0.f;
  bool measuring = false; // this frame is measured (MeasureExposure)
  // The tonemap's EV, mid, contrast, toe and shoulder, and the static EV (see GlowScale),
  // moving linearly from `from` to `to`.
  float from[6] = {};
  float to[6] = {};
  float shown[6] = {};
  // CPostFXManager::TransitionTonemapParams: while `moving`, shown = from + (to - from) *
  // progress, and a new target keeps both `from` and `progress`.
  bool moving = false;
  float progress = 0.f;
  std::chrono::steady_clock::time_point last;
  float carry = 0.f; // seconds not stepped yet
  float exposure = 0.f; // 2^(3 - EV); 0: none yet
  bool hasTone = false;
  float tone[3][4] = {};
};

enum class JobKind { Cube, Volume, Lightmap, Keep };

// A version 21 volume job's input: the area's own tiles and, per key of its region, the
// tiles of the area whose copy is mapped, as sTiles had them when the job was made. The
// shared tile sets keep the data alive whatever the areas do meanwhile.
struct VolumeInput {
  std::shared_ptr<const PortRoomEnvTiles::TileSet> own;
  std::map<PortRoomEnvTiles::TileKey, std::shared_ptr<const PortRoomEnvTiles::TileSet>> mapped;
  // The world texture's job (serial 0): every mapped key, composed in one space.
  bool world = false;
  float worldToPoint[12] = {};
};

struct Job {
  JobKind kind = JobKind::Cube;
  uint32_t area = 0;
  uint32_t serial = 0; // Area::serial
  size_t index = 0;    // of the cube or grid in the file
  const File* file = nullptr;
  bool raw = false; // Lightmap: the device takes its BC6H blocks as they are
  std::shared_ptr<const VolumeInput> input{}; // Volume of a version 21 file
  uint32_t gen = 0;                         // World::gen when it was made
};

struct Result {
  JobKind kind = JobKind::Cube;
  uint32_t area = 0;
  uint32_t serial = 0;
  uint32_t gen = 0;
  bool resolved = false; // Volume: `grid` is the composed one
  bool empty = false;    // Volume: the tiles leave no lit point
  Grid grid;             // Volume of a version 21 file
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
  // Drops the queued volume jobs of a load (a newer one replaces them) and the results of
  // them that are waiting; the one running ends on its own and TakeResults drops its result.
  void DropVolumes(uint32_t serial);
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

// Android keeps each grid under 24 MB so low-memory phones do not run out (halved above this);
// desktop keeps every grid whole, as Remastered samples them.
#ifdef __ANDROID__
constexpr size_t kMaxGridPoints = size_t(1) << 20;
#else
constexpr size_t kMaxGridPoints = 0;
#endif

// Remastered samples one texture for the whole world, by position alone, so a version 21
// file has no grid of its own: the port composes every mapped tile of every loaded area into
// one dense grid and volume (SubmitWorld), rebuilt when a tile changes owner or an area comes
// or goes. The old volume stays bound until the new one is on the GPU.
struct World {
  Grid grid;             // the composed grid; size 0 with none
  uint32_t volume = 0;   // GXCreatePBRVolume's id; 0 until the worker has made it
  uint32_t gen = 0;      // of the composition asked for; a result of an older one is dropped
  bool dirty = false;    // a tile changed owner since the last SubmitWorld
  bool has() const { return volume != 0 && grid.average > 0.f; }
};

// The colour grade on screen: fading from one LUT to another (0 is the identity).
struct GradeFade {
  bool started = false;
  uint32_t from = 0;
  uint32_t to = 0;
  float seconds = 0.f; // how long the fade to `to` takes
  float fadeOut = 0.f; // of `to`, for when the next room has no grade
  std::chrono::steady_clock::time_point start;
};

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

// The volumetric fog (Remastered's CVolumetricFogManager::UpdateSceneNode). The node shows
// `out`; a hint change starts an interpolation from it to the target, whose phase is the
// fade's spline at the seconds since the change (SVolumetricFogDynamicData::
// interpolate_by_phase). The defaults are SVolumetricFogDynamicData's constructor's.
struct FogCore {
  float range = 0.f, scatter = 0.f, absorb = 0.f, m1z = 0.f, decay = 0.f;
  float attenSlope = 0.f, attenBias = 1.f;
  float noiseFreq = 0.f, noiseStrength = 0.f, lightCap = 65535.f;
  float wind[3] = {};
  float worldShift[3] = {};
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

// The whole file, in one read when its size is known. The stream is left as a read
// through istreambuf_iterator leaves it: failed only when the file did not open.
using port::ReadAll;

// The state (port_room_env.cpp).
extern FrameState sFrame;
extern std::vector<uint32_t> sLoadedSpare;
extern int sAuto;
extern int sStatic;
extern int sAreaLights;
extern std::unordered_map<uint32_t, Area> sAreas;
extern uint32_t sNextSerial;
extern Worker sWorker;
extern PortRoomEnvTiles::TileTable sTiles;
extern std::unordered_map<uint32_t, std::shared_ptr<const PortRoomEnvTiles::TileSet>> sTileSets;
extern uint64_t sTileSeq;
extern World sWorld;
extern File sNoFile;
extern uint32_t sNextCube;
extern uint32_t sNextVolume;
extern int sVolumes;
extern int sBombTint;
extern float sPowerBombTime;
extern float sBakedLight[3];
extern float sAmbientScale;
extern float sVolumeView;
extern bool sHint;
extern bool sBrdfSent;
extern uint32_t sHintArea;
extern float sHintCentre[3];
extern float sHintLightmap[3];
extern int sEnabled;
extern int sExposure;
extern int sBloom;
extern uint32_t sViewArea;
extern int sGrade;
extern GradeFade sGradeFade;
extern uint32_t sGradeArea;
extern int sGradeIndex;
extern uint64_t sGradeOrder;
extern BacklightFade sBacklight;
extern uint64_t sBacklightOrder;
extern FogFade sFog;
extern uint64_t sFogOrder;
extern uint64_t sRegionOrder;
extern int sVolFog;
extern bool sFogRegions;
extern bool sPlayerFluid;
extern bool sCameraWater;
extern uint8_t sExposureSwap; // ExposureHints::SwapState of the frame
extern uint64_t sItems;
extern std::vector<WorldVolume> sWorldVolumes; // the world's camera volumes (SetCameraPoint)
extern WorldVolumeState sWorldVolumeState;
// The world's environment variables as they stand (EnvVar: its declaration, `value` its default),
// by guid; filled as areas load, emptied by ResetGrades.
struct EnvVarLive {
  EnvVar var;
  int32_t value = 0;
};
extern std::vector<EnvVarLive> sEnvVars;
// The ops of the world volumes' triggers, from the file that set sWorldVolumes.
extern std::vector<EnvOp> sWorldEnvOps;
extern std::unordered_set<uint32_t> sGradeLuts;
extern bool sLastValid;
extern bool sLastFound;
extern Selection sLast;
extern uint32_t sEpoch;
extern ProbeBlendState sBlend;

// The functions of the three parts.
const std::vector<Grid>& Grids(const Area& area);
void Invalidate();
float HalfToFloat(uint16_t h);
void Free(Area& area);
uint16_t FloatToHalf(float value);
bool Lightmaps();
bool FillVolume(const File& file, const Grid& grid, std::vector<uint8_t>& texels, const std::atomic<bool>& cancel);
bool ResolveVolume(const VolumeInput& input, const File* file, Result& out, const std::atomic<bool>& cancel);
float GridDistance(const Grid& grid, const float pos[3]);
float CubeAverage(const File& file, const Cube& cube, float& peak);
float RoomExposure(const Area& area);
void StartTransition(Area& area, size_t i);
void DriveTransitions(uint32_t mrea, Area& area, uint32_t sender, int state);
double QuantiseTime(double t);
void StepTransitions(Area& area, float dt);
void DriveLights(uint32_t mrea, Area& area, uint32_t sender, int state);
void StepLights(uint32_t mrea, Area& area, float dt);
void DriveGrades(uint32_t mrea, Area& area, uint32_t sender, int state);
// The camera has gone under water: what kSenderCameraWaterItem + k drives, by whether `items`
// has bit k.
void DriveCameraWaterItems(uint32_t mrea, Area& area, uint64_t items);
void StartGrades(uint32_t mrea, Area& area, bool playerFluid, bool cameraWater);
void ReplayWorldVolumes(uint32_t mrea, Area& area);
// Adds the area's declared environment variables the table lacks, then drives its onLoad branches
// that hold.
void LoadEnvVars(uint32_t mrea, Area& area);
// Runs the ops of `ops` that `sender` fires on `state`, then the onChange branches of every loaded
// area for each var changed.
void RunEnvOps(const std::vector<EnvOp>& ops, uint32_t sender, int state);
bool LoadTiming();
double MsSince(std::chrono::steady_clock::time_point t);
Loaded ReadRoomFile(const std::string& path);
void Load(uint32_t mrea, Area& area);
void SubmitWorld();
void RegisterReady();
void Finish(uint32_t mrea, Area& area);
void FinishLoads();
float FrameExposure(const Area& area);
float LightGain();
void KeepData(const File& file, std::vector<uint8_t>& kept, std::vector<size_t>& offsets);
bool DecodeCube(const File& file, const Cube& cube, std::vector<uint16_t>& texels, const std::atomic<bool>& cancel);
bool MakeLightmap(const File& file, bool raw, Result& out, const std::atomic<bool>& cancel);
void TakeWorld(Result& result);
void TakeResults();

}  // namespace EnvDetail
}  // namespace PortRoomEnv
