#include "shadow.hpp"

#include "../gx/pipeline.hpp"
#include "../logging.hpp"
#include "../webgpu/gpu.hpp"
#include "../webgpu/gpu_prof.hpp"
#include "encoding.hpp"
#include "pipeline_cache.hpp"
#include "resource_cache.hpp"
#include "recording.hpp"
#include "resources.hpp"

#include <aurora/gfx.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>
#include "texture_sites.hpp"

// The sun's cascaded shadow map. set_frame fits Remastered's four cascades (shadow_cascade.cpp) to
// the camera, each snapped to whole texels so it does not swim as the camera moves. The casters (the
// frame's opaque world draws, re-drawn depth only with a vs_shadow pipeline) are drawn into their
// relevant cascades once the frame's scene is, and the next frame's receivers use the matrices of
// the frame they were drawn in, so a receiver lags the camera by one frame but never reads a map
// half drawn.
namespace aurora::gfx::shadow {
namespace {
Module Log("aurora::gfx::shadow");
using webgpu::g_device;

struct Matrix {
  float m[4][4];
};

struct State {
  EncoderTaskId task = InvalidEncoderTask;
  uint32_t size = 0;
  wgpu::Texture map;
  wgpu::TextureView mapView;                              // the array, for the receivers
  std::array<wgpu::TextureView, cascade::Count> layerViews; // a cascade each, for the casters
  wgpu::Sampler sampler;
  // The cascades' light clip spaces (world -> map) and far ends, of the frame being drawn and of
  // the map drawn last.
  std::array<Matrix, cascade::Count> pendingClip{};
  std::array<float, cascade::Count> pendingSplits{};
  bool pendingActive = false;
  std::array<Matrix, cascade::Count> mapClip{};
  std::array<float, cascade::Count> mapSplits{};
  bool mapValid = false;
};

State g_state;
std::vector<gx::DrawData> g_casters;
std::array<std::vector<gx::DrawData>, 8> g_recorded;
uint32_t g_nextSlot = 0;
// Read by the console from the main thread.
std::atomic<uint32_t> g_lastCasterCount{0};
std::array<std::atomic<uint32_t>, cascade::Count> g_lastLayerCounts{};
std::atomic<uint32_t> g_lastRejected{0};

uint32_t map_size() {
  static const uint32_t size = [] {
    const char* env = std::getenv("MP_SHADOW_SIZE");
    const long value = env != nullptr ? std::strtol(env, nullptr, 10) : 0;
    return value >= 256 && value <= 8192 ? static_cast<uint32_t>(value) : cascade::DefaultSize;
  }();
  return size;
}

void ensure_map() {
  if (g_state.map) {
    return;
  }
  g_state.size = map_size();
  const wgpu::TextureDescriptor descriptor{
      .label = "Shadow Map",
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
      .dimension = wgpu::TextureDimension::e2D,
      .size = {g_state.size, g_state.size, cascade::Count},
      .format = MapFormat,
  };
  AURORA_NOTE_TEXTURE(&descriptor);
  g_state.map = g_device.CreateTexture(&descriptor);
  const wgpu::TextureViewDescriptor arrayView{
      .label = "Shadow Map",
      .dimension = wgpu::TextureViewDimension::e2DArray,
      .arrayLayerCount = cascade::Count,
  };
  g_state.mapView = g_state.map.CreateView(&arrayView);
  for (uint32_t c = 0; c < cascade::Count; ++c) {
    const wgpu::TextureViewDescriptor layerView{
        .label = "Shadow Cascade",
        .dimension = wgpu::TextureViewDimension::e2D,
        .baseArrayLayer = c,
        .arrayLayerCount = 1,
    };
    g_state.layerViews[c] = g_state.map.CreateView(&layerView);
  }
}

Matrix multiply(const Matrix& a, const Matrix& b) {
  Matrix out{};
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      float sum = 0.f;
      for (int k = 0; k < 4; ++k) {
        sum += a.m[r][k] * b.m[k][c];
      }
      out.m[r][c] = sum;
    }
  }
  return out;
}

// view -> world: the rotation's transpose, and -R^T t.
void view_to_world(const float worldToView[3][4], float out[3][4]) {
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      out[r][c] = worldToView[c][r];
    }
    out[r][3] = -(worldToView[0][r] * worldToView[0][3] + worldToView[1][r] * worldToView[1][3] +
                  worldToView[2][r] * worldToView[2][3]);
  }
}

bool fit_frame(const float worldToView[3][4], const cascade::Frustum& frustum, const float sunDir[3], float distance,
               uint32_t size, cascade::Fit out[cascade::Count], float split[cascade::Count]) {
  cascade::Basis basis;
  if (!(distance > 0.f) || !(frustum.znear > 0.f) || !(frustum.zfar > frustum.znear) ||
      !cascade::light_basis(sunDir, basis)) {
    return false;
  }
  float viewToWorld[3][4];
  view_to_world(worldToView, viewToWorld);
  cascade::splits(frustum.znear, frustum.zfar, distance, split);
  cascade::fit(basis, viewToWorld, frustum, split, size, out);
  return true;
}

void encode(const EncoderTaskContext&, const wgpu::CommandEncoder& cmd, const void* payload, size_t payloadSize,
            void*) {
  uint32_t slot = 0;
  if (payloadSize != sizeof(slot) || !g_state.mapView) {
    return;
  }
  std::memcpy(&slot, payload, sizeof(slot));
  const auto& casters = g_recorded[slot % g_recorded.size()];
  const auto& resources = detail::resources();
  // Membership was computed against this frame's fits before recording. Unknown/deforming
  // geometry retains every bit. vs_shadow selects its cascade using the immediates' serial.
  uint32_t rejected = 0;
  for (uint32_t c = 0; c < cascade::Count; ++c) {
    const wgpu::RenderPassDepthStencilAttachment depth{
        .view = g_state.layerViews[c],
        .depthLoadOp = wgpu::LoadOp::Clear,
        .depthStoreOp = wgpu::StoreOp::Store,
        .depthClearValue = 1.f,
    };
    const wgpu::RenderPassDescriptor passDescriptor{
        .label = "Shadow Map",
        .depthStencilAttachment = &depth,
        .timestampWrites = webgpu::gpu_prof::pass_writes("Shadow map"),
    };
    const auto pass = cmd.BeginRenderPass(&passDescriptor);
    pass.SetBindGroup(0, resources.staticBindGroup);
    uint32_t submitted = 0;
    for (const auto& draw : casters) {
      if ((draw.shadowCascadeMask & (1u << c)) == 0) {
        ++rejected;
        continue;
      }
      wgpu::RenderPipeline pipeline;
      if (!get_pipeline(draw.pipeline, pipeline)) {
        continue;
      }
      auto immediates = draw.immediateData;
      immediates.serial = c;
      pass.SetPipeline(pipeline);
      if (draw.bindGroups.textureBindGroup) {
        // A cutout's pipeline alpha-tests with the draw's own textures.
        pass.SetBindGroup(2, find_bind_group(draw.bindGroups.textureBindGroup));
      }
      pass.SetImmediates(0, &immediates, sizeof(immediates));
      pass.SetBindGroup(1, resources.uniformBindGroup, 1, &draw.uniformRange.offset);
      pass.SetIndexBuffer(resources.indexBuffer, wgpu::IndexFormat::Uint16, draw.idxRange.offset, draw.idxRange.size);
      if (draw.indexCount == 0) {
        pass.Draw(draw.vtxCount, draw.instanceCount);
      } else {
        pass.DrawIndexed(draw.indexCount, draw.instanceCount);
      }
      ++submitted;
    }
    pass.End();
    g_lastLayerCounts[c].store(submitted, std::memory_order_relaxed);
  }
  g_lastRejected.store(rejected, std::memory_order_relaxed);
  forget_texture_group();
}
} // namespace

bool ensure_task() {
  if (g_state.task == InvalidEncoderTask) {
    g_state.task = register_encoder_task_type(EncoderTaskDescriptor{.label = "Shadow Map", .callback = encode});
    if (g_state.task == InvalidEncoderTask) {
      Log.warn("could not register the shadow map task");
      return false;
    }
  }
  return true;
}

bool set_frame(const float worldToView[3][4], const cascade::Frustum& frustum, const float sunDir[3], float distance,
               const float color[3], Uniform& out) {
  // A frame's casters are the draws since its set_frame; a frame that drew no map drops them.
  g_casters.clear();
  cascade::Fit fits[cascade::Count];
  float split[cascade::Count];
  if (!fit_frame(worldToView, frustum, sunDir, distance, map_size(), fits, split)) {
    g_state.pendingActive = false;
    g_state.mapValid = false;
    return false;
  }
  ensure_map();

  Matrix viewToWorld{};
  view_to_world(worldToView, viewToWorld.m);
  viewToWorld.m[3][3] = 1.f;
  for (int c = 0; c < cascade::Count; ++c) {
    Matrix clip;
    std::memcpy(clip.m, fits[c].clip, sizeof(clip.m));
    g_state.pendingClip[c] = clip;
    g_state.pendingSplits[c] = split[c];
    const Matrix caster = multiply(clip, viewToWorld);
    // Before the first map the receivers' fade (below) lights everything, whatever this says.
    const Matrix receiver = multiply(g_state.mapValid ? g_state.mapClip[c] : clip, viewToWorld);
    std::memcpy(out.caster[c], caster.m, sizeof(out.caster[c]));
    std::memcpy(out.receiver[c], receiver.m, sizeof(out.receiver[c]));
  }
  g_state.pendingActive = true;

  // The direction to the sun in view space.
  const float len = std::sqrt(sunDir[0] * sunDir[0] + sunDir[1] * sunDir[1] + sunDir[2] * sunDir[2]);
  for (int r = 0; r < 3; ++r) {
    out.dir[r] = -(worldToView[r][0] * sunDir[0] + worldToView[r][1] * sunDir[1] + worldToView[r][2] * sunDir[2]) / len;
  }
  out.dir[3] = 0.f;
  out.color[0] = std::max(color[0], 0.f);
  out.color[1] = std::max(color[1], 0.f);
  out.color[2] = std::max(color[2], 0.f);
  out.color[3] = 1.f / static_cast<float>(g_state.size);
  if (g_state.mapValid) {
    std::copy(g_state.mapSplits.begin(), g_state.mapSplits.end(), out.splits);
    // The last cascade fades out over its last fifth (c5[25] = (0.8, 1) * its far end).
    out.fade[0] = 0.8f * g_state.mapSplits[cascade::Count - 1];
    out.fade[1] = g_state.mapSplits[cascade::Count - 1];
  } else {
    std::copy_n(split, cascade::Count, out.splits);
    out.fade[0] = -2.f;
    out.fade[1] = -1.f;
  }
  out.fade[2] = out.fade[3] = 0.f;
  return true;
}

bool fit_frame(const float worldToView[3][4], const cascade::Frustum& frustum, const float sunDir[3], float distance,
               cascade::Fit out[cascade::Count]) {
  float split[cascade::Count];
  return fit_frame(worldToView, frustum, sunDir, distance, map_size(), out, split);
}

bool box_casts(const float worldToView[3][4], const cascade::Frustum& frustum, const float sunDir[3], float distance,
               const float min[3], const float max[3]) {
  cascade::Fit fits[cascade::Count];
  cascade::Basis basis;
  if (!fit_frame(worldToView, frustum, sunDir, distance, fits) || !cascade::light_basis(sunDir, basis)) {
    return false;
  }
  return cascade::box_mask(basis, fits, min, max) != 0;
}

void add_caster(const gx::DrawData& draw) {
  if (g_state.pendingActive) {
    g_casters.push_back(draw);
  }
}

uint32_t last_caster_count() { return g_lastCasterCount.load(std::memory_order_relaxed); }

void last_layer_counts(uint32_t submitted[4], uint32_t& rejected) {
  for (int c = 0; c < cascade::Count; ++c) {
    submitted[c] = g_lastLayerCounts[c].load(std::memory_order_relaxed);
  }
  rejected = g_lastRejected.load(std::memory_order_relaxed);
}

bool record() {
  if (g_state.task == InvalidEncoderTask || !g_state.pendingActive || !g_state.mapView) {
    g_casters.clear();
    return false;
  }
  const uint32_t slot = g_nextSlot % g_recorded.size();
  g_recorded[slot].swap(g_casters);
  g_casters.clear();
  g_state.pendingActive = false;
  // Outside a pass the task isn't recorded: the old map stays, so keep its clip with it.
  if (!record_encoder_task(g_state.task, &slot, sizeof(slot))) {
    g_recorded[slot].clear();
    return false;
  }
  ++g_nextSlot;
  g_lastCasterCount.store(static_cast<uint32_t>(g_recorded[slot].size()), std::memory_order_relaxed);
  g_state.mapClip = g_state.pendingClip;
  g_state.mapSplits = g_state.pendingSplits;
  g_state.mapValid = true;
  return true;
}

const wgpu::TextureView& map_view() {
  ensure_map();
  return g_state.mapView;
}

const wgpu::Sampler& sampler() {
  if (!g_state.sampler) {
    const wgpu::SamplerDescriptor descriptor{
        .label = "Shadow Map Sampler",
        .addressModeU = wgpu::AddressMode::ClampToEdge,
        .addressModeV = wgpu::AddressMode::ClampToEdge,
        .addressModeW = wgpu::AddressMode::ClampToEdge,
        .magFilter = wgpu::FilterMode::Linear,
        .minFilter = wgpu::FilterMode::Linear,
        .compare = wgpu::CompareFunction::Less,
    };
    g_state.sampler = g_device.CreateSampler(&descriptor);
  }
  return g_state.sampler;
}

void shutdown() {
  const auto task = g_state.task;
  g_state = {};
  g_casters = {};
  g_recorded = {};
  g_lastCasterCount.store(0, std::memory_order_relaxed);
  for (auto& count : g_lastLayerCounts) {
    count.store(0, std::memory_order_relaxed);
  }
  g_lastRejected.store(0, std::memory_order_relaxed);
  if (task != InvalidEncoderTask) {
    unregister_encoder_task_type(task);
  }
}
} // namespace aurora::gfx::shadow
