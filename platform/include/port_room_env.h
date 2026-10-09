#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "port_maya_spline.h"

// A room's lighting environment for PBR models: reflection probes, each a box of the world
// and a prefiltered HDR cube map of what surrounds it. A mod supplies one per area as
// <MREA id>.roomenv; the port reflects the cube of the probe a model stands in, in place
// of its own live probe. It can also hold the room's baked ambient light: a grid of
// points, each with the light arriving there by direction, which replaces the game's one
// ambient colour.
//
// The file is little endian:
//   'MPEV', u32 version (1 to 18), f32 tonemap[4], u32 probes, u32 cubes
//   probe: f32 worldToBox[12], f32 worldToCube[9], s32 layer, u32 cube, f32 scale, f32 padding
//          and from version 8 on, s32 priority, f32 intensity min, f32 intensity max
//          (before it the padding is unused, 1 m, and the rest 0, 0, 1). From version 9
//          on the layer is the retail script layer the probe is on (-1: every layer);
//          before it, it is unused.
//   cube:  u32 size, u32 mips, u32 signed, u32 bytes, then BC6H blocks, every face of
//          mip 0, then of mip 1 and so on
// Version 2 goes on:
//   u32 grids
//   grid:  f32 worldToGrid[12], u32 size[3], then size[0] * size[1] * size[2] points of 24
//          bytes, x fastest and z slowest
//   point: half mean[3], half lobe[3], u8 sharpness[3], u8 direction[3][3] (of red, green
//          and blue, along the grid's axes; 0..255 is -1..1). A mean of zero is no point
//          (inside a wall).
// Version 3 goes on:
//   f32 exposure[2], the lowest and highest exposure value the room's auto exposure may
//          settle on (0, 0: the room has no auto exposure)
// Version 4 goes on:
//   f32 bias, what the room's auto exposure adds to the exposure value it measures
//   f32 contrast, of the tonemap (0 to 1)
// Version 5 goes on:
//   f32 bloom threshold, of the exposed luminance
//   u32 tints, then that many f32 RGBA: the bloom's colour per level (0 tints: no bloom)
// Version 6 goes on:
//   u32 grades
//   grade: s32 layer (-1: every layer), f32 fadeIn, f32 fadeOut (seconds), then a colour
//          grade LUT of 33^3 RGBA8, red fastest, blue slowest; the frame's tonemapped
//          colour looks itself up in it. Of the grades whose layer is active, the last
//          one is the room's.
// Version 7 goes on:
//   f32 sigma, of the Gaussian that eases the auto exposure towards what it measures, in
//          frames at 60 Hz
//   f32 static lerp, where in the hint's range the exposure value of emissive and unlit
//          surfaces sits (0 the lowest, 1 the highest)
// From version 10 on a grade has, between fadeOut and the LUT:
//   u8 on (the grade is requested from the start), u8 pad[3], s32 priority, u32 links,
//   then links of u32 sender, u8 state, u8 action (PortRoomGeo::LinkAction: show = on,
//   hide = off, toggle), u16 0. A sender is a retail script object's id (its state is an
//   EScriptObjectState), or kSenderPlayerFluid / kSenderCameraWater (state 0 when the
//   player or the camera goes into a fluid, 1 when it comes out). Of the grades that are on
//   and whose layer is active, the highest priority is the room's; of equals, the one
//   turned on last. Before it, every grade is on, with priority 0.
// Version 11 goes on (after the version 7 floats):
//   u32 backlights
//   backlight: s32 layer, f32 fadeIn, f32 fadeOut, u8 on, u8 pad[3], s32 priority, f32 top,
//          f32 back, u32 links, then links as a grade's. Picked as a grade is. The top and
//          back strengths scale the character backlight; with none picked they are 2 and 4.
// Version 12 goes on (after the backlights):
//   u32 fogs
//   fog: s32 layer, f32 fadeIn, f32 fadeOut, u8 on, u8 pad[3], s32 priority, then f32 range,
//          scatter, absorb, m1z, decay, attenSlope, attenBias, noiseFreq, noiseStrength,
//          lightCap, f32 wind[3], u8 useScriptWind, u8 noProbe, u8 pad[2], f32 colorB[4],
//          f32 colorA[4] (already times the intensity), f32 lut[64], u32 links, then links as
//          a grade's. A Remastered VolumetricFogHint plus the VolumetricFog on its entity,
//          picked as a grade is. decay is -ln(residual transmittance at range) / range, the
//          LUT the density profile at distance (i/63)^2 * range, and the height term is
//          clamp(z * attenSlope + attenBias, 0, 1) over retail world z. The wind is in
//          retail world axes, metres a second; it applies when useScriptWind (the property's bool: the script's
//          vector rather than the world's wind, which the port has no source for, so none).
// Version 13 adds, after each fog's links, its fade-in and fade-out interpolations: each a
//   u32 size, then that many bytes of a CMayaSpline (port_maya_spline.h) from elapsed
//   seconds to blend phase, padded to 4. Size 0: none, the fog changes at once. fadeIn and
//   fadeOut are then the splines' last key times (for display).
// Version 14 goes on (after the fogs):
//   u32 regions
//   region: s32 layer, u8 on, u8 fluid (0 always, 1 while the camera is in a fluid, 2 while
//          it is not), u8 hasColor, u8 hasCap, f32 m[3][4], f32 edgeScale[3], f32 mult,
//          f32 edgeBias[3], f32 cap, f32 color[4], f32 density, f32 box[6] (retail world
//          min, max), u32 links, then links as a grade's. A Remastered VolumetricFogRegion
//          as CVolumetricFogRegionGOC makes it (see FogRegion).
// Version 15 adds, after each region's links, f32 distance, f32 transmittance, u8 subtract,
//   u8 pad[3], and goes on (after the regions):
//   u32 transitions
//   transition: u32 region (index in this file), s32 layer, u8 on, u8 autoStart, u8 loop,
//          u8 select (bit 0 distance, 1 transmittance, 2 colour, 3 cap), f32 distance,
//          f32 transmittance, f32 color[4] (rgb already times the intensity), f32 cap, u32 size
//          and that many bytes of the phase's CMayaSpline padded to 4, u32 links, then links
//          as a grade's with the FogTransition actions. A Remastered
//          VolumetricFogRegionTransition (see FogTransition).
// Version 16 adds, after the transitions:
//   u32 suns
//   sun: s32 layer, u8 on, u8 pad[3], f32 toSun[3] (retail world, unit), f32 color[3] (linear,
//        times the intensity). A Remastered directional LightDynamic (see Sun).
// Version 17 adds to each sun: u32 group (PortRoomGeo::kNoGroup: none), the area's .roomgeo
//   script group that shows and hides it; until that script has, `on` holds.
// Version 18 adds, after the suns, the room's baked lightmap (the .roomgeo's LMAP lookups index
//   it): u32 width, which is 0 when there is none; else u32 height, u32 layers (4 or more:
//   colour, then the light's x, y, z), u32 signed, u32 bytes, then the BC6H blocks of mip 0 of
//   every layer, layer-major, each ceil(width / 4) x ceil(height / 4) blocks, rows in stored order.
// Version 19 adds, after the lightmap:
//   u32 lights
//   light: s32 layer, u8 on, u8 spot, u8 falloff (1 linear, 2 squared, 3 smooth), u8 pad,
//          f32 pos[3] (retail world), f32 toLight[3] (spots: unit, against the beam), f32 color[3]
//          (linear, times the intensity), f32 near, f32 far (already times the entity's scale),
//          f32 inner, f32 outer (cone, full angles in degrees), u32 group (as a sun's),
//          u32 links, then links as a grade's with the light actions (kLightStart...),
//          u8 animated, u8 playing, u8 loop, u8 offAtStart, u8 animFalloff, u8 pad[3],
//          f32 length, f32 scale x, f32 scale y, f32 intensity, f32 rgb[3] (the colour's
//          two factors, for a timeline without their spline), then kLightSplines splines (PointLight's
//          order), each u32 bytes (0: none) and that many bytes of a CMayaSpline padded to 4.
//          A Remastered point or spot LightDynamic (see RoomLights).
// The tonemap is Remastered's: the exposure value without auto exposure, the radiance
// that comes out as middle grey once exposed, and how far the curve's toe and shoulder
// are pulled in.
// The cubes are stored normalised; a probe's scale times its cube is the radiance, in the
// same units as the grid's points.
namespace PortRoomEnv {

struct Probe {
  // Rows of world -> box; a point is inside when every coordinate is within -1..1.
  float worldToBox[12];
  // Rows of world direction -> cube lookup direction.
  float worldToCube[9];
  int32_t layer = -1; // the area's script layer it is on; -1: every layer (ProbeOn)
  uint32_t cube;
  float scale; // Remastered's intensity
  // Remastered's ReflectionProbe: how far outside the box (metres) the probe fades out, which
  // probes win where boxes overlap, and the intensity's range for the ambient occlusion
  // (see UpdateBlend).
  float padding = 1.f;
  int32_t priority = 0;
  float intensityMin = 0.f;
  float intensityMax = 1.f;
  // Worked out from worldToBox when the file is parsed (SetExtents): the box's half extent
  // along each row (0 for a row of length 0), and its volume, 8 times their product.
  float half[3];
  float volume;
};

struct Cube {
  uint32_t size = 0;
  uint32_t mipCount = 0;
  bool isSigned = false;
  // Of the blocks, in File::data. Both 0 once the game has made the cubes and dropped the
  // blocks from its copy of the file (port_room_env.cpp keeps only grids and grades).
  size_t offset = 0;
  size_t length = 0;
};

// The room's baked lightmap: a BC6H array texture of `layers` layers, mip 0 only.
struct Lightmap {
  uint32_t width = 0; // 0: the room has none
  uint32_t height = 0;
  uint32_t layers = 0;
  bool isSigned = false;
  // Of the blocks, in File::data; layer l starts at offset + l * length / layers.
  size_t offset = 0;
  size_t length = 0;
};

struct Grid {
  // Rows of world -> grid; point (i, j, k) is at grid coordinate (i, j, k).
  float worldToGrid[12];
  uint32_t size[3] = {};
  size_t offset = 0; // of the points, in File::data
  float average = 0.f; // geometric mean of the lit points' luminance
};

constexpr uint32_t kGradeLutSize = 33;
constexpr size_t kGradeLutBytes = size_t(kGradeLutSize) * kGradeLutSize * kGradeLutSize * 4;

// What drives a grade other than a script object: the player or the camera in a fluid.
constexpr uint32_t kSenderPlayerFluid = 0xfffffff0;
constexpr uint32_t kSenderCameraWater = 0xfffffff1;

struct GradeLink {
  uint32_t sender = 0;
  uint8_t state = 0;
  uint8_t action = 0;
};

struct Grade {
  int32_t layer = -1;
  float fadeIn = 0.f;
  float fadeOut = 0.f;
  bool on = true;  // requested from the start (Remastered's global hints)
  int32_t priority = 0;
  std::vector<GradeLink> links;
  size_t offset = 0; // of the LUT, in File::data
  uint32_t id = 0;   // a hash of the LUT, never 0; 0 here means the LUT is the identity
};

// A Backlight hint: the grade's fields, with the character backlight's top and back
// strengths in place of the LUT.
struct BacklightHint {
  int32_t layer = -1;
  float fadeIn = 1.f;
  float fadeOut = 1.f;
  bool on = false;
  int32_t priority = 50;
  float top = 1.f;
  float back = 1.f;
  std::vector<GradeLink> links;
};

// A VolumetricFogHint with the fog it picks (SVolumetricFogDynamicData's fields).
// The defaults are SVolumetricFogDynamicData's constructor's.
struct FogHint {
  int32_t layer = -1;
  float fadeIn = 0.f;
  float fadeOut = 0.f;
  // Version 13: the interpolations (has* false: none). Older files fade linearly over
  // fadeIn/fadeOut seconds.
  bool hasFadeInSpline = false;
  bool hasFadeOutSpline = false;
  bool linearFade = false;
  PortMayaSpline fadeInSpline;
  PortMayaSpline fadeOutSpline;
  bool on = false;
  int32_t priority = 50;
  float range = 0.f;
  float scatter = 0.f;
  float absorb = 0.f;
  float m1z = 0.f;
  float decay = 0.f;
  float attenSlope = 0.f;
  float attenBias = 1.f;
  float noiseFreq = 0.f;
  float noiseStrength = 0.f;
  float lightCap = 65535.f;
  float wind[3] = {};
  bool useScriptWind = false;
  bool noProbe = false;
  float colorB[4] = {1.f, 1.f, 1.f, 1.f};
  float colorA[4] = {};
  float lut[64] = {};
  std::vector<GradeLink> links;
};

// A VolumetricFogRegion (CVolumetricFogRegionGOC::AddRegion, RebuildPositionalData). The
// rows of m take a retail world point to 0..1 over the region's box; with l = 2 * that - 1,
// its mask is clamp(min over the axes of |l| * edgeScale + edgeBias, 0, 1), and with
// k = 1 + mask * (mult - 1) the fog's scattering colour becomes max(0, mask * color + c * k),
// its density max(0, d * k + mask * density) and its light cap max(0, cap' * k + mask * cap).
// A subtracting region has negative values, an overriding one mult 0. Without hasColor the
// colour is (0, 0, 0, 1) when mult is not 0, else the fog's; without hasCap the cap is 0 when
// mult is not 0, else the fog's.
struct FogRegion {
  int32_t layer = -1;
  bool on = false;
  uint8_t fluid = 0;
  bool hasColor = false;
  bool hasCap = false;
  float m[12] = {};
  float edgeScale[3] = {};
  float mult = 1.f;
  float edgeBias[3] = {};
  float cap = 0.f;
  float color[4] = {};
  float density = 0.f;
  float box[6] = {};
  std::vector<GradeLink> links;
  // Version 15: what the density is made of (-ln transmittance / distance, negated when
  // subtracting), which a transition starts from and sets.
  float distance = 250.f;
  float transmittance = 0.01f;
  bool subtract = false;
};

// A VolumetricFogRegionTransition (CVolumetricFogRegionTransitionGOC): a playback of `phase`
// over its keys' times (CTimePlaybackManager, in steps of 1/60000 s; `loop` wraps it) that
// moves its region from the state it had at Start towards the selected targets. Each frame it
// plays: p = clamp(phase(time), 0, 1); a field both have lerps by p, one only the start or the
// target has is that one; the region then takes distance and transmittance (its density
// recomputed), colour and cap, negated when it subtracts. The state stays when it stops.
// Links: kShow / kHide set it active (it plays only while active and its layer is), Start
// starts it, Restart rewinds and starts it, Stop pauses it (each only while active), Delete
// stops it for good. It starts with the area when autoStart.
constexpr uint8_t kTransitionStart = 16;
constexpr uint8_t kTransitionRestart = 17;
constexpr uint8_t kTransitionStop = 18;
constexpr uint8_t kTransitionDelete = 19;
constexpr uint32_t kNoFogRegion = ~uint32_t(0); // a transition that moves nothing
struct FogTransition {
  uint32_t region = kNoFogRegion; // an index into File::regions
  int32_t layer = -1;
  bool on = true;
  bool autoStart = false;
  bool loop = false;
  uint8_t select = 0; // bit 0 distance, 1 transmittance, 2 colour, 3 cap
  float distance = 250.f;
  float transmittance = 0.01f;
  float color[4] = {1.f, 1.f, 1.f, 1.f};
  float cap = 0.f;
  PortMayaSpline phase;
  std::vector<GradeLink> links;
};

struct SunLight {
  int32_t layer = -1;
  bool on = true;
  float toSun[3] = {};
  float color[3] = {};
  uint32_t group = 0xffffffff;  // PortRoomGeo::kNoGroup
};

// A point or spot LightDynamic (CLightDynamicGOC). Static: colour, range and cone as stored.
// Animated (its Attributes' animated flag): a playback over `length` seconds
// (CTimePlaybackManager, steps of 1/60000 s; it advances only while playing and active,
// stops at the end unless `loop` wraps it; backwards mirrors that) gives t, and then the
// intensity is intensity(t), the colour the gradient (r, g, b) at saturate(color(t)), near and
// far near(t) and far(t) times the entity's scale x and y, the cone inner(t) and outer(t);
// a missing spline keeps the static value. `offAtStart`: its reached-start event (a backward
// play ending at t = 0) deactivates it; the forward end doesn't.
// Links: kShow / kHide / kToggle set it active; the light actions (each only while active):
// Start plays from where it is, Stop pauses, Reset rewinds to 0, Forward / Backward set the
// direction, Reverse flips it.
constexpr uint8_t kLightStart = 20;
constexpr uint8_t kLightStop = 21;
constexpr uint8_t kLightReset = 22;
constexpr uint8_t kLightForward = 23;
constexpr uint8_t kLightBackward = 24;
constexpr uint8_t kLightReverse = 25;
enum LightSpline : uint8_t {
  kLightSplineIntensity,
  kLightSplineNear,
  kLightSplineFar,
  kLightSplineInner,
  kLightSplineOuter,
  kLightSplineColor,
  kLightSplineGradient, // r, g, b, a
  kLightSplines = kLightSplineGradient + 4,
};
struct PointLight {
  int32_t layer = -1;
  bool on = true;
  bool spot = false;
  uint8_t falloff = 3;
  float pos[3] = {};
  float toLight[3] = {};
  float color[3] = {};
  float nearFar[2] = {};
  float cone[2] = {};  // inner, outer
  uint32_t group = 0xffffffff;  // PortRoomGeo::kNoGroup
  std::vector<GradeLink> links;
  bool animated = false;
  bool playing = false;
  bool loop = false;
  bool offAtStart = false;
  uint8_t animFalloff = 3;
  float length = 0.f;
  float scale[2] = {1.f, 1.f};
  float intensity = 0.f;
  float rgb[3] = {};
  PortMayaSpline splines[kLightSplines];
  bool hasSpline[kLightSplines] = {};
};

struct File {
  uint32_t version = 0;
  float tonemap[4] = {};
  float exposure[2] = {}; // EV range; both 0 when the room has no auto exposure
  float exposureBias = 0.f;
  float exposureSigma = 32.f; // Remastered's default
  float staticLerp = 0.5f;
  float contrast = 0.f;
  float bloomThreshold = 0.9f;
  std::vector<float> bloomTints; // RGBA; empty: the room has no bloom
  std::vector<Grade> grades;
  std::vector<BacklightHint> backlights;
  std::vector<FogHint> fogs;
  std::vector<FogRegion> regions;
  std::vector<FogTransition> transitions;
  std::vector<SunLight> suns;
  std::vector<PointLight> lights;
  std::vector<Probe> probes;
  std::vector<Cube> cubes;
  std::vector<Grid> grids;
  Lightmap lightmap;
  std::vector<uint8_t> data;
};

// --- The file (port_room_env_file.cpp; no game or GX dependencies) -------------

// "1A2B3C4D.roomenv" (any case) -> 0x1A2B3C4D.
bool ParseFileName(const std::string& fileName, uint32_t& id);
bool Parse(std::vector<uint8_t>&& data, File& out, std::string& error);
// The roomenv/brdf.lut a mod supplies: a 16x8 RG8 table of Remastered's environment BRDF
// (scale in red, bias in green). The file must be exactly this long.
constexpr size_t kBrdfLutSize = 16 * 8 * 2;
bool ValidBrdfLut(const std::vector<uint8_t>& data, std::string& error);
// Remastered's baked-lighting modulation in a power bomb's flash
// (CPowerBombMP1::UpdateBakedLightingColorModulation), `seconds` after the bomb went off:
// white until 1.75 s, towards (1, 0.643, 0.298) x 35 by 3.5 s, held to 4 s, back to white by
// 4.5 s. A negative time (no bomb) is white.
void PowerBombBakedLight(float seconds, float rgb[3]);
// Bytes of BC6H a cube of this size has.
size_t CubeBytes(uint32_t size, uint32_t mipCount);

struct Pick {
  int probe = -1;
  bool inside = false;
  // Inside: the box's volume. Outside: the distance to the box. Smaller is better.
  float score = 0.f;
  bool Better(const Pick& other) const {
    return probe >= 0 && (other.probe < 0 || (inside != other.inside ? inside : score < other.score));
  }
};
// Whether a probe is in the room now: its layer is one of `activeLayers` (bit n: layer n),
// as Remastered's ReflectionProbe only registers while its layer is loaded.
bool ProbeOn(const Probe& probe, uint64_t activeLayers);

// The probe for a point: the smallest box that holds it, else the nearest one. Probes off
// `activeLayers` are left out. Reads the probes' half extents and volumes, which Parse
// fills in.
Pick PickProbe(const File& file, const float pos[3], uint64_t activeLayers = ~uint64_t(0));
// Fills in a probe's half extents and volume from its worldToBox.
void SetExtents(Probe& probe);

// How much of a probe reaches a point, as Remastered fades it: 1 inside the box, falling to
// 0 at `padding` metres outside it (by the farthest axis). `inside`: within the box itself.
float ProbeFade(const Probe& probe, const float pos[3], bool& inside);

// Remastered's blend of reflection probes (CReflectionProbeManager), for the camera's
// position. Each frame: probes of the last frame stay while the point is within their padding,
// new ones join when it is, then the list is sorted by priority (the old ones first among
// equals) and cut to four. Down that list each takes its fade of what the ones above left,
// and one whose box holds the point takes all of the rest.
struct BlendCandidate {
  uint64_t key = 0; // stays the same for a probe across frames
  const Probe* probe = nullptr;
};
struct BlendEntry {
  uint64_t key = 0;
  const Probe* probe = nullptr;
  float weight = 0.f; // of the probe's cube in the blended one; together they make 1
};
struct Blend {
  std::vector<BlendEntry> entries;
  // The blended intensity (each probe's scale by its share), which the blended cube is
  // multiplied by, and the range the shader's ambient occlusion maps into: the reflection is
  // multiplied by mix(min, intensity, saturate(ambient / max)) in place of the intensity.
  // A lone probe keeps its own values, whatever its fade.
  float intensity = 1.f;
  float min = 0.f;
  float max = 1.f;
};
constexpr size_t kMaxBlend = 4;
// `blend` holds the last frame's list on input. Candidates whose keys repeat count once.
void UpdateBlend(const BlendCandidate* candidates, size_t count, const float pos[3], Blend& blend);

// The baked ambient at a point, as Remastered's shaders evaluate it: per colour channel c
// and for a surface normal n,
//   mean[c] - lobe[c] + 2 * lobe[c] * (1 + sharpness[c]) * q ^ (1 + 2 * sharpness[c]),
//   q = clamp(0.5 + 0.5 * dot(n, direction[c]), 0, 1)
// which averages to the mean over all n. The directions are in world space and not unit
// length.
struct Ambient {
  float mean[3];
  float lobe[3];
  float sharpness[3];
  float direction[3][3];
};
// Blends the points around `pos`, skipping the empty ones; false when there are none.
bool SampleGrid(const File& file, const Grid& grid, const float pos[3], Ambient& out);

// Remastered's CGaussianConvergence: a recursive Gaussian (Young and van Vliet) that a
// value follows its target through, one step a 60 Hz frame,
//   y = B x + b1 y1 + b2 y2 + b3 y3
// so a step in the target becomes an S curve about `sigma` frames long.
struct Convergence {
  float value = 0.f;
  // Doubles: with sigma 32, B is ~1e-4 against feedback terms near 3, so float rounding
  // leaves the value short of its target.
  double history[3] = {}; // the last three values, newest first
  double coeff[4] = {1.0, 0.0, 0.0, 0.0}; // B, b1, b2, b3
  void SetSigma(float sigma);
  // Jumps there, with no easing.
  void SetValue(float v);
  void Step(float target);
};

// --- The game side (port_room_env.cpp) -----------------------------------------

// The areas in memory now. Loads the files of new ones and frees those of areas that left.
// A new area's cubes are decoded and its volumes filled in on a worker thread, and handed
// to the GPU by UpdateFrame; until each is, Select goes without it.
void SetLoadedAreas(const uint32_t* mreas, size_t count);
// The script layers of an area in memory that are active (bit n: layer n); every layer
// until it is set.
void SetAreaLayers(uint32_t mrea, uint64_t active);

struct Selection {
  uint32_t cube = 0;       // for GXSetPBRCube; 0 when the room has none
  float params[4] = {};    // for GXSetPBRCube
  float worldToCube[9] = {};
  // The probe's own intensity inside params[0] (the blended one, or the lone probe's
  // scale; 1 without the room's exposure). Remastered's water takes the cube without it.
  float cubeIntensity = 1.f;
  // For GXSetPBRProbeEx, Remastered's ambient occlusion of the reflection: where the baked
  // light is dark the cube drops to `occlusionMin` of its level, reaching all of it at
  // 1 / `occlusionInvMax` of radiance (0: no occlusion).
  float occlusionMin = 0.f;
  float occlusionInvMax = 0.f;
  bool hasAmbient = false;
  // For GXSetPBRAmbient: scaled so that the game's ambient level multiplies it, with the
  // directions still in world space.
  float ambient[6][3] = {};
  // The ambient is the light itself, at the room's exposure; the game's level stays out.
  bool ambientAbsolute = false;
  // For GXSetPBRVolume, when the model was announced with SetVolumeHint: the grid as
  // textures, which the shader reads per pixel in place of the one sample above.
  uint32_t volume = 0;
  float worldToVolume[12] = {}; // rows of world -> texture coordinates
  float worldToAxes[9] = {};    // rows of world direction -> the grid's axes
  float volumeLevel = 0.f;      // what the baked light is multiplied by
  float volumeBias = 0.f;       // metres off the surface the sample is taken
  float volumeDiagnostic = 0.f; // MP_ROOM_ENV_VOLUME_SHOW: 1 texture coordinates, 2 the light
  // For GXSetPBRLightmap, when the model was announced with SetLightmapHint: the area's baked
  // lightmap, which then lights it in place of the volume (0: none).
  uint32_t lightmap = 0;
  float lightmapRect[4] = {};    // offU, offV, scale, level
  float worldToLightmap[9] = {}; // rows of world direction -> the lightmap's axes
};
// The room cube and baked ambient for a model at `pos`; false when no loaded area has
// either (or MP_ROOM_ENV=0).
bool Select(const float pos[3], Selection& out);
// Room geometry is lit by the baked ambient alone, per pixel. It announces the area and
// the middle of what it draws next, and the following Select answers with that area's
// grid as a volume (and picks the cube by that point, not the model's origin); Clear
// when it is done. MP_ROOM_ENV_VOLUME=0 turns volumes off.
void SetVolumeHint(uint32_t mrea, const float centre[3]);
// After SetVolumeHint: the instance's lookup into its area's lightmap (offU, offV, scale; see
// PortRoomGeo's LMAP), until ClearVolumeHint. A scale of 0 is none. MP_ROOM_ENV_LIGHTMAP=0
// turns lightmaps off; MP_ROOM_ENV_LIGHTMAP_SCALE multiplies their light.
void SetLightmapHint(const float lookup[3]);
void ClearVolumeHint();
// Remastered samples a model's baked probe at the world centre of its local box. The
// first-person gun and grapple arm instead sample at the player's position
// (SetLightProbeEvaluationWorldPosition); this overrides the sample point of the draws
// made while it is set. ProbeOverride answers false when none is set.
void SetProbeOverride(const float pos[3]);
void ClearProbeOverride();
bool ProbeOverride(float pos[3]);
struct ProbeOverrideScope {
  explicit ProbeOverrideScope(const float pos[3]) { SetProbeOverride(pos); }
  ~ProbeOverrideScope() { ClearProbeOverride(); }
  ProbeOverrideScope(const ProbeOverrideScope&) = delete;
  ProbeOverrideScope& operator=(const ProbeOverrideScope&) = delete;
};
// Whether a model announced for this area would get a volume: false until all of the
// area's volumes are on the GPU.
bool HasVolume(uint32_t mrea);
// MP_ROOM_ENV_VOLUME, the console's `roomenv volume`.
void SetVolumesEnabled(bool on);
bool VolumesEnabled();
// What the baked ambient is multiplied by; 0 leaves the game's own ambient
// (MP_ROOM_ENV_AMBIENT, the console's `roomenv ambient`).
void SetAmbientScale(float scale);
float AmbientScale();
// What volume-lit surfaces show, for debugging: 0 the shaded surface, 1 the volume's
// texture coordinates, 2 the baked light alone (MP_ROOM_ENV_VOLUME_SHOW, the console's
// `roomenv show`).
void SetVolumeView(int view);
int VolumeView();
// The GPU cube of a material's own reflection cube, the mod's <fileId>.envcube (see
// PortRemasteredConvert's Cube for the format), made the first time it is asked for; 0
// when the mod has none or it cannot be read. `params` gets what GXSetPBRCube takes with it.
uint32_t MaterialCube(uint32_t fileId, float params[4]);
// Forgets everything (the mods folder changed).
void Reset();
// 0 off, 1 on; the console's `roomenv`.
void SetEnabled(bool enabled);
bool Enabled();
// Whether cubes and ambient are exposed the way Remastered exposes a frame, by the
// radiance of the room the camera is in and that room's exposure hint, and shaped by its
// tone curve (MP_ROOM_ENV_EXPOSURE=0 turns it off, as does the console's `roomenv
// exposure`). Otherwise each cube is exposed to middle grey and the ambient takes the
// game's level.
void SetRoomExposed(bool on);
bool RoomExposed();
// The frame's bloom (Remastered's CRenderPass_Bloom): the threshold of exposed luminance
// above which light blooms, and the five tints (rgb), the last for the bright pass and the
// others for the four levels it is spread over, coarsest first. False when the camera's
// room has none, the frame has no tone curve (see Tone), or MP_BLOOM=0.
bool Bloom(float& threshold, float tints[5][3]);
// MP_BLOOM, the console's `bloom`.
void SetBloomEnabled(bool on);
bool BloomEnabled();
// The frame's colour grade (Remastered's ColorGrade + ColorGradeHint): the LUTs to blend,
// for GXPortPostProcess (0: the identity) and how far towards `b` (0 to 1). `layerActive`
// says whether a layer of the camera's area is active. Moving between grades fades over
// the new one's fade-in, or the old one's fade-out when it was turned off. False when there
// is nothing to grade, or MP_COLOR_GRADE=0.
using LayerActive = bool (*)(int32_t layer, void* context);
bool ColorGrade(LayerActive layerActive, void* context, uint32_t& a, uint32_t& b, float& weight);
// Remastered's character backlight hints (CBacklightManager), picked as the colour grade's
// are. UpdateBacklight runs once a frame before the world is drawn: the camera area's pick
// moves the strengths, a linear fade of both over the new hint's fade-in (set at once when
// there was no hint), or over the old hint's fade-out back to the defaults. Backlight reads
// the strengths now (the defaults 2 and 4 when there is no hint, no file, or MP_ROOM_ENV is
// off). It returns false when the strengths are the defaults for lack of any hint file.
void UpdateBacklight(LayerActive layerActive, void* context);
bool Backlight(float& top, float& back);
// The camera area's sun for real-time shadows: of its directional lights that are on (as its
// room geometry's script last set them, else as they start), on an active layer and shining
// down, the strongest. A sun on a story layer (the hangar intro's) only lights its cinematic. `color` is as the PBR shader's lights take it:
// the light's at the frame's exposure, over pi. False when there is none, no file, the rooms
// aren't exposed (RoomExposed), or MP_ROOM_ENV is off.
bool Sun(LayerActive layerActive, void* context, bool cinematic, float toSun[3], float color[3]);
// The camera area's suns, one line each, and what the last Sun call made of them (console).
std::string SunInfo();
// The loaded areas' point and spot lights that shine now (active, on an active layer, not black),
// in GXPortSetRoomLights' records: 16 floats each, appended to `out`. `worldToView` holds the
// view matrix's three rows (retail world -> the view space the PBR shader lights in). Their
// timelines step in UpdateFog. Nothing when the rooms aren't exposed (see Sun), MP_ROOM_LIGHTS=0
// or MP_ROOM_ENV is off.
void RoomLights(const float worldToView[3][4], std::vector<float>& out);
// The console's `shadow`: each loaded area's point and spot lights and their state.
std::string RoomLightInfo();
// Remastered's volumetric fog hints (CVolumetricFogManager), picked as the grade's are.
// UpdateFog runs once a frame, `dt` seconds long, before the world is drawn. A change of the
// camera area's pick starts an interpolation from the fog on screen (SVolumetricFogDynamicData's
// defaults before any) with the new hint's fade-in, or the old hint's fade-out when none is
// left (towards the same fog with no density). Its phase is the fade's spline at the seconds
// since the change, unclamped: scalars and colours lerp, the wind snaps, `noProbe` snaps
// halfway and the LUT lerps in distance space. A hint without a fade, or a change on the first
// update after ResetGrades (a fresh state manager, update count 0), changes at once. The
// fog draws while a hint is picked or an interpolation runs. noiseOffset is the shown wind
// times the seconds since the start; a hint that uses the world's wind has none here.
struct Fog {
  float range, scatter, absorb, density; // density: the decay (0: no fog)
  float attenSlope, attenBias;           // height term over retail world z
  float noiseFreq, noiseStrength, lightCap;
  float noiseOffset[3]; // wind times elapsed time (world units)
  bool noProbe;               // lit by Remastered's default white volume, not the probes
  float colorB[4], colorA[3]; // colorB.a scales the volume's light; colorA already times the intensity
  float lut[64];
};
void UpdateFog(LayerActive layerActive, void* context, float dt);
// The fog to draw; false when there is none (no hint and the fade done, density about 0,
// MP_VOLFOG=0, or the environment is off).
bool VolumetricFog(Fog& out);
// The fog regions that are on now (CVolumetricFogRegionGOC's active state: requested on, on an
// active script layer of their area, and their fluid gate open for the camera), across the
// loaded areas in the order they loaded, each area's in file order. The fog pass takes the
// first 8 whose box is in view. Empty when the environment or MP_VOLFOG is off.
void FogRegions(std::vector<const FogRegion*>& out);
// MP_VOLFOG (default on), the console's `roomenv volfog`.
void SetVolFogEnabled(bool on);
bool VolFogEnabled();
// Whether Remastered's fog stands for the camera's room: the environment and MP_VOLFOG are on
// and the room's file has the fog hints (version 12 on), hinted or not. Remastered has no
// distance fog, only the volumetric one, and none in a room without a hint: there's no area
// fog, CScriptDistanceFogMP1 only sets the thermal and world-light fades, and the fog volume
// special function draws nothing (no Render or AddToRenderer of its own).
bool FogOwnsRoom();
// Debug: leave the fog regions out (console `roomenv fogregions on|off`).
void SetFogRegionsEnabled(bool on);
// The console's `roomenv fog`: the fog now, and each fog hint of the camera area.
std::string FogInfo();
// A retail script object (`sender`: its editor id without the area bits) of the area `mrea`
// sent `state`: the grades it drives turn on or off.
void OnScriptState(uint32_t mrea, uint32_t sender, int state);
// The console's `roomenv state`: as if `sender` sent `state` in every loaded area.
void SendScriptState(uint32_t sender, int state);
// Once a frame: whether the player and the camera are in a fluid. A change drives the
// grades of the loaded areas linked to kSenderPlayerFluid / kSenderCameraWater.
void SetFluid(bool player, bool camera);
// A new game: every grade back to how it starts.
void ResetGrades();
// The console's `roomenv grades`: the camera area's grades and which are on.
std::string GradeInfo();
// MP_COLOR_GRADE, the console's `grade`.
void SetColorGradeEnabled(bool on);
bool ColorGradeEnabled();
// The area the camera is in: its exposure and tone curve are the frame's.
void SetViewArea(uint32_t mrea);
// Once a frame, after SetViewArea: moves the frame's exposure and tone curve on
// (CPostFXManager::UpdateTonemapping). The tonemap moves linearly to the camera room's
// over a second; with an auto exposure hint, the exposure value eases through a
// Convergence towards the one measured from the last frames' average radiance
// (GXPortFrameRadiance) when `roomGeoDrawing` (only then does the picture follow the
// exposure, so measuring it can settle), else towards the one of the room's probes. It
// jumps when the camera's previous room is gone (a world load or a teleport). Also hands
// one cube or volume the worker has finished to the GPU.
void UpdateFrame(bool roomGeoDrawing);
// How long the player's power bomb has been going off, or a negative value for none; the next
// UpdateFrame tints the baked light with it (PowerBombBakedLight) unless
// MP_REMASTERED_BOMB_TINT=0.
void SetPowerBombTime(float seconds);
// The baked light's modulation the last UpdateFrame set (GXSetPBRBakedLightModulation).
void BakedLightModulation(float rgb[3]);
// The exposure to measure the frame at, for GXPortPostProcess; 0 when nothing would use
// the measurement.
float MeasureExposure();
// The frame's exposure (2^(3 - EV)); 0 before it has one.
float FrameExposure();
// Whether the exposure follows the frame (MP_ROOM_ENV_AUTO_EXPOSURE, the console's
// `roomenv auto`); otherwise it follows the room's probes.
void SetAutoExposure(bool on);
bool AutoExposure();
// What emissive and unlit light in the opaque pass is multiplied by: Remastered draws it
// at the room's static exposure (its InverseTonemapExposure, CSceneTonemapParams' static
// value: the hint's range at the static lerp) while the frame is exposed at the moving
// one, so it is 2^(static EV - frame EV); the sorted pass uses the frame's, so 1 there.
// 1 when rooms are not exposed or MP_ROOM_ENV_STATIC_EXPOSURE=0 (the console's `roomenv
// static`).
float GlowScale();
// What a sky's HDR colour (a Skybox's colour times its intensity) is multiplied by so that,
// after GlowScale, it is exposed at the frame's 2^(3 - EV) as Remastered does. 0 when
// GlowScale is not in use; the sky is then drawn as before.
float SkyGain();
// What an exposed glow (the converter's record mode bit 32: Remastered's bare ICAN x ICNC)
// is multiplied by, so that with GlowScale it is at the frame's 2^(3 - EV): the opaque pass's
// is the static 2^(3 - static EV), a draw GlowScale does not scale (frameExposed: the sorted
// pass) takes the frame's. 0.10 where rooms are not exposed or MP_REMASTERED_GLOW_EXPOSURE=0,
// the constant the glow used to be baked at.
float GlowGain(bool frameExposed);
// What a bare unlit surface (mode bit 131072: the Surface shaders 67135a0b / 6fc4d540) is
// multiplied by besides GlowScale, so that it is exposed at the frame's 2^(3 - EV) as the
// tonemap exposes it; GlowGain's rule without the 0.10 fallback (1 outside exposed rooms).
float UnlitGain(bool frameExposed);
void SetStaticExposure(bool on);
bool StaticExposure();
// Whether a model lit by the baked light (an absolute ambient or a volume) also takes the
// area's lights. Remastered's actors have none: the bake holds that light, and only runtime
// lights (beam, projectiles, the ball's glow) are added. Off unless MP_ROOM_ENV_AREA_LIGHTS=1
// (the console's `roomenv arealights`).
void SetAreaLights(bool on);
bool AreaLights();
// Whether the reflection is Remastered's blend of probes around the camera (UpdateBlend,
// drawn into one cube on the GPU) with its ambient occlusion; otherwise each model reflects
// the one probe it stands in (PickProbe). MP_ROOM_ENV_BLEND=0, the console's `roomenv blend`.
void SetProbeBlend(bool on);
bool ProbeBlend();
// The camera's position, once a frame: the probe blend follows it.
void SetViewPoint(const float pos[3]);
// Remastered's X-ray visor post (shader 00089f0): pass A (distortion = true) after the opaque
// world, pass B (ramp and vignette) after the post-FX. xrayTime is dt accumulated while the
// X-ray visor is active (XRayTick). False when the gate (Remastered rooms exposed, not
// Original experience) is closed; the caller then draws retail's.
bool XRayPass(bool distortion);
void XRayTick(float dt, bool xrayActive);

// The frame's tone curve, for GXSetPBRTone; false when rooms are not exposed or the
// camera's room has no environment.
bool Tone(float rows[3][4]);
// The curve of Remastered's tonemap (NTonemap::build_tonemap_eval_params): `mid` is the
// exposed radiance that comes out at 0.25, with the slope `contrast` sets (0 to 1: between
// the lines from the origin through 0.25 and through 0.75 there), and `toe` and `shoulder`
// say how soon the curve leaves that line at either end.
void BuildTone(float mid, float contrast, float toe, float shoulder, float rows[3][4]);
// Areas with an environment, cubes on the GPU, ambient grids.
void Stats(int& areas, int& probes, int& cubes, int& grids);
// What every loaded area's environment gives a model at `pos` (the console's `roomenv
// info`): exposure and tone curve, the probe it would reflect, the baked ambient there.
std::string Info(const float pos[3]);

} // namespace PortRoomEnv
