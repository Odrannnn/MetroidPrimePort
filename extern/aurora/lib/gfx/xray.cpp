#include "xray.hpp"
#include "tex_copy_conv.hpp"

#include "../logging.hpp"
#include "../webgpu/gpu.hpp"
#include "../webgpu/gpu_prof.hpp"
#include "recording.hpp"

#include <aurora/gfx.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include "texture_sites.hpp"

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
// Pass 1 works in the exposed level on the HDR scene; the ramp pass runs after the post on the
// display-encoded EFB, on the decoded colour, and re-encodes it.
// Remastered draws the whole X-ray world into a 0.7-scale HDR target and blends it back to the
// full-resolution frame (xray_blend_upscale 0xfb4500, a plain textured quad), tone mapping after
// that: pass 1 here therefore writes the level E at 0.7 of the frame, sampling the full-resolution
// frame at each target pixel's centre (the nearest of the pixels the world was rasterised to), and
// a second pass filters it up bilinearly in that HDR domain. The target takes the main frame's
// format, R11G11B10F (kb topic/auto-exposure-average.md), which has no sign: where the depth fade
// pulls E below 0 the store keeps 0, so the sky and far geometry go black, as Remastered shows.
// Pass A reads the HDR scene's exposed linear light as it is and blends the level back into it as
// it is (kb topic/visor-hdr-pipeline.md): the post's tonemap applies the curve once. Outside the
// HDR scene there is no pass A.
namespace aurora::gfx::xray {
namespace {
Module Log("aurora::gfx::xray");
using webgpu::g_device;

// HDR light, unclamped above; the filtering up happens before the curve.
constexpr wgpu::TextureFormat LevelFormat = wgpu::TextureFormat::RGBA16Float;

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

// Pass 2 runs after the post, on the display-encoded EFB.
fn srgb_enc(c: vec3f) -> vec3f {
  let l = clamp(c, vec3f(0.0), vec3f(1.0));
  return select(1.055 * pow(l, vec3f(1.0 / 2.4)) - 0.055, 12.92 * l, l <= vec3f(0.0031308));
}

fn srgb_dec(c: vec3f) -> vec3f {
  let e = clamp(c, vec3f(0.0), vec3f(1.0));
  return select(pow((e + 0.055) / 1.055, vec3f(2.4)), e / 12.92, e <= vec3f(0.04045));
}

// Pass 1: the distortion over the opaque world, as the level E at the low-resolution target.
@fragment
fn fs_distort(in: VertexOutput) -> @location(0) vec4f {
  let size = vec2i(textureDimensions(src));
  let f = textureLoad(src, min(vec2i(in.uv * vec2f(size)), size - vec2i(1)), 0);
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
  // x: the exposed colour, which the HDR scene holds as it is (it can exceed 1, and go negative).
  let x = f.rgb;
  let c = pow(x, vec3f(u.p[0].x));
  let sel = select(c * u.p[0].y, vec3f(1.0) - c * u.p[0].y, u.p[1].w < 0.0);
  let e = sel * abs(u.p[1].w) - vec3f(a * u.p[4].y);
  // Remastered's R11G11B10F target stores a negative level as 0.
  return vec4f(max(e, vec3f(0.0)), 1.0);
}

// Pass 1b: the low-resolution level, filtered up and blended back as the HDR scene holds it; the
// post's tonemap applies the curve.
fn enc(e: vec3f) -> vec3f { return e; }

@fragment
fn fs_upscale(in: VertexOutput) -> @location(0) vec4f {
  let e = textureSampleLevel(src, samp, in.uv, 0.0).rgb;
  return vec4f(enc(e), 1.0);
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
  wgpu::RenderPipeline distort; // into the low-resolution float target
  wgpu::RenderPipeline upscale;
  wgpu::RenderPipeline ramp;
  wgpu::Texture level;
  wgpu::TextureView levelView;
  uint32_t levelWidth = 0;
  uint32_t levelHeight = 0;
  wgpu::BindGroupLayout layout;
  wgpu::TextureFormat pipelineFormat = wgpu::TextureFormat::Undefined;
  uint32_t pipelineSamples = 0;
  wgpu::Buffer uniforms;
  wgpu::Sampler sampler;
  wgpu::Sampler nearestSampler; // Compatibility: depth is read through a comparison sampler
  wgpu::Texture frame;
  wgpu::TextureView frameView;
  wgpu::TextureFormat frameFormat = wgpu::TextureFormat::Undefined;
  uint32_t width = 0;
  uint32_t height = 0;
};
State g_state;

// The targets a slot reads and writes, taken at the FIFO: the recorder's HDR mode is reset every
// frame, so the encode cannot ask it.
struct Capture {
  bool scene = false;                  // pass A on the HDR scene; pass B on the display EFB
  webgpu::TextureWithSampler color;    // the pass's scene attachment (MSAA when samples > 1)
  webgpu::TextureWithSampler resolved; // its resolve target
  webgpu::TextureWithSampler depth;
};

// Params are larger than an encoder task's inline payload, so the task carries a slot of this ring.
std::array<Params, 8> g_recorded;
std::array<Capture, 8> g_recordedCapture;
// A slot's uniform offset in the buffer (the dynamic-offset alignment limit's worst case).
constexpr uint64_t SlotStride = 256;
static_assert(sizeof(Params) <= SlotStride);
uint32_t g_nextSlot = 0;

void ensure_pipelines(wgpu::TextureFormat format, uint32_t samples) {
  if (g_state.distort && g_state.pipelineFormat == format && g_state.pipelineSamples == samples) {
    return;
  }
  const std::string code = source_for(samples);
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
      // Compatibility only: the depth is sampled through a Nearest sampler (see source_for)
      wgpu::BindGroupLayoutEntry{
          .binding = 4,
          .visibility = wgpu::ShaderStage::Fragment,
          .sampler = {.type = wgpu::SamplerBindingType::Comparison},
      },
  };
  const wgpu::BindGroupLayoutDescriptor layoutDescriptor{
      .label = "X-ray Post Layout",
      .entryCount = webgpu::g_hasCoreFeatures ? 4u : 5u,
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
  const wgpu::ColorTargetState levelTarget{.format = LevelFormat, .writeMask = wgpu::ColorWriteMask::All};
  const auto make = [&](const char* label, const char* entry, bool lowRes = false) {
    const wgpu::FragmentState fragment{
        .module = module, .entryPoint = entry, .targetCount = 1, .targets = lowRes ? &levelTarget : &target};
    const wgpu::RenderPipelineDescriptor descriptor{
        .label = label,
        .layout = pipelineLayout,
        .vertex = {.module = module, .entryPoint = "vs_main"},
        .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
        .multisample = {.count = lowRes ? 1u : samples, .mask = UINT32_MAX},
        .fragment = &fragment,
    };
    return g_device.CreateRenderPipeline(&descriptor);
  };
  g_state.distort = make("X-ray Distortion", "fs_distort", true);
  g_state.upscale = make("X-ray Upscale", "fs_upscale");
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
    const wgpu::SamplerDescriptor nearestDescriptor{
        .label = "X-ray Post Depth Sampler",
        .addressModeU = wgpu::AddressMode::ClampToEdge,
        .addressModeV = wgpu::AddressMode::ClampToEdge,
        .magFilter = wgpu::FilterMode::Nearest,
        .minFilter = wgpu::FilterMode::Nearest,
        .compare = wgpu::CompareFunction::LessEqual,
    };
    g_state.nearestSampler = g_device.CreateSampler(&nearestDescriptor);
  }
}

void ensure_level(uint32_t width, uint32_t height) {
  if (g_state.level && g_state.levelWidth == width && g_state.levelHeight == height) {
    return;
  }
  g_state.levelWidth = width;
  g_state.levelHeight = height;
  const wgpu::TextureDescriptor descriptor{
      .label = "X-ray Post Level",
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
      .size = {width, height, 1},
      .format = LevelFormat,
  };
  AURORA_NOTE_TEXTURE(&descriptor);
  g_state.level = g_device.CreateTexture(&descriptor);
  g_state.levelView = g_state.level.CreateView();
}

// The HDR targets a pass reads and writes, checked as one whole: the scene format throughout, the
// depth buffer's size and sample count those of the colour, a resolve target when multisampled.
// Anything else fails closed (no visor post) instead of shrinking 16F texels as 8-bit ones.
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
  AURORA_NOTE_TEXTURE(&descriptor);
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
  const Capture& capture = g_recordedCapture[slot];
  const bool hdr = capture.scene;
  uint32_t hdrSamples = 1;
  if (hdr && !hdr_capture_valid(capture, hdrSamples)) {
    return;
  }
  // The targets this frame was recorded against (a resize may have made newer ones since).
  const auto efb = webgpu::efb_targets();
  const auto& source = hdr ? (hdrSamples > 1 ? capture.resolved : capture.color) : webgpu::present_source();
  const auto& target = hdr ? capture.color : efb.color;
  const auto& resolveTarget = hdr ? capture.resolved : efb.resolved;
  const auto& depth = hdr ? capture.depth : efb.depth;
  const uint32_t samples =
      hdr ? hdrSamples : (webgpu::g_graphicsConfig.msaaSamples > 1 ? webgpu::g_graphicsConfig.msaaSamples : 1);
  const auto format = hdr ? webgpu::SceneHdrFormat : webgpu::g_graphicsConfig.surfaceConfiguration.format;
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

  const auto makeGroup = [&](const char* label, const wgpu::TextureView& view) {
    const std::array groupEntries{
        wgpu::BindGroupEntry{.binding = 0, .buffer = g_state.uniforms, .offset = offset, .size = sizeof(Params)},
        wgpu::BindGroupEntry{.binding = 1, .textureView = view},
        wgpu::BindGroupEntry{.binding = 2, .textureView = depth.view},
        wgpu::BindGroupEntry{.binding = 3, .sampler = g_state.sampler},
        wgpu::BindGroupEntry{.binding = 4, .sampler = g_state.nearestSampler},
    };
    const wgpu::BindGroupDescriptor groupDescriptor{
        .label = label,
        .layout = g_state.layout,
        .entryCount = webgpu::g_hasCoreFeatures ? 4u : 5u,
        .entries = groupEntries.data(),
    };
    return g_device.CreateBindGroup(&groupDescriptor);
  };
  if (distortion) {
    // Pass 1a: the level at the low-resolution target.
    const float scale = params.depth[2] > 0.f && params.depth[2] <= 1.f ? params.depth[2] : 1.f;
    ensure_level(std::max(1u, uint32_t(float(width) * scale)), std::max(1u, uint32_t(float(height) * scale)));
    const wgpu::RenderPassColorAttachment levelAttachment{
        .view = g_state.levelView,
        .loadOp = wgpu::LoadOp::Clear,
        .storeOp = wgpu::StoreOp::Store,
        .clearValue = {0.0, 0.0, 0.0, 0.0},
    };
    const wgpu::RenderPassDescriptor levelDescriptor{
        .label = "X-ray Level",
        .colorAttachmentCount = 1,
        .colorAttachments = &levelAttachment,
        .timestampWrites = webgpu::gpu_prof::pass_writes("X-ray level"),
    };
    const auto levelPass = cmd.BeginRenderPass(&levelDescriptor);
    levelPass.SetPipeline(g_state.distort);
    levelPass.SetBindGroup(0, makeGroup("X-ray Level", g_state.frameView));
    levelPass.Draw(3);
    levelPass.End();
  }
  const auto group = makeGroup("X-ray Post", distortion ? g_state.levelView : g_state.frameView);
  // Every pixel is written without blending, so the frame need not be loaded first.
  const wgpu::RenderPassColorAttachment attachment{
      .view = target.view,
      .resolveTarget = samples > 1 ? resolveTarget.view : wgpu::TextureView{},
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
  pass.SetPipeline(distortion ? g_state.upscale : g_state.ramp);
  pass.SetBindGroup(0, group);
  pass.Draw(3);
  pass.End();
}
} // namespace

// The task's source for a sample count: outside the anonymous namespace so it is the one declared
// in xray.hpp (the anonymous one would be a different function).
std::string source_for(uint32_t samples) {
  std::string code = Source;
  const std::string depthToken = "DEPTH_TYPE";
  const size_t depthAt = code.find(depthToken);
  if (depthAt != std::string::npos) {
    code.replace(depthAt, depthToken.size(),
                 samples > 1 ? "texture_depth_multisampled_2d" : "texture_depth_2d");
  }
  if (!webgpu::g_hasCoreFeatures) {
    // textureLoad on a depth texture is rejected there; a Nearest sample at the texel centre returns the same texel.
    const std::string load = "textureLoad(depthTex, at, 0)";
    const size_t at = code.find(load);
    if (at != std::string::npos) {
      code.replace(at, load.size(),
                   "mp_depth_load(depthTex, depthSamp, at)");
    }
    code += "\n@group(0) @binding(4) var depthSamp: sampler_comparison;\n";
    code += tex_copy_conv::compat_depth_load_wgsl();
  }
  return code;
}

bool ensure_task() {
  // Compatibility devices cannot bind a multisampled depth texture: with MSAA on, the distortion pass has no
  // depth to read there, so the X-ray post stays off (record() then fails closed).
  if (!webgpu::g_hasCoreFeatures && webgpu::g_graphicsConfig.msaaSamples > 1) {
    return false;
  }
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
  // The targets are the pass's, taken here at the FIFO; the encode never asks the recorder, whose HDR
  // mode is reset every frame. Pass A blends into the HDR scene and has no target outside it (no
  // scene, offscreen, a colour pass): fail closed. Pass B runs after the post, which has left the HDR
  // scene, on the display EFB.
  const bool hdr = scene_hdr_pass_active();
  if (params.p[0][2] == 1.f ? !hdr : scene_hdr_active()) {
    return false;
  }
  const auto& size = hdr ? webgpu::g_sceneHdr.size : webgpu::present_source().size;
  if (size.width == 0 || size.height == 0) {
    return false;
  }
  const uint32_t slot = g_nextSlot++ % g_recorded.size();
  g_recorded[slot] = params;
  g_recordedCapture[slot] = hdr ? Capture{.scene = true,
                                          .color = webgpu::g_sceneHdr,
                                          .resolved = webgpu::g_sceneHdrResolved,
                                          .depth = webgpu::g_depthBuffer}
                                : Capture{};
  record_encoder_task(g_state.task, &slot, sizeof(slot));
  return true;
}

void shutdown() {
  const auto task = g_state.task;
  g_state = {};
  g_recordedCapture = {};
  if (task != InvalidEncoderTask) {
    unregister_encoder_task_type(task);
  }
}
} // namespace aurora::gfx::xray
