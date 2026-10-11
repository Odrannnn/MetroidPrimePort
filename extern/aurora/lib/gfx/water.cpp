#include <aurora/water.hpp>

#include "../gx/fifo.hpp"
#include "../gx/gx.hpp"
#include "../gx/texture.hpp"
#include "../logging.hpp"
#include "../webgpu/gpu.hpp"
#include "fx_pipelines.hpp"
#include "pipeline_cache.hpp"
#include "probe.hpp"
#include "recording.hpp"
#include "resource_cache.hpp"
#include "texture.hpp"
#include "volfog.hpp"

#include <aurora/gfx.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

// Remastered's water surface, drawn natively (see aurora/water.hpp). The shader text follows
// Remastered's _fv_00dddc8 and its TOP and BOTTOM pixel shaders line for line
// (build/mpr/water/A-shader.md, D-rain.md); what is the port's own is said where it is: the
// inputs it is fed (the room's volume and cube as the GX PBR path has them) and the blend,
// which Remastered does with dual-source blending in linear light and the port does here
// against a snapshot of the tone-mapped EFB.
namespace aurora::gfx::water {
namespace {
Module Log("aurora::gfx::water");

constexpr uint32_t UniformSize = 640;
constexpr auto EvictAfter = std::chrono::seconds(10);

struct Uniform {
  float proj[16];
  float mv[12];
  float nrm[12];
  float S[8][4];
  float R[3][4];
  float W[3][4];
  float cubeRows[3][4];
  float volRows[3][4];
  float volScale[4];
  float fallbackMean[4];
  float fog[4];   // near, range, exposure, on
  float tone[3][4];
  float depth[4]; // near, far, zMin, zMax
  float misc[4];  // reversed Z, has depth, has volume, has cube
  float cube[4];  // scale, mips
  float pad[4];
};
static_assert(sizeof(Uniform) == UniformSize);

enum Binding : uint32_t {
  BUniform,
  BNormal,
  BNormalSamp,
  BSourceFlow,
  BFlowSamp,
  BFlow,
  BRainNoise,
  BRainSamp,
  BCube,
  BCubeSamp,
  BBrdf,
  BVolume,
  BFroxels,
  BFogSamp,
  BSceneColor,
  BSceneDepth,
  BCount
};

// A view a draw reads, kept alive (with the texture it is of) until no draw has used it for
// EvictAfter. Ids go into the payload, since the views are made on the game thread.
struct Entry {
  wgpu::TextureView view;
  TextureHandle keep;
  std::chrono::steady_clock::time_point lastUse;
};

enum Dummy { DummyBlack, DummyFlat, DummyFlow, DummyCount };

struct State {
  std::mutex mutex;
  std::unordered_map<const void*, uint32_t> byKey;
  std::unordered_map<uint32_t, Entry> byId;
  uint32_t nextId = 1;
  DrawTypeId drawType = InvalidDrawType;
  // Render thread only
  wgpu::BindGroupLayout bindLayout;
  wgpu::PipelineLayout pipelineLayout;
  std::map<std::array<uint64_t, 5>, wgpu::RenderPipeline> pipelines;
  std::map<std::array<const void*, BCount>, wgpu::BindGroup> groups;
  wgpu::Texture dummyTexture[DummyCount];
  wgpu::TextureView dummyView[DummyCount];
};
State g_state;

struct Payload {
  uint32_t flags; // 1 bottom, 2 rain
  uint32_t cull;  // wgpu::CullMode
  uint32_t compare;
  uint32_t indexCount;
  Range verts;
  Range indices;
  Range uniform;
  uint32_t normalId, sourceFlowId, rainId, colorId, depthId; // 0: the dummy
  uint32_t cube, volume;
};
static_assert(sizeof(Payload) <= InlineDrawPayloadSize);

const char* ShaderSource = R"(
struct U {
  proj: mat4x4f,
  mv: mat3x4f,
  nrm: mat3x4f,
  S: array<vec4f, 8>,
  R: array<vec4f, 3>,
  W: array<vec4f, 3>,
  cube0: vec4f,
  cube1: vec4f,
  cube2: vec4f,
  vol0: vec4f,
  vol1: vec4f,
  vol2: vec4f,
  volScale: vec4f,
  fallbackMean: vec4f,
  fog: vec4f,
  tone0: vec4f,
  tone1: vec4f,
  tone2: vec4f,
  depth: vec4f,
  misc: vec4f,
  cubeP: vec4f,
  pad: vec4f,
}
@group(0) @binding(0) var<uniform> u: U;
@group(0) @binding(1) var normalMap: texture_2d<f32>;
@group(0) @binding(2) var normalSamp: sampler;
@group(0) @binding(3) var sourceFlowMap: texture_2d<f32>;
@group(0) @binding(4) var flowSamp: sampler;
@group(0) @binding(5) var flowMap: texture_2d<f32>;
@group(0) @binding(6) var rainNoise: texture_2d<f32>;
@group(0) @binding(7) var rainSamp: sampler;
@group(0) @binding(8) var cube: texture_cube<f32>;
@group(0) @binding(9) var cubeSamp: sampler;
@group(0) @binding(10) var brdfLut: texture_2d<f32>;
@group(0) @binding(11) var volMean: texture_3d<f32>;
@group(0) @binding(12) var froxels: texture_3d<f32>;
@group(0) @binding(13) var fogSamp: sampler;
@group(0) @binding(14) var sceneColor: texture_2d<f32>;
@group(0) @binding(15) var sceneDepth: texture_2d<f32>;

struct VIn {
  @location(0) pos: vec3f,
  @location(1) uv: vec2f,
  @location(2) color: vec4f,
}
// Remastered's o1..o9 (o2, the vertex colour, is unused by the pixel shaders).
struct VOut {
  @builtin(position) pos: vec4f,
  @location(0) n: vec3f,     // o1: the geometric normal (view)
  @location(1) clip: vec4f,  // o3: clip x, y, w, view depth
  @location(2) t: vec3f,     // o4: the tangent (view), not normalised
  @location(3) vpos: vec3f,  // o5: the view position
  @location(4) uv: vec4f,    // o6: the normal map's uv, the mesh's uv
  @location(5) rain: vec2f,  // o7: the rain's plane coordinates
  @location(6) light: vec4f, // o8: the volume's mean light, and its largest channel
  @location(7) fog: vec4f,   // o9: the froxel's in-scatter and transmittance
}

// The flow simulation's height (t0) and the source flow map's (t1).
fn flow_h(uv: vec2f) -> f32 { return textureSampleLevel(flowMap, flowSamp, uv, 0.0).x; }
fn source_w(uv: vec2f) -> f32 { return textureSampleLevel(sourceFlowMap, flowSamp, uv, 0.0).w; }

// The mesh position q lifted by the flow, moved by the two waves, and lowered by the source
// flow's height. Note W[0].xy goes with W[2] and W[0].zw with W[1], as the game fills them.
fn displace(q: vec3f, uv: vec2f, k: f32, r1: vec3f) -> vec3f {
  var p = q + vec3f(0.0, flow_h(uv) * k, 0.0);
  let ph1 = dot(u.W[0].xy, p.xz * u.W[1].w) - u.W[1].z;
  let ph2 = dot(u.W[0].zw, p.xz * u.W[2].w) - u.W[2].z;
  let s1 = sin(ph1);
  let c1 = cos(ph1);
  let s2 = sin(ph2);
  let c2 = cos(ph2);
  let dxz = -u.W[0].xy * u.W[1].x * s1 - u.W[0].zw * u.W[2].x * s2;
  let dy = c1 * u.W[1].y + c2 * u.W[2].y;
  p = p + r1 * vec3f(dxz.x, dy, dxz.y);
  p.y = p.y + (source_w(uv) - 1.0) * k;
  return p;
}

@vertex
fn vs_main(in: VIn) -> VOut {
  var out: VOut;
  let fade = clamp(length(vec4f(in.pos, 1.0) * u.mv) - 1.0, 0.0, 1.0);
  let r1 = fade * in.color.xyz;
  let k = fade * u.S[4].w;
  let e = u.S[4].x;
  let p = displace(in.pos, in.uv, k, r1);
  let a = displace(in.pos + vec3f(0.0, 0.0, -e), in.uv + vec2f(0.0, -u.S[3].w), k, r1);
  let b = displace(in.pos + vec3f(0.0, 0.0, e), in.uv + vec2f(0.0, u.S[3].w), k, r1);
  let c = displace(in.pos + vec3f(-e, 0.0, 0.0), in.uv + vec2f(-u.S[3].z, 0.0), k, r1);
  let d = displace(in.pos + vec3f(e, 0.0, 0.0), in.uv + vec2f(u.S[3].z, 0.0), k, r1);
  let dc = d - c;
  let n = normalize(cross(b - a, dc));
  let t = cross(n, cross(dc, n));
  out.n = vec4f(n, 0.0) * u.nrm;
  out.t = vec4f(t, 0.0) * u.nrm;
  let vp = vec4f(p, 1.0) * u.mv;
  let clip = vec4f(vp, 1.0) * u.proj;
  out.pos = clip;
  out.clip = vec4f(clip.x, clip.y, clip.w, -vp.z);
  out.vpos = vp;
  out.uv = vec4f(in.uv * u.S[0].w - u.S[0].x * u.S[1].z * u.S[1].xy, in.uv);
  out.rain = in.pos.xz + 0.3 * sin(in.pos.zx);
  var mean = u.fallbackMean.rgb;
  if (u.misc.z > 0.5) {
    let v = vec4f(vp, 1.0);
    let uvw = vec3f(dot(u.vol0, v), dot(u.vol1, v), dot(u.vol2, v));
    // Black outside the grid, as Remastered's CLAMP_TO_BORDER sampler reads it.
    let vsize = vec3f(textureDimensions(volMean));
    let vt = uvw * vsize;
    let vw = clamp(min(vt + 0.5, vsize + 0.5 - vt), vec3f(0.0), vec3f(1.0));
    mean = textureSampleLevel(volMean, cubeSamp, uvw, 0.0).rgb * (vw.x * vw.y * vw.z);
  }
  out.light = vec4f(mean * u.volScale.rgb, max(max(mean.r, mean.g), mean.b) * u.volScale.w);
  out.fog = vec4f(0.0, 0.0, 0.0, 1.0);
  if (u.fog.w > 0.5) {
    let ndc = clip.xy / clip.w;
    let span = u.fog.y - u.fog.x;
    let slice = sqrt(clamp(select((-vp.z - u.fog.x) / span, select(0.0, 1.0, -vp.z > u.fog.x), span == 0.0), 0.0, 1.0));
    let f = textureSampleLevel(froxels, fogSamp, vec3f(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5, slice), 0.0);
    out.fog = vec4f(f.rgb * u.fog.z, f.a);
  }
  return out;
}

fn brdf(nv: f32, rough: f32) -> vec2f {
  return textureSampleLevel(brdfLut, cubeSamp, vec2f(nv, rough), 0.0).xy;
}

fn env_at(r: vec3f, lod: f32) -> vec3f {
  if (u.misc.w < 0.5) { return vec3f(0.0); }
  let rb = vec3f(dot(u.cube0.xyz, r), dot(u.cube1.xyz, r), dot(u.cube2.xyz, r));
  return textureSampleLevel(cube, cubeSamp, rb, lod).rgb * u.cubeP.x;
}

// The EFB's tone curve (GXSetPBRTone, or the PBR path's fallback roll-off) and its inverse,
// as volfog.cpp's full-screen pass has them.
// The EFB holds colour as Remastered's sRGB swapchain does: the exact piecewise sRGB curve.
fn srgb_enc(c: vec3f) -> vec3f {
  let l = clamp(c, vec3f(0.0), vec3f(1.0));
  return select(1.055 * pow(l, vec3f(1.0 / 2.4)) - 0.055, 12.92 * l, l <= vec3f(0.0031308));
}

fn srgb_dec(c: vec3f) -> vec3f {
  let e = clamp(c, vec3f(0.0), vec3f(1.0));
  return select(pow((e + 0.055) / 1.055, vec3f(2.4)), e / 12.92, e <= vec3f(0.04045));
}

fn tone(x: f32) -> f32 {
  if (u.tone1.x <= 0.0) {
    return min(x, 0.6) + 0.4 * (1.0 - exp(-max(x - 0.6, 0.0) / 0.4));
  }
  if (x < u.tone1.z) {
    return ((u.tone0.x * x + u.tone0.y) * x + u.tone0.z) * x;
  }
  if (x < u.tone1.w) {
    return u.tone1.x * x + u.tone1.y;
  }
  let st = max(u.tone2.y * x + u.tone2.z, 0.0);
  return u.tone2.x * st / (1.0 + st) + u.tone2.w;
}

fn untone(y: f32) -> f32 {
  if (u.tone1.x <= 0.0) {
    if (y < 0.6) { return y; }
    return 0.6 - 0.4 * log(1.0 - min((y - 0.6) / 0.4, 0.999));
  }
  let mid = u.tone1.z;
  let lineStart = u.tone1.x * mid + u.tone1.y;
  if (y < lineStart) {
    var x = y / max(lineStart, 1e-4) * mid;
    for (var n = 0; n < 4; n++) {
      let slope = (3.0 * u.tone0.x * x + 2.0 * u.tone0.y) * x + u.tone0.z;
      x = clamp(x - (tone(x) - y) / max(slope, 1e-4), 0.0, mid);
    }
    return x;
  }
  let top = u.tone2.w;
  if (y < top || u.tone2.y <= 0.0) {
    return (y - u.tone1.y) / u.tone1.x;
  }
  let q = min((y - top) / max(u.tone2.x, 1e-4), 0.999);
  return q / (1.0 - q) / u.tone2.y + u.tone1.w;
}

@fragment
fn fs_main(in: VOut) -> @location(0) vec4f {
  // The normal map, read twice along the flow and cross-faded.
  let fl = textureSample(sourceFlowMap, flowSamp, in.uv.zw).xyz;
  let dir = (fl.xy * 2.0 - 1.0) * u.S[4].y;
  let ph = u.S[0].x * u.S[4].z + 2.0 * fl.z;
  let pa = fract(ph);
  let pb = fract(ph + 0.5);
  let n1 = textureSample(normalMap, normalSamp, in.uv.xy + pa * dir).xy;
  let n2 = textureSample(normalMap, normalSamp, in.uv.xy + pb * dir + 0.5).xy;
  let n = mix(n1, n2, abs(2.0 * pa - 1.0)) * 1.9921875 - 1.0;
  let nzc = sqrt(1.0 - min(dot(n, n), 1.0));
  var Nw = in.t * n.x + cross(in.n, in.t) * n.y + in.n * nzc;
  var rainLight = vec3f(0.0);
  if (RAIN) {
    let cellId = floor(in.rain / u.R[1].w);
    let nz = textureSampleLevel(rainNoise, rainSamp, cellId * u.R[0].xy, 0.0);
    let off = in.rain - (cellId + 0.5) * u.R[1].w;
    let dur = mix(u.R[2].x, u.R[2].y, nz.x);
    let pause = mix(u.R[2].z, u.R[2].w, nz.y);
    let cycle = dur + pause;
    var tt = u.S[0].x - dur * nz.w;
    tt = tt - cycle * floor(tt / cycle);
    let life = clamp(tt / dur, 0.0, 1.0);
    let rad = mix(u.R[1].x, u.R[1].y, nz.z) * 0.5;
    let dist = length(off);
    let q = (dist / rad - life) * (2.0 / u.R[1].z);
    let prof = max(1.0 - q * q, 0.0);
    let amp = (1.0 - life) * prof;
    let kk = -min(-2.0 * q, amp);
    let g = amp * kk / dist;
    Nw = Nw + u.R[0].w * vec3f(off.x * g, amp, off.y * g);
    rainLight = (amp * u.R[0].z) * u.S[2].xyz;
  }
  let N = normalize(Nw);
  let V = normalize(in.vpos);
  var R = V - 2.0 * dot(V, N) * N;
  let Ng = normalize(in.n);

  // The depth behind the surface, linear like Remastered's linearSceneDepth.
  let size = vec2i(textureDimensions(sceneColor));
  let px = clamp(vec2i(in.pos.xy), vec2i(0), size - 1);
  var behind = 1e30;
  if (u.misc.y > 0.5) {
    var z = textureLoad(sceneDepth, px, 0).r;
    if (u.misc.x > 0.5) { z = 1.0 - z; }
    let dz = clamp((z - u.depth.z) / (u.depth.w - u.depth.z), 0.0, 1.0);
    behind = u.depth.x * u.depth.y / (u.depth.y - dz * (u.depth.y - u.depth.x));
  }
  let d = behind - in.clip.w;

  var o0: vec3f;
  var o1: vec3f;
  if (BOTTOM) {
    let nvB = dot(V, N);
    R = R - clamp(dot(R, Ng), 0.0, 1.0) * Ng;
    let lod = u.cubeP.y * (1.0 - u.S[0].y);
    var env = env_at(R, lod) * u.S[3].y;
    var fr = pow(1.0 - min(abs(nvB), 1.0), u.S[3].x);
    let lut = brdf(clamp(nvB, 0.0, 1.0), max(1.0 - u.S[0].y, 0.02));
    env = env * (lut.x * 0.05 + lut.y);
    fr = fr * clamp(in.clip.w * u.S[7].w, 0.0, 1.0);
    fr = max(fr, u.S[6].w);
    let lit = fr * in.light.rgb;
    var col = env + lit * u.S[2].xyz * 0.3183098733 + lit * rainLight;
    col = col * in.fog.w + in.fog.xyz;
    col = col - (1.0 - fr) * in.fog.xyz;
    let fade = clamp(d * u.S[0].z, 0.0, 1.0);
    o0 = col * fade;
    o1 = vec3f(fade * fr);
  } else {
    let nv = dot(-V, N);
    R = R + clamp(dot(R, -Ng), 0.0, 1.0) * Ng;
    let lod = u.cubeP.y * (1.0 - u.S[1].w);
    var env = env_at(R, lod) * (in.light.w * u.S[3].y);
    let fr = pow(1.0 - min(abs(nv), 1.0), u.S[3].x);
    let lut = brdf(clamp(nv, 0.0, 1.0), max(1.0 - u.S[1].w, 0.02));
    env = env * (lut.x * 0.05 + lut.y);
    let F = mix(u.S[2].w, 1.0, fr);
    let lit = F * in.light.rgb;
    var col = env + lit * u.S[2].xyz * 0.3183098733;
    col = col + lit * rainLight;
    col = col * in.fog.w + in.fog.xyz;
    let tintFade = clamp(d * u.S[7].w, 0.0, 1.0);
    let fade = clamp(d * u.S[0].z, 0.0, 1.0);
    let tint = 1.0 + tintFade * (u.S[7].xyz - 1.0);
    let omF = 1.0 - F;
    o1 = fade * (1.0 - omF * tint);
    col = col - omF * tint * in.fog.xyz;
    o0 = fade * col;
  }

  // Remastered's blend, dst = o0 + dst * (1 - o1), in the light the EFB holds tone mapped.
  let snap = textureLoad(sceneColor, px, 0);
  let y = srgb_dec(snap.rgb);
  let dst = min(vec3f(untone(y.r), untone(y.g), untone(y.b)), vec3f(4.0));
  let x = max(o0 + dst * (1.0 - o1), vec3f(0.0));
  let drawn = vec3f(tone(x.r), tone(x.g), tone(x.b));
  return vec4f(srgb_enc(drawn), snap.a);
}
)";

// Pool threads of the pipeline cache create pipelines too.
std::mutex g_staticMutex;

void ensure_static(const wgpu::Device& device) {
  std::scoped_lock lock{g_staticMutex};
  if (g_state.pipelineLayout) {
    return;
  }
  using SS = wgpu::ShaderStage;
  const auto tex = [](uint32_t binding, SS vis, wgpu::TextureViewDimension dim,
                      wgpu::TextureSampleType type = wgpu::TextureSampleType::Float) {
    return wgpu::BindGroupLayoutEntry{
        .binding = binding, .visibility = vis, .texture = {.sampleType = type, .viewDimension = dim}};
  };
  const auto samp = [](uint32_t binding, SS vis) {
    return wgpu::BindGroupLayoutEntry{
        .binding = binding, .visibility = vis, .sampler = {.type = wgpu::SamplerBindingType::Filtering}};
  };
  const SS both = SS::Vertex | SS::Fragment;
  const wgpu::BindGroupLayoutEntry entries[BCount] = {
      {.binding = BUniform,
       .visibility = both,
       .buffer = {.type = wgpu::BufferBindingType::Uniform, .hasDynamicOffset = true, .minBindingSize = UniformSize}},
      tex(BNormal, SS::Fragment, wgpu::TextureViewDimension::e2D),
      samp(BNormalSamp, SS::Fragment),
      tex(BSourceFlow, both, wgpu::TextureViewDimension::e2D),
      samp(BFlowSamp, both),
      tex(BFlow, SS::Vertex, wgpu::TextureViewDimension::e2D),
      tex(BRainNoise, SS::Fragment, wgpu::TextureViewDimension::e2D),
      samp(BRainSamp, SS::Fragment),
      tex(BCube, SS::Fragment, wgpu::TextureViewDimension::Cube),
      samp(BCubeSamp, both),
      tex(BBrdf, SS::Fragment, wgpu::TextureViewDimension::e2D),
      tex(BVolume, SS::Vertex, wgpu::TextureViewDimension::e3D),
      tex(BFroxels, SS::Vertex, wgpu::TextureViewDimension::e3D),
      samp(BFogSamp, SS::Vertex),
      tex(BSceneColor, SS::Fragment, wgpu::TextureViewDimension::e2D, wgpu::TextureSampleType::UnfilterableFloat),
      tex(BSceneDepth, SS::Fragment, wgpu::TextureViewDimension::e2D, wgpu::TextureSampleType::UnfilterableFloat),
  };
  const wgpu::BindGroupLayoutDescriptor layoutDescriptor{
      .label = "Water Bind Group Layout",
      .entryCount = BCount,
      .entries = entries,
  };
  g_state.bindLayout = device.CreateBindGroupLayout(&layoutDescriptor);
  const wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor{
      .label = "Water Pipeline Layout",
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
      .flags = p.flags,
      .cull = p.cull,
      .compare = p.compare,
  };
  for (uint32_t i = 0; i < layout.colorAttachmentCount; ++i) {
    config.colorFormats[i] = layout.colorAttachments[i].format;
  }
  return config;
}

wgpu::RenderPipeline make_pipeline(const PipelineConfig& p) {
  const wgpu::Device& device = webgpu::g_device;
  ensure_static(device);
  std::string source = std::string("const BOTTOM: bool = ") + ((p.flags & 1) != 0 ? "true" : "false") +
                       ";\nconst RAIN: bool = " + ((p.flags & 2) != 0 ? "true" : "false") + ";\n";
  source += ShaderSource;
  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code = source.c_str();
  const wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &wgsl, .label = "Water Module"};
  const wgpu::ShaderModule module = device.CreateShaderModule(&moduleDescriptor);

  static constexpr std::array<wgpu::VertexAttribute, 3> attributes{{
      {.format = wgpu::VertexFormat::Float32x3, .offset = offsetof(Vertex, pos), .shaderLocation = 0},
      {.format = wgpu::VertexFormat::Float32x2, .offset = offsetof(Vertex, uv), .shaderLocation = 1},
      {.format = wgpu::VertexFormat::Unorm8x4, .offset = offsetof(Vertex, color), .shaderLocation = 2},
  }};
  const wgpu::VertexBufferLayout vertexLayout{
      .stepMode = wgpu::VertexStepMode::Vertex,
      .arrayStride = sizeof(Vertex),
      .attributeCount = attributes.size(),
      .attributes = attributes.data(),
  };
  std::array<wgpu::ColorTargetState, MaxColorAttachments> targets{};
  for (uint32_t i = 0; i < p.colorAttachmentCount; ++i) {
    const bool scene = i == SceneColorAttachmentIndex;
    targets[i] = {
        .format = p.colorFormats[i],
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
      .depthWriteEnabled = false,
      .depthCompare = static_cast<wgpu::CompareFunction>(p.compare),
  };
  const wgpu::RenderPipelineDescriptor descriptor{
      .label = "Water Pipeline",
      .layout = g_state.pipelineLayout,
      .vertex = {.module = module, .entryPoint = "vs_main", .bufferCount = 1, .buffers = &vertexLayout},
      .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList,
                    .frontFace = wgpu::FrontFace::CCW,
                    .cullMode = static_cast<wgpu::CullMode>(p.cull)},
      .depthStencil = p.depthStencilFormat != wgpu::TextureFormat::Undefined ? &depth : nullptr,
      .multisample = {.count = p.msaaSamples, .mask = UINT32_MAX},
      .fragment = &fragment,
  };
  return device.CreateRenderPipeline(&descriptor);
}

// 1x1 stand-ins: black (no flow simulation, and the scene targets when missing), a flat normal
// (128/255 * 1.9921875 - 1 = 0) and a flow map with no flow (xy 0.5, z 0, w 1).
void ensure_dummies(const DrawContext& ctx) {
  if (g_state.dummyView[0]) {
    return;
  }
  const uint16_t half0 = 0, halfHalf = 0x3800, half1 = 0x3C00;
  struct Spec {
    wgpu::TextureFormat format;
    uint8_t texel[8];
    uint32_t size;
  };
  Spec specs[DummyCount] = {
      {wgpu::TextureFormat::RGBA8Unorm, {0, 0, 0, 0}, 4},
      {wgpu::TextureFormat::RGBA8Unorm, {128, 128, 255, 255}, 4},
      {wgpu::TextureFormat::RGBA16Float, {}, 8},
  };
  const uint16_t flow[4] = {halfHalf, halfHalf, half0, half1};
  std::memcpy(specs[DummyFlow].texel, flow, sizeof(flow));
  for (uint32_t i = 0; i < DummyCount; ++i) {
    const wgpu::TextureDescriptor descriptor{
        .label = "Water Dummy",
        .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
        .size = {1, 1, 1},
        .format = specs[i].format,
    };
    g_state.dummyTexture[i] = ctx.device.CreateTexture(&descriptor);
    const wgpu::TexelCopyTextureInfo dst{.texture = g_state.dummyTexture[i]};
    const wgpu::TexelCopyBufferLayout layout{.bytesPerRow = specs[i].size, .rowsPerImage = 1};
    const wgpu::Extent3D size{1, 1, 1};
    ctx.queue.WriteTexture(&dst, specs[i].texel, specs[i].size, &layout, &size);
    g_state.dummyView[i] = g_state.dummyTexture[i].CreateView();
  }
}

// Render thread: the view registered as `id`, `fallback` for 0, or none if it is gone.
wgpu::TextureView entry_view(uint32_t id, Dummy fallback) {
  if (id == 0) {
    return g_state.dummyView[fallback];
  }
  const std::lock_guard lock{g_state.mutex};
  const auto it = g_state.byId.find(id);
  return it == g_state.byId.end() ? wgpu::TextureView{} : it->second.view;
}

void draw_payload(const DrawContext& ctx, const wgpu::RenderPassEncoder& pass, const void* data, size_t size,
                  void*) {
  if (size != sizeof(Payload)) {
    return;
  }
  Payload p;
  std::memcpy(&p, data, sizeof(p));
  ensure_static(ctx.device);
  ensure_dummies(ctx);
  std::array<wgpu::TextureView, BCount> views{};
  views[BNormal] = entry_view(p.normalId, DummyFlat);
  views[BSourceFlow] = entry_view(p.sourceFlowId, DummyFlow);
  views[BFlow] = g_state.dummyView[DummyBlack];
  views[BRainNoise] = entry_view(p.rainId, DummyBlack);
  views[BCube] = probe::cube_view(p.cube);
  views[BBrdf] = probe::brdf_lut_view();
  views[BVolume] = probe::volume_view(p.volume, 0);
  views[BFroxels] = volfog::froxel_view();
  views[BSceneColor] = entry_view(p.colorId, DummyBlack);
  views[BSceneDepth] = entry_view(p.depthId, DummyBlack);
  for (const Binding b : {BNormal, BSourceFlow, BRainNoise, BCube, BBrdf, BVolume, BFroxels, BSceneColor,
                          BSceneDepth}) {
    if (!views[b]) {
      return;
    }
  }
  std::array<wgpu::Sampler, BCount> samplers{};
  samplers[BNormalSamp] = sampler_ref(wgpu::SamplerDescriptor{
      .addressModeU = wgpu::AddressMode::Repeat,
      .addressModeV = wgpu::AddressMode::Repeat,
      .magFilter = wgpu::FilterMode::Linear,
      .minFilter = wgpu::FilterMode::Linear,
      .mipmapFilter = wgpu::MipmapFilterMode::Linear,
      .maxAnisotropy = 1,
  });
  samplers[BFlowSamp] = samplers[BNormalSamp];
  samplers[BRainSamp] = sampler_ref(wgpu::SamplerDescriptor{
      .addressModeU = wgpu::AddressMode::Repeat,
      .addressModeV = wgpu::AddressMode::Repeat,
      .magFilter = wgpu::FilterMode::Nearest,
      .minFilter = wgpu::FilterMode::Nearest,
      .mipmapFilter = wgpu::MipmapFilterMode::Nearest,
      .maxAnisotropy = 1,
  });
  samplers[BCubeSamp] = probe::sampler();
  samplers[BFogSamp] = volfog::sampler();

  const std::array<uint64_t, 5> pipelineKey{p.flags, p.cull, p.compare, ctx.layout.key, ctx.layout.sampleCount};
  auto pipeline = g_state.pipelines.find(pipelineKey);
  if (pipeline == g_state.pipelines.end()) {
    const PipelineConfig config = make_config(ctx.layout, p);
    auto created = require_pipeline(ShaderType::Water, config, [config] { return make_pipeline(config); });
    if (!created) {
      return;
    }
    pipeline = g_state.pipelines.emplace(pipelineKey, std::move(created)).first;
  }
  std::array<const void*, BCount> groupKey{};
  for (uint32_t i = 0; i < BCount; ++i) {
    groupKey[i] = views[i] ? static_cast<const void*>(views[i].Get()) : static_cast<const void*>(samplers[i].Get());
  }
  groupKey[BUniform] = ctx.uniformBuffer.Get();
  auto group = g_state.groups.find(groupKey);
  if (group == g_state.groups.end()) {
    if (g_state.groups.size() > 256) {
      g_state.groups.clear();
    }
    std::array<wgpu::BindGroupEntry, BCount> entries{};
    for (uint32_t i = 0; i < BCount; ++i) {
      entries[i].binding = i;
      if (i == BUniform) {
        entries[i].buffer = ctx.uniformBuffer;
        entries[i].size = UniformSize;
      } else if (views[i]) {
        entries[i].textureView = views[i];
      } else {
        entries[i].sampler = samplers[i];
      }
    }
    const wgpu::BindGroupDescriptor descriptor{
        .label = "Water Bind Group",
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
}

bool ensure_registered() {
  if (g_state.drawType == InvalidDrawType) {
    g_state.drawType = register_draw_type(DrawTypeDescriptor{.label = "Water", .draw = draw_payload});
  }
  return g_state.drawType != InvalidDrawType;
}

// Game thread: an id for `view` (made from `keep`, if a GX texture), stamped as used now.
uint32_t register_view(const void* key, const wgpu::TextureView& view, TextureHandle keep) {
  const std::lock_guard lock{g_state.mutex};
  auto& id = g_state.byKey[key];
  if (id == 0) {
    id = g_state.nextId++;
    g_state.byId[id] = Entry{view, std::move(keep), {}};
  }
  g_state.byId[id].lastUse = std::chrono::steady_clock::now();
  return id;
}

// Game thread: the id of a GX texture's view; 0 with ok = false if it isn't loaded.
uint32_t register_texture(const GXTexObj* obj, bool& ok) {
  if (obj == nullptr) {
    return 0;
  }
  auto handle = gx::texture::resolve_static_texture(*reinterpret_cast<const GXTexObj_*>(obj));
  if (!handle) {
    ok = false;
    return 0;
  }
  const void* key = handle.get();
  const wgpu::TextureView view = handle->sampleTextureView;
  return register_view(key, view, std::move(handle));
}

// Game thread: drops what no draw has used for EvictAfter. A draw queued this frame stamped
// its entries when it was, so they stay; one whose entry is gone anyway is skipped.
void evict_idle() {
  const auto now = std::chrono::steady_clock::now();
  const std::lock_guard lock{g_state.mutex};
  for (auto it = g_state.byKey.begin(); it != g_state.byKey.end();) {
    const auto entry = g_state.byId.find(it->second);
    if (entry == g_state.byId.end() || now - entry->second.lastUse > EvictAfter) {
      if (entry != g_state.byId.end()) {
        g_state.byId.erase(entry);
      }
      it = g_state.byKey.erase(it);
    } else {
      ++it;
    }
  }
}

// The inverse transpose of a row-major 3x4's 3x3, as a 3x4 with no translation.
void normal_matrix(const float m[12], float out[12]) {
  const float a = m[0], b = m[1], c = m[2];
  const float d = m[4], e = m[5], f = m[6];
  const float g = m[8], h = m[9], i = m[10];
  const float A = e * i - f * h, B = f * g - d * i, C = d * h - e * g;
  const float det = a * A + b * B + c * C;
  const float s = std::fabs(det) > 1e-20f ? 1.f / det : 0.f;
  // inverse transpose = cofactor matrix / det
  const float r[12] = {A * s, B * s, C * s, 0.f,
                       (c * h - b * i) * s, (a * i - c * g) * s, (b * g - a * h) * s, 0.f,
                       (b * f - c * e) * s, (c * d - a * f) * s, (a * e - b * d) * s, 0.f};
  std::memcpy(out, r, sizeof(r));
}
} // namespace

wgpu::RenderPipeline create_pipeline(const PipelineConfig& config) { return make_pipeline(config); }

void draw(const DrawDesc& desc, const Vertex* verts, uint32_t vertexCount, const uint32_t* indices,
          uint32_t indexCount) {
  if (verts == nullptr || indices == nullptr || vertexCount == 0 || indexCount < 3 || !ensure_registered()) {
    return;
  }
  evict_idle();
  // Before the textures resolve: the texture cache and the recorder belong to the FIFO thread (see
  // vfx.cpp draw_prims). The GX state the draw sees, and the scene it snapshots, are also those
  // after everything recorded so far.
  gx::fifo::drain();
  bool ok = true;
  const uint32_t normalId = register_texture(desc.normalMap, ok);
  const uint32_t sourceFlowId = register_texture(desc.sourceFlow, ok);
  const bool rain = desc.rain && desc.rainNoise != nullptr;
  const uint32_t rainId = rain ? register_texture(desc.rainNoise, ok) : 0;
  if (!ok) {
    return;
  }
  // The scene so far, to blend against and to read the depth behind the surface from.
  ResolvedTargets targets;
  if (!resolve_pass(ResolveDesc{.color = true, .depth = true}, targets) || !targets.color) {
    return;
  }
  const uint32_t colorId = register_view(targets.color.Get(), targets.color, {});
  const uint32_t depthId = targets.depth ? register_view(targets.depth.Get(), targets.depth, {}) : 0;

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
  static_assert(sizeof(gx.pnMtx[0].pos) == sizeof(u.mv));
  std::memcpy(u.mv, &gx.pnMtx[gx.currentPnMtx].pos, sizeof(u.mv));
  normal_matrix(u.mv, u.nrm);
  std::memcpy(u.S, desc.surface, sizeof(u.S));
  std::memcpy(u.R, desc.rainParams, sizeof(u.R));
  std::memcpy(u.W, desc.waves, sizeof(u.W));
  std::memcpy(u.cubeRows, desc.viewToCube, sizeof(u.cubeRows));
  std::memcpy(u.volRows, desc.viewToVolume, sizeof(u.volRows));
  std::memcpy(u.volScale, desc.volumeScale, sizeof(u.volScale));
  std::memcpy(u.fallbackMean, desc.fallbackMean, sizeof(u.fallbackMean));
  if (gx.volFog) {
    // The froxels the fog pass filled this frame (GX_AURORA_PORT_VOLUMETRIC_FOG).
    u.fog[0] = gx.volFogParams.x();
    u.fog[1] = gx.volFogParams.y();
    u.fog[2] = gx.volFogParams.z();
    u.fog[3] = 1.f;
  }
  std::memcpy(u.tone, desc.tone, sizeof(u.tone));
  u.depth[0] = desc.zNear;
  u.depth[1] = desc.zFar;
  u.depth[2] = desc.zMin;
  u.depth[3] = desc.zMax;
  u.misc[0] = gx::UseReversedZ ? 1.f : 0.f;
  u.misc[1] = depthId != 0 ? 1.f : 0.f;
  u.misc[2] = desc.volume != 0 ? 1.f : 0.f;
  u.misc[3] = desc.cube != 0 ? 1.f : 0.f;
  u.cube[0] = desc.cubeScale;
  u.cube[1] = desc.cubeMips;

  Payload p{};
  p.flags = (desc.bottom ? 1u : 0u) | (rain ? 2u : 0u);
  p.cull = uint32_t(desc.cull == Cull::Front  ? wgpu::CullMode::Front
                    : desc.cull == Cull::Back ? wgpu::CullMode::Back
                                              : wgpu::CullMode::None);
  p.compare = uint32_t(gx::UseReversedZ ? wgpu::CompareFunction::GreaterEqual : wgpu::CompareFunction::LessEqual);
  p.indexCount = indexCount;
  p.verts = push_verts(reinterpret_cast<const uint8_t*>(verts), size_t(vertexCount) * sizeof(Vertex), 4);
  p.indices = push_indices(reinterpret_cast<const uint8_t*>(indices), size_t(indexCount) * sizeof(uint32_t), 4);
  p.uniform = push_uniform(reinterpret_cast<const uint8_t*>(&u), sizeof(u));
  if (overflowed(p.verts) || overflowed(p.indices) || overflowed(p.uniform) || p.verts.size == 0 ||
      p.indices.size == 0 || p.uniform.size == 0) {
    return;
  }
  p.normalId = normalId;
  p.sourceFlowId = sourceFlowId;
  p.rainId = rainId;
  p.colorId = colorId;
  p.depthId = depthId;
  p.cube = desc.cube;
  p.volume = desc.volume;
  push_custom_draw(g_state.drawType, &p, sizeof(p));
}

void shutdown() {
  g_state.pipelines.clear();
  g_state.groups.clear();
  for (uint32_t i = 0; i < DummyCount; ++i) {
    g_state.dummyView[i] = {};
    g_state.dummyTexture[i] = {};
  }
  g_state.bindLayout = {};
  g_state.pipelineLayout = {};
  {
    const std::lock_guard lock{g_state.mutex};
    g_state.byKey.clear();
    g_state.byId.clear();
  }
  const auto draw = g_state.drawType;
  g_state.drawType = InvalidDrawType;
  if (draw != InvalidDrawType) {
    unregister_draw_type(draw);
  }
}
} // namespace aurora::gfx::water
