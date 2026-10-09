#include "xray.hpp"

#include "../logging.hpp"
#include "../webgpu/gpu.hpp"
#include "../webgpu/gpu_prof.hpp"
#include "recording.hpp"

#include <aurora/gfx.hpp>

#include <array>
#include <cstring>
#include <string>

// Remastered's X-ray visor post passes, from its shader 00089f0 (one pixel shader; cb0 p0..p7):
//  - p0.z == 1, run after the opaque world (RenderPostOpaque): per pixel the depth is remapped to
//    d (the viewmodel's slice kept apart), a = max((d - lo) / (hi - lo), 0) with (lo, hi) = (0.03,
//    0.037) for d < 0.037 else (p2.w, p3.w), and the scene c = x^p0.x (x the exposed colour)
//    comes out as p1.w < 0 ? 1 - c p0.y : c p0.y, times |p1.w|, minus a p4.y: the visor's negative
//    of the world, with distant geometry fading to black;
//  - otherwise, run after the post effects (RenderAfterPostFX): p5.z taps of the frame scaled
//    about its centre by s = 1 - (p5.x i + p5.y i^2), each mapped through a piecewise ramp of its
//    luminance (p1.rgb at 0, p6.rgb at p6.w, p3.rgb at 1), averaged, and mixed with p2.rgb by a
//    vignette pow(clamp(|2 (uv - 0.5)| p4.w, 0, 1) p4.x, p4.z).
// The EFB holds the tone-mapped colour sRGB encoded, so pass 1 works in the exposed level (the
// EFB undone through the room's curve, as the fog does) and draws its result through the curve
// again; the ramp pass runs on the decoded colour and re-encodes it.
namespace aurora::gfx::xray {
namespace {
Module Log("aurora::gfx::xray");
using webgpu::g_device;

constexpr const char* Source = R"(
struct Params {
  tone: array<vec4f, 3>,
  p: array<vec4f, 8>,
  depth: vec4f,
};
@group(0) @binding(0) var<uniform> u: Params;
@group(0) @binding(1) var src: texture_2d<f32>;
@group(0) @binding(2) var depthTex: DEPTH_TYPE;
@group(0) @binding(3) var samp: sampler;

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

// The tone curve and its inverse, as bloom.cpp has them.
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

fn srgb_enc(c: vec3f) -> vec3f {
  let l = clamp(c, vec3f(0.0), vec3f(1.0));
  return select(1.055 * pow(l, vec3f(1.0 / 2.4)) - 0.055, 12.92 * l, l <= vec3f(0.0031308));
}

fn srgb_dec(c: vec3f) -> vec3f {
  let e = clamp(c, vec3f(0.0), vec3f(1.0));
  return select(pow((e + 0.055) / 1.055, vec3f(2.4)), e / 12.92, e <= vec3f(0.04045));
}

// Pass 1: the distortion over the opaque world.
@fragment
fn fs_distort(in: VertexOutput) -> @location(0) vec4f {
  let size = vec2i(textureDimensions(src));
  let f = textureLoad(src, min(vec2i(floor(in.pos.xy)), size - vec2i(1)), 0);
  let depthSize = vec2i(textureDimensions(depthTex));
  let at = min(vec2i(in.uv * vec2f(depthSize)), depthSize - vec2i(1));
  // Reversed Z; nearer than the world's depth range is the viewmodel (0.0097656..0.0386719).
  let z = 1.0 - textureLoad(depthTex, at, 0);
  let gun = z < u.depth.x;
  let d = select(clamp((z - u.depth.x) / max(u.depth.y - u.depth.x, 1e-6), 0.0, 1.0),
                 mix(0.0097656, 0.0386719, z / u.depth.x), gun);
  let near = d < 0.037;
  let lo = select(u.p[2].w, 0.03, near);
  let hi = select(u.p[3].w, 0.037, near);
  let a = max((d - lo) / (hi - lo), 0.0);
  // x: the exposed colour; u.p[0].w is 1 / exposure.
  let y = srgb_dec(f.rgb);
  let x = vec3f(untone(y.r), untone(y.g), untone(y.b));
  let c = pow(x, vec3f(u.p[0].x));
  let sel = select(c * u.p[0].y, vec3f(1.0) - c * u.p[0].y, u.p[1].w < 0.0);
  let e = max(sel * abs(u.p[1].w) - vec3f(a * u.p[4].y), vec3f(0.0));
  return vec4f(srgb_enc(vec3f(tone(e.r), tone(e.g), tone(e.b))), 1.0);
}

// Pass 2: the ramp taps and the vignette.
fn ramp(l: f32) -> vec3f {
  let mid = u.p[6].w;
  if (l <= mid) {
    return u.p[1].xyz + clamp(l / mid, 0.0, 1.0) * (u.p[6].xyz - u.p[1].xyz);
  }
  return u.p[6].xyz + clamp((l - mid) / (1.0 - mid), 0.0, 1.0) * (u.p[3].xyz - u.p[6].xyz);
}

@fragment
fn fs_ramp(in: VertexOutput) -> @location(0) vec4f {
  let taps = u32(max(u.p[5].z, 1.0));
  var sum = vec3f(0.0);
  for (var i = 0u; i < taps; i++) {
    let fi = f32(i);
    let s = 1.0 - (u.p[5].x * fi + u.p[5].y * fi * fi);
    let uv = in.uv * s + 0.5 - 0.5 * s;
    let c = srgb_dec(textureSampleLevel(src, samp, uv, 0.0).rgb);
    let l = dot(c, vec3f(0.2126, 0.7152, 0.0722));
    sum += ramp(l);
  }
  let avg = sum / f32(taps);
  let v = pow(clamp(length(2.0 * (in.uv - 0.5)) * u.p[4].w, 0.0, 1.0) * u.p[4].x, u.p[4].z);
  return vec4f(srgb_enc(mix(avg, u.p[2].xyz, v)), 1.0);
}
)";

struct State {
  EncoderTaskId task = InvalidEncoderTask;
  wgpu::RenderPipeline distort;
  wgpu::RenderPipeline ramp;
  wgpu::BindGroupLayout layout;
  wgpu::TextureFormat pipelineFormat = wgpu::TextureFormat::Undefined;
  uint32_t pipelineSamples = 0;
  wgpu::Buffer uniforms;
  wgpu::Sampler sampler;
  wgpu::Texture frame;
  wgpu::TextureView frameView;
  wgpu::TextureFormat frameFormat = wgpu::TextureFormat::Undefined;
  uint32_t width = 0;
  uint32_t height = 0;
};
State g_state;

// Params are larger than an encoder task's inline payload, so the task carries a slot of this ring.
std::array<Params, 8> g_recorded;
// A slot's uniform offset in the buffer (the dynamic-offset alignment limit's worst case).
constexpr uint64_t SlotStride = 256;
static_assert(sizeof(Params) <= SlotStride);
uint32_t g_nextSlot = 0;

void ensure_pipelines(wgpu::TextureFormat format, uint32_t samples) {
  if (g_state.distort && g_state.pipelineFormat == format && g_state.pipelineSamples == samples) {
    return;
  }
  std::string code = Source;
  const std::string token = "DEPTH_TYPE";
  code.replace(code.find(token), token.size(), samples > 1 ? "texture_depth_multisampled_2d" : "texture_depth_2d");
  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code = code.c_str();
  const wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &wgsl, .label = "X-ray Post"};
  const auto module = g_device.CreateShaderModule(&moduleDescriptor);
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
          .sampler = {.type = wgpu::SamplerBindingType::Filtering},
      },
  };
  const wgpu::BindGroupLayoutDescriptor layoutDescriptor{
      .label = "X-ray Post Layout",
      .entryCount = entries.size(),
      .entries = entries.data(),
  };
  g_state.layout = g_device.CreateBindGroupLayout(&layoutDescriptor);
  const wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor{
      .label = "X-ray Post Pipeline Layout",
      .bindGroupLayoutCount = 1,
      .bindGroupLayouts = &g_state.layout,
  };
  const auto pipelineLayout = g_device.CreatePipelineLayout(&pipelineLayoutDescriptor);
  const wgpu::ColorTargetState target{.format = format, .writeMask = wgpu::ColorWriteMask::All};
  const auto make = [&](const char* label, const char* entry) {
    const wgpu::FragmentState fragment{.module = module, .entryPoint = entry, .targetCount = 1, .targets = &target};
    const wgpu::RenderPipelineDescriptor descriptor{
        .label = label,
        .layout = pipelineLayout,
        .vertex = {.module = module, .entryPoint = "vs_main"},
        .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
        .multisample = {.count = samples, .mask = UINT32_MAX},
        .fragment = &fragment,
    };
    return g_device.CreateRenderPipeline(&descriptor);
  };
  g_state.distort = make("X-ray Distortion", "fs_distort");
  g_state.ramp = make("X-ray Ramp", "fs_ramp");
  g_state.pipelineFormat = format;
  g_state.pipelineSamples = samples;
  if (!g_state.uniforms) {
    const wgpu::BufferDescriptor bufferDescriptor{
        .label = "X-ray Post Uniforms",
        .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
        .size = SlotStride * g_recorded.size(),
    };
    g_state.uniforms = g_device.CreateBuffer(&bufferDescriptor);
    const wgpu::SamplerDescriptor samplerDescriptor{
        .label = "X-ray Post Sampler",
        .addressModeU = wgpu::AddressMode::ClampToEdge,
        .addressModeV = wgpu::AddressMode::ClampToEdge,
        .magFilter = wgpu::FilterMode::Linear,
        .minFilter = wgpu::FilterMode::Linear,
    };
    g_state.sampler = g_device.CreateSampler(&samplerDescriptor);
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
      .label = "X-ray Post Frame Copy",
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
  const auto& depth = webgpu::g_depthBuffer;
  const uint32_t samples = webgpu::g_graphicsConfig.msaaSamples > 1 ? webgpu::g_graphicsConfig.msaaSamples : 1;
  const auto format = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  const uint32_t width = source.size.width;
  const uint32_t height = source.size.height;
  if (width == 0 || height == 0 || !source.texture || !target.view || !depth.view) {
    return;
  }
  ensure_pipelines(format, samples);
  ensure_frame(width, height, format);
  const bool distortion = params.p[0][2] == 1.f;
  // One uniform slot per recorded pass: queue writes land before the whole submission.
  const uint64_t offset = uint64_t(slot) * SlotStride;
  ctx.queue.WriteBuffer(g_state.uniforms, offset, &params, sizeof(params));

  const wgpu::TexelCopyTextureInfo copySource{.texture = source.texture};
  const wgpu::TexelCopyTextureInfo copyTarget{.texture = g_state.frame};
  const wgpu::Extent3D copySize{width, height, 1};
  cmd.CopyTextureToTexture(&copySource, &copyTarget, &copySize);

  const std::array groupEntries{
      wgpu::BindGroupEntry{.binding = 0, .buffer = g_state.uniforms, .offset = offset, .size = sizeof(Params)},
      wgpu::BindGroupEntry{.binding = 1, .textureView = g_state.frameView},
      wgpu::BindGroupEntry{.binding = 2, .textureView = depth.view},
      wgpu::BindGroupEntry{.binding = 3, .sampler = g_state.sampler},
  };
  const wgpu::BindGroupDescriptor groupDescriptor{
      .label = "X-ray Post",
      .layout = g_state.layout,
      .entryCount = groupEntries.size(),
      .entries = groupEntries.data(),
  };
  const auto group = g_device.CreateBindGroup(&groupDescriptor);
  // Every pixel is written without blending, so the frame need not be loaded first.
  const wgpu::RenderPassColorAttachment attachment{
      .view = target.view,
      .resolveTarget = samples > 1 ? webgpu::g_frameBufferResolved.view : wgpu::TextureView{},
      .loadOp = wgpu::LoadOp::Clear,
      .storeOp = wgpu::StoreOp::Store,
      .clearValue = {0.0, 0.0, 0.0, 0.0},
  };
  const wgpu::RenderPassDescriptor passDescriptor{
      .label = "X-ray Post",
      .colorAttachmentCount = 1,
      .colorAttachments = &attachment,
      .timestampWrites = webgpu::gpu_prof::pass_writes("X-ray post"),
  };
  const auto pass = cmd.BeginRenderPass(&passDescriptor);
  pass.SetPipeline(distortion ? g_state.distort : g_state.ramp);
  pass.SetBindGroup(0, group);
  pass.Draw(3);
  pass.End();
}
} // namespace

bool ensure_task() {
  if (g_state.task == InvalidEncoderTask) {
    g_state.task = register_encoder_task_type(EncoderTaskDescriptor{.label = "X-ray Post", .callback = encode});
    if (g_state.task == InvalidEncoderTask) {
      Log.warn("could not register the X-ray post task");
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
  if (task != InvalidEncoderTask) {
    unregister_encoder_task_type(task);
  }
}
} // namespace aurora::gfx::xray
