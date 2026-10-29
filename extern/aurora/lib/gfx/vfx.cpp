#include <aurora/vfx.hpp>

#include "../gx/fifo.hpp"
#include "../gx/gx.hpp"
#include "../gx/texture.hpp"
#include "../logging.hpp"
#include "../webgpu/gpu.hpp"
#include "frame.hpp"
#include "fx_pipelines.hpp"
#include "hdr_tone.hpp"
#include "pipeline_cache.hpp"
#include "recording.hpp"
#include "resource_cache.hpp"
#include "texture.hpp"

#include <aurora/gfx.hpp>

#include <algorithm>
#include <cstddef>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>
#include <unordered_map>
#include <vector>
#include "texture_sites.hpp"

// Remastered's VFX particle materials, drawn natively (see aurora/vfx.hpp). The formulas are the
// ones recovered from the Remastered shaders (build/mpr/vfx/SPEC.md section 2); the tone curve,
// gamma and fog repeat what the PBR path of the GX shader does to its output.
namespace aurora::gfx::vfx {
namespace {
Module Log("aurora::gfx::vfx");

constexpr uint32_t MaxQuads = 1u << 14; // 65536 vertices
constexpr uint32_t UniformSize = VfxUniformSize;
constexpr uint32_t SlotCount = 4;
// The cluster mask (IceChargeCollisionLit): a texture_2d<u32>, bound after the scene copy's three entries.
constexpr uint32_t ClusterMaskBinding = 4 + 2 * SlotCount;
constexpr auto EvictAfter = std::chrono::seconds(10);

// The scalar sources, in the order the shader indexes them (Uniform::srcs).
enum SrcIndex : uint32_t {
  SrcErosion,
  SrcThrX,
  SrcThrY,
  SrcThrW,
  SrcFresnelX,
  SrcFresnelY,
  SrcFadeX,
  SrcFadeY,
  SrcIndexScale,
  SrcIndexOffset,
  SrcIndexRow,
  SrcCount
};

// ---- Uniforms (all vec4-sized members, so the layout is the same everywhere) ----
struct Uniform {
  float proj[16];
  float pn[12];
  float tone[3][4];
  float fogColor[4];
  float fog[4];     // type, a, b, c
  float misc[4];    // modulate, reversed Z, ramp row 0, ramp row 1
  float misc2[4];   // add row, opacity gain, opacity bias, DepthSoften softness
  float srcs[6][4]; // Src i = (code, value) at [i / 2][2 * (i % 2)]; code = -1 or row * 4 + comp
  float uvSets[4];  // per slot
  float layers[4];  // per slot
  float warp[2][4]; // per slot (x, y): slots 0, 1 in warp[0], slots 2, 3 in warp[1]
  float soft[4];    // DepthSoften: the viewport's depth range (min, max) and whether the scene depth is bound
  float scene[4];   // SceneWarp: 1 / the copied target's size (x, y), the camera's near plane, whether the colour is bound
  // IceCharge*: CCH0..6 (IceChargeBeam: 0..2) and, at [7], the modular simulation time; VisorRunOff:
  // CCH0..4; VisorSteam: CCH0..4; BarrierShield and BarrierGlow: CCH0. Rows past the recipe's stay zero.
  float ice[8][4];
  // IceChargeCollisionLit: AMBS (ambient), the probe's mean, its lobe, then (view-space direction,
  // 1 + 2 * sharpness) per colour channel, and BLCM. These are the 7 vec4f of padding the uniform
  // already had before the cluster block, so nothing above or below moves.
  float lit[7][4];
  float pad[28];
  // IceChargeCollisionLit: Remastered's clustered light block, c5[0..2] then 32 records of 20 floats, at
  // ClusterBlockOffset (after every field above, so no earlier recipe's offsets move). Zero elsewhere.
  float cluster[ClusterBlockFloats];
};
static_assert(sizeof(Uniform) == UniformSize);
static_assert(offsetof(Uniform, misc2) == 208 && offsetof(Uniform, srcs) == 224 && offsetof(Uniform, soft) == 384 &&
              offsetof(Uniform, scene) == 400);
static_assert(offsetof(Uniform, ice) == 416);
// The block starts at ClusterBlockOffset, where the ShaderSource's 14 vec4f of padding put it, and fills
// the rest of the uniform: 163 vec4f. The lit block takes the padding's first 7 vec4f.
static_assert(offsetof(Uniform, lit) == 544 && offsetof(Uniform, cluster) == ClusterBlockOffset);
static_assert(544 + 7 * 16 + sizeof(Uniform::pad) == ClusterBlockOffset && ClusterBlockFloats == 163 * 4 &&
              UniformSize == 3376);

// ---- Array textures ----
// A slot's texture as a texture_2d_array: a 1-layer view of the GX texture itself, or, for an atlas,
// a copy of its cells that an encoder task makes on first use (a draw can't copy: it only has a
// render pass encoder).
enum class State : int { New = 0, Requested = 1, Ready = 2, Failed = 3 };

struct ArrayEntry {
  uint32_t id = 0;
  TextureHandle source; // keeps the TextureRef (and so its address) alive
  uint32_t cols = 1, rows = 1, layers = 1;
  bool copy = false;
  std::atomic<State> state{State::New};
  wgpu::Texture texture; // the copy; render thread only until state is Ready
  wgpu::TextureView view;
  std::chrono::steady_clock::time_point lastUse;
};

struct State_ {
  std::mutex mutex;
  std::map<std::tuple<const void*, uint32_t, uint32_t, uint32_t>, std::shared_ptr<ArrayEntry>> byKey;
  std::unordered_map<uint32_t, std::shared_ptr<ArrayEntry>> byId;
  uint32_t nextId = 1;
  DrawTypeId drawType = InvalidDrawType;
  EncoderTaskId task = InvalidEncoderTask;
  // Render thread only
  wgpu::BindGroupLayout bindLayout;
  wgpu::PipelineLayout pipelineLayout;
  std::map<std::array<uint64_t, 7>, wgpu::RenderPipeline> pipelines;
  std::map<std::array<const void*, 4 + 2 * SlotCount>, wgpu::BindGroup> groups;
  // Set when an atlas is evicted: the cached bind groups still hold its views, so the render thread drops them.
  std::atomic<bool> groupsStale{false};
  wgpu::Texture dummyTexture;
  wgpu::Texture dummyArrayTexture;
  wgpu::TextureView dummyView;
  wgpu::TextureView dummyColorView; // dummyTexture as a texture_2d
  wgpu::Texture dummyDepthTexture;  // 1x1 R32Float
  wgpu::TextureView dummyDepthView;
  wgpu::Texture dummyMaskTexture;   // 1x1 R32Uint: the cluster mask of a draw that has none
  wgpu::TextureView dummyMaskView;
  // The cluster mask per frame slot (IceChargeCollisionLit), like the scene copy: the game thread writes a
  // slot's only while it records that slot, which it gets back once the render worker has encoded it, so
  // the render thread reads it unlocked.
  struct ClusterMask {
    uint32_t frame = UINT32_MAX; // current_frame() it was uploaded for
    uint32_t width = 0, height = 0;
    uint64_t signature = 0; // of the words in it
    wgpu::Texture texture;
    wgpu::TextureView view;
  };
  std::array<ClusterMask, detail::FrameSlotCount> masks;
  // The scene copy per frame slot. The game thread writes a slot's only while it records that slot,
  // which it gets back once the render worker has encoded it, so the render thread reads it unlocked.
  struct SceneCopy {
    uint32_t frame = UINT32_MAX; // current_frame() when it was taken
    uint32_t width = 0, height = 0;
    bool hdr = false;
    wgpu::TextureView depth; // the pass snapshot pool's (null: the device can't snapshot depth)
    TextureHandle color;
    wgpu::TextureView level0; // color's top level alone, what a copy without mips samples
    bool mips = true;         // the latest copy is the primary one (flags 3), not a second (flags 1)
  };
  std::array<SceneCopy, detail::FrameSlotCount> scene;
};
State_ g_state;

struct Payload {
  uint32_t features;
  uint32_t slotKey; // 3 bits per feature slot (7 = none), see slot_key()
  uint32_t blend;
  uint32_t compare; // wgpu::CompareFunction
  uint32_t depthWrite;
  uint32_t indexCount;
  Range verts;
  Range indices;
  Range uniform;
  uint32_t arrayId[SlotCount]; // 0: the dummy
  uint8_t wrapS[SlotCount];
  uint8_t wrapT[SlotCount];
  uint8_t linear[SlotCount];
  uint8_t sceneSlot; // the frame slot whose scene copy the draw binds, NoScene: the dummies
  uint8_t maskSlot;  // the frame slot whose cluster mask the draw binds, NoMask: the dummy
};
static_assert(sizeof(Payload) <= InlineDrawPayloadSize);
constexpr uint8_t NoScene = 0xff;
constexpr uint8_t NoMask = 0xff;

// In pieces: MSVC rejects one string literal over 16 KB (adjacent ones join).
const char* ShaderSource = R"(
struct U {
  proj: mat4x4f,
  pn: mat3x4f,
  tone0: vec4f,
  tone1: vec4f,
  tone2: vec4f,
  fogColor: vec4f,
  fog: vec4f,
  misc: vec4f,
  misc2: vec4f,
  srcs: array<vec4f, 6>,
  uvSets: vec4f,
  layers: vec4f,
  warp01: vec4f,
  warp23: vec4f,
  soft: vec4f,
  scene: vec4f,
  ice0: vec4f,
  ice1: vec4f,
  ice2: vec4f,
  ice3: vec4f,
  ice4: vec4f,
  ice5: vec4f,
  ice6: vec4f,
  ice7: vec4f,
  // The clustered light block (IceChargeCollisionLit) at ClusterBlockOffset: c5[0..2] then 32 records of
  // 5 vec4f. The 14 vec4f before it keep every member above at its old offset; lit0..lit6 are the first
  // seven (the baked ambient and probe), the rest is padding.
  lit0: vec4f,
  lit1: vec4f,
  lit2: vec4f,
  lit3: vec4f,
  lit4: vec4f,
  lit5: vec4f,
  lit6: vec4f,
  padCluster: array<vec4f, 7>,
  cluster: array<vec4f, 163>,
}
@group(0) @binding(0) var<uniform> u: U;
@group(0) @binding(1) var t0: texture_2d_array<f32>;
@group(0) @binding(2) var t1: texture_2d_array<f32>;
@group(0) @binding(3) var t2: texture_2d_array<f32>;
@group(0) @binding(4) var t3: texture_2d_array<f32>;
@group(0) @binding(5) var s0: sampler;
@group(0) @binding(6) var s1: sampler;
@group(0) @binding(7) var s2: sampler;
@group(0) @binding(8) var s3: sampler;
// The opaque scene at the copy point (capture_scene): raw depth, and colour for SceneWarp-style reads.
@group(0) @binding(9) var sceneDepth: texture_2d<f32>;
@group(0) @binding(10) var sceneColor: texture_2d<f32>;
@group(0) @binding(11) var sceneSampler: sampler;
// The clustered light gather's mask (IceChargeCollisionLit): one R32Uint word per (tile, slice). A draw
// without a mask binds a 1x1 dummy.
@group(0) @binding(12) var clusterMask: texture_2d<u32>;

struct VIn {
  @location(0) pos: vec3f,
  @location(1) uv0: vec3f,
  @location(2) uv1: vec3f,
  @location(3) color: vec4f,
  @location(4) e0: vec4f,
  @location(5) e1: vec4f,
  @location(6) e2: vec4f,
  @location(7) e3: vec4f,
  @location(8) nrm: vec3f,
  @location(9) uv2: vec3f,
  @location(10) tan: vec4f,
  // BarrierShield: the mesh's own vertex colour (Vertex::mesh), next to the instance colour at 3.
  @location(11) mesh: vec4f,
}
struct VOut {
  @builtin(position) pos: vec4f,
  @location(0) uv0: vec3f,
  @location(1) uv1: vec3f,
  @location(2) color: vec4f,
  @location(3) e0: vec4f,
  @location(4) e1: vec4f,
  @location(5) e2: vec4f,
  @location(6) e3: vec4f,
  @location(7) nrm: vec3f,
  @location(8) uv2: vec3f,
  @location(9) vz: f32,
  @location(10) sw: f32,
  @location(11) tan: vec4f,
  @location(12) vp: vec3f,
  @location(13) mesh: vec4f,
}

@vertex
fn vs_main(in: VIn) -> VOut {
  var out: VOut;
  let mv = vec4f(in.pos, 1.0) * u.pn;
  out.pos = vec4f(mv, 1.0) * u.proj;
  out.uv0 = in.uv0;
  out.uv1 = in.uv1;
  out.uv2 = in.uv2;
  out.color = in.color;
  out.e0 = in.e0;
  out.e1 = in.e1;
  out.e2 = in.e2;
  out.e3 = in.e3;
  out.nrm = in.nrm;
  out.vz = mv.z;
  out.tan = in.tan;
  out.mesh = in.mesh;
  out.vp = mv;
  if ((FEAT & 32768u) != 0u) {
    // SceneWarp: the warp's perspective scale, near / view distance (the VS's o3.x =
    // vp_c4[0].w / -viewZ).
    out.sw = u.scene.z / -mv.z;
  }
  if ((FEAT & (268435456u | 262144u)) != 0u) {
    // VisorRain (and FrameBufferColorErosion's o3.x): o6.x = game near / HUD view depth. Remastered draws HUD billboards in the HUD
    // camera at basewidget_visorFX (FRME_BaseHud z -2.2311, eye at the origin: the scale
    // 0xcb0ef8 fills the frustum there); the port scales that scene by (near + 0.01) / 2.2311
    // onto GetNearClipDistance, so map its depth back (kb topic/hud-billboard-visor-draw).
    out.sw = u.scene.z * (u.scene.z + 0.01) / (2.2311 * -mv.z);
  }
  if ((FEAT & 2097152u) != 0u || (FEAT & 33554432u) != 0u) {
    // IceChargeCollision: the refraction's perspective scale, min(|INDP.w / -viewZ|, 0.2) (VS o5.x / o6.x).
    out.sw = min(abs(u.scene.z / -mv.z), 0.2);
  }
  return out;
}

fn layer(z: f32, n: f32) -> i32 {
  // round() is round-half-to-even
  return i32(round(clamp(z, 0.0, max(n - 1.0, 0.0))));
}
fn uv_of(in: VOut, uvSet: f32) -> vec3f {
  if (uvSet > 1.5) { return in.uv2; }
  return select(in.uv0, in.uv1, uvSet > 0.5);
}
fn row_of(in: VOut, i: i32) -> vec4f {
  if (i <= 0) { return in.e0; }
  if (i == 1) { return in.e1; }
  if (i == 2) { return in.e2; }
  return in.e3;
}
fn comp_of(v: vec4f, c: i32) -> f32 {
  if (c <= 0) { return v.x; }
  if (c == 1) { return v.y; }
  if (c == 2) { return v.z; }
  return v.w;
}
// Scalar source i: a per-particle component, or a constant when its code is negative.
fn src(in: VOut, i: i32) -> f32 {
  let p = u.srcs[i / 2];
  let s = select(p.xy, p.zw, (i & 1) != 0);
  if (s.x < 0.0) { return s.y; }
  let code = i32(s.x);
  let v = comp_of(row_of(in, code / 4), code % 4);
  // The fresnel and fade angles (4..7) are degrees in the effect; the shader's vertex stage takes their cosine.
  if (i >= 4 && i <= 7) { return cos(v * 0.0174532942); }
  return v;
}
fn sat(x: f32) -> f32 { return clamp(x, 0.0, 1.0); }
fn smooth3(x: f32) -> f32 { return x * x * (3.0 - 2.0 * x); }
fn warp_of(i: i32) -> vec2f {
  if (i == 0) { return u.warp01.xy; }
  if (i == 1) { return u.warp01.zw; }
  if (i == 2) { return u.warp23.xy; }
  return u.warp23.zw;
}
fn sample_raw(i: i32, uv: vec2f, l: i32) -> vec4f {
  switch (i) {
    case 0: { return textureSample(t0, s0, uv, l); }
    case 1: { return textureSample(t1, s1, uv, l); }
    case 2: { return textureSample(t2, s2, uv, l); }
    case 3: { return textureSample(t3, s3, uv, l); }
    default: { return vec4f(1.0); }
  }
}
// Slot i at its own uv set, shifted by the indirect warp w (scaled per slot, zero if not warped).
fn sample_slot(in: VOut, i: i32, w: vec2f) -> vec4f {
  if (i < 0 || i > 3) { return vec4f(1.0); }
  var uv = uv_of(in, u.uvSets[i]);
  uv = vec3f(uv.xy + w * warp_of(i), uv.z);
  return sample_raw(i, uv.xy, layer(uv.z, u.layers[i]));
}
// Threshold edge: S(sat((t - lo) / (hi - lo))), a step when the band has no width.
fn band(t: f32, lo: f32, hi: f32) -> f32 {
  let d = hi - lo;
  if (d <= 0.0) { return select(0.0, 1.0, t >= lo); }
  return smooth3(sat((t - lo) / d));
}

fn tone(c: vec3f) -> vec3f {
  var o = c;
  var tm: vec3f;
  if (u.tone1.x > 0.0) {
    if (u.tone0.w > 0.0) { o = o * u.tone0.w; }
    let toe = ((u.tone0.x * o + u.tone0.y) * o + u.tone0.z) * o;
    let line = u.tone1.x * o + u.tone1.y;
    let st = max(u.tone2.y * o + u.tone2.z, vec3f(0.0));
    let sh = u.tone2.x * st / (1.0 + st) + u.tone2.w;
    tm = select(select(sh, line, o < vec3f(u.tone1.w)), toe, o < vec3f(u.tone1.z));
  } else {
    // Without a room's tone data: Remastered's static default (STonemapParams::BuildLinear(3.0)),
    // a straight line clipped at 1.
    tm = o;
  }
  // Encoded as Remastered's sRGB swapchain does it: the exact piecewise sRGB curve.
  let l = clamp(tm, vec3f(0.0), vec3f(1.0));
  return select(1.055 * pow(l, vec3f(1.0 / 2.4)) - 0.055, 12.92 * l, l <= vec3f(0.0031308));
}

// The clustered light gather's mask word for this fragment (390fc33b): c5[0] holds 1/ppt and the tile grid,
// c5[1] the depth mapping (slices / maxDepth and the depth origin), c5[2].y the row pitch as int bits. The
// word's bit i says unshadowed record i reaches this (tile, slice); 0 means the light loop is skipped.
// c5[i] is u.cluster[i]: c5[0..2] are the block's first 12 floats, c5[3] record 0.
fn cluster_mask_word(in: VOut) -> u32 {
  let h0 = u.cluster[0];
  let h1 = u.cluster[1];
  let h2 = u.cluster[2];
  let tile = vec2i(i32(trunc(in.pos.x * h0.x)), i32(trunc(in.pos.y * h0.y)));
  let pitch = bitcast<i32>(h2.y);
  let slice = clamp(i32((-in.vz - h1.z) * h1.y), 0, 31);
  let row = clamp(tile.y, 0, i32(h0.w) - 1) * pitch;
  let col = clamp(tile.x, 0, i32(h0.z) - 1);
  return textureLoad(clusterMask, vec2i(row + col, slice), 0).x;
}

// One colour channel of Remastered's baked probe (BLPD, REPORT.md section 4): the record's mean and range
// with its own lobe direction. `sharp` is the record's lane w, 1 + 2 * s, so 0.5 * w + 0.5 is 1 + s.
fn ambient_lobe(mean: f32, lobe: f32, sharp: f32, dir: vec3f, n: vec3f) -> f32 {
  let q = sat(dot(n, dir) * 0.5 + 0.5);
  return fma(lobe * (sharp + 1.0), pow(q, sharp), mean - lobe);
}

// The probe's three channels. The port has no BLPD buffer and no BLPI index; the runtime fills u.lit1..
// lit5 from the room's baked ambient at the particle (PortRoomEnv::AmbientAt), view space.
fn probe_ambient(ngeo: vec3f) -> vec3f {
  return vec3f(ambient_lobe(u.lit1.x, u.lit2.x, u.lit3.w, u.lit3.xyz, ngeo),
               ambient_lobe(u.lit1.y, u.lit2.y, u.lit4.w, u.lit4.xyz, ngeo),
               ambient_lobe(u.lit1.z, u.lit2.z, u.lit5.w, u.lit5.xyz, ngeo));
}

// The clustered lights 390fc33b sums (REPORT.md section 3): one 32-bit word per (tile, slice) names the
// records that reach this cluster, each 80 bytes = 5 vec4f after the 3-slot header. A light adds its
// colour x intensity x sat(dot(N, L)) x the cone x the falloff cubic at t = sat(d * A + B), with L from
// the view position and no shadow and no specular term. Record 0 is c5[3], record i at c5[3 + 5i].
fn cluster_lights(in: VOut, ngeo: vec3f) -> vec3f {
  var mask = cluster_mask_word(in);
  var sum = vec3f(0.0);
  loop {
    if (mask == 0u) { break; }
    let bit = firstTrailingBit(mask);
    mask = mask & (mask - 1u);
    let i = 3u + bit * 5u;
    let r0 = u.cluster[i];      // +0x00 colour x intensity, +0x0c widen
    let r1 = u.cluster[i + 1u]; // +0x10 view position, +0x1c cone scale
    let r2 = u.cluster[i + 2u]; // +0x20 unit direction surface -> light, +0x2c cone bias
    let r3 = u.cluster[i + 3u]; // +0x30 A, +0x34 B
    let r4 = u.cluster[i + 4u]; // +0x40 a3, a2, a1, a0
    let dv = r1.xyz - in.vp;
    let d = length(dv);
    let l = dv / max(d, 1e-8);
    let cone = sat(dot(l, r2.xyz) * r1.w + r2.w);
    let t = sat(fma(d, r3.x, r3.y));
    let poly = ((r4.x * t + r4.y) * t + r4.z) * t + r4.w;
    sum = sum + r0.rgb * (sat(dot(ngeo, l)) * cone * poly);
  }
  return sum;
}

fn fog_factor(fz: f32) -> f32 {
  let ft = i32(u.fog.x);
  if (ft == 0) { return 0.0; }
  let depth = select(fz, 1.0 - fz, u.misc.w > 0.5);
  var base: f32;
  if ((ft & 8) != 0) { base = u.fog.y * depth; } else { base = u.fog.y / (u.fog.z - depth); }
  var f = clamp(base - u.fog.w, 0.0, 1.0);
  var r = f;
  switch (ft & 7) {
    case 4: { r = 1.0 - exp2(-8.0 * f); }
    case 5: { r = 1.0 - exp2(-8.0 * f * f); }
    case 6: { r = exp2(-8.0 * (1.0 - f)); }
    case 7: { f = 1.0 - f; r = exp2(-8.0 * f * f); }
    default: {}
  }
  return clamp(r, 0.0, 1.0);
}

@fragment
fn fs_main(in: VOut) -> @location(0) vec4f {
  let vc = in.color;
  let M = u.misc.x;
  // Indirect: the map is read at its own uv, and warps the slots flagged `warped`.
  var w = vec2f(0.0);
  if ((FEAT & 16u) != 0u) {
    let d = sample_slot(in, S_INDIRECT, vec2f(0.0)).xy;
    w = d * 0.99609375 - vec2f(0.5);
  }
  var rgb = vc.rgb * M;
  var x = 1.0;
  if ((FEAT & 65536u) != 0u) {
    // VFX_VisorSplat (9e2cbd46, perm 000_0): the warp map read at the world position's x, y times PMTR
    // row 1.xy plus row 0.zw, (d * 0.99609375 - 0.5) x row 0.xy as a view-space offset, taken to the
    // quad's axes by the inverse basis rows (VS o5, o6) and added to the colour map's uv. The alpha
    // threshold is t.w + row 1.z - 1. M is PCMD.x.
    let m = sample_raw(S_INDIRECT, in.e2.xy * in.e1.xy + in.e0.zw, 0).xy;
    let wv = (m * 0.99609375 - vec2f(0.5)) * in.e0.xy;
    let uvs = uv_of(in, u.uvSets[max(S_COLOR, 0)]);
    let uvw = uvs.xy + vec2f(dot(in.e3.xy, wv), dot(in.e3.zw, wv));
    let t = sample_raw(S_COLOR, uvw, layer(uvs.z, u.layers[max(S_COLOR, 0)]));
    rgb = t.rgb * vc.rgb * M;
    x = max(0.0, t.w + in.e1.z - 1.0);
  } else if ((FEAT & 1048576u) != 0u) {
    // VFX_IceChargeBeam (9e0605cd, FS perm 000_0; ICECHARGE.md section 2). The VS half (the hash
    // rotation and shake) ran on the CPU. BCLR at layer 0 is read twice: its w at the mesh uv
    // offsets both uvs by CCH1.y x 0.1 x (w - 0.5) x sin(time) x 2, and u also scrolls by time x CCH1.z.
    // NMAP bumps the view normal in the tangent frame (N, T, cross(N, T) x handedness); the facing
    // term is smoothstep(sat(dot(N', -V)^2)) x CCH1.w. Colour: PMTR C x CCH0.x x COLOR.z x CCH2.x,
    // plus mix(PMTR A, B, NMAP.x x BCLR.x) x sat(max(BCLR.a, 0.25)) x CCH1.x, plus the facing term
    // x BCLR.rgb, all x the instance colour and PCMD.x (M). Alpha is the instance alpha alone.
    let c0 = u.ice0;
    let c1 = u.ice1;
    let c2 = u.ice2;
    let tm = u.ice7.x;
    let uv = in.uv0.xy;
    let d = c1.y * 0.1 * ((sample_raw(S_COLOR, uv, 0).w - 0.5) * sin(tm));
    let uv2 = vec2f(fma(tm, c1.z, fma(d, 2.0, uv.x)), fma(d, 2.0, uv.y));
)"
R"(    let nm = sample_raw(S_NORMAL, uv2, 0).xy;
    let tc = sample_raw(S_COLOR, uv2, 0);
    // vec and tan are camera-local in (x, z, y) order; Remastered's view space is (x, y, -z) of that.
    let N = normalize(vec3f(in.nrm.x, in.nrm.y, -in.nrm.z));
    let T = normalize(vec3f(in.tan.x, in.tan.y, -in.tan.z));
    let B = cross(N, T) * in.tan.w;
    let bx = fma(nm.x, 1.9921875, -1.0);
    let by = fma(nm.y, 1.9921875, -1.0);
    let bz = sqrt(1.0 - sat(bx * bx + by * by));
    let pn = N * bz + T * bx + B * by;
    let dif = max(0.0, dot(pn, -normalize(in.vp)));
    let fp = sat(dif * dif);
    let fr = smooth3(fp) * c1.w;
    let m = nm.x * tc.x;
    let ka = sat(max(tc.w, 0.25));
    rgb = (c0.x * in.e2.rgb * in.uv1.x * c2.x + mix(in.e0.rgb, in.e1.rgb, m) * ka * c1.x + fr * tc.rgb) * vc.rgb * M;
  } else if ((FEAT & (2097152u | 33554432u)) != 0u) {
    // VFX_IceChargeCollision_Unlit (1c651f97, FS perm 000_0; ICECHARGE.md). BCLR.y at the mesh uv wobbles
    // both uvs by CCH2.x x 0.01 x sin(tm) x 2 (tm = CCH2.z x time x 0.01, which also scrolls u); NMAP there
    // bumps the view normal in the tangent frame. R1 (bump x CCH3.w) gives the facing term g and the
    // TCH0 reflection; R2 (bump x CCH3.z) the rim. The scene copy at the fragment, offset by
    // (g + BCLR.y) x sw x CCH2.w (v down), is desaturated by CCH3.x. The copy is exposed, so x the
    // inverse exposure (no copy bound: no scene term). Alpha is the instance alpha alone.
    let c0 = u.ice0;
    let c1 = u.ice1;
    let c2 = u.ice2;
    let c3 = u.ice3;
    let c5 = u.ice5;
    let tm = c2.z * u.ice7.x * 0.01;
    let uv = in.uv0.xy;
    let d = c2.x * 0.01 * (sin(tm) * (sample_raw(S_COLOR, uv, 0).y - 0.5));
    let uv2 = vec2f(tm + fma(d, 2.0, uv.x), fma(d, 2.0, uv.y));
    let nm = sample_raw(S_NORMAL, uv2, 0).xy;
    let b = sample_raw(S_COLOR, uv2, 0).xy;
    // vec and tan are camera-local in (x, z, y) order; Remastered's view space is (x, y, -z) of that.
    let N = normalize(vec3f(in.nrm.x, in.nrm.y, -in.nrm.z));
    let T = normalize(vec3f(in.tan.x, in.tan.y, -in.tan.z));
    let B = cross(N, T) * in.tan.w;
    let bx = fma(nm.x, 1.9921875, -1.0);
    let by = fma(nm.y, 1.9921875, -1.0);
    let bz = sqrt(1.0 - sat(bx * bx + by * by));
    let D = N * bz + T * bx + B * by - N;
    let V = normalize(in.vp);
    let R1 = normalize(N + D * c3.w);
    let fp = sat(pow(max(0.0, dot(R1, -V)), 2.0));
    let g = 1.0 - smooth3(fp);
    let ofs = (g + b.y) * in.sw * c2.w;
    var ie = 1.0;
    if (u.tone1.x > 0.0 && u.tone0.w > 0.0) { ie = 1.0 / u.tone0.w; }
    var sc = vec3f(0.0);
    if (u.scene.w > 0.5) {
      let fbuv = vec2f(in.pos.x * u.scene.x + ofs, in.pos.y * u.scene.y - ofs);
      sc = scene_light(textureSampleLevel(sceneColor, sceneSampler, fbuv, 0.0).rgb) * ie;
    }
    let env = sample_raw(S_ENV, fma(R1.xy, vec2f(0.5), vec2f(0.5)), 0).rgb;
    let R2 = N + D * c3.z;
    let f = sat(1.0 - pow(max(0.0, -dot(V, normalize(R2))), c3.y));
    let fs = smooth3(f);
    let sum = sc.r + sc.g + sc.b;
    let desat = (sc - vec3f(sum * 0.33)) * c3.x + vec3f(sum * 0.33);
    let col = b.x * b.x * (c1.rgb - c0.rgb) + c0.rgb;
    let base = desat + b.y * c5.x * col + b.x * g * env * c2.y * u.ice4.rgb;
    // VFX_IceChargeCollision_Lit (390fc33b, perm 000; REPORT.md sections 3 to 5). Same front end; the tail
    // is Remastered's "ambient + probes + clustered lights" instead of the unlit rim:
    //   lit = AMBS + probe + sum over the mask's records
    //   rgb = (vc.rgb * base * lit * BLCM) / pi + base * CCH5.z / exposure, then x PCMD
    // with N_geo the interpolated view normal (no bump), and no CCH6 rim term in this perm.
    if ((FEAT & 33554432u) != 0u) {
      let lighting = u.lit0.xyz + probe_ambient(N) + cluster_lights(in, N);
      let shaded = vc.rgb * base * lighting * u.lit6.rgb;
      rgb = (shaded * 0.3183098862 + base * (c5.z * ie)) * M;
    } else {
      rgb = (fs * u.ice6.rgb * c5.y + base) * (c5.z * ie + vc.rgb) * M;
    }
  } else if ((FEAT & (8388608u | 16777216u)) != 0u) {
    // VFX_VisorRunOff (b5e12668 unlit, d85d4169 lit; FS perm 000; SPEC-fb-visor.md sections 5/6). The
    // TCH0 flow map, scrolled by CCH1 (u by .w, v by .x x CCH2.y), gives the mask m from its y and w and
    // the glow's signed u. The scene copy at the fragment is offset by CCH0.x x CCH1.z x the map x
    // 0.01 (c1[0].x) and tinted between CCH3.rgb and CCH4.rgb by m: x the tint (b5e12668) or lerped
    // toward it by m^CCH4.w (d85d4169). The fade (CCH1.z x CCH3.w) is per particle; v2.w is unresolved
    // and taken as 1, so alpha is the instance alpha (x stays 1 below).
    let c0 = u.ice0;
    let c1 = u.ice1;
    let c2 = u.ice2;
    let c3 = u.ice3;
    let c4 = u.ice4;
    let uvs = uv_of(in, u.uvSets[max(S_COLOR, 0)]);
    let g = sample_raw(S_COLOR, vec2f(uvs.x + c1.w, uvs.y + c1.x * c2.y), layer(uvs.z, u.layers[max(S_COLOR, 0)]));
    let uu = g.y * 2.0 - 1.0;
    let k = g.w * c2.x;
    let m = 1.0 - sat(sat(c1.x * c1.y + uvs.y + k - 1.0) * sat(c1.x * c0.y + g.z * c0.z + uvs.y - 1.0) * c2.w);
    let T = c0.x * c1.z;
    var fb = vec3f(0.0);
    if (u.scene.w > 0.5) {
      // v down: the y offset negates. The copy is exposed, so x the inverse exposure (the tail re-exposes).
      let o = vec2f(T * (g.x * 2.0 - 1.0) * c1.z * 0.01,
                    -(T * uu * c1.z + m * k * (-(c1.z * c0.w))) * 0.01);
      let fbuv = vec2f(in.pos.x * u.scene.x + o.x, in.pos.y * u.scene.y + o.y);
      var ie = 1.0;
      if (u.tone1.x > 0.0 && u.tone0.w > 0.0) { ie = 1.0 / u.tone0.w; }
      fb = scene_light(textureSampleLevel(sceneColor, sceneSampler, fbuv, 0.0).rgb) * ie;
    }
    let fade = (c1.z * c3.w - c1.z) + 1.0;
    let glow = (m * 0.5 + 0.5) * uu * uu * c2.z * c1.z * 0.01;
    let tint = mix(c3.rgb, c4.rgb, m);
    if ((FEAT & 16777216u) != 0u) {
      rgb = (glow + fb + pow(max(m, 0.0), c4.w) * (tint - fb)) * fade * vc.rgb * M;
    } else {
      rgb = ((tint + c1.x * (vec3f(1.0) - tint)) * fb + glow) * fade * vc.rgb * M;
    }
  } else if ((FEAT & 268435456u) != 0u) {
    // VFX_VisorRain (0ec41df0, FS perm 000; vfx/0ec41df0.md section 3): the visor's rain splash.
    // TCH0 is a two-channel noise map, read twice: at a per-particle patch (PMTR row 0's xy offset,
    // scaled by CCH0.y) and at the texel the roundEven sample lands on over the whole quad (p and q
    // are in [-0.125, 0.625], so round(p - 0.25) is always 0). The patch's channels warp a sum of two
    // sines, and the constant sample shrinks it as PMTR row 0's z (the lifetime fade, times
    // 0.355/0.365) grows into H. 0.5 < H < 1 is inside the drop: its height Z gives the normal n
    // (each axis' cosine, Z kept as the unnormalised height) that offsets the scene copy at the
    // fragment by (1 - fade) x CCH0.w x near / view depth (v down: the y offset negates), and f =
    // Z^4 x 10 + 0.75 + 0.25 x fade is the edge darkening. Elsewhere alpha is 0 and the fragment is
    // discarded. The bigger drops ("runners": row 1's y < CCH0.z, size row 1's w > row 2's y) slide
    // the pattern down by min(2 x row 1's x, 0.5) x 0.33, narrow the mask and take their own fade
    // (row 1's z; row 0's z otherwise). M is PCMD.x. x stays the instance alpha.
    let c = u.ice0;
    let R0 = in.e0;
    let R1 = in.e1;
    let uvs = uv_of(in, u.uvSets[max(S_COLOR, 0)]);
    let qu = sat(uvs.x);
    var qv = sat(uvs.y);
    let runner = R1.y < c.z && R1.w > in.e2.y;
    if (runner) { qv = qv + min(2.0 * R1.x, 0.5) * 0.33; }
    let p = qu * 0.75 - 0.125;
    let q = qv * 0.75 - 0.125;
    let lay = layer(uvs.z, u.layers[max(S_COLOR, 0)]);
    let n1 = sample_raw(S_COLOR, vec2f(p * c.y + R0.x, q * c.y + R0.y), lay).xy;
    let n2 = sample_raw(S_COLOR, vec2f(round(p - 0.25), round(q - 0.25)), lay).xy;
    let sa = p * 6.28318530718 + (n1.x - 0.5) * 2.0 * c.x * R0.w;
    let sb = q * 6.28318530718 + (n1.y - 0.5) * 2.0 * c.x * R0.w;
    let ca = cos(sa);
    let cb = cos(sb);
    var sn = sin(sa) + sin(sb);
    if (runner) {
      // The runner's closing window: the sides and the top and bottom come in with row 1's x.
      sn = sn * select(0.0, 1.0, qu > 0.25 * R1.x && qu < 1.0 - 0.25 * R1.x) *
          select(0.0, 1.0, abs(qv) > 0.1 * R1.x && abs(qv) < 1.0 - 0.1 * R1.x);
    }
    let k = select(0.365, 0.355, runner);
    let H = sat(max(0.0, 1.0 - sat(max(n2.x, 0.02) * (R0.z * k) + n2.y - 0.175)) * sn);
    var Z = 0.0;
    var nn = vec2f(0.0);
    if (H > 0.5 && H < 1.0) {
      Z = (H - 0.5) * 1.8 + 0.1;
      let e = vec3f(-sign(ca) * ca * ca, -sign(cb) * cb * cb, Z);
      nn = -(normalize(e).xy);
    }
    let g = select(R0.z, R1.z, runner);
    let T = (1.0 - g) * c.w * in.sw;
    var fb = vec3f(0.0);
    if (u.scene.w > 0.5) {
      // v down: the copy's y offset negates. The copy is exposed, so x the inverse exposure.
      let fbuv = vec2f(in.pos.x * u.scene.x + T * nn.x, in.pos.y * u.scene.y - T * nn.y);
      var ie = 1.0;
      if (u.tone1.x > 0.0 && u.tone0.w > 0.0) { ie = 1.0 / u.tone0.w; }
      fb = scene_light(textureSampleLevel(sceneColor, sceneSampler, fbuv, 0.0).rgb) * ie;
    }
    let f = sat(Z * Z * Z * Z * 10.0 + (0.75 + 0.25 * g) * select(0.0, 1.0, Z > 0.0));
    rgb = f * fb * vc.rgb * M;
    x = select(0.0, 1.0, Z > 0.0);
)"
R"(  } else if ((FEAT & 536870912u) != 0u) {
    // VFX_BarrierShield (f197ec38, FS perm 002; SHADER-IMPLEMENTATION.md). TCH0 at (-2u, 2v) is the
    // pattern a: its .b scrolls the four fract() noise reads of TCH1 (s, r, q, p) that make the breakup
    // mask from their .g, and its .a widens it. The fragment breaks up where fade < .33 (CCH0.x); the
    // alpha keeps the mesh colour's green (CCH0.y) on top of the instance alpha. The scene copy at the
    // fragment, offset by the .45-period sawtooth of 2 x (a.rg - .5) + time, x .008 and with no
    // near/depth scale, is the base colour, read at three offsets (+0, -.002, +.002: the first's .rg,
    // the second's .rb, the third's .r); the rim, fresnel and mesh-colour glow add over it, then x
    // vc.rgb x PCMD. The copy already holds exposed light, so scene_light (the identity) need not be
    // applied; the tail re-exposes. No copy bound leaves the scene term 0 (the glow alone), as the other
    // scene-reading features do. x carries the fade and the mesh gain, the tail x vc.w.
    let c = u.ice0;
    let tm = u.ice7.x;
    let uvs = uv_of(in, u.uvSets[max(S_COLOR, 0)]);
    let lay = layer(uvs.z, u.layers[max(S_COLOR, 0)]);
    let a = sample_raw(S_COLOR, vec2f(-2.0 * uvs.x, 2.0 * uvs.y), lay);
    let p = fract(tm * 0.375 - a.z);
    let q = fract(tm * -0.0225 - a.z);
    let r = fract(tm * 0.3 - a.z);
    let s = fract(tm * 0.03 - a.z);
    let n0 = sample_raw(S_INDIRECT, vec2f(a.y - uvs.x + s, a.y + uvs.y + r), 0).y;
    let n1 = sample_raw(S_INDIRECT, vec2f(a.y + uvs.x + q, a.y - uvs.y + p), 0).y;
    let mask = a.w * sat(n1 - 0.1) * sat(n0 - 0.1);
    let edge = sat(mask * 10.0);
    let fade = sat(edge + 1.0 - 2.0 * c.x);
    if (fade < 0.33) { discard; }
    let mx = 2.0 * (a.x - 0.5) + tm;
    let my = 2.0 * (a.y - 0.5) + tm;
    let wx = (mx - floor(mx * 2.22222233) * 0.45) * 0.008;
    let wy = (my - floor(my * 2.22222233) * 0.45) * 0.008;
    var sc = vec3f(0.0);
    if (u.scene.w > 0.5) {
      let suv = vec2f(in.pos.x * u.scene.x, in.pos.y * u.scene.y);
      // v down: the y offsets negate.
      let first = textureSampleLevel(sceneColor, sceneSampler, vec2f(suv.x + wx, suv.y - wy), 0.0);
      let second =
          textureSampleLevel(sceneColor, sceneSampler, vec2f(suv.x + wx - 0.002, suv.y - (wy - 0.002)), 0.0);
      let third = textureSampleLevel(sceneColor, sceneSampler, vec2f(suv.x + wx + 0.002, suv.y - (wy + 0.002)), 0.0);
      var ie = 1.0;
      if (u.tone1.x > 0.0 && u.tone0.w > 0.0) { ie = 1.0 / u.tone0.w; }
      sc = vec3f(third.x * 1.165, first.x + 0.33 * (1.5 * first.y - first.x),
                 second.x + 0.33 * (1.5 * second.z - second.x)) *
           ie;
    }
    // Remastered's view space is (x, y, -z) of the port's camera-local vectors (as the other mesh
    // recipes take them); the epsilons only keep normalize() defined for a degenerate feed.
    let N = normalize(vec3f(in.nrm.x, in.nrm.y, -in.nrm.z) + vec3f(0.0, 0.0, 1e-6));
    let V = normalize(in.vp + vec3f(0.0, 0.0, 1e-6));
    let fr = 1.0 - dot(V, -N);
    let rim = edge * clamp(fr * fr, 0.05, 0.66);
    let high = pow(abs(fr), 8.0);
    let rg = in.mesh.x + in.mesh.y;
    let gain = 10.0 - 9.0 * fade;
    rgb = vec3f(
              gain * sc.r,
              gain * (sc.g + 40.0 * rim * (edge * (2.0 * mask - 0.1) + 0.1) + 10.5 * rg + 2.8 * in.mesh.z + 3.0 * high +
                      0.5),
              gain * (sc.b + 40.0 * rim * (edge * (10.0 * mask - 0.5) + 0.5) + 28.0 * rg + 5.95000029 * in.mesh.z +
                      12.0 * high + 0.9)) *
          vc.rgb * M;
    x = fade * (1.0 + in.mesh.y * (c.y - 1.0)) * 0.75;
  } else if ((FEAT & 1073741824u) != 0u) {
    // VFX_BarrierGlow (b1eb2b67, FS perm 000: MATI ba0acbd6 carries neither MFC0 nor MFC1;
    // GLOW-IMPLEMENTATION.md). No scene copy: the glow is additive over the frame. The pattern is read
    // three times at (-u, v): its .r is the alpha a that scrolls the second read's v, its .b the breakup
    // of the third read's .a (which drives k and the fade). phase is the modular time's sawtooth; the
    // mesh colour's r lifts k (through ceil) and pulls G and B down at their own weights. The VS's
    // displacement already ran on the CPU, in the model coordinates: only the instance colour scales
    // the result, the mesh colour feeds the maths.
    let c = u.ice0;
    let tm = u.ice7.x;
    let uvs = uv_of(in, u.uvSets[max(S_COLOR, 0)]);
    let lay = layer(uvs.z, u.layers[max(S_COLOR, 0)]);
    let a = sample_raw(S_COLOR, vec2f(-uvs.x, uvs.y), lay).r;
    let phase = tm - floor(tm * 0.555555582) * 1.79999995;
    let b = sample_raw(S_COLOR, vec2f(-uvs.x - 0.200000003, uvs.y - phase + 0.550000012), lay).b;
    let d = sample_raw(S_COLOR, vec2f(-uvs.x, uvs.y + a + 0.5 * tm + a), lay).a;
    let k = d * (ceil(in.mesh.x) * 0.200000018 + 0.100000001);
    // vec is camera-local (x, z, y); Remastered's view space is (x, y, -z) of it. The epsilons only keep
    // normalize() defined for a degenerate feed.
    let N = normalize(vec3f(in.nrm.x, in.nrm.y, -in.nrm.z) + vec3f(0.0, 0.0, 1e-6));
    let V = normalize(in.vp + vec3f(0.0, 0.0, 1e-6));
    let f = 1.0 - dot(V, -N);
    let p = sat(b * (1.0 - 0.5 * phase));
    let fade = sat(d + 1.0 - 2.0 * c.x);
    let rim = sat(f * f) * fade;
    let gain = 10.0 - 9.0 * fade;
    rgb = rim * gain *
              vec3f(k * 1.125, k * 7.5 - 0.5 * in.mesh.x + 1.4 * p + 0.5,
                    k * 30.0 - 0.899999976 * in.mesh.x + 3.0 * p + 0.9) *
          vc.rgb * M;
    // The alpha is the instance alpha alone: the tail multiplies vc.w in and discards it at 0.
    x = 1.0;
)"
R"(  } else if ((FEAT & 524288u) != 0u) {
    // VFX_VisorSteam (53eceaa4, FS perm 000_0, VS 000_1; vfx/53eceaa4.md). No scene copy and no fog: the
    // steam is a tinted cloud alpha-blended over the visor. TCH1 (the macro map Mt) at the uv offsets the
    // TCH0 repeat read R, scaled by CCH0.x and scrolled by the VS's o2.yz; o2 depends on the CCH rows
    // only, so it is computed here. The cloud's radial mask t70 grows with CCH0.z (the amount) and o2.x
    // (the CCH0.w triangle wave x CCH3.w); CCH0.z past 1 (t42) brings in the Mt.w breakup. R.zw, blended
    // by the triangle wave of R.x + Mt.z + CCH0.w, shade the colour CCH2.rgb. x leaves vc.w to the tail.
    let c0 = u.ice0;
    let c1 = u.ice1;
    let c2 = u.ice2;
    let c3 = u.ice3;
    let c4 = u.ice4;
    let ox = c3.w * (1.0 - abs(1.0 - 2.0 * fract(c0.w * 0.25)));
    let t60 = c2.w * (1.0 - (clamp(c0.z, -1.0, 1.0) * 0.5 + 0.5)) - abs(c0.w) * abs(c2.w);
    let oy = t60 * c4.x;
    let oz = -t60 * c4.y;
    let uvs = uv_of(in, u.uvSets[max(S_COLOR, 0)]);
    let mt = sample_raw(S_INDIRECT, uvs.xy, 0);
    let d9 = mt.x - 0.5;
    let d10 = mt.y - 0.5;
    let R = sample_raw(S_COLOR, vec2f(uvs.x * c0.x - oy + d9 * c3.z, uvs.y * c0.x - oz + d10 * c3.z), 0);
    let r29 = R.x - 0.5;
    let r30 = R.y - 0.5;
    let t28 = ox + 0.1;
    let t38 = -0.5 * c1.x + r29 * t28 + d9 * 0.25 + uvs.x - 0.5;
    let t39 = 0.5 * c1.y + r30 * t28 + d10 * 0.25 + uvs.y - 0.5;
    let t42 = sat(c0.z - 1.0);
    let t47 = sat(t42 * 10.0);
    let t56 = sqrt(t38 * t38 + t39 * t39);
    let t70 = 1.0 - sat(-min(max(c0.z, -1.0), 0.1) + ox * t56 + t56);
    x = t70 * sat(1.0 - t42 + ((d9 + d10) * ((r29 + r30) * 0.5 + 0.5) * 0.25 + mt.w) * t47 - t47) *
        max(1.0 - mt.w - c1.w, c3.x * 0.25) * c3.x;
    let t78 = R.x + mt.z + c0.w;
    let t86 = (R.z - 0.5) * 2.0 * c3.y;
    let t92 = (R.w - 0.5) * c3.y * 2.0 - t86;
    let t101 = max(min((t86 + t92 * (1.0 - abs(1.0 - 2.0 * fract(t78)))) * (mt.z * t70), 1.0), -0.25);
    rgb = (vec3f(t101) + c2.rgb) * vc.rgb * M;
  } else if ((FEAT & 16384u) != 0u) {
    // VFX_Disintegration (d749057f, perm 000_0): the mask's x squared, PMTR rows 0..2 as the colours at
    // s = 0 and 1 and the edge colour, row 3 as (threshold, edge width, alpha lo, alpha hi). ceil(e)
    // picks the alpha scale: hi on the burning edge (e > 0), lo elsewhere. M is PCMD.x.
    let uvd = uv_of(in, u.uvSets[max(S_COLOR, 0)]);
    let t = sample_raw(S_COLOR, uvd.xy, layer(uvd.z, u.layers[max(S_COLOR, 0)])).x;
    let s = t * t;
    let p = in.e3;
    let m = sat(s - p.x);
    let q = sat(1.0 - m - p.y);
    let e = fma(q, -q, q);
    x = sat(m * fma(p.w - p.z, ceil(e), p.z));
    rgb = fma(fma(vec3f(s), in.e1.rgb - in.e0.rgb, in.e0.rgb), vc.rgb, e * in.e2.rgb) * M;
  } else if ((FEAT & 8192u) != 0u) {
    // VFX_Pyro (8278c22d, and 7498e54a with DepthSoften; perm 060_0): the Pyro map P at the frame's
    // layer, the gradient LUT read at (P.r, 0) and (P.g, 0) in the same layer, PMTR row 1 as
    // (scale r, scale g, opacity, threshold). The shader's lit term (Light map weighted by the
    // normal, x vc.rgb) is dropped: its vertex colour is the scene's flat ambient (AMBS), zero here.
    // max(.., 0): a constant index of -1 fails to compile even in a branch FEAT never takes.
    let uvp = uv_of(in, u.uvSets[max(S_COLOR, 0)]);
    let P = sample_raw(S_COLOR, uvp.xy, layer(uvp.z, u.layers[max(S_COLOR, 0)]));
    let gl = layer(uvp.z, u.layers[max(S_RAMP, 0)]);
    let G0 = sample_raw(S_RAMP, vec2f(P.x, 0.0), gl).rgb;
    let G1 = sample_raw(S_RAMP, vec2f(P.y, 0.0), gl).rgb;
    let e1 = in.e1;
    // x the inverse exposure (GlobalSystemValues.z): the light is unexposed, the tone's exposure cancels it.
    var ie = 1.0;
    if (u.tone1.x > 0.0 && u.tone0.w > 0.0) { ie = 1.0 / u.tone0.w; }
    rgb = P.z * (P.x * e1.x * G0 + P.y * e1.y * G1) * ie * M;
    x = smooth3(sat((P.w - e1.w) / (1.0 - e1.w))) * e1.z;
  } else if ((FEAT & 8u) != 0u) {
    // ramp (DualMod: the product of two ramp maps picks the colour)
    let r = sample_slot(in, S_RAMP, w);
    var k = r.x;
    var ra = r.y;
    if ((FEAT & 128u) != 0u) {
      let r2 = sample_slot(in, S_RAMP2, w);
      k = k * r2.x;
      ra = ra * r2.y;
    }
    let k3 = k * k * k;
    let c0 = row_of(in, i32(u.misc.z));
    let c1 = row_of(in, i32(u.misc.w));
    rgb = mix(c0.rgb, c1.rgb, k3) * vc.rgb * M;
    x = ra * mix(c0.w, c1.w, k3);
  } else if ((FEAT & 512u) != 0u) {
    // colour indexing: the lookup map gives (index, alpha), the palette is read at (index * s + o, z)
    let i = sample_slot(in, S_COLOR, w).xy;
    let pc = vec2f(i.x * src(in, 8) + src(in, 9), src(in, 10));
    let g = sample_raw(S_PALETTE, pc, 0);
    rgb = g.rgb * vc.rgb * M;
    x = i.y * g.w;
  } else if ((FEAT & 262144u) != 0u) {
    // VFX_FrameBuffer_Color_Erosion_Unlit (d8ece62c, FS perm 000_0): BCLR at the sprite's uv and layer;
    // its x and y offset the scene copy at the fragment by near / view depth x CCH0.xy (v down: -y),
    // read at lod max(0, CCH0.z - 1), and the copy is added to the colour, x vc.rgb x PCMD (c5.x, M).
    // The copy is exposed, so x the inverse exposure: the tail exposes it again. Alpha t.w: Erosion
    // follows.
    let c = u.ice0;
    let uvs = uv_of(in, u.uvSets[max(S_COLOR, 0)]);
    let t = sample_raw(S_COLOR, uvs.xy, layer(uvs.z, u.layers[max(S_COLOR, 0)]));
    var fb = vec3f(0.0);
    if (u.scene.w > 0.5) {
      let fbuv = vec2f(in.pos.x * u.scene.x + t.x * in.sw * c.x, in.pos.y * u.scene.y - t.y * in.sw * c.y);
      var ie = 1.0;
      if (u.tone1.x > 0.0 && u.tone0.w > 0.0) { ie = 1.0 / u.tone0.w; }
      fb = scene_light(textureSampleLevel(sceneColor, sceneSampler, fbuv, max(0.0, c.z - 1.0)).rgb) * ie;
    }
    rgb = (t.rgb + fb) * vc.rgb * M;
    x = t.w;
  } else if ((FEAT & 1u) != 0u) {
    let c = sample_slot(in, S_COLOR, w);
    rgb = c.rgb * vc.rgb * M;
    if ((FEAT & 4096u) == 0u) { x = c.w; }
  }
  if ((FEAT & 1024u) != 0u) {
    let add = row_of(in, i32(u.misc2.x));
    rgb = rgb * add.w + add.xyz;
  }
  if ((FEAT & 2u) != 0u) {
    x = x * sample_slot(in, S_OPACITY, w).x;
  }
  if ((FEAT & 64u) != 0u) {
    // thresholding: the map's y and x channels against two soft edges
    let T = sample_slot(in, S_THRESHOLD, w);
    let px = src(in, 1);
    let py = src(in, 2);
    let sw = src(in, 3);
    x = x * band(T.y, sat(py - sw), sat(py + sw)) * band(T.x, sat(1.0 - px - sw), sat(1.0 - px + sw));
  }
  if ((FEAT & 256u) != 0u) {
    let d = abs(in.nrm.z) / max(length(in.nrm), 1e-8);
    let fx = src(in, 4);
    let fy = src(in, 5);
    let q = fy - fx;
    x = x * u.misc2.y * sat(smooth3(select(select(0.0, 1.0, d >= fx), sat((d - fx) / q), q != 0.0)) - u.misc2.z);
  }
  if ((FEAT & 2048u) != 0u) {
    let fx = src(in, 6);
    let fy = src(in, 7);
    let q = fy - fx;
    x = x * u.misc2.y * sat(smooth3(select(select(0.0, 1.0, fx <= 0.0), sat(-fx / q), q != 0.0)) - u.misc2.z);
  }
  if ((FEAT & 4u) != 0u) {
    x = max(0.0, x - src(in, 0));
  }
  if ((FEAT & 32768u) != 0u) {
    // VFX_FrameBuffer_Indirect_* (cc155f99 = 04276ae1, d955396d; perms 000_0 and 016_0): the scene
    // copy at this pixel, offset by the warp map's (d * 0.99609375 - 0.5) x CCH0.xy x near / view
    // distance (v down: -y), times the colour. The copy is exposed, so x the inverse exposure: the
    // tail exposes it again. No copy bound: nothing to draw.
    if (u.scene.w < 0.5) { discard; }
    let d = sample_slot(in, S_INDIRECT, vec2f(0.0)).xy;
    let o = (d * 0.99609375 - vec2f(0.5)) * warp_of(max(S_INDIRECT, 0)) * in.sw;
    let fbuv = vec2f(in.pos.x * u.scene.x + o.x, in.pos.y * u.scene.y - o.y);
    var ie = 1.0;
    if (u.tone1.x > 0.0 && u.tone0.w > 0.0) { ie = 1.0 / u.tone0.w; }
    rgb = scene_light(textureSampleLevel(sceneColor, sceneSampler, fbuv, 0.0).rgb) * rgb * ie;
  }
  if ((FEAT & 67108864u) != 0u) {
    // ParticleLit: a VFX_*_Lit shader's lit perm (20e149b2 080_0..091_0) is its unlit colour x (AMBS +
    // the probe + the clustered lights) x BLCM x PCMD / pi; PCMD (M) is already in rgb. vec is camera-
    // local (x, z, y); Remastered's view normal is (x, y, -z) of it.
    let N = normalize(vec3f(in.nrm.x, in.nrm.y, -in.nrm.z) + vec3f(0.0, 0.0, 1e-6));
    let lighting = u.lit0.xyz + probe_ambient(N) + cluster_lights(in, N);
    rgb = rgb * lighting * u.lit6.rgb * 0.3183098862;
  }
  var a = x * vc.w;
  if ((FEAT & 32u) != 0u && u.soft.z > 0.5 && u.misc2.w > 0.0) {
    // DepthSoften, as VFX_DualModColorRamp_Indirect_Unlit (71dc2465, perm 000) does it: the opaque
    // scene's depth at this pixel back to view space through the draw's own projection, and the
    // view-z gap to the fragment over the softness, smoothstepped, scales the alpha before the test.
    let b = textureLoad(sceneDepth, vec2i(in.pos.xy), 0).x;
    let n = (b - u.soft.x) / (u.soft.y - u.soft.x);
    let sz = (u.proj[2].w - n * u.proj[3].w) / (n * u.proj[3].z - u.proj[2].z);
    let f = clamp((1.0 / u.misc2.w) * abs(sz - in.vz), 0.0, 1.0);
    a = (3.0 - 2.0 * f) * f * f * a;
  }
  if (a <= 0.0) { discard; }
  var alpha = clamp(a, 0.0, 1.0);
  if (BLEND == 4u) {
    // Multiply: the target times rgb where alpha covers it. Unfogged, as every blend but
    // alpha and opaque (SetupFogFromBlendMode 0x2887b0).
    let k = mix(vec3f(1.0), max(rgb, vec3f(0.0)), alpha);
    return vec4f(k, alpha);
  }
  var col = tone(rgb);
  // Remastered blends in HDR: a sparse web at intensity 35 and alpha 0.2 still adds 7 and blooms white.
)"
R"(  // The tone map runs before this blend, so an alpha blend brighter than 1 trades alpha for brightness.
  let hdr = max(rgb.r, max(rgb.g, rgb.b));
  if (BLEND == 0u && hdr > 1.0) {
    alpha = clamp(a * hdr, 0.0, 1.0);
    col = tone(rgb * (a / alpha));
  }
  // BLEND 5 (AdditiveOne) takes no alpha compensation above and no fog below, as Additive; its tone is unchanged.
  // Remastered sets a no-fog bit for every particle blend but alpha and opaque
  // (SetupFogFromBlendMode), so additive and premultiplied draws are not fogged at all.
  // Fogged (1 << 27) keeps the fog of an alpha/opaque row whose MATI EBlendMode draws with other factors.
  // IceChargeBeam: DFOG 1 (perm 000), so unfogged in either blend; IceChargeCollision too where Unfogged.
  // BarrierShield: DFOG 1 on its refraction perm (002), which has no fog code, and its fogged sibling
  // (003) is not ported (the recipe rejects a draw without DFOG 1). BarrierGlow: likewise DFOG 1 on its
  // non-refraction perm (000), whose VS/FS carry no fog.
  if ((BLEND == 0u || BLEND == 3u || (FEAT & 134217728u) != 0u) && (FEAT & 1048576u) == 0u &&
      (FEAT & 4194304u) == 0u && (FEAT & 536870912u) == 0u && (FEAT & 1073741824u) == 0u) {
    col = mix(col, u.fogColor.rgb, fog_factor(in.pos.z));
  }
  return vec4f(col, alpha);
}
)";

// ---- Helpers ----
wgpu::AddressMode to_address(uint8_t mode) {
  switch (mode) {
  case GX_REPEAT:
    return wgpu::AddressMode::Repeat;
  case GX_MIRROR:
    return wgpu::AddressMode::MirrorRepeat;
  default:
    return wgpu::AddressMode::ClampToEdge;
  }
}

wgpu::CompareFunction to_compare(GXCompare func) {
  const bool rev = uses_reversed_z();
  switch (func) {
  case GX_NEVER:
    return wgpu::CompareFunction::Never;
  case GX_LESS:
    return rev ? wgpu::CompareFunction::Greater : wgpu::CompareFunction::Less;
  case GX_EQUAL:
    return wgpu::CompareFunction::Equal;
  case GX_LEQUAL:
    return rev ? wgpu::CompareFunction::GreaterEqual : wgpu::CompareFunction::LessEqual;
  case GX_GREATER:
    return rev ? wgpu::CompareFunction::Less : wgpu::CompareFunction::Greater;
  case GX_NEQUAL:
    return wgpu::CompareFunction::NotEqual;
  case GX_GEQUAL:
    return rev ? wgpu::CompareFunction::LessEqual : wgpu::CompareFunction::GreaterEqual;
  default:
    return wgpu::CompareFunction::Always;
  }
}

uint32_t block_size(wgpu::TextureFormat format) {
  switch (format) {
  case wgpu::TextureFormat::BC1RGBAUnorm:
  case wgpu::TextureFormat::BC1RGBAUnormSrgb:
  case wgpu::TextureFormat::BC2RGBAUnorm:
  case wgpu::TextureFormat::BC2RGBAUnormSrgb:
  case wgpu::TextureFormat::BC3RGBAUnorm:
  case wgpu::TextureFormat::BC3RGBAUnormSrgb:
  case wgpu::TextureFormat::BC4RUnorm:
  case wgpu::TextureFormat::BC4RSnorm:
  case wgpu::TextureFormat::BC5RGUnorm:
  case wgpu::TextureFormat::BC5RGSnorm:
  case wgpu::TextureFormat::BC6HRGBUfloat:
  case wgpu::TextureFormat::BC6HRGBFloat:
  case wgpu::TextureFormat::BC7RGBAUnorm:
  case wgpu::TextureFormat::BC7RGBAUnormSrgb:
  case wgpu::TextureFormat::ASTC4x4Unorm:
  case wgpu::TextureFormat::ASTC4x4UnormSrgb:
    return 4;
  default:
    return 1;
  }
}

} // namespace

// The PBDM factors (RESOLVED-state.md Q1): alpha (SrcAlpha, 1-SrcAlpha | One, 1-SrcAlpha), premultiplied
// (One, 1-SrcAlpha | Zero, One), additive (SrcAlpha, One | Zero, One), opaque (One, Zero for both).
// AdditiveOne (port-only, the wave orb's model blend) is (One, One | Zero, One). PremultipliedAlpha and AdditiveAlpha
// are the MATI EBlendModes 5 (One, 1-SrcAlpha) and 6 (SrcAlpha, One), colour and alpha alike. Not in the anonymous namespace:
// the unit tests call it.
wgpu::BlendState blend_state(Blend blend) {
  wgpu::BlendComponent c{.operation = wgpu::BlendOperation::Add};
  wgpu::BlendComponent a{.operation = wgpu::BlendOperation::Add};
  switch (blend) {
  case Blend::Alpha:
    c.srcFactor = wgpu::BlendFactor::SrcAlpha;
    c.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
    a.srcFactor = wgpu::BlendFactor::One;
    a.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
    break;
  case Blend::Premultiplied:
    c.srcFactor = wgpu::BlendFactor::One;
    c.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
    a.srcFactor = wgpu::BlendFactor::Zero;
    a.dstFactor = wgpu::BlendFactor::One;
    break;
  case Blend::Additive:
    c.srcFactor = wgpu::BlendFactor::SrcAlpha;
    c.dstFactor = wgpu::BlendFactor::One;
    a.srcFactor = wgpu::BlendFactor::Zero;
    a.dstFactor = wgpu::BlendFactor::One;
    break;
  case Blend::AdditiveOne:
    c.srcFactor = wgpu::BlendFactor::One;
    c.dstFactor = wgpu::BlendFactor::One;
    a.srcFactor = wgpu::BlendFactor::Zero;
    a.dstFactor = wgpu::BlendFactor::One;
    break;
  case Blend::Opaque:
    c.srcFactor = a.srcFactor = wgpu::BlendFactor::One;
    c.dstFactor = a.dstFactor = wgpu::BlendFactor::Zero;
    break;
  case Blend::PremultipliedAlpha:
    c.srcFactor = a.srcFactor = wgpu::BlendFactor::One;
    c.dstFactor = a.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
    break;
  case Blend::AdditiveAlpha:
    c.srcFactor = a.srcFactor = wgpu::BlendFactor::SrcAlpha;
    c.dstFactor = a.dstFactor = wgpu::BlendFactor::One;
    break;
  case Blend::Multiply:
    c.srcFactor = wgpu::BlendFactor::Dst;
    c.dstFactor = wgpu::BlendFactor::Zero;
    a.srcFactor = wgpu::BlendFactor::Zero;
    a.dstFactor = wgpu::BlendFactor::One;
    break;
  }
  return {.color = c, .alpha = a};
}

namespace {

// Pool threads of the pipeline cache create pipelines too.
std::mutex g_staticMutex;

void ensure_static(const wgpu::Device& device) {
  std::scoped_lock lock{g_staticMutex};
  if (g_state.pipelineLayout) {
    return;
  }
  wgpu::BindGroupLayoutEntry entries[ClusterMaskBinding + 1]{};
  entries[0] = {
      .binding = 0,
      .visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment,
      .buffer = {.type = wgpu::BufferBindingType::Uniform, .hasDynamicOffset = true, .minBindingSize = UniformSize},
  };
  for (uint32_t i = 0; i < SlotCount; ++i) {
    entries[1 + i] = {
        .binding = 1 + i,
        .visibility = wgpu::ShaderStage::Fragment,
        .texture = {.sampleType = wgpu::TextureSampleType::Float, .viewDimension = wgpu::TextureViewDimension::e2DArray},
    };
    entries[1 + SlotCount + i] = {
        .binding = 1 + SlotCount + i,
        .visibility = wgpu::ShaderStage::Fragment,
        .sampler = {.type = wgpu::SamplerBindingType::Filtering},
    };
  }
  entries[1 + 2 * SlotCount] = {
      .binding = 1 + 2 * SlotCount,
      .visibility = wgpu::ShaderStage::Fragment,
      .texture = {.sampleType = wgpu::TextureSampleType::UnfilterableFloat,
                  .viewDimension = wgpu::TextureViewDimension::e2D},
  };
  entries[2 + 2 * SlotCount] = {
      .binding = 2 + 2 * SlotCount,
      .visibility = wgpu::ShaderStage::Fragment,
      .texture = {.sampleType = wgpu::TextureSampleType::Float, .viewDimension = wgpu::TextureViewDimension::e2D},
  };
  entries[3 + 2 * SlotCount] = {
      .binding = 3 + 2 * SlotCount,
      .visibility = wgpu::ShaderStage::Fragment,
      .sampler = {.type = wgpu::SamplerBindingType::Filtering},
  };
  entries[ClusterMaskBinding] = {
      .binding = ClusterMaskBinding,
      .visibility = wgpu::ShaderStage::Fragment,
      .texture = {.sampleType = wgpu::TextureSampleType::Uint, .viewDimension = wgpu::TextureViewDimension::e2D},
  };
  const wgpu::BindGroupLayoutDescriptor layoutDescriptor{
      .label = "VFX Bind Group Layout",
      .entryCount = std::size(entries),
      .entries = entries,
  };
  g_state.bindLayout = device.CreateBindGroupLayout(&layoutDescriptor);
  const wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor{
      .label = "VFX Pipeline Layout",
      .bindGroupLayoutCount = 1,
      .bindGroupLayouts = &g_state.bindLayout,
  };
  g_state.pipelineLayout = device.CreatePipelineLayout(&pipelineLayoutDescriptor);
}

PipelineConfig make_config(const RenderTargetLayout& layout, const Payload& p) {
  PipelineConfig config{
      .depthStencilFormat = layout.depthStencilFormat,
      .colorAttachmentCount = layout.colorAttachmentCount,
      .msaaSamples = layout.sampleCount,
      .features = p.features,
      .slotKey = p.slotKey,
      .blend = p.blend,
      .compare = p.compare,
      .depthWrite = p.depthWrite,
  };
  for (uint32_t i = 0; i < layout.colorAttachmentCount; ++i) {
    config.colorFormats[i] = layout.colorAttachments[i].format;
  }
  return config;
}

wgpu::RenderPipeline make_pipeline(const PipelineConfig& p) {
  const wgpu::Device& device = webgpu::g_device;
  ensure_static(device);
  const std::string source = pipeline_source(p);
  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code = source.c_str();
  const wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &wgsl, .label = "VFX Module"};
  const wgpu::ShaderModule module = device.CreateShaderModule(&moduleDescriptor);

  static constexpr std::array<wgpu::VertexAttribute, 12> attributes{{
      {.format = wgpu::VertexFormat::Float32x3, .offset = offsetof(Vertex, pos), .shaderLocation = 0},
      {.format = wgpu::VertexFormat::Float32x3, .offset = offsetof(Vertex, uv[0]), .shaderLocation = 1},
      {.format = wgpu::VertexFormat::Float32x3, .offset = offsetof(Vertex, uv[1]), .shaderLocation = 2},
      {.format = wgpu::VertexFormat::Float32x4, .offset = offsetof(Vertex, color), .shaderLocation = 3},
      {.format = wgpu::VertexFormat::Float32x4, .offset = offsetof(Vertex, extra[0]), .shaderLocation = 4},
      {.format = wgpu::VertexFormat::Float32x4, .offset = offsetof(Vertex, extra[1]), .shaderLocation = 5},
      {.format = wgpu::VertexFormat::Float32x4, .offset = offsetof(Vertex, extra[2]), .shaderLocation = 6},
      {.format = wgpu::VertexFormat::Float32x4, .offset = offsetof(Vertex, extra[3]), .shaderLocation = 7},
      {.format = wgpu::VertexFormat::Float32x3, .offset = offsetof(Vertex, vec), .shaderLocation = 8},
      {.format = wgpu::VertexFormat::Float32x3, .offset = offsetof(Vertex, uv[2]), .shaderLocation = 9},
      {.format = wgpu::VertexFormat::Float32x4, .offset = offsetof(Vertex, tan), .shaderLocation = 10},
      // BarrierShield's mesh colour: a full row of its own, so the instance colour at 3 stays separate.
      {.format = wgpu::VertexFormat::Float32x4, .offset = offsetof(Vertex, mesh), .shaderLocation = 11},
  }};
  const wgpu::VertexBufferLayout vertexLayout{
      .stepMode = wgpu::VertexStepMode::Vertex,
      .arrayStride = sizeof(Vertex),
      .attributeCount = attributes.size(),
      .attributes = attributes.data(),
  };

  const wgpu::BlendState blend = pipeline_blend(p);
  std::array<wgpu::ColorTargetState, MaxColorAttachments> targets{};
  for (uint32_t i = 0; i < p.colorAttachmentCount; ++i) {
    const bool scene = i == SceneColorAttachmentIndex;
    targets[i] = {
        .format = p.colorFormats[i],
        .blend = scene ? &blend : nullptr,
        .writeMask = scene ? wgpu::ColorWriteMask::All : wgpu::ColorWriteMask::None,
    };
  }
  const wgpu::FragmentState fragment{
      .module = module,
      .entryPoint = "fs_main",
      .targetCount = p.colorAttachmentCount,
      .targets = targets.data(),
  };
  const wgpu::DepthStencilState depth{
      .format = p.depthStencilFormat,
      .depthWriteEnabled = p.depthWrite != 0,
      .depthCompare = static_cast<wgpu::CompareFunction>(p.compare),
  };
  const wgpu::RenderPipelineDescriptor descriptor{
      .label = "VFX Pipeline",
      .layout = g_state.pipelineLayout,
      .vertex = {.module = module, .entryPoint = "vs_main", .bufferCount = 1, .buffers = &vertexLayout},
      .primitive = pipeline_primitive(p),
      .depthStencil = p.depthStencilFormat != wgpu::TextureFormat::Undefined ? &depth : nullptr,
      .multisample = {.count = p.msaaSamples, .mask = UINT32_MAX},
      .fragment = &fragment,
  };
  return device.CreateRenderPipeline(&descriptor);
}

void ensure_dummy(const DrawContext& ctx) {
  if (g_state.dummyView) {
    return;
  }
  const wgpu::TextureDescriptor descriptor{
      .label = "VFX Dummy",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .size = {1, 1, 1},
      .format = wgpu::TextureFormat::RGBA8Unorm,
  };
  AURORA_NOTE_TEXTURE(&descriptor);
  g_state.dummyTexture = ctx.device.CreateTexture(&descriptor);
  const uint8_t white[4] = {255, 255, 255, 255};
  const wgpu::TexelCopyTextureInfo dst{.texture = g_state.dummyTexture};
  const wgpu::TexelCopyBufferLayout layout{.bytesPerRow = 4, .rowsPerImage = 1};
  const wgpu::Extent3D size{1, 1, 1};
  ctx.queue.WriteTexture(&dst, white, 4, &layout, &size);
  // A Compatibility device fixes a texture's binding view dimension, so the array view gets a texture of its own.
  wgpu::TextureBindingViewDimension arrayBinding;
  arrayBinding.textureBindingViewDimension = wgpu::TextureViewDimension::e2DArray;
  const wgpu::TextureDescriptor arrayDescriptor{
      .nextInChain = &arrayBinding,
      .label = "VFX Dummy Array",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .size = {1, 1, 1},
      .format = wgpu::TextureFormat::RGBA8Unorm,
  };
  AURORA_NOTE_TEXTURE(&arrayDescriptor);
  g_state.dummyArrayTexture = ctx.device.CreateTexture(&arrayDescriptor);
  const wgpu::TexelCopyTextureInfo arrayDst{.texture = g_state.dummyArrayTexture};
  ctx.queue.WriteTexture(&arrayDst, white, 4, &layout, &size);
  const wgpu::TextureViewDescriptor view{.dimension = wgpu::TextureViewDimension::e2DArray, .arrayLayerCount = 1};
  g_state.dummyView = g_state.dummyArrayTexture.CreateView(&view);
  const wgpu::TextureViewDescriptor view2D{.dimension = wgpu::TextureViewDimension::e2D};
  g_state.dummyColorView = g_state.dummyTexture.CreateView(&view2D);
  const wgpu::TextureDescriptor depthDescriptor{
      .label = "VFX Dummy Scene Depth",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .size = {1, 1, 1},
      .format = wgpu::TextureFormat::R32Float,
  };
  AURORA_NOTE_TEXTURE(&depthDescriptor);
  g_state.dummyDepthTexture = ctx.device.CreateTexture(&depthDescriptor);
  const float zero = 0.f;
  const wgpu::TexelCopyTextureInfo depthDst{.texture = g_state.dummyDepthTexture};
  ctx.queue.WriteTexture(&depthDst, &zero, 4, &layout, &size);
  g_state.dummyDepthView = g_state.dummyDepthTexture.CreateView();
  const wgpu::TextureDescriptor maskDescriptor{
      .label = "VFX Dummy Cluster Mask",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .size = {1, 1, 1},
      .format = wgpu::TextureFormat::R32Uint,
  };
  AURORA_NOTE_TEXTURE(&maskDescriptor);
  g_state.dummyMaskTexture = ctx.device.CreateTexture(&maskDescriptor);
  const uint32_t noLight = 0;
  const wgpu::TexelCopyTextureInfo maskDst{.texture = g_state.dummyMaskTexture};
  ctx.queue.WriteTexture(&maskDst, &noLight, sizeof(noLight), &layout, &size);
  g_state.dummyMaskView = g_state.dummyMaskTexture.CreateView();
}

std::shared_ptr<ArrayEntry> find_entry(uint32_t id) {
  const std::lock_guard lock{g_state.mutex};
  const auto it = g_state.byId.find(id);
  return it == g_state.byId.end() ? nullptr : it->second;
}

// Render thread: the view a slot samples, or an empty one if it can't be drawn yet.
wgpu::TextureView entry_view(uint32_t id) {
  if (id == 0) {
    return g_state.dummyView;
  }
  const auto entry = find_entry(id);
  if (!entry) {
    return {};
  }
  if (!entry->copy) {
    if (!entry->view) {
      const wgpu::TextureViewDescriptor view{.dimension = wgpu::TextureViewDimension::e2DArray, .arrayLayerCount = 1};
      entry->view = entry->source->texture.CreateView(&view);
      entry->state.store(State::Ready, std::memory_order_release);
    }
    return entry->view;
  }
  return entry->state.load(std::memory_order_acquire) == State::Ready ? entry->view : wgpu::TextureView{};
}

void build_array(const EncoderTaskContext& ctx, const wgpu::CommandEncoder& cmd, const void* payload, size_t size,
                 void*) {
  uint32_t id;
  if (size != sizeof(id)) {
    return;
  }
  std::memcpy(&id, payload, sizeof(id));
  const auto entry = find_entry(id);
  if (!entry || entry->state.load() != State::Requested) {
    return;
  }
  const auto& src = *entry->source;
  const uint32_t cw = src.size.width / entry->cols;
  const uint32_t ch = src.size.height / entry->rows;
  const uint32_t block = block_size(src.format);
  const auto fail = [&](const char* why) {
    Log.warn("VFX atlas {}x{} layers {} not built: {}", src.size.width, src.size.height, entry->layers, why);
    entry->state.store(State::Failed, std::memory_order_release);
  };
  if (cw == 0 || ch == 0 || cw % block != 0 || ch % block != 0) {
    return fail("cell size");
  }
  // Compatibility devices can't copy compressed textures on the GPU; the layers are written from the bytes the
  // source was uploaded from instead (kept by the texture upload on those devices).
  const bool writeFromBytes = !webgpu::g_hasCoreFeatures && block != 1;
  if (writeFromBytes && !src.cpuBytes) {
    return fail("compressed source without its uploaded bytes on a Compatibility device");
  }
  // Mip m of a cell is its own copy only if the cell halves evenly that far (and stays block aligned).
  uint32_t mips = 1;
  while (mips < src.mipCount && (cw % (1u << mips)) == 0 && (ch % (1u << mips)) == 0 &&
         ((cw >> mips) % block) == 0 && ((ch >> mips) % block) == 0) {
    ++mips;
  }
  wgpu::TextureBindingViewDimension arrayBinding;
  arrayBinding.textureBindingViewDimension = wgpu::TextureViewDimension::e2DArray;
  const wgpu::TextureDescriptor descriptor{
      .nextInChain = &arrayBinding,
      .label = "VFX Atlas",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .size = {cw, ch, entry->layers},
      .format = src.format,
      .mipLevelCount = mips,
  };
  AURORA_NOTE_TEXTURE(&descriptor);
  entry->texture = ctx.device.CreateTexture(&descriptor);
  // Byte offset of each source mip in cpuBytes, and its row pitch in blocks.
  std::array<size_t, 16> mipOffset{};
  std::array<uint32_t, 16> mipBlocksWide{};
  if (writeFromBytes) {
    const auto info = format_info(src.format);
    size_t offset = 0;
    for (uint32_t mip = 0; mip < mips && mip < mipOffset.size(); ++mip) {
      const uint32_t w = std::max(src.size.width >> mip, 1u);
      const uint32_t h = std::max(src.size.height >> mip, 1u);
      const uint32_t wideBlocks = (w + info.blockWidth - 1) / info.blockWidth;
      const uint32_t highBlocks = (h + info.blockHeight - 1) / info.blockHeight;
      mipOffset[mip] = offset;
      mipBlocksWide[mip] = wideBlocks;
      offset += size_t(wideBlocks) * highBlocks * info.blockSize;
    }
    if (mips > mipOffset.size() || offset > src.cpuBytes->size()) {
      return fail("kept source bytes don't cover the mip chain");
    }
  }
  std::vector<uint8_t> cell;
  for (uint32_t layer = 0; layer < entry->layers; ++layer) {
    const uint32_t col = layer % entry->cols;
    const uint32_t row = layer / entry->cols;
    for (uint32_t mip = 0; mip < mips; ++mip) {
      const uint32_t w = cw >> mip;
      const uint32_t h = ch >> mip;
      if (writeFromBytes) {
        const auto info = format_info(src.format);
        const uint32_t cellBlocksWide = w / info.blockWidth;
        const uint32_t cellBlocksHigh = h / info.blockHeight;
        const size_t rowBytes = size_t(cellBlocksWide) * info.blockSize;
        cell.resize(rowBytes * cellBlocksHigh);
        const size_t firstBlockX = size_t(col) * cellBlocksWide;
        const size_t firstBlockY = size_t(row) * cellBlocksHigh;
        for (uint32_t by = 0; by < cellBlocksHigh; ++by) {
          const size_t from =
              mipOffset[mip] + ((firstBlockY + by) * mipBlocksWide[mip] + firstBlockX) * info.blockSize;
          std::memcpy(cell.data() + by * rowBytes, src.cpuBytes->data() + from, rowBytes);
        }
        const wgpu::TexelCopyTextureInfo to{.texture = entry->texture, .mipLevel = mip, .origin = {0, 0, layer}};
        const wgpu::TexelCopyBufferLayout layout{.bytesPerRow = uint32_t(rowBytes), .rowsPerImage = cellBlocksHigh};
        const wgpu::Extent3D extent{w, h, 1};
        ctx.queue.WriteTexture(&to, cell.data(), cell.size(), &layout, &extent);
        continue;
      }
      const wgpu::TexelCopyTextureInfo from{.texture = src.texture, .mipLevel = mip, .origin = {col * w, row * h, 0}};
      const wgpu::TexelCopyTextureInfo to{.texture = entry->texture, .mipLevel = mip, .origin = {0, 0, layer}};
      const wgpu::Extent3D extent{w, h, 1};
      cmd.CopyTextureToTexture(&from, &to, &extent);
    }
  }
  const wgpu::TextureViewDescriptor view{.dimension = wgpu::TextureViewDimension::e2DArray};
  entry->view = entry->texture.CreateView(&view);
  entry->state.store(State::Ready, std::memory_order_release);
}

bool ensure_registered() {
  if (g_state.task == InvalidEncoderTask) {
    g_state.task = register_encoder_task_type(EncoderTaskDescriptor{.label = "VFX Atlas", .callback = build_array});
  }
  if (g_state.drawType == InvalidDrawType) {
    g_state.drawType = register_draw_type(DrawTypeDescriptor{
        .label = "VFX Quads",
        .draw = [](const DrawContext& ctx, const wgpu::RenderPassEncoder& pass, const void* data, size_t size, void*) {
          if (size != sizeof(Payload)) {
            return;
          }
          Payload p;
          std::memcpy(&p, data, sizeof(p));
          if (!scene_format_matches(ctx.layout, p.blend)) {
            // Fail closed: an HDR shader into an LDR target (or the reverse) would draw the wrong domain.
            return;
          }
          ensure_static(ctx.device);
          ensure_dummy(ctx);
          wgpu::TextureView views[SlotCount];
          for (uint32_t i = 0; i < SlotCount; ++i) {
            views[i] = entry_view(p.arrayId[i]);
            if (!views[i]) {
              return;
            }
          }
          wgpu::Sampler samplers[SlotCount];
          for (uint32_t i = 0; i < SlotCount; ++i) {
            const auto filter = p.linear[i] ? wgpu::FilterMode::Linear : wgpu::FilterMode::Nearest;
            samplers[i] = sampler_ref(wgpu::SamplerDescriptor{
                .addressModeU = to_address(p.wrapS[i]),
                .addressModeV = to_address(p.wrapT[i]),
                .magFilter = filter,
                .minFilter = filter,
                .mipmapFilter = p.linear[i] ? wgpu::MipmapFilterMode::Linear : wgpu::MipmapFilterMode::Nearest,
                .maxAnisotropy = 1,
            });
          }
          const std::array<uint64_t, 7> pipelineKey{p.features,
                                                    p.slotKey,
                                                    p.blend,
                                                    p.compare,
                                                    p.depthWrite,
                                                    ctx.layout.key,
                                                    ctx.layout.sampleCount};
          auto pipeline = g_state.pipelines.find(pipelineKey);
          if (pipeline == g_state.pipelines.end()) {
            const PipelineConfig config = make_config(ctx.layout, p);
            auto created = require_pipeline(ShaderType::Vfx, config, [config] { return make_pipeline(config); });
            if (!created) {
              return;
            }
            pipeline = g_state.pipelines.emplace(pipelineKey, std::move(created)).first;
          }
          wgpu::TextureView sceneDepth = g_state.dummyDepthView;
          wgpu::TextureView sceneColor = g_state.dummyColorView;
          if (p.sceneSlot < detail::FrameSlotCount) {
            const auto& scene = g_state.scene[p.sceneSlot];
            if (scene.depth) {
              sceneDepth = scene.depth;
            }
            if (scene.color) {
              sceneColor = scene.color->sampleTextureView;
            }
          }
          // SpecialEffectDrawUtils' scene copy reads: textureLod(..., 0), bilinear, clamped.
          const wgpu::Sampler sceneSampler = sampler_ref(wgpu::SamplerDescriptor{
              .addressModeU = wgpu::AddressMode::ClampToEdge,
              .addressModeV = wgpu::AddressMode::ClampToEdge,
              .magFilter = wgpu::FilterMode::Linear,
              .minFilter = wgpu::FilterMode::Linear,
              .mipmapFilter = wgpu::MipmapFilterMode::Nearest,
              .maxAnisotropy = 1,
          });
          std::array<const void*, 4 + 2 * SlotCount> groupKey{};
          for (uint32_t i = 0; i < SlotCount; ++i) {
            groupKey[i] = views[i].Get();
            groupKey[SlotCount + i] = samplers[i].Get();
          }
          // The cluster mask: this frame's slot when the draw carries one, else the dummy.
          wgpu::TextureView clusterMask = g_state.dummyMaskView;
          if (p.maskSlot < detail::FrameSlotCount) {
            const auto& mask = g_state.masks[p.maskSlot];
            if (mask.view) {
              clusterMask = mask.view;
            }
          }
          groupKey[2 * SlotCount] = ctx.uniformBuffer.Get();
          groupKey[2 * SlotCount + 1] = sceneDepth.Get();
          groupKey[2 * SlotCount + 2] = sceneColor.Get();
          groupKey[3 + 2 * SlotCount] = clusterMask.Get();
          if (g_state.groupsStale.exchange(false, std::memory_order_acq_rel)) {
            g_state.groups.clear();
          }
          auto group = g_state.groups.find(groupKey);
          if (group == g_state.groups.end()) {
            if (g_state.groups.size() > 512) {
              g_state.groups.clear();
            }
            std::array<wgpu::BindGroupEntry, 5 + 2 * SlotCount> entries{};
            entries[0] = {.binding = 0, .buffer = ctx.uniformBuffer, .offset = 0, .size = UniformSize};
            for (uint32_t i = 0; i < SlotCount; ++i) {
              entries[1 + i] = {.binding = 1 + i, .textureView = views[i]};
              entries[1 + SlotCount + i] = {.binding = 1 + SlotCount + i, .sampler = samplers[i]};
            }
            entries[1 + 2 * SlotCount] = {.binding = 1 + 2 * SlotCount, .textureView = sceneDepth};
            entries[2 + 2 * SlotCount] = {.binding = 2 + 2 * SlotCount, .textureView = sceneColor};
            entries[3 + 2 * SlotCount] = {.binding = 3 + 2 * SlotCount, .sampler = sceneSampler};
            entries[ClusterMaskBinding] = {.binding = ClusterMaskBinding, .textureView = clusterMask};
            const wgpu::BindGroupDescriptor descriptor{
                .label = "VFX Bind Group",
                .layout = g_state.bindLayout,
                .entryCount = entries.size(),
                .entries = entries.data(),
            };
            group = g_state.groups.emplace(groupKey, ctx.device.CreateBindGroup(&descriptor)).first;
          }
          pass.SetPipeline(pipeline->second);
          pass.SetBindGroup(0, group->second, 1, &p.uniform.offset);
          pass.SetVertexBuffer(0, ctx.vertexBuffer, p.verts.offset, p.verts.size);
          pass.SetIndexBuffer(ctx.indexBuffer, wgpu::IndexFormat::Uint32, p.indices.offset, p.indices.size);
          pass.DrawIndexed(p.indexCount);
        }});
  }
  return g_state.task != InvalidEncoderTask && g_state.drawType != InvalidDrawType;
}

// Game thread: the entry for a slot's texture and layout, its array build requested if it needs one.
// False if the draw has to wait for it.
bool resolve_slot(const Texture& tex, uint32_t& id) {
  id = 0;
  if (tex.obj == nullptr) {
    return true;
  }
  const auto handle = gx::texture::resolve_static_texture(*reinterpret_cast<const GXTexObj_*>(tex.obj));
  if (!handle) {
    return false;
  }
  const uint32_t cols = std::max(tex.cols, 1u);
  const uint32_t rows = std::max(tex.rows, 1u);
  const uint32_t layers = std::max(tex.layers, 1u);
  // Compatibility devices can't view a plain 2D texture as a 2D array, so every entry is a copy there.
  const bool copy = layers > 1 || cols > 1 || rows > 1 || !webgpu::g_hasCoreFeatures;
  std::shared_ptr<ArrayEntry> entry;
  bool request = false;
  {
    const std::lock_guard lock{g_state.mutex};
    const auto now = std::chrono::steady_clock::now();
    auto& slot = g_state.byKey[{handle.get(), cols, rows, layers}];
    if (!slot) {
      slot = std::make_shared<ArrayEntry>();
      slot->id = g_state.nextId++;
      slot->source = handle;
      slot->cols = cols;
      slot->rows = rows;
      slot->layers = layers;
      slot->copy = copy;
      g_state.byId[slot->id] = slot;
    }
    slot->lastUse = now;
    entry = slot;
    request = copy && entry->state.load() == State::New;
  }
  id = entry->id;
  if (!copy) {
    return true;
  }
  if (request) {
    State expected = State::New;
    // Requested only once the task is really recorded, so a refusal retries on the next call.
    if (push_encoder_task(g_state.task, &entry->id, sizeof(entry->id))) {
      entry->state.compare_exchange_strong(expected, State::Requested);
    }
  }
  return entry->state.load(std::memory_order_acquire) == State::Ready;
}

// Game thread, at the start of every draw. An entry a draw queued this frame can't go: resolve_slot
// stamps lastUse when the draw is queued, and an entry goes only after EvictAfter without one. A
// draw whose entry is gone anyway (the render thread stalled for longer than that) is skipped by
// entry_view, not drawn with a stale view.
void evict_idle() {
  const auto now = std::chrono::steady_clock::now();
  const std::lock_guard lock{g_state.mutex};
  for (auto it = g_state.byKey.begin(); it != g_state.byKey.end();) {
    if (now - it->second->lastUse > EvictAfter && it->second->state.load() != State::Requested) {
      g_state.byId.erase(it->second->id);
      it = g_state.byKey.erase(it);
      g_state.groupsStale.store(true, std::memory_order_release);
    } else {
      ++it;
    }
  }
}

// FNV-1a over the mask words: what tells a rebuilt gather from the one already uploaded.
uint64_t mask_signature(const uint32_t* words, size_t count) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < count; ++i) {
    h = (h ^ words[i]) * 0x100000001b3ull;
  }
  return h;
}

// Game thread, once a draw that carries a mask: this frame slot's R32Uint mask texture, written again only
// when the words changed (native rebuilds them only when the camera, the viewport or the lights did).
// False: no usable mask, so the draw binds the 1x1 dummy.
bool upload_cluster_mask(const ClusterView& view) {
  if (!webgpu::g_device || view.mask == nullptr || !valid_cluster_view(view)) {
    return false;
  }
  const size_t words = size_t(view.maskWidth) * size_t(view.maskHeight);
  const uint64_t signature = mask_signature(view.mask, words);
  auto& slot = g_state.masks[current_frame_slot()];
  if (slot.texture && slot.frame == current_frame() && slot.signature == signature &&
      slot.width == view.maskWidth && slot.height == view.maskHeight) {
    return true;
  }
  if (!slot.texture || slot.width != view.maskWidth || slot.height != view.maskHeight) {
    const wgpu::TextureDescriptor descriptor{
        .label = "VFX Cluster Mask",
        .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
        .size = {view.maskWidth, view.maskHeight, 1},
        .format = wgpu::TextureFormat::R32Uint,
    };
    AURORA_NOTE_TEXTURE(&descriptor);
    slot.texture = webgpu::g_device.CreateTexture(&descriptor);
    slot.view = slot.texture.CreateView();
    slot.width = view.maskWidth;
    slot.height = view.maskHeight;
    slot.frame = UINT32_MAX;
    // Bind groups still hold the old view.
    g_state.groupsStale.store(true, std::memory_order_release);
  }
  // Through the frame's uploads, not the queue: a queue write from this thread records into Dawn's pending
  // command buffer, and past 16 MiB of pending staging Dawn submits it from here, under the render worker
  // recording the last frame into it (Adreno crashes on the ended command buffer).
  const wgpu::TexelCopyTextureInfo dst{.texture = slot.texture};
  const auto mask = reinterpret_cast<const uint8_t*>(view.mask);
  queue_texture_upload_data(mask, 4 * view.maskWidth, view.maskHeight, dst, {view.maskWidth, view.maskHeight, 1});
  slot.frame = current_frame();
  slot.signature = signature;
  return true;
}
} // namespace

wgpu::RenderPipeline create_pipeline(const PipelineConfig& config) { return make_pipeline(config); }

wgpu::BlendState pipeline_blend(const PipelineConfig& config) {
  return blend_state(static_cast<Blend>(config.blend & ~BlendSceneHdr));
}

void write_cluster_block(const ClusterView& view, float* uniform) {
  if (uniform == nullptr || view.block == nullptr) {
    return;
  }
  std::memcpy(uniform + ClusterBlockOffset / 4, view.block, sizeof(float) * ClusterBlockFloats);
}

bool scene_format_matches(const RenderTargetLayout& layout, uint32_t blend) noexcept {
  const bool hdrTarget = layout.colorAttachmentCount > SceneColorAttachmentIndex &&
                         layout.colorAttachments[SceneColorAttachmentIndex].format == webgpu::SceneHdrFormat;
  return hdrTarget == ((blend & BlendSceneHdr) != 0);
}

std::string pipeline_source(const PipelineConfig& p) {
  const bool hdr = (p.blend & BlendSceneHdr) != 0;
  // The slot a feature reads is a pipeline constant (3 bits each, 7 = none).
  static constexpr const char* SlotNames[] = {"S_COLOR",  "S_OPACITY",   "S_RAMP",   "S_RAMP2",
                                              "S_THRESHOLD", "S_INDIRECT", "S_PALETTE", "S_NORMAL",
                                              "S_ENV"};
  std::string source = "const FEAT: u32 = " + std::to_string(p.features) + "u;\nconst BLEND: u32 = " +
                       std::to_string(p.blend & ~BlendSceneHdr) + "u;\n";
  for (uint32_t i = 0; i < std::size(SlotNames); ++i) {
    const uint32_t v = (p.slotKey >> (3 * i)) & 7;
    source += std::string("const ") + SlotNames[i] + ": i32 = " + (v == 7 ? "-1" : std::to_string(v)) + ";\n";
  }
  // The scene copy's light: the HDR copy holds the exposed linear the HDR tail writes already. LDR
  // draws never read a scene copy (draw_prims drops them), so theirs is never called.
  static constexpr std::string_view SceneLight = "fn scene_light(c: vec3f) -> vec3f { return c; }\n";
  if (!hdr) {
    source += ShaderSource;
    source += SceneLight;
    return source;
  }
  source += "const HDR: bool = true;\n";
  std::string body = ShaderSource;
  static constexpr std::string_view Anchor = "  var col = tone(rgb);\n";
  // The HDR scene holds exposed linear light: the exposure once, no tone, clamp or encode, and no
  // trade of alpha for brightness (the blend keeps what is above 1). Multiply has returned already,
  // unscaled. Fog mixes in linear, its display colour taken back through the room's curve. Only that
  // reconstruction is capped (at the RGBA16F limit); the effect's own light is not, and a source
  // above 65504 is outside what the target holds.
  static constexpr std::string_view HdrTail = R"(  if (HDR) {
    var lin = max(rgb, vec3f(0.0));
    if (u.tone1.x > 0.0 && u.tone0.w > 0.0) { lin = lin * u.tone0.w; }
    if ((BLEND == 0u || BLEND == 3u || (FEAT & 134217728u) != 0u) && (FEAT & 1048576u) == 0u &&
        (FEAT & 4194304u) == 0u && (FEAT & 536870912u) == 0u && (FEAT & 1073741824u) == 0u) {
      lin = mix(lin, hdr_exposed(u.fogColor.rgb, u.tone0, u.tone1, u.tone2), fog_factor(in.pos.z));
    }
    return vec4f(lin, alpha);
  }
)";
  const size_t at = body.find(Anchor);
  if (at == std::string::npos) {
    Log.fatal("VFX HDR: the shader's tone anchor is gone");
  }
  body.insert(at, HdrTail);
  source += body;
  source += SceneLight;
  source += hdr::ToneWgsl;
  return source;
}

// Quads (4 vertices, 6 indices), triangles (3 vertices, drawn in order), or, with `index`, the
// triangles `index` lists.
static void draw_prims(const DrawDesc& desc, const Vertex* verts, uint32_t vertexCount, bool quads,
                       const uint32_t* index = nullptr, uint32_t indexCount = 0) {
  const uint32_t maxVerts = index != nullptr ? MaxIndexedVertices : MaxQuads * 4;
  if (verts == nullptr || vertexCount == 0 || vertexCount > maxVerts || !ensure_registered()) {
    return;
  }
  if (index != nullptr && (indexCount == 0 || indexCount % 3 != 0)) {
    return;
  }
  evict_idle();
  const uint32_t features = shader_features(desc.features);
  // The slots each enabled feature reads; a slot nothing reads is left unbound (the dummy).
  const std::pair<uint32_t, int8_t> reads[] = {
      {ColorTex | ColorIndexing | Pyro | Disintegration | VisorSplat | IceChargeBeam | IceChargeCollision |
           IceChargeCollisionLit | VisorRunOff | VisorRunOffLit | VisorRain | BarrierShield | BarrierGlow |
           FrameBufferColorErosion | VisorSteam,
       desc.colorSlot},
      {OpacityTex, desc.opacitySlot},
      {Ramp | Pyro, desc.rampSlot},
      {Ramp | DualMod, desc.ramp2Slot},
      {Thresholding, desc.thresholdSlot},
      {Indirect | SceneWarp | VisorSplat | BarrierShield | VisorSteam, desc.indirectSlot},
      {ColorIndexing, desc.paletteSlot},
      {IceChargeBeam | IceChargeCollision | IceChargeCollisionLit, desc.normalSlot},
      {IceChargeCollision | IceChargeCollisionLit, desc.envSlot},
  };
  uint32_t slotKey = 0;
  bool used[SlotCount]{};
  for (uint32_t i = 0; i < std::size(reads); ++i) {
    const auto [mask, slot] = reads[i];
    // DualMod's second ramp needs both bits; every other row needs any of its bits.
    const bool on = i == 3 ? (features & Ramp) != 0 && (features & DualMod) != 0 : (features & mask) != 0;
    const uint32_t v = on && slot >= 0 && slot < int(SlotCount) ? uint32_t(slot) : 7u;
    slotKey |= v << (3 * i);
    if (v != 7) {
      used[v] = true;
    }
  }
  // Before the slots resolve: the texture cache and the recorder belong to the FIFO thread, and a
  // texture created while it is between sealing a pass and starting the next one aborts the frame.
  // The GX state the draw sees is also the one after everything recorded so far.
  gx::fifo::drain();
  const Texture none{};
  uint32_t ids[SlotCount];
  bool ready = true;
  for (uint32_t i = 0; i < SlotCount; ++i) {
    ready = resolve_slot(used[i] ? desc.tex[i] : none, ids[i]) && ready;
  }
  if (!ready) {
    return;
  }
  const auto tex = [&](uint32_t i) -> const Texture& { return used[i] ? desc.tex[i] : none; };

  const auto& gx = gx::g_gxState;
  Uniform u{};
  Mat4x4<float> proj = gx.proj;
  if (gx::UseReversedZ) {
    proj.m2 = proj.m2 * Vec4<float>{-1.f, -1.f, -1.f, -1.f};
  } else {
    proj.m2 = proj.m2 + proj.m3;
  }
  static_assert(sizeof(proj) == sizeof(u.proj));
  std::memcpy(u.proj, &proj, sizeof(u.proj));
  static_assert(sizeof(gx.pnMtx[0].pos) == sizeof(u.pn));
  std::memcpy(u.pn, &gx.pnMtx[gx.currentPnMtx].pos, sizeof(u.pn));
  static_assert(sizeof(gx.pbrTone) == sizeof(u.tone));
  std::memcpy(u.tone, &gx.pbrTone, sizeof(u.tone));
  // Per draw: the GX state keeps the room's exposure for every later draw.
  const bool hdr = scene_hdr_pass_active();
  if (!hdr &&
      (features & (SceneWarp | IceChargeCollision | IceChargeCollisionLit | VisorRunOff | VisorRunOffLit | VisorRain |
                   BarrierShield | FrameBufferColorErosion)) != 0) {
    // The scene-reading effects read the HDR scene's light; outside it there is none to read.
    return;
  }
  u.tone[0][3] = draw_exposure(u.tone[0][3], hdr, desc.alreadyExposed, desc.exposureOverride);
  std::memcpy(u.fogColor, &gx.fog.color, sizeof(u.fogColor));
  u.fog[0] = float(gx.fog.type);
  u.fog[1] = gx.fog.a;
  u.fog[2] = gx.fog.b;
  u.fog[3] = gx.fog.c;
  u.misc[0] = desc.modulate;
  u.misc[1] = gx::UseReversedZ ? 1.f : 0.f;
  u.misc[2] = float(desc.rampRow[0]);
  u.misc[3] = float(desc.rampRow[1]);
  u.misc2[0] = float(desc.addRow);
  u.misc2[1] = desc.opacityGain;
  u.misc2[2] = desc.opacityBias;
  u.misc2[3] = desc.depthSoften;
  // The scene copy this draw may read: this frame's, taken earlier in it, of the target it draws into.
  const size_t frameSlot = current_frame_slot();
  const auto& scene = g_state.scene[frameSlot];
  const auto target = get_render_target_size();
  const bool sceneValid = scene.frame == current_frame() && !is_offscreen() && scene.width == target.x &&
                          scene.height == target.y;
  if (sceneValid && scene.depth) {
    const auto& vp = gx.logicalViewport;
    u.soft[0] = gx::UseReversedZ ? 1.f - vp.zfar : vp.znear;
    u.soft[1] = gx::UseReversedZ ? 1.f - vp.znear : vp.zfar;
    u.soft[2] = u.soft[1] > u.soft[0] ? 1.f : 0.f;
  }
  // SceneWarp, IceChargeCollision: the copy of this pass's scene (HDR or display), under a perspective
  // camera, whose near plane GX's projection gives as m23 / (m22 - 1).
  if (sceneValid && scene.color && scene.hdr == hdr && gx.projType == GX_PERSPECTIVE && gx.proj.m2[2] != 1.f) {
    u.scene[0] = 1.f / float(scene.width);
    u.scene[1] = 1.f / float(scene.height);
    u.scene[2] = gx.proj.m2[3] / (gx.proj.m2[2] - 1.f);
    u.scene[3] = 1.f;
  }
  if ((features & (IceChargeBeam | IceChargeCollision | IceChargeCollisionLit | VisorRunOff | VisorRunOffLit |
                   VisorRain | BarrierShield | BarrierGlow | FrameBufferColorErosion | VisorSteam)) != 0) {
    // The recipe's CCH rows: IceChargeCollision 0..6, VisorRunOff and VisorSteam 0..4, IceChargeBeam 0..2,
    // VisorRain, BarrierShield, BarrierGlow and FrameBufferColorErosion 0 only.
    uint32_t rows = 3;
    if ((features & (IceChargeCollision | IceChargeCollisionLit)) != 0) {
      rows = 7;
    } else if ((features & (VisorRunOff | VisorRunOffLit | VisorSteam)) != 0) {
      rows = 5;
    } else if ((features & (VisorRain | BarrierShield | BarrierGlow | FrameBufferColorErosion)) != 0) {
      rows = 1;
    }
    for (uint32_t i = 0; i < rows; ++i) {
      std::memcpy(u.ice[i], desc.params[i], sizeof(u.ice[i]));
    }
    u.ice[7][0] = desc.time;
  }
  // IceChargeCollisionLit: AMBS, the probe record's mean/lobe/three lobes and BLCM into the padding before
  // the cluster block (u.lit0..u.lit6).
  if ((features & (IceChargeCollisionLit | ParticleLit)) != 0) {
    const LitAmbient& lit = desc.lit;
    std::memcpy(u.lit[0], lit.ambient, sizeof(lit.ambient));
    std::memcpy(u.lit[1], lit.mean, sizeof(lit.mean));
    std::memcpy(u.lit[2], lit.lobe, sizeof(lit.lobe));
    for (int c = 0; c < 3; ++c) {
      std::memcpy(u.lit[3 + c], lit.dir[c], sizeof(lit.dir[c]));
    }
    std::memcpy(u.lit[6], lit.modulate, sizeof(lit.modulate));
  }
  // IceChargeCollisionLit: the gather's block into the uniform (c5[0] at ClusterBlockOffset) and its mask
  // into this frame slot's texture. Validated once here; an unusable view leaves the block zeroed and the
  // draw on the dummy mask.
  uint8_t maskSlot = NoMask;
  if ((features & (IceChargeCollisionLit | ParticleLit)) != 0 && valid_cluster_view(desc.cluster)) {
    write_cluster_block(desc.cluster, reinterpret_cast<float*>(&u));
    if (upload_cluster_mask(desc.cluster)) {
      maskSlot = uint8_t(frameSlot);
    }
  }
  const Src* srcs[SrcCount] = {&desc.erosion,   &desc.thrX,      &desc.thrY,        &desc.thrW,
                               &desc.fresnelX,  &desc.fresnelY,  &desc.fadeX,       &desc.fadeY,
                               &desc.indexScale, &desc.indexOffset, &desc.indexRow};
  for (uint32_t i = 0; i < SrcCount; ++i) {
    const Src& s = *srcs[i];
    u.srcs[i / 2][2 * (i % 2)] = s.row < 0 ? -1.f : float(s.row * 4 + std::clamp<int>(s.comp, 0, 3));
    u.srcs[i / 2][2 * (i % 2) + 1] = s.value;
  }
  for (uint32_t i = 0; i < SlotCount; ++i) {
    const Texture& t = tex(i);
    u.uvSets[i] = float(t.uvSet);
    u.layers[i] = float(std::max(t.layers, 1u));
    u.warp[i / 2][2 * (i % 2)] = t.warped ? t.warpScale[0] : 0.f;
    u.warp[i / 2][2 * (i % 2) + 1] = t.warped ? t.warpScale[1] : 0.f;
  }

  std::vector<uint32_t> indices;
  if (index != nullptr) {
    indices.assign(index, index + indexCount);
    for (const uint32_t i : indices) {
      if (i >= vertexCount) {
        return;
      }
    }
  } else if (quads) {
    const uint32_t quadCount = vertexCount / 4;
    indices.resize(size_t(quadCount) * 6);
    for (uint32_t q = 0; q < quadCount; ++q) {
      const uint32_t b = q * 4;
      const uint32_t i[6] = {b, b + 1, b + 2, b, b + 2, b + 3};
      std::memcpy(&indices[size_t(q) * 6], i, sizeof(i));
    }
  } else {
    indices.resize(vertexCount);
    for (uint32_t i = 0; i < vertexCount; ++i) {
      indices[i] = i;
    }
  }
  Payload p{};
  p.features = features;
  p.slotKey = slotKey;
  p.blend = uint32_t(desc.blend) | (hdr ? BlendSceneHdr : 0u);
  p.compare = uint32_t(gx.depthCompare ? to_compare(gx.depthFunc) : wgpu::CompareFunction::Always);
  p.depthWrite = gx.depthCompare && gx.depthUpdate ? 1 : 0;
  p.indexCount = uint32_t(indices.size());
  p.verts = push_verts(reinterpret_cast<const uint8_t*>(verts), size_t(vertexCount) * sizeof(Vertex), 4);
  p.indices = push_indices(reinterpret_cast<const uint8_t*>(indices.data()), indices.size() * sizeof(uint32_t), 4);
  p.uniform = push_uniform(reinterpret_cast<const uint8_t*>(&u), sizeof(u));
  if (overflowed(p.verts) || overflowed(p.indices) || overflowed(p.uniform) || p.verts.size == 0 ||
      p.indices.size == 0 || p.uniform.size == 0) {
    return;
  }
  for (uint32_t i = 0; i < SlotCount; ++i) {
    const Texture& t = tex(i);
    p.arrayId[i] = ids[i];
    p.wrapS[i] = uint8_t(t.wrapS);
    p.wrapT[i] = uint8_t(t.wrapT);
    p.linear[i] = t.linear ? 1 : 0;
  }
  p.sceneSlot = sceneValid ? uint8_t(frameSlot) : NoScene;
  p.maskSlot = maskSlot;
  push_custom_draw(g_state.drawType, &p, sizeof(p));
}

void draw_quads(const DrawDesc& desc, const Vertex* verts, uint32_t quadCount) {
  if (quadCount > MaxQuads) {
    return;
  }
  draw_prims(desc, verts, quadCount * 4, true);
}

void draw_triangles(const DrawDesc& desc, const Vertex* verts, uint32_t triCount) {
  // Split so no draw exceeds the vertex budget (a multiple of 3).
  constexpr uint32_t MaxTris = MaxTrianglesPerDraw;
  for (uint32_t done = 0; done < triCount;) {
    const uint32_t n = std::min(triCount - done, MaxTris);
    draw_prims(desc, verts + size_t(done) * 3, n * 3, false);
    done += n;
  }
}

void draw_indexed_triangles(const DrawDesc& desc, const Vertex* verts, uint32_t vertexCount, const uint32_t* indices,
                            uint32_t indexCount) {
  draw_prims(desc, verts, vertexCount, false, indices, indexCount);
}

void capture_scene(bool again) {
  gx::fifo::drain();
  if (is_offscreen()) {
    return;
  }
  const auto target = get_render_target_size();
  if (target.x == 0 || target.y == 0) {
    return;
  }
  auto& scene = g_state.scene[current_frame_slot()];
  const bool hdr = scene_hdr_pass_active();
  if (scene.frame == current_frame() &&
      (!again || scene.hdr != hdr || scene.width != target.x || scene.height != target.y)) {
    return; // one copy per frame; a second only into the same texture, which earlier draws bound
  }
  // copy_opaque_color_buffer_for_context (0xf4a30): flag 1 halves the size (at least 1), flag 2 adds a
  // mip chain. The primary copy takes both (3); the pause screen's second copy only half size (1), so
  // a read at lod 1 clamps to the top level. That copy goes into the same texture, sampled through a
  // view of its top level.
  const uint32_t width = std::max(target.x >> 1, 1u);
  const uint32_t height = std::max(target.y >> 1, 1u);
  if (!scene.color || scene.color->size.width != width || scene.color->size.height != height || scene.hdr != hdr) {
    uint32_t mips = 1;
    for (uint32_t edge = std::max(width, height); edge > 1; edge >>= 1) {
      ++mips;
    }
    // HDR: the exposed linear scene, as GXPortCopyScene's. Without the HDR scene the display copy is
    // what there is (Remastered always renders HDR).
    scene.color = hdr ? new_render_texture(width, height, GX_TF_RGBA8, "VFX Scene Copy", mips,
                                           wgpu::TextureFormat::RGBA16Float)
                      : new_render_texture(width, height, GX_TF_RGBA8, "VFX Scene Copy", mips);
    const wgpu::TextureViewDescriptor level0{
        .label = "VFX Scene Copy Level 0",
        .baseMipLevel = 0,
        .mipLevelCount = 1,
    };
    scene.level0 = scene.color->texture.CreateView(&level0);
    scene.hdr = hdr;
    g_state.groupsStale.store(true, std::memory_order_release);
  }
  ResolveHdr resolveHdr{.linear = true};
  static_assert(sizeof(resolveHdr.tone) == sizeof(gx::g_gxState.pbrTone));
  std::memcpy(resolveHdr.tone.data(), gx::g_gxState.pbrTone.data(), sizeof(resolveHdr.tone));
  wgpu::TextureView depth;
  if (!resolve_scene_copy(scene.color, hdr ? &resolveHdr : nullptr, depth)) {
    return;
  }
  scene.frame = current_frame();
  scene.width = target.x;
  scene.height = target.y;
  scene.depth = std::move(depth);
  scene.mips = !again;
}

wgpu::TextureView scene_copy_view(bool hdr) noexcept {
  const auto& scene = g_state.scene[current_frame_slot()];
  const auto target = get_render_target_size();
  if (scene.frame != current_frame() || is_offscreen() || scene.width != target.x || scene.height != target.y ||
      !scene.color || scene.hdr != hdr) {
    return {};
  }
  return scene.mips ? scene.color->sampleTextureView : scene.level0;
}

bool scene_copy_available() {
  gx::fifo::drain();
  return static_cast<bool>(scene_copy_view(scene_hdr_pass_active()));
}

void render_target_size(uint32_t& width, uint32_t& height) {
  gx::fifo::drain();
  const auto target = get_render_target_size();
  width = target.x;
  height = target.y;
}

void shutdown() {
  g_state.pipelines.clear();
  g_state.groups.clear();
  g_state.scene = {};
  g_state.masks = {};
  g_state.dummyMaskView = {};
  g_state.dummyMaskTexture = {};
  g_state.dummyColorView = {};
  g_state.dummyDepthView = {};
  g_state.dummyDepthTexture = {};
  g_state.dummyView = {};
  g_state.dummyTexture = {};
  g_state.dummyArrayTexture = {};
  g_state.bindLayout = {};
  g_state.pipelineLayout = {};
  const auto task = g_state.task;
  const auto draw = g_state.drawType;
  {
    const std::lock_guard lock{g_state.mutex};
    g_state.byKey.clear();
    g_state.byId.clear();
  }
  g_state.task = InvalidEncoderTask;
  g_state.drawType = InvalidDrawType;
  if (task != InvalidEncoderTask) {
    unregister_encoder_task_type(task);
  }
  if (draw != InvalidDrawType) {
    unregister_draw_type(draw);
  }
}
} // namespace aurora::gfx::vfx
