#include "thermal.hpp"

#include "../logging.hpp"
#include "../webgpu/gpu.hpp"
#include "../webgpu/gpu_prof.hpp"
#include "recording.hpp"

#include <aurora/gfx.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// Remastered's thermal visor post (NRenderThermalVisor), run on the finished EFB:
//  - cold (shader 7e5c16a5, after the cold actors): the frame sampled at the pixel plus a
//    heat-haze offset from a 64x64 R8 noise texture (nearest, repeat; uv scaled by viewport width
//    / 64 and shifted by two CRandom16 draws per call, offset = (r, 0) 0.02 - 0.01, y flipped),
//    each channel x of the exposed colour mapped to x/(x+1) 0.5;
//  - hot (d9ee4f80, after the hot actors) into a half-resolution target (fb >> 1, in sixths while
//    the visor ramps up for 0.5 s): the luminance L of the exposed frame goes through a 256-texel
//    heat gradient, lut(heat + L) rescaled to luminance 2L + 0.5 clamp((heat - 0.8) / 0.2) and
//    mixed by smoothstep(clamp((L - 0.45) 10)) with lut(0.3 heat + L); then, past 0.1 s of
//    thermal time, the previous frame's target blended in at 0.6;
//  - upscale (a1dc45c4) back to the frame: the target, darkened by vertical stripes of period
//    width / 200 pixels (down to 0.896).
// Remastered keeps this in HDR and divides the exposure out of the luminance (a shader constant
// E on both sides), so in the port's exposed domain (the EFB undone through the room's tone
// curve) E cancels. The EFB holds the tone-mapped colour sRGB encoded; the result is drawn
// through the curve and encoded again.
namespace aurora::gfx::thermal {
namespace {
Module Log("aurora::gfx::thermal");
using webgpu::g_device;

constexpr wgpu::TextureFormat TargetFormat = wgpu::TextureFormat::RGBA16Float;
constexpr uint32_t NoiseSize = 64;
constexpr size_t LutBytes = 256 * 4 * 4;

constexpr const char* Source = R"(
struct Uniforms {
  tone: array<vec4f, 3>,
  v: vec4f,
  r: vec4f,
  rt: vec4f,  // the target's region this frame (w, h) and the previous frame's
  fb: vec4f,  // the frame's size, the target's allocation
};
@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var src: texture_2d<f32>;
@group(0) @binding(2) var lutTex: texture_2d<f32>;
@group(0) @binding(3) var noiseTex: texture_2d<f32>;
@group(0) @binding(4) var prevTex: texture_2d<f32>;
@group(0) @binding(5) var clampSamp: sampler;
@group(0) @binding(6) var repeatSamp: sampler;

struct VertexOutput {
  @builtin(position) pos: vec4f,
};

@vertex
fn vs_main(@builtin(vertex_index) i: u32) -> VertexOutput {
  var corners = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
  var out: VertexOutput;
  out.pos = vec4f(corners[i], 0.0, 1.0);
  return out;
}

fn tone(x: f32) -> f32 {
  if (x < u.tone[1].z) {
    return ((u.tone[0].x * x + u.tone[0].y) * x + u.tone[0].z) * x;
  }
  if (x < u.tone[1].w) {
    return u.tone[1].x * x + u.tone[1].y;
  }
  let st = max(u.tone[2].y * x + u.tone[2].z, 0.0);
  return u.tone[2].x * st / (1.0 + st) + u.tone[2].w;
}

fn untone(y: f32) -> f32 {
  let mid = u.tone[1].z;
  let lineStart = u.tone[1].x * mid + u.tone[1].y;
  if (y < lineStart) {
    var x = y / max(lineStart, 1e-4) * mid;
    for (var n = 0; n < 4; n++) {
      let slope = (3.0 * u.tone[0].x * x + 2.0 * u.tone[0].y) * x + u.tone[0].z;
      x = clamp(x - (tone(x) - y) / max(slope, 1e-4), 0.0, mid);
    }
    return x;
  }
  let top = u.tone[2].w;
  if (y < top || u.tone[2].y <= 0.0) {
    return (y - u.tone[1].y) / u.tone[1].x;
  }
  let t = min((y - top) / max(u.tone[2].x, 1e-4), 0.999);
  return t / (1.0 - t) / u.tone[2].y + u.tone[1].w;
}

fn tone3(x: vec3f) -> vec3f { return vec3f(tone(x.r), tone(x.g), tone(x.b)); }
fn untone3(y: vec3f) -> vec3f { return vec3f(untone(y.r), untone(y.g), untone(y.b)); }

fn srgb_enc(c: vec3f) -> vec3f {
  let l = clamp(c, vec3f(0.0), vec3f(1.0));
  return select(1.055 * pow(l, vec3f(1.0 / 2.4)) - 0.055, 12.92 * l, l <= vec3f(0.0031308));
}

fn srgb_dec(c: vec3f) -> vec3f {
  let e = clamp(c, vec3f(0.0), vec3f(1.0));
  return select(pow((e + 0.055) / 1.055, vec3f(2.4)), e / 12.92, e <= vec3f(0.04045));
}

// The exposed colour of the frame at a uv.
fn scene(uv: vec2f) -> vec3f {
  return untone3(srgb_dec(textureSampleLevel(src, clampSamp, uv, 0.0).rgb));
}

@fragment
fn fs_cold(@builtin(position) pos: vec4f) -> @location(0) vec4f {
  let size = u.fb.xy;
  let uv = pos.xy / size;
  // uv scale = viewport width / noise width (0xfb1aa0); the R8 noise reads as (r, 0).
  let n = textureSampleLevel(noiseTex, repeatSamp, uv * (size.x / 64.0) + u.r.xy, 0.0).r;
  let off = vec2f(n, 0.0) * 0.02 - 0.01;
  let x = scene(uv + vec2f(off.x, -off.y));
  let g = x / (x + vec3f(1.0)) * 0.5;
  return vec4f(srgb_enc(tone3(g)), 1.0);
}

fn lut(v: f32) -> vec3f {
  return textureSampleLevel(lutTex, clampSamp, vec2f(clamp(v, 0.0, 1.0), 0.5), 0.0).rgb;
}

fn luma(c: vec3f) -> f32 { return dot(c, vec3f(0.2126, 0.7152, 0.0722)); }

@fragment
fn fs_hot(@builtin(position) pos: vec4f) -> @location(0) vec4f {
  let x = scene(pos.xy / u.rt.xy);
  let l = luma(x);
  let heat = u.v.y;
  let lut1 = lut(heat * 1.0 + l);
  let lut2 = lut(heat * 0.3 + l);
  let k = 2.0 * l + clamp((heat - 0.8) / 0.2, 0.0, 1.0) * 0.5;
  let col = clamp(lut1 / max(luma(lut1), 1.0 / 255.0) * k, vec3f(0.0), vec3f(1.0));
  let m = smoothstep(0.0, 1.0, clamp((l - 0.45) * 10.0, 0.0, 1.0));
  return vec4f(mix(col, lut2, m), 1.0);
}

// Blur mode 1 (blur_instance 0x187710): a 5-tap binomial (0.0625, 0.25, 0.375, 0.25, 0.0625), one axis
// a pass, taps a texel apart, clamped to the drawn region.
fn blur(pos: vec2f, dir: vec2f) -> vec4f {
  let texel = 1.0 / u.fb.zw;
  let lo = 0.5 * texel;
  let hi = (u.rt.xy - vec2f(0.5)) * texel;
  let uv = pos / u.fb.zw;
  var sum = textureSampleLevel(src, clampSamp, uv, 0.0).rgb * 0.375;
  sum += textureSampleLevel(src, clampSamp, clamp(uv + dir * texel, lo, hi), 0.0).rgb * 0.25;
  sum += textureSampleLevel(src, clampSamp, clamp(uv - dir * texel, lo, hi), 0.0).rgb * 0.25;
  sum += textureSampleLevel(src, clampSamp, clamp(uv + dir * 2.0 * texel, lo, hi), 0.0).rgb * 0.0625;
  sum += textureSampleLevel(src, clampSamp, clamp(uv - dir * 2.0 * texel, lo, hi), 0.0).rgb * 0.0625;
  return vec4f(sum, 1.0);
}

@fragment
fn fs_blur_h(@builtin(position) pos: vec4f) -> @location(0) vec4f { return blur(pos.xy, vec2f(1.0, 0.0)); }

@fragment
fn fs_blur_v(@builtin(position) pos: vec4f) -> @location(0) vec4f { return blur(pos.xy, vec2f(0.0, 1.0)); }

fn target_uv(uvFrame: vec2f, region: vec2f) -> vec2f {
  return clamp(uvFrame * region / u.fb.zw, 0.5 / u.fb.zw, (region - vec2f(0.5)) / u.fb.zw);
}

// The previous frame's target, blended over the new one by the pipeline (alpha 0.6).
@fragment
fn fs_ghost(@builtin(position) pos: vec4f) -> @location(0) vec4f {
  let uv = pos.xy / u.rt.xy;
  return vec4f(textureSampleLevel(prevTex, clampSamp, target_uv(uv, u.rt.zw), 0.0).rgb, 0.6);
}

@fragment
fn fs_upscale(@builtin(position) pos: vec4f) -> @location(0) vec4f {
  let uv = pos.xy / u.fb.xy;
  let col = textureSampleLevel(src, clampSamp, target_uv(uv, u.rt.xy), 0.0).rgb;
  let t = fract(pos.x * 200.0 / u.fb.x);
  let a = clamp(1.0 - 0.2 * (1.0 - abs(1.0 - 2.0 * t)), 0.0, 1.0);
  let s = a * a * (3.0 - 2.0 * a);
  return vec4f(srgb_enc(tone3(col * s)), 1.0);
}
)";

struct Uniforms {
  Params params;
  float rt[4];
  float fb[4];
};
static_assert(sizeof(Uniforms) == 28 * 4);

struct State {
  EncoderTaskId task = InvalidEncoderTask;
  wgpu::RenderPipeline cold;
  wgpu::RenderPipeline hot;
  wgpu::RenderPipeline ghost;
  wgpu::RenderPipeline blurH;
  wgpu::RenderPipeline blurV;
  wgpu::RenderPipeline upscale;
  wgpu::BindGroupLayout layout;
  wgpu::TextureFormat pipelineFormat = wgpu::TextureFormat::Undefined;
  uint32_t pipelineSamples = 0;
  wgpu::Buffer uniforms;
  wgpu::Sampler clampSampler;
  wgpu::Sampler repeatSampler;
  wgpu::Texture lut;
  wgpu::TextureView lutView;
  wgpu::Texture noise;
  wgpu::TextureView noiseView;
  std::array<wgpu::Texture, 3> targets;  // the two ping-pong targets and the blur scratch
  std::array<wgpu::TextureView, 3> targetViews;
  uint32_t targetWidth = 0;
  uint32_t targetHeight = 0;
  uint32_t current = 0;    // the target the next hot pass draws
  uint32_t prevWidth = 0;  // the region the other target holds
  uint32_t prevHeight = 0;
  wgpu::Texture frame;
  wgpu::TextureView frameView;
  wgpu::TextureFormat frameFormat = wgpu::TextureFormat::Undefined;
  uint32_t width = 0;
  uint32_t height = 0;
};
State g_state;

std::mutex g_lutMutex;
std::vector<uint8_t> g_lutData;
std::vector<uint8_t> g_noiseData;  // the 64x64 R8 noise
bool g_lutUploaded = false;

std::array<Params, 8> g_recorded;
constexpr uint64_t SlotStride = 256;
static_assert(sizeof(Uniforms) <= SlotStride);
uint32_t g_nextSlot = 0;

void ensure_pipelines(wgpu::TextureFormat format, uint32_t samples) {
  if (g_state.cold && g_state.pipelineFormat == format && g_state.pipelineSamples == samples) {
    return;
  }
  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code = Source;
  const wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &wgsl, .label = "Thermal Post"};
  const auto module = g_device.CreateShaderModule(&moduleDescriptor);
  const auto texture = [](uint32_t binding) {
    return wgpu::BindGroupLayoutEntry{
        .binding = binding,
        .visibility = wgpu::ShaderStage::Fragment,
        .texture = {.sampleType = wgpu::TextureSampleType::Float, .viewDimension = wgpu::TextureViewDimension::e2D},
    };
  };
  const auto sampler = [](uint32_t binding) {
    return wgpu::BindGroupLayoutEntry{
        .binding = binding,
        .visibility = wgpu::ShaderStage::Fragment,
        .sampler = {.type = wgpu::SamplerBindingType::Filtering},
    };
  };
  const std::array entries{
      wgpu::BindGroupLayoutEntry{
          .binding = 0,
          .visibility = wgpu::ShaderStage::Fragment,
          .buffer = {.type = wgpu::BufferBindingType::Uniform, .minBindingSize = sizeof(Uniforms)},
      },
      texture(1), texture(2), texture(3), texture(4), sampler(5), sampler(6),
  };
  const wgpu::BindGroupLayoutDescriptor layoutDescriptor{
      .label = "Thermal Post Layout",
      .entryCount = entries.size(),
      .entries = entries.data(),
  };
  g_state.layout = g_device.CreateBindGroupLayout(&layoutDescriptor);
  const wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor{
      .label = "Thermal Post Pipeline Layout",
      .bindGroupLayoutCount = 1,
      .bindGroupLayouts = &g_state.layout,
  };
  const auto pipelineLayout = g_device.CreatePipelineLayout(&pipelineLayoutDescriptor);
  const wgpu::ColorTargetState frameTarget{.format = format, .writeMask = wgpu::ColorWriteMask::All};
  const wgpu::ColorTargetState levelTarget{.format = TargetFormat, .writeMask = wgpu::ColorWriteMask::All};
  const wgpu::BlendState alphaBlend{
      .color = {.operation = wgpu::BlendOperation::Add,
                .srcFactor = wgpu::BlendFactor::SrcAlpha,
                .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha},
      .alpha = {.operation = wgpu::BlendOperation::Add,
                .srcFactor = wgpu::BlendFactor::Zero,
                .dstFactor = wgpu::BlendFactor::One},
  };
  const wgpu::ColorTargetState ghostTarget{
      .format = TargetFormat, .blend = &alphaBlend, .writeMask = wgpu::ColorWriteMask::All};
  const auto make = [&](const char* label, const char* entry, const wgpu::ColorTargetState* target, uint32_t count) {
    const wgpu::FragmentState fragment{.module = module, .entryPoint = entry, .targetCount = 1, .targets = target};
    const wgpu::RenderPipelineDescriptor descriptor{
        .label = label,
        .layout = pipelineLayout,
        .vertex = {.module = module, .entryPoint = "vs_main"},
        .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
        .multisample = {.count = count, .mask = UINT32_MAX},
        .fragment = &fragment,
    };
    return g_device.CreateRenderPipeline(&descriptor);
  };
  g_state.cold = make("Thermal Cold", "fs_cold", &frameTarget, samples);
  g_state.hot = make("Thermal Hot", "fs_hot", &levelTarget, 1);
  g_state.ghost = make("Thermal Ghost", "fs_ghost", &ghostTarget, 1);
  g_state.blurH = make("Thermal Blur H", "fs_blur_h", &levelTarget, 1);
  g_state.blurV = make("Thermal Blur V", "fs_blur_v", &levelTarget, 1);
  g_state.upscale = make("Thermal Upscale", "fs_upscale", &frameTarget, samples);
  g_state.pipelineFormat = format;
  g_state.pipelineSamples = samples;
  if (!g_state.uniforms) {
    const wgpu::BufferDescriptor bufferDescriptor{
        .label = "Thermal Post Uniforms",
        .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
        .size = SlotStride * g_recorded.size(),
    };
    g_state.uniforms = g_device.CreateBuffer(&bufferDescriptor);
    wgpu::SamplerDescriptor samplerDescriptor{
        .label = "Thermal Post Clamp",
        .addressModeU = wgpu::AddressMode::ClampToEdge,
        .addressModeV = wgpu::AddressMode::ClampToEdge,
        .magFilter = wgpu::FilterMode::Linear,
        .minFilter = wgpu::FilterMode::Linear,
    };
    g_state.clampSampler = g_device.CreateSampler(&samplerDescriptor);
    samplerDescriptor.label = "Thermal Post Repeat";
    samplerDescriptor.addressModeU = wgpu::AddressMode::Repeat;
    samplerDescriptor.addressModeV = wgpu::AddressMode::Repeat;
    // The noise's sampler is Simple(filter 0, wrap 1): NVN nearest, repeat (0x10dc50, table 0x1d0d0dc).
    samplerDescriptor.magFilter = wgpu::FilterMode::Nearest;
    samplerDescriptor.minFilter = wgpu::FilterMode::Nearest;
    g_state.repeatSampler = g_device.CreateSampler(&samplerDescriptor);
  }
}

// The gradient (sRGB tagged, as Remastered's) and the noise texture (GetNoiseTexture(4), a 64x64
// R8 table out of the executable); both arrive together in thermal.lut.
void ensure_textures(const wgpu::Queue& queue) {
  std::lock_guard lock(g_lutMutex);
  if (!g_state.noise) {
    const wgpu::TextureDescriptor descriptor{
        .label = "Thermal Noise",
        .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
        .size = {NoiseSize, NoiseSize, 1},
        .format = wgpu::TextureFormat::R8Unorm,
    };
    g_state.noise = g_device.CreateTexture(&descriptor);
    g_state.noiseView = g_state.noise.CreateView();
    g_lutUploaded = false;
  }
  if (!g_state.lut) {
    const wgpu::TextureDescriptor descriptor{
        .label = "Thermal Gradient",
        .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
        .size = {256, 4, 1},
        .format = wgpu::TextureFormat::RGBA8UnormSrgb,
    };
    g_state.lut = g_device.CreateTexture(&descriptor);
    g_state.lutView = g_state.lut.CreateView();
    g_lutUploaded = false;
  }
  if (!g_lutUploaded && g_lutData.size() == 256 * 4 * 4) {
    const wgpu::TexelCopyTextureInfo dst{.texture = g_state.lut};
    const wgpu::TexelCopyBufferLayout layout{.bytesPerRow = 256 * 4, .rowsPerImage = 4};
    const wgpu::Extent3D size{256, 4, 1};
    queue.WriteTexture(&dst, g_lutData.data(), g_lutData.size(), &layout, &size);
    const wgpu::TexelCopyTextureInfo noiseDst{.texture = g_state.noise};
    const wgpu::TexelCopyBufferLayout noiseLayout{.bytesPerRow = NoiseSize, .rowsPerImage = NoiseSize};
    const wgpu::Extent3D noiseSize{NoiseSize, NoiseSize, 1};
    queue.WriteTexture(&noiseDst, g_noiseData.data(), g_noiseData.size(), &noiseLayout, &noiseSize);
    g_lutUploaded = true;
  }
}

void ensure_targets(uint32_t width, uint32_t height) {
  if (g_state.targets[0] && g_state.targetWidth == width && g_state.targetHeight == height) {
    return;
  }
  g_state.targetWidth = width;
  g_state.targetHeight = height;
  g_state.prevWidth = g_state.prevHeight = 0;
  for (size_t i = 0; i < g_state.targets.size(); ++i) {
    const wgpu::TextureDescriptor descriptor{
        .label = "Thermal Target",
        .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
        .size = {width, height, 1},
        .format = TargetFormat,
    };
    g_state.targets[i] = g_device.CreateTexture(&descriptor);
    g_state.targetViews[i] = g_state.targets[i].CreateView();
  }
}

void ensure_frame(uint32_t width, uint32_t height, wgpu::TextureFormat format) {
  if (g_state.frame && g_state.width == width && g_state.height == height && g_state.frameFormat == format) {
    return;
  }
  g_state.width = width;
  g_state.height = height;
  g_state.frameFormat = format;
  const wgpu::TextureDescriptor descriptor{
      .label = "Thermal Post Frame Copy",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .size = {width, height, 1},
      .format = format,
  };
  g_state.frame = g_device.CreateTexture(&descriptor);
  g_state.frameView = g_state.frame.CreateView();
}

void encode(const EncoderTaskContext& ctx, const wgpu::CommandEncoder& cmd, const void* payload, size_t payloadSize,
            void*) {
  uint32_t slot = 0;
  if (payloadSize != sizeof(slot)) {
    return;
  }
  std::memcpy(&slot, payload, sizeof(slot));
  slot %= g_recorded.size();
  const Params& params = g_recorded[slot];
  const auto& source = webgpu::present_source();
  const auto& target = webgpu::g_frameBuffer;
  const uint32_t samples = webgpu::g_graphicsConfig.msaaSamples > 1 ? webgpu::g_graphicsConfig.msaaSamples : 1;
  const auto format = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  const uint32_t width = source.size.width;
  const uint32_t height = source.size.height;
  if (width == 0 || height == 0 || !source.texture || !target.view) {
    return;
  }
  const bool hot = params.v[0] == 1.f;
  ensure_pipelines(format, samples);
  ensure_frame(width, height, format);
  ensure_textures(ctx.queue);
  const uint32_t fullWidth = std::max(1u, width >> 1);
  const uint32_t fullHeight = std::max(1u, height >> 1);
  ensure_targets(fullWidth, fullHeight);

  // init_frame 0xfb0d30: while the visor ramps up (0.5 s) the target is scaled by
  // ((int)(t / 0.5 6) + 1) / 6.
  float scale = 1.f;
  if (params.v[2] < 0.5f) {
    scale = float(int(std::max(params.v[2], 0.f) / 0.5f * 6.f) + 1) / 6.f;
  }
  const uint32_t regionWidth = std::max(1u, uint32_t(float(fullWidth) * scale));
  const uint32_t regionHeight = std::max(1u, uint32_t(float(fullHeight) * scale));

  Uniforms uniforms{};
  uniforms.params = params;
  uniforms.rt[0] = float(regionWidth);
  uniforms.rt[1] = float(regionHeight);
  uniforms.rt[2] = float(g_state.prevWidth ? g_state.prevWidth : regionWidth);
  uniforms.rt[3] = float(g_state.prevHeight ? g_state.prevHeight : regionHeight);
  uniforms.fb[0] = float(width);
  uniforms.fb[1] = float(height);
  uniforms.fb[2] = float(fullWidth);
  uniforms.fb[3] = float(fullHeight);
  const uint64_t offset = uint64_t(slot) * SlotStride;
  ctx.queue.WriteBuffer(g_state.uniforms, offset, &uniforms, sizeof(uniforms));

  const uint32_t cur = g_state.current;
  const uint32_t prev = cur ^ 1;
  const wgpu::TexelCopyTextureInfo copySource{.texture = source.texture};
  const wgpu::TexelCopyTextureInfo copyTarget{.texture = g_state.frame};
  const wgpu::Extent3D copySize{width, height, 1};
  cmd.CopyTextureToTexture(&copySource, &copyTarget, &copySize);

  const auto makeGroup = [&](const char* label, const wgpu::TextureView& src, const wgpu::TextureView& previous) {
    const std::array groupEntries{
        wgpu::BindGroupEntry{.binding = 0, .buffer = g_state.uniforms, .offset = offset, .size = sizeof(Uniforms)},
        wgpu::BindGroupEntry{.binding = 1, .textureView = src},
        wgpu::BindGroupEntry{.binding = 2, .textureView = g_state.lutView},
        wgpu::BindGroupEntry{.binding = 3, .textureView = g_state.noiseView},
        wgpu::BindGroupEntry{.binding = 4, .textureView = previous},
        wgpu::BindGroupEntry{.binding = 5, .sampler = g_state.clampSampler},
        wgpu::BindGroupEntry{.binding = 6, .sampler = g_state.repeatSampler},
    };
    const wgpu::BindGroupDescriptor groupDescriptor{
        .label = label,
        .layout = g_state.layout,
        .entryCount = groupEntries.size(),
        .entries = groupEntries.data(),
    };
    return g_device.CreateBindGroup(&groupDescriptor);
  };
  // A pass into the frame (every pixel written without blending, so it need not be loaded).
  const auto drawFrame = [&](const char* label, const wgpu::RenderPipeline& pipeline, const wgpu::BindGroup& group) {
    const wgpu::RenderPassColorAttachment attachment{
        .view = target.view,
        .resolveTarget = samples > 1 ? webgpu::g_frameBufferResolved.view : wgpu::TextureView{},
        .loadOp = wgpu::LoadOp::Clear,
        .storeOp = wgpu::StoreOp::Store,
        .clearValue = {0.0, 0.0, 0.0, 0.0},
    };
    const wgpu::RenderPassDescriptor passDescriptor{
        .label = label,
        .colorAttachmentCount = 1,
        .colorAttachments = &attachment,
        .timestampWrites = webgpu::gpu_prof::pass_writes(label),
    };
    const auto pass = cmd.BeginRenderPass(&passDescriptor);
    pass.SetPipeline(pipeline);
    pass.SetBindGroup(0, group);
    pass.Draw(3);
    pass.End();
  };

  if (!hot) {
    drawFrame("Thermal cold", g_state.cold, makeGroup("Thermal Cold", g_state.frameView, g_state.frameView));
    return;
  }
  // The new target, then (past 0.1 s) the previous one over it.
  const auto drawTarget = [&](const char* label, const wgpu::RenderPipeline& pipeline, const wgpu::BindGroup& group,
                              bool clear, uint32_t into) {
    const wgpu::RenderPassColorAttachment attachment{
        .view = g_state.targetViews[into],
        .loadOp = clear ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load,
        .storeOp = wgpu::StoreOp::Store,
        .clearValue = {0.0, 0.0, 0.0, 0.0},
    };
    const wgpu::RenderPassDescriptor passDescriptor{
        .label = label,
        .colorAttachmentCount = 1,
        .colorAttachments = &attachment,
        .timestampWrites = webgpu::gpu_prof::pass_writes(label),
    };
    const auto pass = cmd.BeginRenderPass(&passDescriptor);
    pass.SetViewport(0.f, 0.f, float(regionWidth), float(regionHeight), 0.f, 1.f);
    pass.SetScissorRect(0, 0, regionWidth, regionHeight);
    pass.SetPipeline(pipeline);
    pass.SetBindGroup(0, group);
    pass.Draw(3);
    pass.End();
  };
  drawTarget("Thermal hot", g_state.hot, makeGroup("Thermal Hot", g_state.frameView, g_state.targetViews[prev]), true, cur);
  // Blur mode 2 (LDTA +0x48 -> size index 1 -> blur mode 1, 0xfb1cf0): H into the scratch, V back.
  drawTarget("Thermal blur H", g_state.blurH, makeGroup("Thermal Blur H", g_state.targetViews[cur], g_state.targetViews[prev]), true, 2);
  drawTarget("Thermal blur V", g_state.blurV, makeGroup("Thermal Blur V", g_state.targetViews[2], g_state.targetViews[prev]), true, cur);
  if (params.v[3] > 0.f && g_state.prevWidth != 0) {
    drawTarget("Thermal ghost", g_state.ghost,
               makeGroup("Thermal Ghost", g_state.frameView, g_state.targetViews[prev]), false, cur);
  }
  drawFrame("Thermal upscale", g_state.upscale, makeGroup("Thermal Upscale", g_state.targetViews[cur], g_state.targetViews[prev]));
  g_state.prevWidth = regionWidth;
  g_state.prevHeight = regionHeight;
  g_state.current = prev;
}
} // namespace

bool set_lut(const uint8_t* rgba, uint32_t size) {
  if (rgba == nullptr || size != LutBytes + NoiseSize * NoiseSize) {
    return false;
  }
  std::lock_guard lock(g_lutMutex);
  g_lutData.assign(rgba, rgba + LutBytes);
  g_noiseData.assign(rgba + LutBytes, rgba + size);
  g_lutUploaded = false;
  return true;
}

bool has_lut() {
  std::lock_guard lock(g_lutMutex);
  return !g_lutData.empty();
}

bool ensure_task() {
  if (!has_lut()) {
    return false;
  }
  if (g_state.task == InvalidEncoderTask) {
    g_state.task = register_encoder_task_type(EncoderTaskDescriptor{.label = "Thermal Post", .callback = encode});
    if (g_state.task == InvalidEncoderTask) {
      Log.warn("could not register the thermal post task");
      return false;
    }
  }
  return true;
}

bool record(const Params& params) {
  if (g_state.task == InvalidEncoderTask) {
    return false;
  }
  const auto& size = webgpu::present_source().size;
  if (size.width == 0 || size.height == 0) {
    return false;
  }
  const uint32_t slot = g_nextSlot++ % g_recorded.size();
  g_recorded[slot] = params;
  record_encoder_task(g_state.task, &slot, sizeof(slot));
  return true;
}

void shutdown() {
  const auto task = g_state.task;
  g_state = {};
  g_lutUploaded = false;
  if (task != InvalidEncoderTask) {
    unregister_encoder_task_type(task);
  }
}
} // namespace aurora::gfx::thermal
