#include "bloom.hpp"

#include "../gx/fifo.hpp"
#include "../logging.hpp"
#include "../webgpu/gpu.hpp"
#include "../webgpu/gpu_prof.hpp"
#include "frame.hpp"
#include "hdr_tone.hpp"
#include "readback_slots.hpp"
#include "recording.hpp"

#include <aurora/gfx.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "texture_sites.hpp"

// Remastered's bloom, as its CRenderPass_Bloom and shaders do it:
//  - a bright pass over the exposed colour X, at a quarter of the frame's size, on four bilinear
//    taps a source texel out diagonally (each tap filtered as light, then bright-passed, / 4):
//    X * min(max(L - threshold, 0), 8) / max(L, 0.001) times tint 4, L being X's luminance;
//  - four downsamples to 1/64 (the centre four times and four corners a texel out, / 8);
//  - four upsamples back, each added to the next finer level, the coarsest first with
//    tint 0 (four edge taps a texel out and four corners half one, the corners doubled, / 12);
//  - the result b added to the frame as b / (1 + b).
// It reads the HDR scene (gfx::set_scene_hdr), which holds the exposed linear light X itself, as
// it is (HdrSource): no decode, no inverse curve, no cap, as Remastered reads its HDR frame.
// The colour grade comes first in the same composite, as Remastered's tonemap shader does it
// right after its tone curve: a 33^3 LUT over the tone-mapped linear colour, sampled at
// c * 32/33 + 0.5/33; two LUTs mixed while a grade fades into another. The bloom is added to
// the graded colour and clamped, as Remastered's composite runs after the tonemap pass.
// Auto exposure measures elsewhere (measure_scene): the HDR scene at the opaque/sorted boundary,
// as Remastered's RenderPrimaryPass copies its opaque colour (CopyOpaqueColorBuffer, flags 3), in
// a chain of mips. Mip 0 is CeilPow2(max(w >> 1, 1)) x CeilPow2(max(h >> 1, 1)), drawn from the
// full frame; each further mip is drawn from the one before, down to 1x1, all through a bilinear
// clamped sampler. So mip 0 is a bilinear resample of the frame and every halving after it an exact
// 2x2 box. The 1x1 is copied to a staging buffer from a pool that grows (three to start, no cap),
// and the newest one the GPU has finished is the one read: at least a frame late, no fixed depth.
// Remastered's copy is R11G11B10F; this one is RGBA16F, as the device is not asked for
// rg11b10ufloat-renderable (gpu.cpp) and RGBA16F renders and filters in core WebGPU. Remastered's
// float11/10 rounding is not matched.
// The composite tones the scene once into the display EFB, which the frame goes on drawing into.
// There is no post outside the HDR scene: without it (the target could not be had) the frame is
// drawn without Remastered's post.
namespace aurora::gfx::bloom {
namespace {
Module Log("aurora::gfx::bloom");
using webgpu::g_device;

constexpr uint32_t Levels = 5;
constexpr uint32_t PassCount = 1 + (Levels - 1) * 2 + 1;
constexpr uint64_t SlotSize = 256;
constexpr auto LevelFormat = wgpu::TextureFormat::RGBA16Float;
constexpr auto MeasureFormat = wgpu::TextureFormat::RGBA16Float;
constexpr uint32_t MeasureRowBytes = 256; // as copies need
constexpr uint64_t MeasureBytes = 8;      // the 1x1's RGBA16F texel
constexpr size_t ReadbackCount = 3;       // to start with; the pool grows
// The measures recorded and not yet encoded: one a frame, so a ring of 8 outlives every frame in flight.
constexpr size_t MeasureSlots = 8;
static_assert(detail::FrameSlotCount + 1 <= MeasureSlots, "measure slots would be overwritten while pending");
// Params::bloom's bits as the task gets them: the bloom, and the composite left to the next pass.
constexpr uint32_t BloomOn = 1;
constexpr uint32_t CompositeInPass = 2;

// Each recorded post keeps its parameters and source, and its own uniform ranges, in a
// slot: two posts in one submit must not share either (queue writes all land before the submit). The
// ring outlives every frame in flight at MaxPostsPerFrame; a post beyond that in one frame is refused.
constexpr size_t RecordedSlots = 8;
constexpr size_t MaxPostsPerFrame = 2;
static_assert(MaxPostsPerFrame * detail::FrameSlotCount <= RecordedSlots, "bloom slots would be overwritten while pending");

struct Uniform {
  float texel[4];
  float tint[4]; // w: the threshold
  float tone[3][4];
  float grade[4]; // x: B's weight, y: A is a LUT, z: B is a LUT, w: bloom on
  float add[4];   // the HDR composite's added term (Params::add)
  float radial[4][4]; // c0, c1, c2 (Params::radial); [3].x: on
};
static_assert(sizeof(Uniform) <= SlotSize);

constexpr const char* ShaderSource = R"(
struct Params {
  texel: vec4f,
  tint: vec4f,
  tone: array<vec4f, 3>,
  grade: vec4f,
  add: vec4f,
  radial: array<vec4f, 4>,
};
@group(0) @binding(0) var samp: sampler;
@group(0) @binding(1) var src: texture_2d<f32>;
@group(0) @binding(2) var<uniform> p: Params;
@group(0) @binding(3) var bloomTex: texture_2d<f32>;
@group(0) @binding(4) var lutA: texture_3d<f32>;
@group(0) @binding(5) var lutB: texture_3d<f32>;

struct VertexOutput {
  @builtin(position) pos: vec4f,
  @location(0) uv: vec2f,
};

@vertex
fn vs_main(@builtin(vertex_index) i: u32) -> VertexOutput {
  var corners = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
  var out: VertexOutput;
  let c = corners[i];
  out.pos = vec4f(c, 0.0, 1.0);
  out.uv = vec2f(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5);
  return out;
}

// One texel of the measure chain: the source sampled bilinearly at this texel's centre (Remastered's
// copy, bilinear and clamped). Each pow2 level averages the four texels of the one before.
@fragment
fn fs_measure(in: VertexOutput) -> @location(0) vec4f {
  return vec4f(textureSampleLevel(src, samp, in.uv, 0.0).rgb, 1.0);
}

@fragment
fn fs_down(in: VertexOutput) -> @location(0) vec4f {
  let t = p.texel.xy;
  var c = textureSampleLevel(src, samp, in.uv, 0.0).rgb * 4.0;
  c += textureSampleLevel(src, samp, in.uv + vec2f(-t.x, -t.y), 0.0).rgb;
  c += textureSampleLevel(src, samp, in.uv + vec2f(t.x, -t.y), 0.0).rgb;
  c += textureSampleLevel(src, samp, in.uv + vec2f(-t.x, t.y), 0.0).rgb;
  c += textureSampleLevel(src, samp, in.uv + vec2f(t.x, t.y), 0.0).rgb;
  return vec4f(c * 0.125, 1.0);
}

@fragment
fn fs_up(in: VertexOutput) -> @location(0) vec4f {
  let t = p.texel.xy;
  let h = t * 0.5;
  var c = textureSampleLevel(src, samp, in.uv + vec2f(-t.x, 0.0), 0.0).rgb;
  c += textureSampleLevel(src, samp, in.uv + vec2f(t.x, 0.0), 0.0).rgb;
  c += textureSampleLevel(src, samp, in.uv + vec2f(0.0, -t.y), 0.0).rgb;
  c += textureSampleLevel(src, samp, in.uv + vec2f(0.0, t.y), 0.0).rgb;
  c += textureSampleLevel(src, samp, in.uv + vec2f(-h.x, -h.y), 0.0).rgb * 2.0;
  c += textureSampleLevel(src, samp, in.uv + vec2f(h.x, -h.y), 0.0).rgb * 2.0;
  c += textureSampleLevel(src, samp, in.uv + vec2f(-h.x, h.y), 0.0).rgb * 2.0;
  c += textureSampleLevel(src, samp, in.uv + vec2f(h.x, h.y), 0.0).rgb * 2.0;
  return vec4f(c / 12.0 * p.tint.rgb, 1.0);
}


)";

// The bright pass and the composite, appended after ShaderSource, hdr::ToneWgsl, hdr::PostToneWgsl and
// CompositeAddWgsl in one module. src is the exposed linear scene.
constexpr std::string_view HdrSource = R"(
fn hdr_raw(c: vec3f) -> vec3f {
  return clamp(c, vec3f(0.0), vec3f(hdr_f16_max));
}

const hdr_luma = vec3f(0.2126, 0.7152, 0.0722);

// One bilinear tap of the scene at uv: the four texels it blends, filtered as light, then the bright
// pass over the blend. Remastered's 000d768 runs the bright pass on each filtered tap: X * min(max(s*L
// - t, 0), 8) / max(s*L, 0.001) * s, L being X's luminance. The scene texel is already exposed (s * X),
// so with c = s * X that is c * min(max(lum(c) - t, 0), 8) / max(lum(c), 0.001).
fn bright_tap_hdr(uv: vec2f, size: vec2i) -> vec3f {
  let pos = uv * vec2f(size) - vec2f(0.5);
  let base = floor(pos);
  let f = pos - base;
  let last = size - vec2i(1);
  let j0 = clamp(vec2i(base), vec2i(0), last);
  let j1 = clamp(vec2i(base) + vec2i(1), vec2i(0), last);
  let d00 = hdr_raw(textureLoad(src, vec2i(j0.x, j0.y), 0).rgb);
  let d10 = hdr_raw(textureLoad(src, vec2i(j1.x, j0.y), 0).rgb);
  let d01 = hdr_raw(textureLoad(src, vec2i(j0.x, j1.y), 0).rgb);
  let d11 = hdr_raw(textureLoad(src, vec2i(j1.x, j1.y), 0).rgb);
  // The blend's luminance is a weighted mean of its texels': none above the threshold adds nothing.
  let hi = max(max(dot(d00, hdr_luma), dot(d10, hdr_luma)), max(dot(d01, hdr_luma), dot(d11, hdr_luma)));
  if (hi <= p.tint.w) {
    return vec3f(0.0);
  }
  let c = mix(mix(d00, d10, f.x), mix(d01, d11, f.x), f.y);
  let l = dot(c, hdr_luma);
  return c * (min(max(l - p.tint.w, 0.0), 8.0) / max(l, 0.001));
}

// The bright pass and the reduction to 1/4 in one (Remastered's 000d768): four diagonal taps a
// source texel out, each bright-passed, / 4, times the level's tint.
@fragment
fn fs_bright_hdr(in: VertexOutput) -> @location(0) vec4f {
  let size = vec2i(textureDimensions(src));
  let t = p.texel.xy;
  var sum = bright_tap_hdr(in.uv + vec2f(-t.x, -t.y), size);
  sum += bright_tap_hdr(in.uv + vec2f(t.x, -t.y), size);
  sum += bright_tap_hdr(in.uv + vec2f(-t.x, t.y), size);
  sum += bright_tap_hdr(in.uv + vec2f(t.x, t.y), size);
  return vec4f(sum * 0.25 * p.tint.rgb, 1.0);
}

// The single tone of the HDR scene (hdr_display_linear: the curve, or its identity line), then the
// colour grade over the tone-mapped linear colour, and the bloom's b / (1 + b) added to the graded
// colour (Remastered's composite runs after the tonemap pass), encoded for the display EFB.
@fragment
fn fs_composite_hdr(in: VertexOutput) -> @location(0) vec4f {
  let size = vec2i(textureDimensions(src));
  let f = textureLoad(src, min(vec2i(floor(in.pos.xy)), size - vec2i(1)), 0);
  let lin = hdr_display_linear(f.rgb, p.tone[0], p.tone[1], p.tone[2]);
  let at = lin * (32.0 / 33.0) + vec3f(0.5 / 33.0);
  let a = select(lin, textureSampleLevel(lutA, samp, at, 0.0).rgb, p.grade.y > 0.5);
  let g = select(lin, textureSampleLevel(lutB, samp, at, 0.0).rgb, p.grade.z > 0.5);
  var graded = mix(a, g, p.grade.x);
  if (p.grade.w > 0.5) {
    let b = max(textureSampleLevel(bloomTex, samp, in.uv, 0.0).rgb, vec3f(0.0));
    graded += b / (1.0 + b);
  }
  // Remastered folds the HUD's damage Add filter in here (material 0007259), not after the encode.
  if (p.add.w > 0.0) {
    graded = composite_add(graded, p.add);
  }
  // And the radial damage fan (shader 00075d2), the HUD's other filter shape.
  if (p.radial[3].x > 0.5) {
    let ndc = vec2f(in.uv.x * 2.0 - 1.0, 1.0 - in.uv.y * 2.0);
    let o = composite_radial(vec4f(graded, f.a), ndc, p.radial[0], p.radial[1], p.radial[2]);
    return vec4f(hdr_srgb_enc(o.rgb), o.a);
  }
  return vec4f(hdr_srgb_enc(graded), f.a);
}
)";

struct Level {
  wgpu::Texture texture;
  wgpu::TextureView view;
  uint32_t width = 0;
  uint32_t height = 0;
};

struct BindGroupKey {
  WGPUTextureView source;
  WGPUTextureView bloomSource;
  WGPUTextureView lutA;
  WGPUTextureView lutB;
  uint32_t slot;

  bool operator==(const BindGroupKey&) const = default;
};
struct BindGroupKeyHash {
  size_t operator()(const BindGroupKey& key) const noexcept {
    size_t hash = std::hash<const void*>{}(key.source);
    for (const void* view : {static_cast<const void*>(key.bloomSource), static_cast<const void*>(key.lutA),
                             static_cast<const void*>(key.lutB)}) {
      hash = hash * 31 + std::hash<const void*>{}(view);
    }
    return hash * 31 + key.slot;
  }
};

// A post as recorded (game thread), and what its encode left the in-pass composite (render worker).
struct Recorded {
  Params params{};
  // The HDR scene the post reads, as it was when recorded.
  webgpu::TextureWithSampler color;
  webgpu::TextureWithSampler resolved;
};
struct Encoded {
  bool ready = false;
  wgpu::TextureView source;
  wgpu::TextureView lutA;
  wgpu::TextureView lutB;
};
std::mutex g_recordMutex;
std::array<Recorded, RecordedSlots> g_recorded;
uint32_t g_nextSlot = 0;
uint64_t g_postFrame = UINT64_MAX;
size_t g_postCount = 0;
std::array<Encoded, RecordedSlots> g_encoded;

struct State {
  EncoderTaskId task = InvalidEncoderTask;
  wgpu::ShaderModule module;
  wgpu::BindGroupLayout layout;
  wgpu::PipelineLayout pipelineLayout;
  wgpu::Sampler sampler;
  wgpu::Buffer uniforms;
  wgpu::RenderPipeline bright;
  wgpu::RenderPipeline down;
  wgpu::RenderPipeline up;
  // The composite as the first draw of the EFB pass after the task (see record), with the
  // pipeline for that pass's layout, and what the task left it to draw with.
  DrawTypeId compositeDraw = InvalidDrawType;
  wgpu::RenderPipeline passComposite;
  uint64_t passCompositeKey = 0;
  // The levels; remade when the frame's size changes.
  uint32_t width = 0;
  uint32_t height = 0;
  std::array<Level, Levels> levels;
  // Grade LUTs by id, and a 1^3 stand-in bound where a pass draws none.
  std::unordered_map<uint32_t, wgpu::TextureView> luts;
  wgpu::TextureView noLut;
  // The scene measure's task, its pipeline and its chain of mips (one view per mip, as each mip is
  // drawn from the one before); remade when the chain's size changes.
  EncoderTaskId measureTask = InvalidEncoderTask;
  wgpu::RenderPipeline measure;
  wgpu::Texture measureChain;
  std::vector<wgpu::TextureView> measureViews;
  uint32_t measureWidth = 0;
  uint32_t measureHeight = 0;
  // The passes' bind groups, which only change with the targets, the frame or a LUT. A group
  // holds its views, so no view in a key can be freed and its handle reused while cached.
  std::unordered_map<BindGroupKey, wgpu::BindGroup, BindGroupKeyHash> groups;
};
State g_state;
// ensure_task runs on the producer while the FIFO worker may preflight scene entry.
std::mutex g_taskRegistrationMutex;

// Readbacks of the measure's 1x1. Each slot goes Available -> CopySubmitted (encoded) -> MapPending
// (after the submit) -> Available (mapped and read); readback_slots.hpp holds the transitions and
// their lock, since a map callback can arrive on any thread. The set grows when every slot is busy,
// as Remastered's staging pool does.
using ReadbackSlots = detail::ReadbackSlots<wgpu::Buffer, ReadbackCount>;
wgpu::Buffer make_readback() {
  const wgpu::BufferDescriptor descriptor{
      .label = "Scene Measure Readback",
      .usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
      .size = MeasureBytes,
  };
  return g_device.CreateBuffer(&descriptor);
}
ReadbackSlots g_readbacks(make_readback, true);
// The scene's measure, in g_radianceMutex, with the claim order it came from: a map that finishes
// after a newer one is dropped (Remastered's RetrieveLastCompleted keeps the newest finished). It is
// written from a map callback, under the slots' lock, so the two are always taken in that order.
std::mutex g_radianceMutex;
float g_radiance[3] = {};
uint32_t g_radianceSerial = 0;
uint64_t g_radianceOrder = 0;

float half_to_float(uint16_t h) {
  const uint32_t sign = uint32_t(h >> 15) << 31;
  const uint32_t exponent = (h >> 10) & 0x1f;
  uint32_t mantissa = h & 0x3ff;
  uint32_t bits = 0;
  if (exponent == 0x1f) {
    bits = sign | 0x7f800000u | (mantissa << 13);
  } else if (exponent != 0) {
    bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
  } else if (mantissa != 0) {
    int e = 113;
    while ((mantissa & 0x400) == 0) {
      mantissa <<= 1;
      --e;
    }
    bits = sign | (uint32_t(e) << 23) | ((mantissa & 0x3ff) << 13);
  } else {
    bits = sign;
  }
  float out;
  std::memcpy(&out, &bits, sizeof(out));
  return out;
}

void complete_readback(const ReadbackSlots::Key& key, wgpu::MapAsyncStatus status, wgpu::StringView message) {
  if (status != wgpu::MapAsyncStatus::Success && status != wgpu::MapAsyncStatus::CallbackCancelled &&
      status != wgpu::MapAsyncStatus::Aborted) {
    Log.warn("scene measure readback failed: {}", message);
  }
  g_readbacks.complete(key, [status](const wgpu::Buffer& buffer, float exposure, uint64_t order) {
    if (status != wgpu::MapAsyncStatus::Success) {
      return;
    }
    // A failed map keeps the last value, as Remastered's does.
    const auto* texel = static_cast<const uint16_t*>(buffer.GetConstMappedRange(0, MeasureBytes));
    if (texel != nullptr && exposure > 0.f) {
      const std::lock_guard lock(g_radianceMutex);
      if (order > g_radianceOrder) {
        g_radianceOrder = order;
        // The HDR scene holds the exposed light: undone by the exposure the frame was drawn at.
        for (int c = 0; c < 3; ++c) {
          g_radiance[c] = half_to_float(texel[c]) / exposure;
        }
        ++g_radianceSerial;
      }
    }
    buffer.Unmap();
  });
}

// LUTs set by the game thread, uploaded by the next encode.
std::mutex g_pendingMutex;
std::unordered_map<uint32_t, std::vector<uint8_t>> g_pendingLuts;

wgpu::TextureView make_lut(uint32_t size, const uint8_t* rgba, const wgpu::Queue& queue) {
  const wgpu::TextureDescriptor descriptor{
      .label = "Colour Grade LUT",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e3D,
      .size = {size, size, size},
      .format = wgpu::TextureFormat::RGBA8Unorm,
  };
  AURORA_NOTE_TEXTURE(&descriptor);
  const auto texture = g_device.CreateTexture(&descriptor);
  const wgpu::TexelCopyTextureInfo dst{.texture = texture};
  const wgpu::TexelCopyBufferLayout layout{.bytesPerRow = size * 4, .rowsPerImage = size};
  const wgpu::Extent3D extent{size, size, size};
  queue.WriteTexture(&dst, rgba, size_t(size) * size * size * 4, &layout, &extent);
  return texture.CreateView();
}

void upload_pending(const wgpu::Queue& queue) {
  std::unordered_map<uint32_t, std::vector<uint8_t>> pending;
  {
    std::lock_guard lock(g_pendingMutex);
    pending.swap(g_pendingLuts);
  }
  for (const auto& [id, data] : pending) {
    g_state.luts[id] = make_lut(GradeLutSize, data.data(), queue);
  }
  if (!pending.empty()) {
    g_state.groups.clear();
  }
  if (!g_state.noLut) {
    const uint8_t white[4]{255, 255, 255, 255};
    g_state.noLut = make_lut(1, white, queue);
  }
}

wgpu::TextureView find_lut(uint32_t id) {
  if (id != 0) {
    if (const auto it = g_state.luts.find(id); it != g_state.luts.end()) {
      return it->second;
    }
  }
  return {};
}

wgpu::RenderPipeline make_pipeline(const char* label, const char* entry, wgpu::TextureFormat format,
                                   const wgpu::BlendState* blend) {
  const wgpu::ColorTargetState target{
      .format = format,
      .blend = blend,
      .writeMask = wgpu::ColorWriteMask::All,
  };
  const wgpu::FragmentState fragment{
      .module = g_state.module,
      .entryPoint = entry,
      .targetCount = 1,
      .targets = &target,
  };
  const wgpu::RenderPipelineDescriptor descriptor{
      .label = label,
      .layout = g_state.pipelineLayout,
      .vertex = {.module = g_state.module, .entryPoint = "vs_main"},
      .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
      .fragment = &fragment,
  };
  return g_device.CreateRenderPipeline(&descriptor);
}

void ensure_pipelines() {
  if (g_state.module) {
    return;
  }
  std::string code{ShaderSource};
  code += hdr::ToneWgsl;
  code += hdr::PostToneWgsl;
  code += CompositeAddWgsl;
  code += CompositeRadialWgsl;
  code += HdrSource;
  wgpu::ShaderSourceWGSL source{};
  source.code = code.c_str();
  const wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &source, .label = "Bloom Module"};
  g_state.module = g_device.CreateShaderModule(&moduleDescriptor);
  const std::array entries{
      wgpu::BindGroupLayoutEntry{
          .binding = 0,
          .visibility = wgpu::ShaderStage::Fragment,
          .sampler = {.type = wgpu::SamplerBindingType::Filtering},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 1,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture = {.sampleType = wgpu::TextureSampleType::Float, .viewDimension = wgpu::TextureViewDimension::e2D},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 2,
          .visibility = wgpu::ShaderStage::Fragment,
          .buffer = {.type = wgpu::BufferBindingType::Uniform, .minBindingSize = sizeof(Uniform)},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 3,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture = {.sampleType = wgpu::TextureSampleType::Float, .viewDimension = wgpu::TextureViewDimension::e2D},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 4,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture = {.sampleType = wgpu::TextureSampleType::Float, .viewDimension = wgpu::TextureViewDimension::e3D},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 5,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture = {.sampleType = wgpu::TextureSampleType::Float, .viewDimension = wgpu::TextureViewDimension::e3D},
      },
  };
  const wgpu::BindGroupLayoutDescriptor layoutDescriptor{
      .label = "Bloom Bind Group Layout",
      .entryCount = entries.size(),
      .entries = entries.data(),
  };
  g_state.layout = g_device.CreateBindGroupLayout(&layoutDescriptor);
  const wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor{
      .label = "Bloom Pipeline Layout",
      .bindGroupLayoutCount = 1,
      .bindGroupLayouts = &g_state.layout,
  };
  g_state.pipelineLayout = g_device.CreatePipelineLayout(&pipelineLayoutDescriptor);
  const wgpu::SamplerDescriptor samplerDescriptor{
      .label = "Bloom Sampler",
      .addressModeU = wgpu::AddressMode::ClampToEdge,
      .addressModeV = wgpu::AddressMode::ClampToEdge,
      .magFilter = wgpu::FilterMode::Linear,
      .minFilter = wgpu::FilterMode::Linear,
  };
  g_state.sampler = g_device.CreateSampler(&samplerDescriptor);
  const wgpu::BufferDescriptor bufferDescriptor{
      .label = "Bloom Uniforms",
      .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
      .size = SlotSize * PassCount * RecordedSlots,
  };
  g_state.uniforms = g_device.CreateBuffer(&bufferDescriptor);
  g_state.bright = make_pipeline("Bloom Bright Pass", "fs_bright_hdr", LevelFormat, nullptr);
  g_state.down = make_pipeline("Bloom Downsample", "fs_down", LevelFormat, nullptr);
  const wgpu::BlendState add{
      .color = {.operation = wgpu::BlendOperation::Add,
                .srcFactor = wgpu::BlendFactor::One,
                .dstFactor = wgpu::BlendFactor::One},
      .alpha = {.operation = wgpu::BlendOperation::Add,
                .srcFactor = wgpu::BlendFactor::Zero,
                .dstFactor = wgpu::BlendFactor::One},
  };
  g_state.up = make_pipeline("Bloom Upsample", "fs_up", LevelFormat, &add);
  g_state.measure = make_pipeline("Scene Measure", "fs_measure", MeasureFormat, nullptr);
}

void ensure_levels(uint32_t width, uint32_t height) {
  if (g_state.levels[0].texture && g_state.width == width && g_state.height == height) {
    return;
  }
  g_state.width = width;
  g_state.height = height;
  g_state.groups.clear();
  uint32_t w = (width + 3) / 4;
  uint32_t h = (height + 3) / 4;
  for (auto& level : g_state.levels) {
    level.width = std::max(w, 1u);
    level.height = std::max(h, 1u);
    const wgpu::TextureDescriptor descriptor{
        .label = "Bloom Level",
        .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
        .size = {level.width, level.height, 1},
        .format = LevelFormat,
    };
    AURORA_NOTE_TEXTURE(&descriptor);
    level.texture = g_device.CreateTexture(&descriptor);
    level.view = level.texture.CreateView();
    w = (w + 1) / 2;
    h = (h + 1) / 2;
  }
}

wgpu::BindGroup bind_group(uint32_t slot, const wgpu::TextureView& source, const wgpu::TextureView& bloomSource,
                           const wgpu::TextureView& lutA, const wgpu::TextureView& lutB) {
  const wgpu::TextureView& viewA = lutA ? lutA : g_state.noLut;
  const wgpu::TextureView& viewB = lutB ? lutB : g_state.noLut;
  const BindGroupKey key{source.Get(), bloomSource.Get(), viewA.Get(), viewB.Get(), slot};
  // A frame that keeps changing (a source remade every frame) cannot grow this for ever.
  if (g_state.groups.size() > 256 && !g_state.groups.contains(key)) {
    g_state.groups.clear();
  }
  wgpu::BindGroup& group = g_state.groups[key];
  if (!group) {
    const std::array entries{
        wgpu::BindGroupEntry{.binding = 0, .sampler = g_state.sampler},
        wgpu::BindGroupEntry{.binding = 1, .textureView = source},
        wgpu::BindGroupEntry{
            .binding = 2, .buffer = g_state.uniforms, .offset = slot * SlotSize, .size = sizeof(Uniform)},
        wgpu::BindGroupEntry{.binding = 3, .textureView = bloomSource},
        wgpu::BindGroupEntry{.binding = 4, .textureView = viewA},
        wgpu::BindGroupEntry{.binding = 5, .textureView = viewB},
    };
    const wgpu::BindGroupDescriptor groupDescriptor{
        .layout = g_state.layout,
        .entryCount = entries.size(),
        .entries = entries.data(),
    };
    group = g_device.CreateBindGroup(&groupDescriptor);
  }
  return group;
}

void draw(const wgpu::CommandEncoder& cmd, const char* name, const wgpu::RenderPipeline& pipeline,
          const wgpu::BindGroup& group, const wgpu::TextureView& target, bool load) {
  const wgpu::RenderPassColorAttachment attachment{
      .view = target,
      .loadOp = load ? wgpu::LoadOp::Load : wgpu::LoadOp::Clear,
      .storeOp = wgpu::StoreOp::Store,
      .clearValue = {0.0, 0.0, 0.0, 0.0},
  };
  const wgpu::RenderPassDescriptor passDescriptor{
      .label = "Bloom Pass",
      .colorAttachmentCount = 1,
      .colorAttachments = &attachment,
      .timestampWrites = webgpu::gpu_prof::pass_writes(name),
  };
  const auto pass = cmd.BeginRenderPass(&passDescriptor);
  pass.SetPipeline(pipeline);
  pass.SetBindGroup(0, group);
  pass.Draw(3);
  pass.End();
}

void draw(const wgpu::CommandEncoder& cmd, const char* name, const wgpu::RenderPipeline& pipeline, uint32_t slot,
          const wgpu::TextureView& source, const wgpu::TextureView& bloomSource, const wgpu::TextureView& target,
          bool load) {
  draw(cmd, name, pipeline, bind_group(slot, source, bloomSource, {}, {}), target, load);
}

// The composite's pipeline for the EFB pass it is drawn in: the frame as its first target, the
// pass's other targets untouched, its depth neither tested nor written.
wgpu::RenderPipeline make_pass_composite(const RenderTargetLayout& layout, const wgpu::ShaderModule& module,
                                         const char* entry) {
  std::array<wgpu::ColorTargetState, MaxColorAttachments> targets{};
  for (uint32_t i = 0; i < layout.colorAttachmentCount; ++i) {
    targets[i] = {
        .format = layout.colorAttachments[i].format,
        .writeMask = i == SceneColorAttachmentIndex ? wgpu::ColorWriteMask::All : wgpu::ColorWriteMask::None,
    };
  }
  const wgpu::FragmentState fragment{
      .module = module,
      .entryPoint = entry,
      .targetCount = layout.colorAttachmentCount,
      .targets = targets.data(),
  };
  const wgpu::DepthStencilState depth{
      .format = layout.depthStencilFormat,
      .depthWriteEnabled = false,
      .depthCompare = wgpu::CompareFunction::Always,
  };
  const wgpu::RenderPipelineDescriptor descriptor{
      .label = "Bloom Composite (EFB pass)",
      .layout = g_state.pipelineLayout,
      .vertex = {.module = module, .entryPoint = "vs_main"},
      .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
      .depthStencil = layout.depthStencilFormat != wgpu::TextureFormat::Undefined ? &depth : nullptr,
      .multisample = {.count = layout.sampleCount, .mask = UINT32_MAX},
      .fragment = &fragment,
  };
  return g_device.CreateRenderPipeline(&descriptor);
}

// The composite drawn first in the pass after the task, which cleared the frame rather than load
// it: a full-resolution store and load fewer than a pass of its own (see record). The payload is the
// post's slot. The pass is the display EFB's (record_encoder_task_overwriting's leaveSceneHdr): a
// scene-format target here would be the wrong one, and is left undrawn.
void draw_pass_composite(const DrawContext& ctx, const wgpu::RenderPassEncoder& pass, const void* payload,
                         size_t payloadSize, void*) {
  uint32_t rec = 0;
  if (payloadSize != sizeof(rec)) {
    return;
  }
  std::memcpy(&rec, payload, sizeof(rec));
  if (rec >= RecordedSlots) {
    return;
  }
  Encoded& encoded = g_encoded[rec];
  if (!encoded.ready) {
    return;
  }
  encoded.ready = false;
  const auto& frame = ctx.layout.colorAttachments[SceneColorAttachmentIndex];
  if (frame.format == webgpu::SceneHdrFormat) {
    Log.warn("HDR post composite reached an HDR scene target; not drawn");
    encoded = {};
    return;
  }
  if (!g_state.passComposite || g_state.passCompositeKey != ctx.layout.key) {
    g_state.passComposite = make_pass_composite(ctx.layout, g_state.module, "fs_composite_hdr");
    g_state.passCompositeKey = ctx.layout.key;
  }
  pass.SetPipeline(g_state.passComposite);
  pass.SetBindGroup(0, bind_group(rec * PassCount + PassCount - 1, encoded.source, g_state.levels[0].view,
                                  encoded.lutA, encoded.lutB));
  pass.SetViewport(0.f, 0.f, float(frame.width), float(frame.height), 0.f, 1.f);
  pass.SetScissorRect(0, 0, frame.width, frame.height);
  pass.Draw(3);
  encoded = {};
}

// The HDR scene a post recorded, checked whole: the scene format, a single-sample resolve of the same
// size when multisampled. Anything else fails closed (no post) rather than read 16F texels as 8-bit.
bool hdr_source_valid(const webgpu::TextureWithSampler& c, const webgpu::TextureWithSampler& v, uint32_t samples) {
  if (!c.texture || !c.view || c.format != webgpu::SceneHdrFormat || c.texture.GetFormat() != webgpu::SceneHdrFormat ||
      c.texture.GetSampleCount() != samples || c.size.width == 0 || c.size.height == 0) {
    return false;
  }
  if (samples > 1) {
    return v.texture && v.view && v.format == webgpu::SceneHdrFormat &&
           v.texture.GetFormat() == webgpu::SceneHdrFormat && v.texture.GetSampleCount() == 1 &&
           v.size.width == c.size.width && v.size.height == c.size.height;
  }
  return true;
}

void encode(const EncoderTaskContext& ctx, const wgpu::CommandEncoder& cmd, const void* payload, size_t payloadSize,
            void*) {
  uint32_t rec = 0;
  if (payloadSize != sizeof(rec)) {
    return;
  }
  std::memcpy(&rec, payload, sizeof(rec));
  if (rec >= RecordedSlots) {
    return;
  }
  g_encoded[rec] = {};
  Recorded recorded;
  {
    const std::lock_guard lock(g_recordMutex);
    recorded = g_recorded[rec];
  }
  Params& params = recorded.params;
  const uint32_t samples = webgpu::g_graphicsConfig.msaaSamples > 1 ? webgpu::g_graphicsConfig.msaaSamples : 1;
  if (!hdr_source_valid(recorded.color, recorded.resolved, samples)) {
    // The pass after it was retargeted to the display EFB and clears it: its composite stays undrawn.
    Log.warn("HDR post: the recorded scene is not a valid {}-sample RGBA16F target; skipped", samples);
    return;
  }
  // The HDR scene is read where it is (resolved when multisampled): the composite writes the display EFB.
  const auto& hdrSource = samples > 1 ? recorded.resolved : recorded.color;
  const uint32_t width = hdrSource.size.width;
  const uint32_t height = hdrSource.size.height;
  ensure_pipelines();
  ensure_levels(width, height);
  upload_pending(ctx.queue);
  const auto lutA = find_lut(params.gradeA);
  const auto lutB = find_lut(params.gradeB);
  const bool bloom = (params.bloom & BloomOn) != 0;
  // tone[0][3] is unused (it carried the exposure of the frame average measure_scene replaced).
  params.tone[0][3] = 0.f;

  // Every pass's uniforms: bright, the downsamples, the upsamples, the composite; in the post's own
  // ranges, as another post in this submit writes its own before either runs.
  const uint32_t base = rec * PassCount;
  std::array<std::array<uint8_t, SlotSize>, PassCount> slots{};
  const auto put = [&](uint32_t slot, float texelX, float texelY, const float* tint) {
    Uniform u{};
    u.texel[0] = texelX;
    u.texel[1] = texelY;
    if (tint != nullptr) {
      std::memcpy(u.tint, tint, sizeof(float) * 3);
    }
    u.tint[3] = params.threshold;
    std::memcpy(u.tone, params.tone, sizeof(u.tone));
    u.grade[0] = std::clamp(params.gradeWeight, 0.f, 1.f);
    u.grade[1] = lutA ? 1.f : 0.f;
    u.grade[2] = lutB ? 1.f : 0.f;
    u.grade[3] = bloom ? 1.f : 0.f;
    std::memcpy(u.add, params.add, sizeof(u.add));
    std::memcpy(u.radial, params.radial, sizeof(params.radial));
    u.radial[3][0] = params.radialOn != 0 ? 1.f : 0.f;
    std::memcpy(slots[slot].data(), &u, sizeof(u));
  };
  uint32_t slot = 0;
  put(slot++, 1.f / float(width), 1.f / float(height), params.tints[4]);
  for (uint32_t i = 1; i < Levels; ++i) {
    const auto& from = g_state.levels[i - 1];
    put(slot++, 1.f / float(from.width), 1.f / float(from.height), nullptr);
  }
  for (uint32_t i = Levels - 1; i-- > 0;) {
    const auto& from = g_state.levels[i + 1];
    put(slot++, 1.f / float(from.width), 1.f / float(from.height), params.tints[Levels - 2 - i]);
  }
  put(slot++, 0.f, 0.f, nullptr);
  ctx.queue.WriteBuffer(g_state.uniforms, uint64_t(base) * SlotSize, slots.data(), sizeof(slots));

  // The chain reads the HDR scene as it is: the composite writes another target.
  const wgpu::TextureView& frameView = hdrSource.view;

  slot = base;
  auto& levels = g_state.levels;
  if (bloom) {
    draw(cmd, "Bloom bright", g_state.bright, slot++, frameView, frameView, levels[0].view,
         false);
    for (uint32_t i = 1; i < Levels; ++i) {
      draw(cmd, "Bloom down", g_state.down, slot++, levels[i - 1].view, levels[i - 1].view, levels[i].view, false);
    }
    for (uint32_t i = Levels - 1; i-- > 0;) {
      draw(cmd, "Bloom up", g_state.up, slot++, levels[i + 1].view, levels[i + 1].view, levels[i].view, true);
    }
  }
  // The composite is the first draw of the display EFB's pass after the task (record).
  g_encoded[rec] = {.ready = true, .source = frameView, .lutA = lutA, .lutB = lutB};
}

// The measures recorded by measure_scene, each the scene it reads and the exposure it was drawn at.
struct Measure {
  float exposure = 0.f;
  webgpu::TextureWithSampler color;
  webgpu::TextureWithSampler resolved;
};
std::mutex g_measureMutex;
std::array<Measure, MeasureSlots> g_measures;
uint32_t g_nextMeasure = 0;
uint64_t g_measureFrame = UINT64_MAX;

uint32_t ceil_pow2(uint32_t v) {
  uint32_t p = 1;
  while (p < v) {
    p <<= 1;
  }
  return p;
}

void ensure_measure_chain(uint32_t width, uint32_t height) {
  if (g_state.measureChain && g_state.measureWidth == width && g_state.measureHeight == height) {
    return;
  }
  g_state.measureWidth = width;
  g_state.measureHeight = height;
  g_state.groups.clear();
  uint32_t mips = 1;
  for (uint32_t edge = std::max(width, height); edge > 1; edge >>= 1) {
    ++mips;
  }
  const wgpu::TextureDescriptor descriptor{
      .label = "Scene Measure Chain",
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
               wgpu::TextureUsage::CopySrc,
      .size = {width, height, 1},
      .format = MeasureFormat,
      .mipLevelCount = mips,
  };
  AURORA_NOTE_TEXTURE(&descriptor);
  g_state.measureChain = g_device.CreateTexture(&descriptor);
  g_state.measureViews.clear();
  for (uint32_t mip = 0; mip < mips; ++mip) {
    const wgpu::TextureViewDescriptor view{.baseMipLevel = mip, .mipLevelCount = 1};
    g_state.measureViews.push_back(g_state.measureChain.CreateView(&view));
  }
}

void encode_measure(const EncoderTaskContext& ctx, const wgpu::CommandEncoder& cmd, const void* payload,
                    size_t payloadSize, void*) {
  uint32_t index = 0;
  if (payloadSize != sizeof(index)) {
    return;
  }
  std::memcpy(&index, payload, sizeof(index));
  if (index >= MeasureSlots) {
    return;
  }
  Measure measure;
  {
    const std::lock_guard lock(g_measureMutex);
    measure = g_measures[index];
  }
  const uint32_t samples = webgpu::g_graphicsConfig.msaaSamples > 1 ? webgpu::g_graphicsConfig.msaaSamples : 1;
  if (!hdr_source_valid(measure.color, measure.resolved, samples)) {
    Log.warn("scene measure: the recorded scene is not a valid {}-sample RGBA16F target; skipped", samples);
    return;
  }
  const auto& source = samples > 1 ? measure.resolved : measure.color;
  ensure_pipelines();
  ensure_measure_chain(ceil_pow2(std::max(source.size.width >> 1, 1u)),
                       ceil_pow2(std::max(source.size.height >> 1, 1u)));
  upload_pending(ctx.queue);
  // None free cannot happen (the pool grows); an empty buffer would only follow a failed creation.
  const wgpu::Buffer buffer = g_readbacks.claim(measure.exposure);
  if (!buffer) {
    return;
  }
  // The shader reads no uniforms; slot 0 only fills the layout.
  const auto& views = g_state.measureViews;
  draw(cmd, "Scene measure", g_state.measure, 0, source.view, source.view, views[0], false);
  for (size_t mip = 1; mip < views.size(); ++mip) {
    draw(cmd, "Scene measure", g_state.measure, 0, views[mip - 1], views[mip - 1], views[mip], false);
  }
  const wgpu::TexelCopyTextureInfo copySource{.texture = g_state.measureChain,
                                              .mipLevel = uint32_t(views.size() - 1)};
  const wgpu::TexelCopyBufferInfo copyTarget{
      .layout = {.bytesPerRow = MeasureRowBytes, .rowsPerImage = 1},
      .buffer = buffer,
  };
  const wgpu::Extent3D copySize{1, 1, 1};
  cmd.CopyTextureToBuffer(&copySource, &copyTarget, &copySize);
}

// A slot for a post in this frame; false (warned) past MaxPostsPerFrame. Under g_recordMutex.
bool claim_slot(uint32_t& slot) {
  const uint64_t frame = current_frame();
  if (g_postFrame != frame) {
    g_postFrame = frame;
    g_postCount = 0;
  }
  if (g_postCount >= MaxPostsPerFrame) {
    Log.warn("more than {} posts in one frame; this one is skipped", MaxPostsPerFrame);
    return false;
  }
  ++g_postCount;
  slot = g_nextSlot++ % RecordedSlots;
  return true;
}
} // namespace

bool ensure_task() {
  const std::lock_guard registrationLock(g_taskRegistrationMutex);
  if (g_state.task == InvalidEncoderTask) {
    g_state.task = register_encoder_task_type(EncoderTaskDescriptor{.label = "Bloom", .callback = encode});
    if (g_state.task == InvalidEncoderTask) {
      Log.warn("could not register the bloom task");
      return false;
    }
  }
  if (g_state.compositeDraw == InvalidDrawType) {
    g_state.compositeDraw = register_draw_type(DrawTypeDescriptor{.label = "Bloom Composite", .draw = draw_pass_composite});
  }
  return true;
}

bool measure_scene(float exposure) {
  {
    const std::lock_guard registrationLock(g_taskRegistrationMutex);
    if (g_state.measureTask == InvalidEncoderTask) {
      g_state.measureTask =
          register_encoder_task_type(EncoderTaskDescriptor{.label = "Scene Measure", .callback = encode_measure});
      if (g_state.measureTask == InvalidEncoderTask) {
        Log.warn("could not register the scene measure task");
        return false;
      }
    }
  }
  // Pending FIFO commands first, so the measure reads what they draw.
  gx::fifo::drain();
  if (!scene_hdr_pass_active() || !(exposure > 0.f)) {
    return false;
  }
  uint32_t slot = 0;
  {
    const std::lock_guard lock(g_measureMutex);
    const uint64_t frame = current_frame();
    if (g_measureFrame == frame) {
      return false; // one copy per frame
    }
    g_measureFrame = frame;
    slot = g_nextMeasure++ % MeasureSlots;
    g_measures[slot] = Measure{
        .exposure = exposure, .color = webgpu::g_sceneHdr, .resolved = webgpu::g_sceneHdrResolved};
  }
  return record_encoder_task(g_state.measureTask, &slot, sizeof(slot));
}

bool scene_post_available() {
  // The FIFO preflight and all post records share one writer. Normal teardown stops it before these
  // types are unregistered; invalidation after this check is unsupported failure injection.
  {
    const std::lock_guard registrationLock(g_taskRegistrationMutex);
    if (g_state.task == InvalidEncoderTask || g_state.compositeDraw == InvalidDrawType ||
        !detail::find_runtime_encoder_task_type(g_state.task) ||
        !detail::find_runtime_draw_type(g_state.compositeDraw)) {
      return false;
    }
  }
  const std::lock_guard lock(g_recordMutex);
  const uint64_t frame = current_frame();
  const size_t claimed = g_postFrame == frame ? g_postCount : 0;
  return claimed < MaxPostsPerFrame;
}

bool record(const Params& params) {
  if (g_state.task == InvalidEncoderTask) {
    return false;
  }
  if (!scene_hdr_pass_active()) {
    // No HDR scene to read: outside it the frame is drawn without the post.
    if (scene_hdr_active()) {
      // The HDR scene is on but not this pass's target (an offscreen pass).
      Log.warn("post recorded outside the HDR scene's pass; skipped");
    }
    return false;
  }
  if (g_state.compositeDraw == InvalidDrawType) {
    Log.warn("HDR post without the composite draw type; skipped");
    return false;
  }
  // The composite is drawn as the first thing in the display EFB's pass the FIFO goes on in: on a
  // tile-based GPU a pass of its own stores the frame only for that pass to load it again. That pass
  // clears depth instead of loading it when nothing after the bloom (the HUD) tests against the world's.
  // The HDR scene is converted to display colour whatever else the post does, so it always draws.
  Params task = params;
  task.bloom &= BloomOn | CostNoDepthReload | CostKeepDepth;
  task.bloom |= CompositeInPass;
  uint32_t slot = 0;
  {
    const std::lock_guard lock(g_recordMutex);
    if (!claim_slot(slot)) {
      return false;
    }
    g_recorded[slot] = Recorded{
        .params = task,
        .color = webgpu::g_sceneHdr,
        .resolved = webgpu::g_sceneHdrResolved,
    };
  }
  const auto depth = (task.bloom & CostNoDepthReload) ? DepthAfter::Clear
                     : (task.bloom & CostKeepDepth)   ? DepthAfter::Load
                                                      : DepthAfter::IfUnread;
  return record_encoder_task_overwriting(g_state.task, &slot, sizeof(slot), g_state.compositeDraw, depth, &slot,
                                         sizeof(slot), true);
}

void set_grade_lut(uint32_t id, const uint8_t* rgba) {
  if (id == 0 || rgba == nullptr) {
    return;
  }
  std::lock_guard lock(g_pendingMutex);
  g_pendingLuts[id].assign(rgba, rgba + size_t(GradeLutSize) * GradeLutSize * GradeLutSize * 4);
}

void after_submit() noexcept {
  const auto submitted = g_readbacks.submit();
  for (size_t i = 0; i < submitted.count; ++i) {
    const auto& mapping = submitted.mappings[i];
    // complete_readback calls Unmap, which can submit Dawn's pending Metal commands. Dispatch
    // through the render worker's post-encode ProcessEvents points instead of a spontaneous
    // notification callback. Carry only the key so the callback cannot keep a retired buffer
    // alive. No slots' lock is held while requesting the map, since its callback takes it again.
    mapping.buffer.MapAsync(wgpu::MapMode::Read, 0, MeasureBytes, wgpu::CallbackMode::AllowProcessEvents,
                            [key = mapping.key](wgpu::MapAsyncStatus status, wgpu::StringView message) {
                              complete_readback(key, status, message);
                            });
  }
}

bool frame_radiance(float out[3], uint32_t& serial) {
  std::lock_guard lock(g_radianceMutex);
  serial = g_radianceSerial;
  if (g_radianceSerial == 0) {
    return false;
  }
  std::memcpy(out, g_radiance, sizeof(g_radiance));
  return true;
}

void shutdown() {
  // The readback buffers come out of the slots under the slots' lock, and are released only once
  // it is gone: releasing one with a map still in flight runs that map's callback on this thread,
  // and the callback takes the lock again.
  auto retired = g_readbacks.retire([] {
    const std::lock_guard lock(g_radianceMutex);
    g_radianceSerial = 0;
    g_radianceOrder = 0;
  });
  retired.clear();
  const auto task = g_state.task;
  const auto measureTask = g_state.measureTask;
  const auto compositeDraw = g_state.compositeDraw;
  g_state = {};
  g_encoded = {};
  {
    const std::lock_guard lock(g_recordMutex);
    g_recorded = {};
    g_nextSlot = 0;
    g_postFrame = UINT64_MAX;
    g_postCount = 0;
  }
  {
    std::lock_guard lock(g_pendingMutex);
    g_pendingLuts.clear();
  }
  {
    const std::lock_guard lock(g_measureMutex);
    g_measures = {};
    g_nextMeasure = 0;
    g_measureFrame = UINT64_MAX;
  }
  if (task != InvalidEncoderTask) {
    unregister_encoder_task_type(task);
  }
  if (measureTask != InvalidEncoderTask) {
    unregister_encoder_task_type(measureTask);
  }
  if (compositeDraw != InvalidDrawType) {
    unregister_draw_type(compositeDraw);
  }
}
} // namespace aurora::gfx::bloom
