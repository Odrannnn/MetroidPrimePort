#include "volfog.hpp"
#include "tex_copy_conv.hpp"

#include "../logging.hpp"
#include "../webgpu/gpu.hpp"
#include "../webgpu/gpu_prof.hpp"
#include "frame.hpp"
#include "probe.hpp"
#include "recording.hpp"

#include <aurora/gfx.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include "texture_sites.hpp"

// Remastered's volumetric fog, as its CRenderPass_VolumetricFog and shaders do it:
//  - a froxel grid of 16x16 pixels by 64 slices, slice k starting at a view depth of
//    near + (k / 63)^2 (range - near);
//  - per froxel (shader 006012c) the density: a height term clamp(z slope + bias) times the
//    density, a 3D Perlin noise 1 - strength + strength n, and a profile over distance; the
//    light: the baked ambient volume's mean plus colour A, its largest channel capped, times
//    colour B; in-scatter thickness density scatter light, extinction thickness density
//    (scatter + absorb);
//  - front to back along each column (006072b) the in-scatter so far and the transmittance;
//  - the frame (0029257) as scene T + in-scatter, at w = sqrt((z - near) / (range - near)).
// Remastered applies it to its HDR frame before the tone curve, and so does this: the apply pass
// fogs the HDR scene's exposed linear light (the in-scatter times the exposure).
// The froxels are filled by one compute pass, a thread per column.
namespace aurora::gfx::volfog {
namespace {
Module Log("aurora::gfx::volfog");
using webgpu::g_device;

constexpr uint32_t Slices = 64;
constexpr uint32_t NoiseSize = 32;
constexpr auto FroxelFormat = wgpu::TextureFormat::RGBA16Float;

constexpr const char* CommonSource = R"(
struct Params {
  viewToWorld: array<vec4f, 3>,
  worldToVolume: array<vec4f, 3>,
  frustum: vec4f,
  depth: vec4f,
  fog: vec4f,
  shape: vec4f,
  noise: vec4f,
  colorB: vec4f,
  colorA: vec4f,
  tone: array<vec4f, 3>,
  lut: array<vec4f, 16>,
  regions: array<vec4f, 56>,
  regionInfo: vec4u,
  misc: vec4u,
};
)";

constexpr const char* ComputeSource = R"(
@group(0) @binding(0) var<uniform> p: Params;
@group(0) @binding(1) var repeatSamp: sampler;
@group(0) @binding(2) var noiseTex: texture_3d<f32>;
@group(0) @binding(3) var clampSamp: sampler;
@group(0) @binding(4) var volMean: texture_3d<f32>;
@group(0) @binding(5) var froxels: texture_storage_3d<rgba16float, write>;

// The profile as Remastered samples its 64-texel LUT: linearly, at t (texel centres at (i + 0.5) / 64).
fn lut_at(t: f32) -> f32 {
  let x = clamp(t * 64.0 - 0.5, 0.0, 63.0);
  let i = u32(floor(x));
  let j = min(i + 1u, 63u);
  return mix(p.lut[i / 4u][i % 4u], p.lut[j / 4u][j % 4u], x - f32(i));
}

@compute @workgroup_size(8, 8)
fn cs_main(@builtin(global_invocation_id) id: vec3u) {
  let w = p.misc.z;
  let h = p.misc.w;
  if (id.x >= w || id.y >= h) {
    return;
  }
  // Row 0 is the top. Remastered's grid spans the frame edge to edge (ndc = 2 x / (W - 1) - 1).
  let s = vec2f(f32(id.x) / f32(w - 1u), 1.0 - f32(id.y) / f32(h - 1u));
  let ray = vec2f(mix(p.frustum.x, p.frustum.y, s.x), mix(p.frustum.z, p.frustum.w, s.y));
  let near = p.depth.x;
  let range = p.fog.x;
  var inscatter = vec3f(0.0);
  var optical = 0.0;
  for (var k = 0u; k < 64u; k++) {
    let t0 = f32(k) / 63.0;
    let t1 = f32(k + 1u) / 63.0;
    let thickness = (t1 * t1 - t0 * t0) * range;
    let z = near + t0 * t0 * (range - near);
    let v = vec4f(ray * z, -z, 1.0);
    let world = vec3f(dot(p.viewToWorld[0], v), dot(p.viewToWorld[1], v), dot(p.viewToWorld[2], v));
    var height = clamp(world.z * p.shape.x + p.shape.y, 0.0, 1.0) * p.fog.w;
    // The regions, chained in order (their density map is the default one-texel white volume).
    var added = p.colorA.rgb;
    var cap = p.noise.w;
    let wv = vec4f(world, 1.0);
    for (var r = 0u; r < min(p.regionInfo.x, 8u); r++) {
      let b = r * 7u;
      let uvw = vec3f(dot(p.regions[b], wv), dot(p.regions[b + 1u], wv), dot(p.regions[b + 2u], wv));
      let e = abs(uvw * 2.0 - 1.0) * p.regions[b + 3u].xyz + p.regions[b + 4u].xyz;
      let mask = clamp(min(min(e.x, e.y), e.z), 0.0, 1.0);
      let k = 1.0 + mask * (p.regions[b + 3u].w - 1.0);
      added = max(vec3f(0.0), mask * p.regions[b + 5u].rgb + added * k);
      height = max(0.0, height * k + mask * p.regions[b + 6u].x);
      cap = max(0.0, cap * k + mask * p.regions[b + 4u].w);
    }
    // The noise in Remastered's world: retail's turned to (-x, z, y); noise.xyz holds its origin.
    let n = textureSampleLevel(noiseTex, repeatSamp, (vec3f(-world.x, world.z, world.y) + p.noise.xyz) * p.shape.z, 0.0).r;
    let density = (1.0 - p.shape.w + p.shape.w * n) * height * lut_at(min(t0, 1.0));
    var light = added;
    if (p.misc.x != 0u) {
      let wv = vec4f(world, 1.0);
      let uvw = vec3f(dot(p.worldToVolume[0], wv), dot(p.worldToVolume[1], wv), dot(p.worldToVolume[2], wv));
      // Remastered's sampler has a black border (CLAMP_TO_BORDER) and unmapped tiles read 0,
      // so froxels outside the baked grid get no light from it; the weight turns the
      // clamp-to-edge sample into that.
      let vsize = vec3f(textureDimensions(volMean));
      let vt = uvw * vsize;
      let vw = clamp(min(vt + 0.5, vsize + 0.5 - vt), vec3f(0.0), vec3f(1.0));
      light += textureSampleLevel(volMean, clampSamp, uvw, 0.0).rgb * (vw.x * vw.y * vw.z) * p.colorB.w;
    } else {
      light += vec3f(p.colorB.w); // Remastered's default volume: one white texel
    }
    let top = max(max(light.r, light.g), light.b);
    if (top > cap) {
      light *= cap / top;
    }
    light *= p.colorB.rgb;
    inscatter += exp(-optical) * thickness * density * p.fog.y * light;
    optical += thickness * density * (p.fog.y + p.fog.z);
    textureStore(froxels, vec3u(id.x, id.y, k), vec4f(inscatter, min(exp(-optical), 1.0)));
  }
}
)";

constexpr const char* ApplySource = R"(
@group(0) @binding(0) var<uniform> p: Params;
@group(0) @binding(1) var src: texture_2d<f32>;
@group(0) @binding(2) var depthTex: DEPTH_TYPE;
@group(0) @binding(3) var froxels: texture_3d<f32>;
@group(0) @binding(4) var samp: sampler;

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

@fragment
fn fs_apply(in: VertexOutput) -> @location(0) vec4f {
  let size = vec2i(textureDimensions(src));
  let f = textureLoad(src, min(vec2i(floor(in.pos.xy)), size - vec2i(1)), 0);
  let depthSize = vec2i(textureDimensions(depthTex));
  let at = min(vec2i(in.uv * vec2f(depthSize)), depthSize - vec2i(1));
  // Reversed Z. Nearer than the world's depth range is the viewmodel: Remastered draws it in
  // 0.0097656..0.0386719 of the depth buffer and this pass reads that as a full-range depth.
  let z = 1.0 - textureLoad(depthTex, at, 0);
  let gun = z < p.depth.z;
  let d = select(clamp((z - p.depth.z) / max(p.depth.w - p.depth.z, 1e-6), 0.0, 1.0),
                 mix(0.0097656, 0.0386719, z / p.depth.z), gun);
  let near = p.depth.x;
  let far = p.depth.y;
  let zlin = near * far / (far - d * (far - near));
  // As 0029257: a NaN-safe clamp of (z - near) / (range - near), so a range at or short of the
  // near plane reads the first slice, or the last past the near plane when they're equal.
  let span = p.fog.x - near;
  let w = sqrt(clamp(select((zlin - near) / span, select(0.0, 1.0, zlin > near), span == 0.0), 0.0, 1.0));
  let fog = textureSampleLevel(froxels, samp, vec3f(in.uv, w), 0.0);
  let light = max(fog.rgb, vec3f(0.0)) * p.colorA.w;
  if (fog.a > 0.9995 && max(max(light.r, light.g), light.b) < 1e-4) {
    return f;
  }
  // The HDR scene is exposed linear light (RGBA16F): fogged raw and written back as light; the
  // post-tone after the scene curves and encodes it. The in-scatter's exposure is colorA.w.
  let x = f.rgb * fog.a + light;
  return vec4f(x, f.a);
}
)";

// What a recorded fog fogs: the HDR scene's targets, captured when the fog is recorded
// (scene_hdr_pass_active at the FIFO), so a later frame reset or resize can't change them.
// Recorded fog slots. The most fogs one frame records is 2 (the scan visor zoom draws the world twice),
// and a slot is live from record to the encode of its frame, at most FrameSlotCount frames.
constexpr size_t RecordedSlots = 8;
constexpr size_t MaxFogsPerFrame = 2;
static_assert(MaxFogsPerFrame * detail::FrameSlotCount <= RecordedSlots, "volfog slots would be overwritten while pending");

struct Capture {
  webgpu::TextureWithSampler color;    // the pass's scene attachment (MSAA when samples > 1)
  webgpu::TextureWithSampler resolved; // its resolve target
  webgpu::TextureWithSampler depth;
};

struct State {
  EncoderTaskId task = InvalidEncoderTask;
  wgpu::ComputePipeline compute;
  wgpu::BindGroupLayout computeLayout;
  // One uniform buffer per recorded slot: every fog of a frame shares one encoder and one submit, and a
  // queue write lands before the submit, so a shared buffer would give each fog the last one's params.
  std::array<wgpu::Buffer, RecordedSlots> uniforms;
  wgpu::Sampler repeatSampler;
  wgpu::Sampler clampSampler;
  wgpu::Sampler nearestSampler; // Compatibility: depth is read through a comparison sampler
  wgpu::TextureView noise;
  // The apply pass for the scene's sample count.
  struct Apply {
    wgpu::RenderPipeline pipeline;
    wgpu::BindGroupLayout layout;
    wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
    uint32_t samples = 0;
  };
  Apply apply;
  // The froxels, made when the fog is recorded so that the draws after it can bind them (see
  // froxel_view); remade when the frame's size changes. The placeholder is bound until then.
  wgpu::Texture froxels;
  wgpu::TextureView froxelView;
  wgpu::TextureView placeholder;
  uint32_t gridWidth = 0;
  uint32_t gridHeight = 0;
  // The scene as it was (RGBA16F, never narrowed to 8 bits); remade when its size or format changes.
  struct Frame {
    wgpu::Texture texture;
    wgpu::TextureView view;
    wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
    uint32_t width = 0;
    uint32_t height = 0;
  };
  Frame frame;
};
State g_state;

// Params are larger than an encoder task's inline payload (InlineDrawPayloadSize), so the task
// carries a slot in this ring; frames are encoded well within its length of being recorded.
std::array<Params, RecordedSlots> g_recorded;
// The froxels each slot fills, as they were when it was recorded.
std::array<wgpu::TextureView, RecordedSlots> g_recordedFroxels;
// The targets each slot fogs, as they were when it was recorded.
std::array<Capture, RecordedSlots> g_recordedCapture;
uint32_t g_nextSlot = 0;

// Perlin's gradient noise, repeating every `period` units (CMath::Noise3d(x, y, z, 4)).
constexpr std::array<uint8_t, 256> Permutation{
    151, 160, 137, 91,  90,  15,  131, 13,  201, 95,  96,  53,  194, 233, 7,   225, 140, 36,  103, 30,  69,  142,
    8,   99,  37,  240, 21,  10,  23,  190, 6,   148, 247, 120, 234, 75,  0,   26,  197, 62,  94,  252, 219, 203,
    117, 35,  11,  32,  57,  177, 33,  88,  237, 149, 56,  87,  174, 20,  125, 136, 171, 168, 68,  175, 74,  165,
    71,  134, 139, 48,  27,  166, 77,  146, 158, 231, 83,  111, 229, 122, 60,  211, 133, 230, 220, 105, 92,  41,
    55,  46,  245, 40,  244, 102, 143, 54,  65,  25,  63,  161, 1,   216, 80,  73,  209, 76,  132, 187, 208, 89,
    18,  169, 200, 196, 135, 130, 116, 188, 159, 86,  164, 100, 109, 198, 173, 186, 3,   64,  52,  217, 226, 250,
    124, 123, 5,   202, 38,  147, 118, 126, 255, 82,  85,  212, 207, 206, 59,  227, 47,  16,  58,  17,  182, 189,
    28,  42,  223, 183, 170, 213, 119, 248, 152, 2,   44,  154, 163, 70,  221, 153, 101, 155, 167, 43,  172, 9,
    129, 22,  39,  253, 19,  98,  108, 110, 79,  113, 224, 232, 178, 185, 112, 104, 218, 246, 97,  228, 251, 34,
    242, 193, 238, 210, 144, 12,  191, 179, 162, 241, 81,  51,  145, 235, 249, 14,  239, 107, 49,  192, 214, 31,
    181, 199, 106, 157, 184, 84,  204, 176, 115, 121, 50,  45,  127, 4,   150, 254, 138, 236, 205, 93,  222, 114,
    67,  29,  24,  72,  243, 141, 128, 195, 78,  66,  215, 61,  156, 180};

float fade(float t) { return t * t * t * (t * (t * 6.f - 15.f) + 10.f); }

float grad(int hash, float x, float y, float z) {
  const int h = hash & 15;
  const float u = h < 8 ? x : y;
  const float v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
  return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
}

float periodic_noise(float x, float y, float z, int period) {
  const int xi = int(std::floor(x));
  const int yi = int(std::floor(y));
  const int zi = int(std::floor(z));
  x -= float(xi);
  y -= float(yi);
  z -= float(zi);
  const auto hash = [period](int a, int b, int c) {
    const auto wrap = [period](int v) { return ((v % period) + period) % period; };
    return int(Permutation[(Permutation[(Permutation[wrap(a)] + wrap(b)) & 255] + wrap(c)) & 255]);
  };
  const float u = fade(x);
  const float v = fade(y);
  const float w = fade(z);
  const auto lerp = [](float t, float a, float b) { return a + t * (b - a); };
  const auto corner = [&](int dx, int dy, int dz) {
    return grad(hash(xi + dx, yi + dy, zi + dz), x - float(dx), y - float(dy), z - float(dz));
  };
  return lerp(w,
              lerp(v, lerp(u, corner(0, 0, 0), corner(1, 0, 0)), lerp(u, corner(0, 1, 0), corner(1, 1, 0))),
              lerp(v, lerp(u, corner(0, 0, 1), corner(1, 0, 1)), lerp(u, corner(0, 1, 1), corner(1, 1, 1))));
}

// Remastered's noise volume (build_perlin_3D_texture(32, 32, 32)): four periods across it.
wgpu::TextureView make_noise(const wgpu::Queue& queue) {
  std::vector<uint8_t> texels(size_t(NoiseSize) * NoiseSize * NoiseSize);
  size_t i = 0;
  for (uint32_t z = 0; z < NoiseSize; ++z) {
    for (uint32_t y = 0; y < NoiseSize; ++y) {
      for (uint32_t x = 0; x < NoiseSize; ++x) {
        const float scale = 4.f / float(NoiseSize);
        const float n = periodic_noise(float(x) * scale, float(y) * scale, float(z) * scale, 4);
        texels[i++] = uint8_t(std::clamp(n * 0.5f + 0.5f, 0.f, 1.f) * 255.f + 0.5f);
      }
    }
  }
  const wgpu::TextureDescriptor descriptor{
      .label = "Volumetric Fog Noise",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e3D,
      .size = {NoiseSize, NoiseSize, NoiseSize},
      .format = wgpu::TextureFormat::R8Unorm,
  };
  AURORA_NOTE_TEXTURE(&descriptor);
  const auto texture = g_device.CreateTexture(&descriptor);
  const wgpu::TexelCopyTextureInfo dst{.texture = texture};
  const wgpu::TexelCopyBufferLayout layout{.bytesPerRow = 256, .rowsPerImage = NoiseSize};
  // Rows padded to 256 bytes, as copies need.
  std::vector<uint8_t> padded(size_t(256) * NoiseSize * NoiseSize);
  for (size_t row = 0; row < size_t(NoiseSize) * NoiseSize; ++row) {
    std::memcpy(padded.data() + row * 256, texels.data() + row * NoiseSize, NoiseSize);
  }
  const wgpu::Extent3D extent{NoiseSize, NoiseSize, NoiseSize};
  queue.WriteTexture(&dst, padded.data(), padded.size(), &layout, &extent);
  return texture.CreateView();
}

wgpu::ShaderModule make_module(const char* label, const std::string& code) {
  wgpu::ShaderSourceWGSL source{};
  source.code = code.c_str();
  const wgpu::ShaderModuleDescriptor descriptor{.nextInChain = &source, .label = label};
  return g_device.CreateShaderModule(&descriptor);
}

void ensure_clamp_sampler() {
  if (g_state.clampSampler) {
    return;
  }
  const wgpu::SamplerDescriptor clampDescriptor{
      .label = "Volumetric Fog Sampler",
      .addressModeU = wgpu::AddressMode::ClampToEdge,
      .addressModeV = wgpu::AddressMode::ClampToEdge,
      .addressModeW = wgpu::AddressMode::ClampToEdge,
      .magFilter = wgpu::FilterMode::Linear,
      .minFilter = wgpu::FilterMode::Linear,
  };
  g_state.clampSampler = g_device.CreateSampler(&clampDescriptor);
  const wgpu::SamplerDescriptor nearestDescriptor{
      .label = "Volumetric Fog Depth Sampler",
      .addressModeU = wgpu::AddressMode::ClampToEdge,
      .addressModeV = wgpu::AddressMode::ClampToEdge,
      .addressModeW = wgpu::AddressMode::ClampToEdge,
      .magFilter = wgpu::FilterMode::Nearest,
      .minFilter = wgpu::FilterMode::Nearest,
      .compare = wgpu::CompareFunction::LessEqual,
  };
  g_state.nearestSampler = g_device.CreateSampler(&nearestDescriptor);
}

void ensure_compute(const wgpu::Queue& queue) {
  if (g_state.compute) {
    return;
  }
  const auto module = make_module("Volumetric Fog Froxels", std::string(CommonSource) + ComputeSource);
  const std::array entries{
      wgpu::BindGroupLayoutEntry{
          .binding = 0,
          .visibility = wgpu::ShaderStage::Compute,
          .buffer = {.type = wgpu::BufferBindingType::Uniform, .minBindingSize = sizeof(Params)},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 1,
          .visibility = wgpu::ShaderStage::Compute,
          .sampler = {.type = wgpu::SamplerBindingType::Filtering},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 2,
          .visibility = wgpu::ShaderStage::Compute,
          .texture = {.sampleType = wgpu::TextureSampleType::Float, .viewDimension = wgpu::TextureViewDimension::e3D},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 3,
          .visibility = wgpu::ShaderStage::Compute,
          .sampler = {.type = wgpu::SamplerBindingType::Filtering},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 4,
          .visibility = wgpu::ShaderStage::Compute,
          .texture = {.sampleType = wgpu::TextureSampleType::Float, .viewDimension = wgpu::TextureViewDimension::e3D},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 5,
          .visibility = wgpu::ShaderStage::Compute,
          .storageTexture = {.access = wgpu::StorageTextureAccess::WriteOnly,
                             .format = FroxelFormat,
                             .viewDimension = wgpu::TextureViewDimension::e3D},
      },
  };
  const wgpu::BindGroupLayoutDescriptor layoutDescriptor{
      .label = "Volumetric Fog Froxels Layout",
      .entryCount = entries.size(),
      .entries = entries.data(),
  };
  g_state.computeLayout = g_device.CreateBindGroupLayout(&layoutDescriptor);
  const wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor{
      .label = "Volumetric Fog Froxels Pipeline Layout",
      .bindGroupLayoutCount = 1,
      .bindGroupLayouts = &g_state.computeLayout,
  };
  const auto pipelineLayout = g_device.CreatePipelineLayout(&pipelineLayoutDescriptor);
  const wgpu::ComputePipelineDescriptor descriptor{
      .label = "Volumetric Fog Froxels",
      .layout = pipelineLayout,
      .compute = {.module = module, .entryPoint = "cs_main"},
  };
  g_state.compute = g_device.CreateComputePipeline(&descriptor);
  const wgpu::BufferDescriptor bufferDescriptor{
      .label = "Volumetric Fog Uniforms",
      .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
      .size = sizeof(Params),
  };
  for (auto& uniforms : g_state.uniforms) {
    uniforms = g_device.CreateBuffer(&bufferDescriptor);
  }
  ensure_clamp_sampler();
  const wgpu::SamplerDescriptor repeatDescriptor{
      .label = "Volumetric Fog Noise Sampler",
      .addressModeU = wgpu::AddressMode::Repeat,
      .addressModeV = wgpu::AddressMode::Repeat,
      .addressModeW = wgpu::AddressMode::Repeat,
      .magFilter = wgpu::FilterMode::Linear,
      .minFilter = wgpu::FilterMode::Linear,
  };
  g_state.repeatSampler = g_device.CreateSampler(&repeatDescriptor);
  g_state.noise = make_noise(queue);
}

void ensure_apply(wgpu::TextureFormat format, uint32_t samples) {
  auto& apply = g_state.apply;
  if (apply.pipeline && apply.format == format && apply.samples == samples) {
    return;
  }
  const std::string code = apply_source(samples);
  const auto module = make_module("Volumetric Fog Apply", code);
  const std::array entries{
      wgpu::BindGroupLayoutEntry{
          .binding = 0,
          .visibility = wgpu::ShaderStage::Fragment,
          .buffer = {.type = wgpu::BufferBindingType::Uniform, .minBindingSize = sizeof(Params)},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 1,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture = {.sampleType = wgpu::TextureSampleType::Float, .viewDimension = wgpu::TextureViewDimension::e2D},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 2,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture = {.sampleType = wgpu::TextureSampleType::Depth,
                      .viewDimension = wgpu::TextureViewDimension::e2D,
                      .multisampled = samples > 1},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 3,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture = {.sampleType = wgpu::TextureSampleType::Float, .viewDimension = wgpu::TextureViewDimension::e3D},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 4,
          .visibility = wgpu::ShaderStage::Fragment,
          .sampler = {.type = wgpu::SamplerBindingType::Filtering},
      },
      // Compatibility only: the depth is sampled through a Nearest sampler (see apply_source)
      wgpu::BindGroupLayoutEntry{
          .binding = 5,
          .visibility = wgpu::ShaderStage::Fragment,
          .sampler = {.type = wgpu::SamplerBindingType::Comparison},
      },
  };
  const wgpu::BindGroupLayoutDescriptor layoutDescriptor{
      .label = "Volumetric Fog Apply Layout",
      .entryCount = webgpu::g_hasCoreFeatures ? 5u : 6u,
      .entries = entries.data(),
  };
  apply.layout = g_device.CreateBindGroupLayout(&layoutDescriptor);
  const wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor{
      .label = "Volumetric Fog Apply Pipeline Layout",
      .bindGroupLayoutCount = 1,
      .bindGroupLayouts = &apply.layout,
  };
  const auto pipelineLayout = g_device.CreatePipelineLayout(&pipelineLayoutDescriptor);
  const wgpu::ColorTargetState target{.format = format, .writeMask = wgpu::ColorWriteMask::All};
  const wgpu::FragmentState fragment{
      .module = module,
      .entryPoint = "fs_apply",
      .targetCount = 1,
      .targets = &target,
  };
  const wgpu::RenderPipelineDescriptor descriptor{
      .label = "Volumetric Fog Apply",
      .layout = pipelineLayout,
      .vertex = {.module = module, .entryPoint = "vs_main"},
      .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
      .multisample = {.count = samples, .mask = UINT32_MAX},
      .fragment = &fragment,
  };
  apply.pipeline = g_device.CreateRenderPipeline(&descriptor);
  apply.format = format;
  apply.samples = samples;
}

void ensure_targets(uint32_t width, uint32_t height, wgpu::TextureFormat format) {
  auto& frame = g_state.frame;
  if (frame.texture && frame.width == width && frame.height == height && frame.format == format) {
    return;
  }
  frame.width = width;
  frame.height = height;
  frame.format = format;
  const wgpu::TextureDescriptor frameDescriptor{
      .label = "Volumetric Fog Frame Copy",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .size = {width, height, 1},
      .format = format,
  };
  AURORA_NOTE_TEXTURE(&frameDescriptor);
  frame.texture = g_device.CreateTexture(&frameDescriptor);
  frame.view = frame.texture.CreateView();
}

// The HDR targets a slot fogs, checked as one whole: the scene format throughout, the depth buffer's
// size and sample count those of the colour, a resolve target when multisampled. Anything else
// fails closed (no fog) instead of fogging 16F texels as 8-bit ones or the reverse.
bool hdr_capture_valid(const Capture& c, uint32_t& samples) {
  if (!c.color.texture || !c.color.view || !c.depth.texture || !c.depth.view ||
      c.color.format != webgpu::SceneHdrFormat || c.color.texture.GetFormat() != webgpu::SceneHdrFormat) {
    return false;
  }
  samples = c.color.texture.GetSampleCount();
  if (c.depth.texture.GetSampleCount() != samples || c.depth.size.width != c.color.size.width ||
      c.depth.size.height != c.color.size.height) {
    return false;
  }
  if (samples > 1) {
    return c.resolved.texture && c.resolved.view && c.resolved.format == webgpu::SceneHdrFormat &&
           c.resolved.texture.GetFormat() == webgpu::SceneHdrFormat && c.resolved.texture.GetSampleCount() == 1 &&
           c.resolved.size.width == c.color.size.width && c.resolved.size.height == c.color.size.height;
  }
  return true;
}

void ensure_froxels(uint32_t width, uint32_t height) {
  // CRenderPass_VolumetricFog::Initialize: 16 pixels to a froxel, the width a multiple of 4.
  const uint32_t gridWidth = std::max(((width + 15) >> 4) & ~3u, 4u);
  const uint32_t gridHeight = std::max((height + 15) >> 4, 2u);
  if (g_state.froxels && g_state.gridWidth == gridWidth && g_state.gridHeight == gridHeight) {
    return;
  }
  g_state.gridWidth = gridWidth;
  g_state.gridHeight = gridHeight;
  const wgpu::TextureDescriptor froxelDescriptor{
      .label = "Volumetric Fog Froxels",
      .usage = wgpu::TextureUsage::StorageBinding | wgpu::TextureUsage::TextureBinding,
      .dimension = wgpu::TextureDimension::e3D,
      .size = {gridWidth, gridHeight, Slices},
      .format = FroxelFormat,
  };
  AURORA_NOTE_TEXTURE(&froxelDescriptor);
  g_state.froxels = g_device.CreateTexture(&froxelDescriptor);
  g_state.froxelView = g_state.froxels.CreateView();
}

void encode(const EncoderTaskContext& ctx, const wgpu::CommandEncoder& cmd, const void* payload, size_t payloadSize,
            void*) {
  uint32_t slot = 0;
  if (payloadSize != sizeof(slot)) {
    return;
  }
  std::memcpy(&slot, payload, sizeof(slot));
  Params params = g_recorded[slot % g_recorded.size()];
  const wgpu::TextureView froxelView = g_recordedFroxels[slot % g_recordedFroxels.size()];
  const Capture& capture = g_recordedCapture[slot % g_recordedCapture.size()];
  uint32_t samples = 1;
  if (!hdr_capture_valid(capture, samples)) {
    return;
  }
  const auto& source = samples > 1 ? capture.resolved : capture.color;
  const auto& target = capture.color;
  const auto& resolveTarget = capture.resolved;
  const auto& depth = capture.depth;
  const auto format = webgpu::SceneHdrFormat;
  const uint32_t width = source.size.width;
  const uint32_t height = source.size.height;
  if (width == 0 || height == 0 || !source.texture || !target.view || !depth.view || !froxelView) {
    return;
  }
  ensure_compute(ctx.queue);
  const wgpu::Buffer uniforms = g_state.uniforms[slot % g_state.uniforms.size()];
  ensure_apply(format, samples);
  ensure_targets(width, height, format);
  const auto& apply = g_state.apply;
  const auto& frame = g_state.frame;
  const uint32_t volumeId = params.volume;
  const bool volume = volumeId != 0 && probe::has_volume(volumeId);
  // misc.x: the volume's light is read, else a white texel's; the bindings always hold a texture.
  params.volume = volume ? 1 : 0;
  ctx.queue.WriteBuffer(uniforms, 0, &params, sizeof(params));

  const std::array computeEntries{
      wgpu::BindGroupEntry{.binding = 0, .buffer = uniforms, .size = sizeof(Params)},
      wgpu::BindGroupEntry{.binding = 1, .sampler = g_state.repeatSampler},
      wgpu::BindGroupEntry{.binding = 2, .textureView = g_state.noise},
      wgpu::BindGroupEntry{.binding = 3, .sampler = g_state.clampSampler},
      wgpu::BindGroupEntry{.binding = 4, .textureView = probe::volume_view(volume ? volumeId : 0, 0)},
      wgpu::BindGroupEntry{.binding = 5, .textureView = froxelView},
  };
  const wgpu::BindGroupDescriptor computeGroupDescriptor{
      .label = "Volumetric Fog Froxels",
      .layout = g_state.computeLayout,
      .entryCount = computeEntries.size(),
      .entries = computeEntries.data(),
  };
  const auto computeGroup = g_device.CreateBindGroup(&computeGroupDescriptor);
  {
    const wgpu::ComputePassDescriptor passDescriptor{.label = "Volumetric Fog Froxels"};
    const auto pass = cmd.BeginComputePass(&passDescriptor);
    pass.SetPipeline(g_state.compute);
    pass.SetBindGroup(0, computeGroup);
    pass.DispatchWorkgroups((params.grid[0] + 7) / 8, (params.grid[1] + 7) / 8, 1);
    pass.End();
  }

  const wgpu::TexelCopyTextureInfo copySource{.texture = source.texture};
  const wgpu::TexelCopyTextureInfo copyTarget{.texture = frame.texture};
  const wgpu::Extent3D copySize{width, height, 1};
  cmd.CopyTextureToTexture(&copySource, &copyTarget, &copySize);

  const std::array applyEntries{
      wgpu::BindGroupEntry{.binding = 0, .buffer = uniforms, .size = sizeof(Params)},
      wgpu::BindGroupEntry{.binding = 1, .textureView = frame.view},
      wgpu::BindGroupEntry{.binding = 2, .textureView = depth.view},
      wgpu::BindGroupEntry{.binding = 3, .textureView = froxelView},
      wgpu::BindGroupEntry{.binding = 4, .sampler = g_state.clampSampler},
      wgpu::BindGroupEntry{.binding = 5, .sampler = g_state.nearestSampler},
  };
  const wgpu::BindGroupDescriptor applyGroupDescriptor{
      .label = "Volumetric Fog Apply",
      .layout = apply.layout,
      .entryCount = webgpu::g_hasCoreFeatures ? 5u : 6u,
      .entries = applyEntries.data(),
  };
  const auto applyGroup = g_device.CreateBindGroup(&applyGroupDescriptor);
  // Every pixel is written without blending, so the frame need not be loaded first.
  const wgpu::RenderPassColorAttachment attachment{
      .view = target.view,
      .resolveTarget = samples > 1 ? resolveTarget.view : wgpu::TextureView{},
      .loadOp = wgpu::LoadOp::Clear,
      .storeOp = wgpu::StoreOp::Store,
      .clearValue = {0.0, 0.0, 0.0, 0.0},
  };
  const wgpu::RenderPassDescriptor passDescriptor{
      .label = "Volumetric Fog Apply",
      .colorAttachmentCount = 1,
      .colorAttachments = &attachment,
      .timestampWrites = webgpu::gpu_prof::pass_writes("Volumetric fog apply"),
  };
  const auto pass = cmd.BeginRenderPass(&passDescriptor);
  pass.SetPipeline(apply.pipeline);
  pass.SetBindGroup(0, applyGroup);
  pass.Draw(3);
  pass.End();
}
} // namespace

bool ensure_task() {
  // Compatibility devices cannot bind a multisampled depth texture: with MSAA on, the apply pass has no
  // depth to read there, so the fog stays off (record() then fails closed).
  if (!webgpu::g_hasCoreFeatures && webgpu::g_graphicsConfig.msaaSamples > 1) {
    return false;
  }
  if (g_state.task == InvalidEncoderTask) {
    g_state.task = register_encoder_task_type(EncoderTaskDescriptor{.label = "Volumetric Fog", .callback = encode});
    if (g_state.task == InvalidEncoderTask) {
      Log.warn("could not register the volumetric fog task");
      return false;
    }
  }
  return true;
}

bool record(const Params& params) {
  if (g_state.task == InvalidEncoderTask) {
    return false;
  }
  // The targets are the pass's, here at the FIFO; the encode never asks the recorder, whose HDR mode
  // is reset every frame. Outside the HDR scene's pass (offscreen, a colour pass, no HDR scene) there
  // is no scene this fog knows: fail closed.
  if (!scene_hdr_pass_active()) {
    return false;
  }
  const auto& size = webgpu::g_sceneHdr.size;
  if (size.width == 0 || size.height == 0) {
    return false;
  }
  ensure_froxels(size.width, size.height);
  const uint32_t slot = g_nextSlot++ % g_recorded.size();
  g_recorded[slot] = params;
  g_recorded[slot].grid[0] = g_state.gridWidth;
  g_recorded[slot].grid[1] = g_state.gridHeight;
  g_recordedFroxels[slot] = g_state.froxelView;
  g_recordedCapture[slot] = Capture{
      .color = webgpu::g_sceneHdr, .resolved = webgpu::g_sceneHdrResolved, .depth = webgpu::g_depthBuffer};
  record_encoder_task(g_state.task, &slot, sizeof(slot));
  return true;
}

std::string apply_source(uint32_t samples) {
  std::string code = std::string(CommonSource) + ApplySource;
  const std::string token = "DEPTH_TYPE";
  code.replace(code.find(token), token.size(),
               samples > 1 ? "texture_depth_multisampled_2d" : "texture_depth_2d");
  if (!webgpu::g_hasCoreFeatures) {
    // textureLoad on a depth texture is rejected there; a Nearest sample at the texel centre returns the same texel.
    const std::string load = "textureLoad(depthTex, at, 0)";
    code.replace(code.find(load), load.size(),
                 "mp_depth_load(depthTex, depthSamp, at)");
    code += "\n@group(0) @binding(5) var depthSamp: sampler_comparison;\n";
    code += tex_copy_conv::compat_depth_load_wgsl();
  }
  return code;
}

const wgpu::TextureView& froxel_view() {
  if (g_state.froxelView) {
    return g_state.froxelView;
  }
  if (!g_state.placeholder) {
    const wgpu::TextureDescriptor descriptor{
        .label = "Volumetric Fog Froxels Placeholder",
        .usage = wgpu::TextureUsage::TextureBinding,
        .dimension = wgpu::TextureDimension::e3D,
        .size = {1, 1, 1},
        .format = FroxelFormat,
    };
    AURORA_NOTE_TEXTURE(&descriptor);
    g_state.placeholder = g_device.CreateTexture(&descriptor).CreateView();
  }
  return g_state.placeholder;
}

const wgpu::Sampler& sampler() {
  ensure_clamp_sampler();
  return g_state.clampSampler;
}

void shutdown() {
  const auto task = g_state.task;
  g_state = {};
  g_recordedFroxels = {};
  g_recordedCapture = {};
  if (task != InvalidEncoderTask) {
    unregister_encoder_task_type(task);
  }
}
} // namespace aurora::gfx::volfog
