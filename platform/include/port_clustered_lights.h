#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Remastered's clustered light gather, the CPU side: the cluster grid, the 80-byte light
// records and the per-cluster 32-bit mask that a grouped particle material reads (shader
// 390fc33b, VFX_IceChargeCollision_Lit / Effect_IcePop). Addresses are the Remastered
// (Switch) binary: CRenderPass_ClusteredLightGather::BuildClusteredData 0x3b4088,
// SetupClusterContextBase 0x3b43a4, NClusteredLighting::build_clustered_light_material_data
// 0x3b8750, CLightMaterialData::SCachedLight::Initialize 0x16b5c8, CLightGatherBin::
// FilterLights 0x3b21c0. Notes: kb func/remastered-tiled-light-gather, and
// build/s-tiledlight-20261020/REPORT.md (§2 grid, §3 record, §4 probe).
//
// Native builds the mask on the GPU (CullLightsGPUByType 0x3b5ac4) from CScene's light cache;
// this is the same gather on the CPU for the port's room lights (the only lights the port has
// as light nodes), so the grid, the slice formula and the record bytes are exact and the cull
// is the same set of rules written out in view space. See the gaps at the end of the header.
namespace PortClusteredLights {

// NRenderDebugDefaults: skClusterPixelsPerTile 0x1d0dff0 = 48, skClusterMaxDepth 0x1d0dfec
// = 500, skClusteredSliceChoice 0x1d0dfe8 = 2 (table 0x1d14730 = 8, 16, 32), and the tile
// caps of SetupClusterContextBase (0x3b43a4).
inline constexpr int kPixelsPerTile = 48;
inline constexpr int kMaxTilesX = 34;
inline constexpr int kMaxTilesY = 19;
inline constexpr int kSlices = 32;
inline constexpr float kMaxDepth = 500.f;
// The constant block is 0x30 of header and 32 records of 0x50: 0xa30 bytes (2608).
inline constexpr int kMaxLights = 32;
inline constexpr size_t kLightBytes = 0x50;
inline constexpr size_t kLightFloats = kLightBytes / sizeof(float);
inline constexpr size_t kHeaderBytes = 0x30;
inline constexpr size_t kHeaderFloats = kHeaderBytes / sizeof(float);
inline constexpr size_t kBlockBytes = kHeaderBytes + kMaxLights * kLightBytes;
inline constexpr size_t kBlockFloats = kBlockBytes / sizeof(float);
// split[i] = i * 500 / 32 (0x3b4564), so the slices are linear in view distance.
inline constexpr float kSliceDepth = kMaxDepth / float(kSlices); // 15.625

// One light as the material takes it: the 80-byte SCachedLight record that native already
// stores per light (colour x intensity, no exposure, no 1/pi; view space), plus the
// influence radius that native keeps on the light node and uses for the cull.
struct RawLight {
  // byte 0x00 rgb, 0x0c widen, 0x10 view position (0x1c: cone scale), 0x20 unit direction
  // surface -> light (0x2c: cone bias), 0x30 A, 0x34 B, 0x38/0x3c zero, 0x40 a3,a2,a1,a0.
  float record[kLightFloats] = {};
  // GetInfluenceRadius(): the far distance for a fading light; the cull's sphere radius.
  float radius = 0.f;
};

// A room light in world space, as port_room_env's walk finds it (colour already times the
// intensity, no exposure and no 1/pi).
struct LightSource {
  float color[3] = {};
  float pos[3] = {};
  float toLight[3] = {}; // unit, along the spot (zero for a point light)
  float nearD = 0.f;
  float farD = 0.f;
  bool spot = false;
  float innerDeg = 0.f; // the authored full cone angles, degrees (halved by the loader)
  float outerDeg = 0.f;
  int falloff = 3; // ELightDistanceAttn: 0 none, 1 linear, 2 squared, 3 smooth
};

// SLightDistanceAttenuation::FromValues 0x173798: far <= near moves far just past near, then
// A = 1/(far-near), B = -near*A. `far` comes back as the record's and node's far.
void DistanceAttenuation(float nearD, float farD, float& a, float& b, float& far);
// SLightAngleAttenuation::FromValues 0x173a50 (and CLight ctor's None for a point light):
// Halve the authored degree angles, wrap to [0,2pi), widen outer<=inner by 0.001 rad,
// then scale = 1/(cosIn - cosOut), bias = -cosOut * scale.
void ConeAttenuation(bool spot, float innerDeg, float outerDeg, float& scale, float& bias);
// The distance falloff cubic, stored a3, a2, a1, a0 (SLightDistanceAttenuation's table:
// (0,0,0,1), (0,0,-1,1), (0,1,-2,1), (2,-3,0,1) for types 0 to 3).
void FalloffCubic(int falloff, float cubic[4]);
// SCachedLight::Initialize 0x16b5c8 for one light: `toLight` and `pos` are world space and
// `worldToView` holds the view matrix's three rows (world -> the view space the material
// lights in, as PortRoomEnv::RoomLights takes it). `widen` (CLight+0x74) is 0 for a script
// light, which is every light the port has.
RawLight MakeLight(const LightSource& source, const float worldToView[3][4]);

// The view the grid and the cull are built from: the world -> view rows, the projection's
// half-tangents, and the camera's depth range.
struct Camera {
  float worldToView[3][4] = {};
  float tanHalfX = 1.f;
  float tanHalfY = 1.f;
  float nearZ = 0.f;
  float farZ = kMaxDepth;
};
// From a CGraphics::CProjectionState window (left, right, top, bottom, near, far) and the
// view rows: tanHalfX = (right-left)/2/near, tanHalfY = (top-bottom)/2/near.
Camera MakeCamera(const float worldToView[3][4], float left, float right, float top, float bottom, float nearZ,
                  float farZ);

struct Grid {
  uint32_t width = 0;  // the render target's pixels, as the viewport is
  uint32_t height = 0;
  int tilesX = 0;
  int tilesY = 0;
  int Clusters() const { return tilesX * tilesY * kSlices; }
};
// tilesX = min(ceil(width/48), 34), tilesY = min(ceil(height/48), 19). `width` and `height`
// are the viewport the world is drawn with, in pixels (CGraphics::GetViewport's mWidth and
// mHeight, which the port's screen-space code already uses; GXGetViewportv reports the same).
Grid MakeGrid(uint32_t width, uint32_t height);
// The shader's slice for a fragment at view z: trunc(clamp(-z - origin, 0, 500) * 32/500),
// kept inside 0..31 (native's texture fetch clamps; here the last slice takes the rest).
int SliceForViewZ(float viewZ, float depthOrigin = 0.f);
// The same for a positive view distance.
int SliceForDepth(float depth, float depthOrigin = 0.f);
// c5[0..2], the header the shader reads: (1/48, 1/48, tilesX, tilesY), (1, 32/500,
// depthOrigin, 500), then tilesX*tilesY and tilesX as int bits, the validity mask and the
// mask source (0: a texture).
void Header(const Grid& grid, float out[kHeaderFloats], uint32_t validity, float depthOrigin);

struct Frame {
  Grid grid;
  int lightCount = 0;        // records written, at most kMaxLights
  uint32_t validity = 0;     // c5[2].z: one bit per record (native clears a shadow caster's)
  float depthOrigin = 0.f;   // c5[1].z: unverified in native, 0 here (see the gaps)
  float header[kHeaderFloats] = {};
  float records[kMaxLights * kLightFloats] = {};
  // One 32-bit word per (tile, slice), the mask texture's contents: tilesX*tilesY wide and
  // kSlices tall, texel (tileY*tilesX + tileX, slice) = mask[slice][tileY][tileX]. Bit i
  // stands for records[i]; zero clusters are skipped by the shader's light loop.
  std::vector<uint32_t> mask;

  const float* Light(int i) const { return records + i * kLightFloats; }
  // 0 outside the grid.
  uint32_t Mask(int tileX, int tileY, int slice) const;
  int SetBits() const; // how many (cluster, light) pairs the mask holds
};

// Culls `lights` (already in view space, in the order native's vector would have them) and
// fills `out`: the grid and its header, the first kMaxLights that pass, and one mask word
// per cluster. Rules, from CLightGatherBin::FilterLights and CullLightsGPUByType: a ~black
// colour (1e-5), a zero radius, a sphere that misses the camera's frustum, a sphere farther
// than origin + 1 + radius * 1.2584085, and then per cluster only those its sphere reaches.
// Invalid input (a NaN light, a viewport smaller than a tile) leaves the frame empty.
void Build(const Camera& camera, uint32_t width, uint32_t height, const RawLight* lights, int count, Frame& out);

// The frame's data, rebuilt only when something it reads changed: the camera, the viewport,
// the validity mask or the light list. Call it once a frame before the particle draw; the
// shader's block and mask are `Current()`. Cheap on a frame that changes nothing (a compare
// of a few hundred bytes).
const Frame& Update(const Camera& camera, uint32_t width, uint32_t height, const RawLight* lights, int count);
const Frame& Current();
void ResetCache();

// Gaps, as the CPU side cannot close them (report §5, §6):
// - depthOrigin is *(CCameraSceneNode+0x64), unverified and 0 in the port.
// - The list is the port's room lights only: the port has no light scene cache, so no muzzle,
//   beam, glow or explosion light competes for the 32 bits as it does in native.
// - Nothing here does native's per-light occlusion query (FilterLights' FUN_003b2940).
// - validity has no shadow-casting bit to clear: the port's lights carry no such flag. The
//   particle material does not read c5[2].z (only the PBR one does).
// - Shadowed lights (native's group 1) are not built: this material reads group 0 only.
// - c5[2].w (mask source) is 0, as native's CClusteredData+0x24, since the mask is a texture.

} // namespace PortClusteredLights
