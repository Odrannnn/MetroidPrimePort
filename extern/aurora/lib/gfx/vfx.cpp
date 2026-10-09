#include <aurora/vfx.hpp>

#include "../gx/fifo.hpp"
#include "../gx/gx.hpp"
#include "../gx/texture.hpp"
#include "../logging.hpp"
#include "../webgpu/gpu.hpp"
#include "fx_pipelines.hpp"
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

// Remastered's VFX particle materials, drawn natively (see aurora/vfx.hpp). The formulas are the
// ones recovered from the Remastered shaders (build/mpr/vfx/SPEC.md section 2); the tone curve,
// gamma and fog repeat what the PBR path of the GX shader does to its output.
namespace aurora::gfx::vfx {
namespace {
Module Log("aurora::gfx::vfx");

constexpr uint32_t MaxQuads = 1u << 14; // 65536 vertices
constexpr uint32_t UniformSize = 512;
constexpr uint32_t SlotCount = 4;
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
  float misc2[4];   // add row
  float srcs[6][4]; // Src i = (code, value) at [i / 2][2 * (i % 2)]; code = -1 or row * 4 + comp
  float uvSets[4];  // per slot
  float layers[4];  // per slot
  float warp[2][4]; // per slot (x, y): slots 0, 1 in warp[0], slots 2, 3 in warp[1]
  float pad[32];
};
static_assert(sizeof(Uniform) == UniformSize);

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
  std::map<std::array<const void*, 1 + 2 * SlotCount>, wgpu::BindGroup> groups;
  wgpu::Texture dummyTexture;
  wgpu::TextureView dummyView;
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
};
static_assert(sizeof(Payload) <= InlineDrawPayloadSize);

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
  if ((FEAT & 8u) != 0u) {
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
    x = x * smooth3(select(select(0.0, 1.0, d >= fx), sat((d - fx) / q), q != 0.0));
  }
  if ((FEAT & 2048u) != 0u) {
    let fx = src(in, 6);
    let fy = src(in, 7);
    let q = fy - fx;
    x = x * smooth3(select(select(0.0, 1.0, fx <= 0.0), sat(-fx / q), q != 0.0));
  }
  if ((FEAT & 4u) != 0u) {
    x = max(0.0, x - src(in, 0));
  }
  let a = x * vc.w;
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
  // The tone map runs before this blend, so an alpha blend brighter than 1 trades alpha for brightness.
  let hdr = max(rgb.r, max(rgb.g, rgb.b));
  if (BLEND == 0u && hdr > 1.0) {
    alpha = clamp(a * hdr, 0.0, 1.0);
    col = tone(rgb * (a / alpha));
  }
  // Remastered sets a no-fog bit for every particle blend but alpha and opaque
  // (SetupFogFromBlendMode), so additive and premultiplied draws are not fogged at all.
  if (BLEND == 0u || BLEND == 3u) {
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

// The PBDM factors (RESOLVED-state.md Q1): alpha (SrcAlpha, 1-SrcAlpha | One, 1-SrcAlpha), premultiplied
// (One, 1-SrcAlpha | Zero, One), additive (SrcAlpha, One | Zero, One), opaque (One, Zero for both).
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
  case Blend::Opaque:
    c.srcFactor = a.srcFactor = wgpu::BlendFactor::One;
    c.dstFactor = a.dstFactor = wgpu::BlendFactor::Zero;
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

// Pool threads of the pipeline cache create pipelines too.
std::mutex g_staticMutex;

void ensure_static(const wgpu::Device& device) {
  std::scoped_lock lock{g_staticMutex};
  if (g_state.pipelineLayout) {
    return;
  }
  wgpu::BindGroupLayoutEntry entries[1 + 2 * SlotCount]{};
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
  const wgpu::BindGroupLayoutDescriptor layoutDescriptor{
      .label = "VFX Bind Group Layout",
      .entryCount = 1 + 2 * SlotCount,
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
      .features = p.features & ~uint32_t(DepthSoften),
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
  // The slot a feature reads is a pipeline constant (3 bits each, 7 = none).
  static constexpr const char* SlotNames[] = {"S_COLOR",  "S_OPACITY",   "S_RAMP",   "S_RAMP2",
                                              "S_THRESHOLD", "S_INDIRECT", "S_PALETTE"};
  std::string source = "const FEAT: u32 = " + std::to_string(p.features) + "u;\nconst BLEND: u32 = " +
                       std::to_string(p.blend) + "u;\n";
  for (uint32_t i = 0; i < std::size(SlotNames); ++i) {
    const uint32_t v = (p.slotKey >> (3 * i)) & 7;
    source += std::string("const ") + SlotNames[i] + ": i32 = " + (v == 7 ? "-1" : std::to_string(v)) + ";\n";
  }
  source += ShaderSource;
  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code = source.c_str();
  const wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &wgsl, .label = "VFX Module"};
  const wgpu::ShaderModule module = device.CreateShaderModule(&moduleDescriptor);

  static constexpr std::array<wgpu::VertexAttribute, 10> attributes{{
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
  }};
  const wgpu::VertexBufferLayout vertexLayout{
      .stepMode = wgpu::VertexStepMode::Vertex,
      .arrayStride = sizeof(Vertex),
      .attributeCount = attributes.size(),
      .attributes = attributes.data(),
  };

  const wgpu::BlendState blend = blend_state(static_cast<Blend>(p.blend));
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
      .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
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
  g_state.dummyTexture = ctx.device.CreateTexture(&descriptor);
  const uint8_t white[4] = {255, 255, 255, 255};
  const wgpu::TexelCopyTextureInfo dst{.texture = g_state.dummyTexture};
  const wgpu::TexelCopyBufferLayout layout{.bytesPerRow = 4, .rowsPerImage = 1};
  const wgpu::Extent3D size{1, 1, 1};
  ctx.queue.WriteTexture(&dst, white, 4, &layout, &size);
  const wgpu::TextureViewDescriptor view{.dimension = wgpu::TextureViewDimension::e2DArray, .arrayLayerCount = 1};
  g_state.dummyView = g_state.dummyTexture.CreateView(&view);
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
  // Mip m of a cell is its own copy only if the cell halves evenly that far (and stays block aligned).
  uint32_t mips = 1;
  while (mips < src.mipCount && (cw % (1u << mips)) == 0 && (ch % (1u << mips)) == 0 &&
         ((cw >> mips) % block) == 0 && ((ch >> mips) % block) == 0) {
    ++mips;
  }
  const wgpu::TextureDescriptor descriptor{
      .label = "VFX Atlas",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .size = {cw, ch, entry->layers},
      .format = src.format,
      .mipLevelCount = mips,
  };
  entry->texture = ctx.device.CreateTexture(&descriptor);
  for (uint32_t layer = 0; layer < entry->layers; ++layer) {
    const uint32_t col = layer % entry->cols;
    const uint32_t row = layer / entry->cols;
    for (uint32_t mip = 0; mip < mips; ++mip) {
      const uint32_t w = cw >> mip;
      const uint32_t h = ch >> mip;
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
          const std::array<uint64_t, 7> pipelineKey{p.features & ~uint32_t(DepthSoften),
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
          std::array<const void*, 1 + 2 * SlotCount> groupKey{};
          for (uint32_t i = 0; i < SlotCount; ++i) {
            groupKey[i] = views[i].Get();
            groupKey[SlotCount + i] = samplers[i].Get();
          }
          groupKey[2 * SlotCount] = ctx.uniformBuffer.Get();
          auto group = g_state.groups.find(groupKey);
          if (group == g_state.groups.end()) {
            if (g_state.groups.size() > 512) {
              g_state.groups.clear();
            }
            std::array<wgpu::BindGroupEntry, 1 + 2 * SlotCount> entries{};
            entries[0] = {.binding = 0, .buffer = ctx.uniformBuffer, .offset = 0, .size = UniformSize};
            for (uint32_t i = 0; i < SlotCount; ++i) {
              entries[1 + i] = {.binding = 1 + i, .textureView = views[i]};
              entries[1 + SlotCount + i] = {.binding = 1 + SlotCount + i, .sampler = samplers[i]};
            }
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
  const bool copy = layers > 1 || cols > 1 || rows > 1;
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
    } else {
      ++it;
    }
  }
}
} // namespace

wgpu::RenderPipeline create_pipeline(const PipelineConfig& config) { return make_pipeline(config); }

// Quads (4 vertices, 6 indices) or triangles (3 vertices, drawn in order).
static void draw_prims(const DrawDesc& desc, const Vertex* verts, uint32_t vertexCount, bool quads) {
  if (verts == nullptr || vertexCount == 0 || vertexCount > MaxQuads * 4 || !ensure_registered()) {
    return;
  }
  evict_idle();
  const uint32_t features = desc.features & ~uint32_t(DepthSoften);
  // The slots each enabled feature reads; a slot nothing reads is left unbound (the dummy).
  const std::pair<uint32_t, int8_t> reads[] = {
      {ColorTex | ColorIndexing, desc.colorSlot},
      {OpacityTex, desc.opacitySlot},
      {Ramp, desc.rampSlot},
      {Ramp | DualMod, desc.ramp2Slot},
      {Thresholding, desc.thresholdSlot},
      {Indirect, desc.indirectSlot},
      {ColorIndexing, desc.paletteSlot},
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
  if (quads) {
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
  p.blend = uint32_t(desc.blend);
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

void shutdown() {
  g_state.pipelines.clear();
  g_state.groups.clear();
  g_state.dummyView = {};
  g_state.dummyTexture = {};
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
