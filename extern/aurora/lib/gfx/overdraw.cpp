#include "overdraw.hpp"

#include "../logging.hpp"
#include "../webgpu/gpu.hpp"
#include "hash.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include <absl/container/flat_hash_map.h>

namespace aurora::gfx::overdraw {
namespace {
Module Log("aurora::gfx::overdraw");

constexpr uint32_t MaxQueries = 16384;
constexpr uint32_t Interval = 240; // frames between captures

struct PassInfo {
  uint32_t index;
  uint32_t width, height, samples;
};
struct DrawInfo {
  uint32_t pass; // index into g_passes
  PipelineRef ref;
};
enum class State : uint8_t { Idle, Recording, InFlight, Mapped, Failed };

const char* g_path = nullptr;
bool g_checked = false;
uint32_t g_frame = 0;
wgpu::QuerySet g_querySet;
wgpu::Buffer g_resolve;
wgpu::Buffer g_readback;
std::atomic<State> g_state{State::Idle};
bool g_pending = false;
bool g_passOpen = false; // between a capturing pass_begin and its pass_end
std::vector<PassInfo> g_passes;
std::vector<DrawInfo> g_draws;
std::mutex g_pipelineMutex;
absl::flat_hash_map<PipelineRef, std::string> g_pipelines;

void write_dump() {
  const auto* counts = static_cast<const uint64_t*>(g_readback.GetConstMappedRange(0, g_draws.size() * sizeof(uint64_t)));
  if (counts == nullptr) {
    return;
  }
  const std::string tmp = std::string{g_path} + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "w");
  if (f == nullptr) {
    Log.warn("cannot write {}", tmp);
    return;
  }
  std::fprintf(f, "pass\twidth\theight\tmsaa\tdraw\tsamples\tpipeline\tpbr\tkind\tcut\tblend\tsrc\tdst\tzcomp\tzupd\tcolor\tshader\n");
  std::lock_guard lock{g_pipelineMutex};
  for (size_t i = 0; i < g_draws.size(); ++i) {
    const auto& d = g_draws[i];
    const auto& p = g_passes[d.pass];
    const auto it = g_pipelines.find(d.ref);
    std::fprintf(f, "%u\t%u\t%u\t%u\t%zu\t%llu\t%016llx\t%s\n", p.index, p.width, p.height, p.samples, i,
                 static_cast<unsigned long long>(counts[i]), static_cast<unsigned long long>(d.ref),
                 it != g_pipelines.end() ? it->second.c_str() : "?\t?\t?\t?\t?\t?\t?\t?\t?\t?");
  }
  std::fclose(f);
  std::rename(tmp.c_str(), g_path);
}
} // namespace

bool enabled() {
  if (!g_checked) {
    g_checked = true;
    const char* path = std::getenv("AURORA_OVERDRAW");
    g_path = path != nullptr && *path != '\0' ? path : nullptr;
  }
  return g_path != nullptr;
}

void note_pipeline(PipelineRef ref, const gx::PipelineConfig& config) {
  if (!enabled()) {
    return;
  }
  const auto& sc = config.shaderConfig;
  std::lock_guard lock{g_pipelineMutex};
  if (g_pipelines.contains(ref)) {
    return;
  }
  g_pipelines.emplace(ref, fmt::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{:016x}", sc.pbr, sc.pbrKind, sc.pbrCut,
                                       static_cast<int>(config.blendMode), static_cast<int>(config.blendFacSrc),
                                       static_cast<int>(config.blendFacDst), config.depthCompare, config.depthUpdate,
                                       config.colorUpdate, xxh3_hash(sc)));
}

void frame_begin() {
  if (!enabled() || ++g_frame % Interval != 0 || g_state.load(std::memory_order_acquire) != State::Idle) {
    return;
  }
  if (!g_querySet) {
    const wgpu::QuerySetDescriptor qs{.label = "Overdraw occlusion", .type = wgpu::QueryType::Occlusion, .count = MaxQueries};
    g_querySet = webgpu::g_device.CreateQuerySet(&qs);
    const wgpu::BufferDescriptor resolve{.label = "Overdraw resolve",
                                         .usage = wgpu::BufferUsage::QueryResolve | wgpu::BufferUsage::CopySrc,
                                         .size = MaxQueries * sizeof(uint64_t)};
    g_resolve = webgpu::g_device.CreateBuffer(&resolve);
    const wgpu::BufferDescriptor readback{.label = "Overdraw readback",
                                          .usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
                                          .size = MaxQueries * sizeof(uint64_t)};
    g_readback = webgpu::g_device.CreateBuffer(&readback);
  }
  g_passes.clear();
  g_draws.clear();
  g_state.store(State::Recording, std::memory_order_release);
}

wgpu::QuerySet pass_begin(uint32_t passIndex, const wgpu::Extent3D& size, uint32_t samples) {
  if (g_path == nullptr || g_state.load(std::memory_order_relaxed) != State::Recording) {
    return {};
  }
  g_passes.push_back({passIndex, size.width, size.height, samples});
  g_passOpen = true;
  return g_querySet;
}

void pass_end() { g_passOpen = false; }

uint32_t draw_begin(PipelineRef ref, const wgpu::RenderPassEncoder& pass) {
  if (g_path == nullptr || g_state.load(std::memory_order_relaxed) != State::Recording || !g_passOpen ||
      g_draws.size() >= MaxQueries) {
    return UINT32_MAX;
  }
  const auto query = static_cast<uint32_t>(g_draws.size());
  g_draws.push_back({static_cast<uint32_t>(g_passes.size() - 1), ref});
  pass.BeginOcclusionQuery(query);
  return query;
}

void draw_end(uint32_t query, const wgpu::RenderPassEncoder& pass) {
  if (query != UINT32_MAX) {
    pass.EndOcclusionQuery();
  }
}

void frame_end(const wgpu::CommandEncoder& encoder) {
  if (g_path == nullptr || g_state.load(std::memory_order_relaxed) != State::Recording) {
    return;
  }
  if (g_draws.empty()) {
    g_state.store(State::Idle, std::memory_order_release);
    return;
  }
  const auto count = static_cast<uint32_t>(g_draws.size());
  encoder.ResolveQuerySet(g_querySet, 0, count, g_resolve, 0);
  encoder.CopyBufferToBuffer(g_resolve, 0, g_readback, 0, count * sizeof(uint64_t));
  g_pending = true;
}

void after_submit() {
  if (g_path == nullptr) {
    return;
  }
  if (g_pending) {
    g_pending = false;
    g_state.store(State::InFlight, std::memory_order_release);
    g_readback.MapAsync(wgpu::MapMode::Read, 0, g_draws.size() * sizeof(uint64_t), wgpu::CallbackMode::AllowSpontaneous,
                        [](wgpu::MapAsyncStatus status, wgpu::StringView) {
                          g_state.store(status == wgpu::MapAsyncStatus::Success ? State::Mapped : State::Failed,
                                        std::memory_order_release);
                        });
  }
  if (g_state.load(std::memory_order_acquire) == State::InFlight) {
    webgpu::g_instance.ProcessEvents();
  }
  const auto state = g_state.load(std::memory_order_acquire);
  if (state == State::Mapped) {
    write_dump();
    g_readback.Unmap();
  }
  if (state == State::Mapped || state == State::Failed) {
    g_state.store(State::Idle, std::memory_order_release);
  }
}
} // namespace aurora::gfx::overdraw
