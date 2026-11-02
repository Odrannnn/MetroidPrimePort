#include <aurora/vis_query.hpp>

#include "vis_query.hpp"

#include "../gx/fifo.hpp"
#include "../gx/gx.hpp"
#include "../logging.hpp"
#include "../webgpu/gpu.hpp"
#include "recording.hpp"

#include <aurora/gfx.hpp>

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include <absl/container/flat_hash_map.h>

namespace aurora::gfx::vis_query {
namespace {
Module Log("aurora::gfx::vis_query");

constexpr uint32_t MaxQueries = 1024; // per frame
constexpr uint32_t MaxVertices = 36;  // a box
constexpr size_t SlotCount = 3;       // frames whose results may be in flight

struct Payload {
  uint64_t key;
  Range verts;
  uint32_t vertexCount;
  uint32_t depthTest;
};

enum class State : uint8_t { Idle, Recording, Resolved, InFlight, Mapped, Failed };

struct Slot {
  wgpu::QuerySet querySet;
  wgpu::Buffer resolve;
  wgpu::Buffer readback;
  std::atomic<State> state{State::Idle};
  std::vector<uint64_t> keys; // per query index
};

std::atomic<bool> g_active{false}; // set by the first draw
DrawTypeId g_drawType = InvalidDrawType;
std::array<Slot, SlotCount> g_slots;
Slot* g_frameSlot = nullptr; // render thread: this frame's slot
bool g_passOpen = false;     // render thread: the open pass carries g_frameSlot's set
absl::flat_hash_map<std::array<uint64_t, 3>, wgpu::RenderPipeline> g_pipelines;
constexpr uint32_t ResultLife = 8; // mapped frames an untaken result stays (its owner may be gone)
struct Result {
  uint64_t samples;
  uint32_t mapped; // g_mapped when it arrived
};
std::mutex g_resultMutex;
absl::flat_hash_map<uint64_t, Result> g_results;
uint32_t g_mapped = 0; // readbacks mapped so far

wgpu::RenderPipeline make_pipeline(const DrawContext& ctx, bool depthTest) {
  static constexpr char Source[] = R"(
@vertex fn vs_main(@location(0) pos: vec4f) -> @builtin(position) vec4f { return pos; }
@fragment fn fs_color() -> @location(0) vec4f { return vec4f(0.0); }
@fragment fn fs_none() {}
)";
  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code = Source;
  const wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &wgsl, .label = "Vis Query Module"};
  const wgpu::ShaderModule module = ctx.device.CreateShaderModule(&moduleDescriptor);
  static constexpr wgpu::VertexAttribute attribute{
      .format = wgpu::VertexFormat::Float32x4, .offset = 0, .shaderLocation = 0};
  const wgpu::VertexBufferLayout vertexLayout{
      .stepMode = wgpu::VertexStepMode::Vertex,
      .arrayStride = sizeof(float) * 4,
      .attributeCount = 1,
      .attributes = &attribute,
  };
  const auto& layout = ctx.layout;
  std::array<wgpu::ColorTargetState, MaxColorAttachments> targets{};
  for (uint32_t i = 0; i < layout.colorAttachmentCount; ++i) {
    targets[i] = {.format = layout.colorAttachments[i].format, .writeMask = wgpu::ColorWriteMask::None};
  }
  const wgpu::FragmentState fragment{
      .module = module,
      .entryPoint = layout.colorAttachmentCount != 0 ? "fs_color" : "fs_none",
      .targetCount = layout.colorAttachmentCount,
      .targets = targets.data(),
  };
  const wgpu::CompareFunction nearer = gx::UseReversedZ ? wgpu::CompareFunction::Greater : wgpu::CompareFunction::Less;
  const wgpu::DepthStencilState depth{
      .format = layout.depthStencilFormat,
      .depthWriteEnabled = false,
      .depthCompare = depthTest ? nearer : wgpu::CompareFunction::Always,
  };
  const wgpu::RenderPipelineDescriptor descriptor{
      .label = "Vis Query Pipeline",
      .vertex = {.module = module, .entryPoint = "vs_main", .bufferCount = 1, .buffers = &vertexLayout},
      .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
      .depthStencil = &depth,
      .multisample = {.count = layout.sampleCount, .mask = UINT32_MAX},
      .fragment = &fragment,
  };
  return ctx.device.CreateRenderPipeline(&descriptor);
}

// Render thread, replaying the pass the draw was recorded into.
void draw_payload(const DrawContext& ctx, const wgpu::RenderPassEncoder& pass, const void* data, size_t size, void*) {
  if (size != sizeof(Payload) || !g_passOpen || g_frameSlot == nullptr || g_frameSlot->keys.size() >= MaxQueries ||
      ctx.layout.depthStencilFormat == wgpu::TextureFormat::Undefined) {
    return;
  }
  Payload p;
  std::memcpy(&p, data, sizeof(p));
  const std::array<uint64_t, 3> key{ctx.layout.key, ctx.layout.sampleCount, p.depthTest};
  auto pipeline = g_pipelines.find(key);
  if (pipeline == g_pipelines.end()) {
    pipeline = g_pipelines.emplace(key, make_pipeline(ctx, p.depthTest != 0)).first;
  }
  const auto query = static_cast<uint32_t>(g_frameSlot->keys.size());
  g_frameSlot->keys.push_back(p.key);
  pass.BeginOcclusionQuery(query);
  pass.SetPipeline(pipeline->second);
  pass.SetVertexBuffer(0, ctx.vertexBuffer, p.verts.offset, p.verts.size);
  pass.Draw(p.vertexCount);
  pass.EndOcclusionQuery();
}

void ensure_slot(Slot& slot) {
  if (slot.querySet) {
    return;
  }
  const wgpu::QuerySetDescriptor qs{.label = "Vis query occlusion", .type = wgpu::QueryType::Occlusion, .count = MaxQueries};
  slot.querySet = webgpu::g_device.CreateQuerySet(&qs);
  const wgpu::BufferDescriptor resolve{.label = "Vis query resolve",
                                       .usage = wgpu::BufferUsage::QueryResolve | wgpu::BufferUsage::CopySrc,
                                       .size = MaxQueries * sizeof(uint64_t)};
  slot.resolve = webgpu::g_device.CreateBuffer(&resolve);
  const wgpu::BufferDescriptor readback{.label = "Vis query readback",
                                        .usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
                                        .size = MaxQueries * sizeof(uint64_t)};
  slot.readback = webgpu::g_device.CreateBuffer(&readback);
}
} // namespace

void draw(uint64_t key, const float (*verts)[3], uint32_t vertexCount, bool depthTest) {
  if (verts == nullptr || vertexCount == 0 || vertexCount > MaxVertices || vertexCount % 3 != 0) {
    return;
  }
  if (g_drawType == InvalidDrawType) {
    g_drawType = register_draw_type(DrawTypeDescriptor{.label = "Vis Query", .draw = draw_payload});
    if (g_drawType == InvalidDrawType) {
      return;
    }
  }
  g_active.store(true, std::memory_order_release);
  // The matrices queued in the FIFO (SetModelMatrix's load) land in g_gxState only when it drains.
  gx::fifo::drain();
  // The vfx transform: clip = proj x pn x pos, the projection's z taken to the target's range.
  const auto& gx = gx::g_gxState;
  Mat4x4<float> proj = gx.proj;
  if (gx::UseReversedZ) {
    proj.m2 = proj.m2 * Vec4<float>{-1.f, -1.f, -1.f, -1.f};
  } else {
    proj.m2 = proj.m2 + proj.m3;
  }
  const Mat3x4<float>& pn = gx.pnMtx[gx.currentPnMtx].pos;
  const auto dot = [](const Vec4<float>& r, const float (&v)[4]) {
    return r[0] * v[0] + r[1] * v[1] + r[2] * v[2] + r[3] * v[3];
  };
  std::array<std::array<float, 4>, MaxVertices> clip;
  for (uint32_t i = 0; i < vertexCount; ++i) {
    const float world[4] = {verts[i][0], verts[i][1], verts[i][2], 1.f};
    const float view[4] = {dot(pn.m0, world), dot(pn.m1, world), dot(pn.m2, world), 1.f};
    clip[i] = std::array<float, 4>{dot(proj.m0, view), dot(proj.m1, view), dot(proj.m2, view), dot(proj.m3, view)};
  }
  Payload p{.key = key, .vertexCount = vertexCount, .depthTest = depthTest ? 1u : 0u};
  p.verts = push_verts(reinterpret_cast<const uint8_t*>(clip.data()), vertexCount * sizeof(clip[0]), 4);
  if (overflowed(p.verts) || p.verts.size == 0) {
    return;
  }
  push_custom_draw(g_drawType, &p, sizeof(p));
}

bool take(uint64_t key, uint64_t& samples) {
  std::lock_guard lock{g_resultMutex};
  const auto it = g_results.find(key);
  if (it == g_results.end()) {
    return false;
  }
  samples = it->second.samples;
  g_results.erase(it);
  return true;
}

namespace detail {
wgpu::QuerySet pass_begin(bool claimed) {
  g_passOpen = false;
  if (!g_active.load(std::memory_order_acquire) || claimed) {
    return {};
  }
  if (g_frameSlot == nullptr) {
    for (auto& slot : g_slots) {
      if (slot.state.load(std::memory_order_acquire) == State::Idle) {
        ensure_slot(slot);
        slot.keys.clear();
        slot.state.store(State::Recording, std::memory_order_release);
        g_frameSlot = &slot;
        break;
      }
    }
    if (g_frameSlot == nullptr) {
      return {}; // every slot's results are still on the way: no queries this frame
    }
  }
  g_passOpen = true;
  return g_frameSlot->querySet;
}

void pass_end() { g_passOpen = false; }

void frame_end(const wgpu::CommandEncoder& encoder) {
  Slot* slot = g_frameSlot;
  g_frameSlot = nullptr;
  g_passOpen = false;
  if (slot == nullptr) {
    return;
  }
  if (slot->keys.empty()) {
    slot->state.store(State::Idle, std::memory_order_release);
    return;
  }
  const auto count = static_cast<uint32_t>(slot->keys.size());
  encoder.ResolveQuerySet(slot->querySet, 0, count, slot->resolve, 0);
  encoder.CopyBufferToBuffer(slot->resolve, 0, slot->readback, 0, count * sizeof(uint64_t));
  slot->state.store(State::Resolved, std::memory_order_release);
}

void after_submit() {
  if (!g_active.load(std::memory_order_acquire)) {
    return;
  }
  bool inFlight = false;
  for (auto& slot : g_slots) {
    if (slot.state.load(std::memory_order_acquire) == State::Resolved) {
      slot.state.store(State::InFlight, std::memory_order_release);
      Slot* const target = &slot;
      slot.readback.MapAsync(wgpu::MapMode::Read, 0, slot.keys.size() * sizeof(uint64_t),
                             wgpu::CallbackMode::AllowSpontaneous,
                             [target](wgpu::MapAsyncStatus status, wgpu::StringView) {
                               target->state.store(status == wgpu::MapAsyncStatus::Success ? State::Mapped
                                                                                           : State::Failed,
                                                   std::memory_order_release);
                             });
    }
    inFlight = inFlight || slot.state.load(std::memory_order_acquire) == State::InFlight;
  }
  if (inFlight) {
    webgpu::g_instance.ProcessEvents();
  }
  for (auto& slot : g_slots) {
    const auto state = slot.state.load(std::memory_order_acquire);
    if (state == State::Mapped) {
      const auto* counts =
          static_cast<const uint64_t*>(slot.readback.GetConstMappedRange(0, slot.keys.size() * sizeof(uint64_t)));
      if (counts != nullptr) {
        std::lock_guard lock{g_resultMutex};
        ++g_mapped;
        absl::erase_if(g_results, [](const auto& entry) { return g_mapped - entry.second.mapped > ResultLife; });
        for (size_t i = 0; i < slot.keys.size(); ++i) {
          g_results[slot.keys[i]] = {counts[i], g_mapped};
        }
      }
      slot.readback.Unmap();
    } else if (state == State::Failed) {
      Log.warn("occlusion query readback failed");
    }
    if (state == State::Mapped || state == State::Failed) {
      slot.state.store(State::Idle, std::memory_order_release);
    }
  }
}
} // namespace detail
} // namespace aurora::gfx::vis_query
