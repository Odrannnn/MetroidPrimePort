#include "frame.hpp"
#include "overdraw.hpp"

#include "depth_peek.hpp"
#include "pipeline_cache.hpp"
#include "bloom.hpp"
#include "shadow.hpp"
#include "actor_shadow.hpp"
#include "volfog.hpp"
#include "thermal.hpp"
#include "xray.hpp"
#include <bit>
#include <aurora/vfx.hpp>
#include <aurora/envfx.hpp>
#include <aurora/water.hpp>
#include "probe.hpp"
#include "recording.hpp"
#include "render_worker.hpp"
#include "resource_cache.hpp"
#include "tex_copy_conv.hpp"
#include "tex_palette_conv.hpp"
#include "texture_replacement.hpp"
#include "../gx/gx.hpp"
#ifdef AURORA_ENABLE_RMLUI
#include "../rmlui/pipeline.hpp"
#endif
#include "../webgpu/gpu.hpp"
#include "../webgpu/gpu_prof.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <tracy/Tracy.hpp>
#include <magic_enum.hpp>

namespace aurora::gfx {
using namespace detail;
using webgpu::g_device;
using webgpu::g_instance;
using webgpu::g_queue;

// Set once when the device is created (webgpu::initialize); see resources.hpp.
bool g_noCompute = false;
bool g_vertexFetchTexture = false;

namespace {
constexpr Module Log{"aurora::gfx"};

Resources g_resources;

enum class BufferMapState {
  Unmapped,
  Mapping,
  Mapped,
};

FrameBufferSizes g_bufferSizes;
std::array<std::atomic<uint64_t>, 4> g_stagingGrowthWant{};
// aurora_reserve_frame_buffers() asked for the held limits; begin_frame takes them.
std::atomic<bool> g_reserveHeldFrameBuffers{false};
// They were taken: a re-initialisation (device loss) keeps them.
bool g_heldFrameBuffersTaken = false;
constexpr uint32_t StorageGrowthCapScale = 6;
std::array<wgpu::Buffer, StagingBufferCount> g_stagingBuffers;
std::array<std::atomic<BufferMapState>, StagingBufferCount> g_mappingStates;
uint32_t g_frameIndex = UINT32_MAX;

std::array<FramePacket, FrameSlotCount> g_framePackets;
uint64_t g_nextFrameId = 1;
render_worker::FrameSlotPool g_frameSlots{FrameSlotCount};
render_worker::FrameSlotPool g_stagingSlots{StagingBufferCount};

// The uniform buffer is bound as a MaxUniformSize window, and some passes bind more of it.
constexpr uint64_t MinDeviceUniformSize = 64u << 10;

struct SharedBuffer {
  StagingKind kind;
  wgpu::Buffer Resources::*member;
  wgpu::BufferUsage usage;
  const char* label;
  const char* name;
};
const std::array<SharedBuffer, 4> SharedBuffers{{
    {StagingKind::Vertex, &Resources::vertexBuffer,
     wgpu::BufferUsage::Storage | wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::CopySrc,
     "Shared Vertex Buffer", "vertex"},
    {StagingKind::Uniform, &Resources::uniformBuffer,
     wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::CopySrc, "Shared Uniform Buffer",
     "uniform"},
    {StagingKind::Index, &Resources::indexBuffer,
     wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::CopySrc, "Shared Index Buffer",
     "index"},
    {StagingKind::Storage, &Resources::storageBuffer,
     wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::CopySrc, "Shared Storage Buffer",
     "storage"},
}};

// Texture vertex fetch: one fetch texture row is 4096 R32Uint texels (16 KiB), so a buffer's
// bytes are its size / 16 KiB rows of the texture the shaders read it through.
constexpr uint32_t FetchTextureWidth = 4096;

// The allocation size a shared buffer needs: whole fetch texture rows for the kinds the shaders
// read as textures, so its texture covers it exactly.
uint64_t fetch_aligned_buffer_size(StagingKind kind, uint64_t bytes) noexcept {
  if (!g_vertexFetchTexture || (kind != StagingKind::Vertex && kind != StagingKind::Storage)) {
    return bytes;
  }
  return AURORA_ALIGN(bytes, FetchTextureRowBytes);
}

// Creates the fetch texture mirroring `kind`'s device buffer of `bytes` bytes.
void create_fetch_texture(StagingKind kind, uint64_t bytes) {
  const bool vertex = kind == StagingKind::Vertex;
  wgpu::Texture& texture = vertex ? g_resources.vertexFetchTexture : g_resources.storageFetchTexture;
  wgpu::TextureView& view = vertex ? g_resources.vertexFetchTextureView : g_resources.storageFetchTextureView;
  const char* label = vertex ? "Vertex fetch texture" : "Storage fetch texture";
  const uint64_t rows = AURORA_ALIGN(bytes, FetchTextureRowBytes) / FetchTextureRowBytes;
  const uint32_t maxRows = g_resources.limits.maxTextureDimension2D;
  if (maxRows != 0 && rows > maxRows) {
    // The shaders address the data by row, so it cannot go in a smaller texture.
    FATAL("{} needs {} rows of {} bytes, more than this device's maxTextureDimension2D of {}; "
          "lower frameBufferScale or leave AURORA_VERTEX_FETCH_TEXTURE unset",
          label, rows, FetchTextureRowBytes, maxRows);
  }
  const wgpu::TextureDescriptor descriptor{
      .label = label,
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e2D,
      .size = {FetchTextureWidth, static_cast<uint32_t>(rows), 1},
      .format = wgpu::TextureFormat::R32Uint,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  texture = g_device.CreateTexture(&descriptor);
  view = texture.CreateView();
}

void create_static_bind_group() {
  std::array<wgpu::BindGroupEntry, 2> entries{};
  for (size_t i = 0; i < entries.size(); ++i) {
    entries[i].binding = static_cast<uint32_t>(i);
  }
  if (g_vertexFetchTexture) {
    entries[0].textureView = g_resources.vertexFetchTextureView;
    entries[1].textureView = g_resources.storageFetchTextureView;
  } else {
    entries[0].buffer = g_resources.vertexBuffer;
    entries[1].buffer = g_resources.storageBuffer;
  }
  const wgpu::BindGroupDescriptor bindGroupDescriptor{
      .label = "Static bind group",
      .layout = g_resources.staticBindGroupLayout,
      .entryCount = entries.size(),
      .entries = entries.data(),
  };
  g_resources.staticBindGroup = g_device.CreateBindGroup(&bindGroupDescriptor);
}

void create_uniform_bind_group() {
  const std::array entries{
      wgpu::BindGroupEntry{
          .binding = 0,
          .buffer = g_resources.uniformBuffer,
          .size = gx::MaxUniformSize,
      },
  };
  const wgpu::BindGroupDescriptor bindGroupDescriptor{
      .label = "Uniform bind group",
      .layout = g_resources.uniformBindGroupLayout,
      .entryCount = entries.size(),
      .entries = entries.data(),
  };
  g_resources.uniformBindGroup = g_device.CreateBindGroup(&bindGroupDescriptor);
}

struct RuntimeDrawType {
  std::string label;
  DrawCallback draw = nullptr;
  void* userdata = nullptr;
  uint32_t generation = 1;
};

struct RuntimeEncoderTaskType {
  std::string label;
  EncoderTaskCallback callback = nullptr;
  void* userdata = nullptr;
  EncoderTaskCompletionCallback afterSubmit = nullptr;
  uint32_t generation = 1;
};

constexpr uint32_t draw_type_index(DrawTypeId id) { return static_cast<uint32_t>(id & 0xFFFFFFFFu); }
constexpr uint32_t draw_type_generation(DrawTypeId id) { return static_cast<uint32_t>(id >> 32); }
constexpr DrawTypeId make_draw_type_id(uint32_t index, uint32_t generation) {
  return static_cast<DrawTypeId>(generation) << 32 | index;
}

std::vector<RuntimeDrawType> g_runtimeDrawTypes;
std::vector<uint32_t> g_freeDrawTypeSlots;
std::vector<RuntimeEncoderTaskType> g_runtimeEncoderTaskTypes;
std::vector<uint32_t> g_freeEncoderTaskTypeSlots;
std::mutex g_runtimeTypeMutex;

// Requires g_runtimeTypeMutex held.
const RuntimeDrawType* find_runtime_draw_type_locked(DrawTypeId id) {
  const auto idx = draw_type_index(id);
  if (id == InvalidDrawType || idx >= g_runtimeDrawTypes.size()) {
    return nullptr;
  }
  const auto& slot = g_runtimeDrawTypes[idx];
  if (slot.generation != draw_type_generation(id) || slot.draw == nullptr) {
    return nullptr;
  }
  return &slot;
}

// Requires g_runtimeTypeMutex held.
const RuntimeEncoderTaskType* find_runtime_encoder_task_type_locked(EncoderTaskId id) {
  const auto idx = draw_type_index(id);
  if (id == InvalidEncoderTask || idx >= g_runtimeEncoderTaskTypes.size()) {
    return nullptr;
  }
  const auto& slot = g_runtimeEncoderTaskTypes[idx];
  if (slot.generation != draw_type_generation(id) || slot.callback == nullptr) {
    return nullptr;
  }
  return &slot;
}

using PresentClock = std::chrono::steady_clock;
constexpr auto PresentFpsWindow = std::chrono::seconds{1};
std::mutex g_presentStatsMutex;
std::deque<PresentClock::time_point> g_presentTimes;
std::atomic_bool g_processEventsQueued = false;
std::atomic_int64_t g_lastPresentNs = 0;
std::atomic_int64_t g_presentPeriodNs = 0;
std::atomic_int64_t g_cpuFrameTimeNs = 0;
PresentClock::time_point g_cpuFrameStart;
constexpr auto FrameStartSafetyMargin = std::chrono::milliseconds{2};
constexpr auto MaxPacingSample = std::chrono::milliseconds{250};
constexpr uint32_t PacingEmaWeight = 8;

int64_t timestamp_ns(PresentClock::time_point time) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}

int64_t duration_ns(PresentClock::duration duration) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

void update_ema(std::atomic_int64_t& value, int64_t sample) {
  if (sample <= 0 || sample > duration_ns(MaxPacingSample)) {
    return;
  }

  int64_t current = value.load(std::memory_order_acquire);
  while (true) {
    const int64_t next = current == 0 ? sample : current + (sample - current) / static_cast<int64_t>(PacingEmaWeight);
    if (value.compare_exchange_weak(current, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
      return;
    }
  }
}

void prune_present_times(PresentClock::time_point now) {
  while (!g_presentTimes.empty() && g_presentTimes.front() + PresentFpsWindow < now) {
    g_presentTimes.pop_front();
  }
}

void process_events() {
  ZoneScopedN("ProcessEvents");
  if (g_instance) {
    g_instance.ProcessEvents();
  }
}

void enqueue_process_events() {
  if (render_worker::is_worker_thread()) {
    process_events();
    return;
  }

  bool expected = false;
  if (!g_processEventsQueued.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
    return;
  }

  render_worker::enqueue_work([] {
    process_events();
    g_processEventsQueued.store(false, std::memory_order_release);
  });
}

void wait_for_gpu_progress(std::chrono::nanoseconds sleepDuration) {
  if (render_worker::is_idle()) {
    enqueue_process_events();
  }
  std::this_thread::sleep_for(sleepDuration);
}

void pace_frame_start() {
  ZoneScopedN("Frame start pacing");
  if (g_frameSlots.free_count() == FrameSlotCount) {
    return;
  }

  const int64_t lastPresentNs = g_lastPresentNs.load(std::memory_order_acquire);
  const int64_t presentPeriodNs = g_presentPeriodNs.load(std::memory_order_acquire);
  const int64_t cpuFrameTimeNs = g_cpuFrameTimeNs.load(std::memory_order_acquire);
  if (lastPresentNs == 0 || presentPeriodNs == 0 || cpuFrameTimeNs == 0) {
    return;
  }

  const int64_t safetyMarginNs = duration_ns(FrameStartSafetyMargin);
  const int64_t targetStartNs = lastPresentNs + presentPeriodNs - cpuFrameTimeNs - safetyMarginNs;
  int64_t nowNs = timestamp_ns(PresentClock::now());
  if (targetStartNs <= nowNs) {
    return;
  }

  const double initialWaitMs = static_cast<double>(targetStartNs - nowNs) / 1'000'000.0;
  TracyPlot("aurora: frameStartPaceWaitMs", initialWaitMs);
  while (nowNs < targetStartNs) {
    const int64_t remainingNs = targetStartNs - nowNs;
    const auto sleepDuration =
        remainingNs > 1'000'000 ? std::chrono::milliseconds{1} : std::chrono::nanoseconds{remainingNs};
    wait_for_gpu_progress(sleepDuration);
    nowNs = timestamp_ns(PresentClock::now());
  }
}

void map_staging_buffer(size_t slot, bool releaseSlotOnCompletion = false) {
  auto expected = BufferMapState::Unmapped;
  if (!g_mappingStates[slot].compare_exchange_strong(expected, BufferMapState::Mapping, std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
    return;
  }

  g_stagingBuffers[slot].MapAsync(
      wgpu::MapMode::Write, 0, g_bufferSizes.staging(), wgpu::CallbackMode::AllowSpontaneous,
      [slot, releaseSlotOnCompletion](wgpu::MapAsyncStatus status, wgpu::StringView message) {
        if (status == wgpu::MapAsyncStatus::CallbackCancelled || status == wgpu::MapAsyncStatus::Aborted) {
          Log.warn("Buffer mapping {}: {}", magic_enum::enum_name(status), message);
          g_mappingStates[slot].store(BufferMapState::Unmapped, std::memory_order_release);
          if (releaseSlotOnCompletion) {
            g_stagingSlots.release(slot);
          }
          return;
        }
        AURORA_ASSERT(status == wgpu::MapAsyncStatus::Success, "Buffer mapping failed: {} {}",
                      magic_enum::enum_name(status), message);
        g_mappingStates[slot].store(BufferMapState::Mapped, std::memory_order_release);
        if (releaseSlotOnCompletion) {
          g_stagingSlots.release(slot);
        }
      });
}
} // namespace

const FrameBufferSizes& frame_buffer_sizes() noexcept { return g_bufferSizes; }

Range resident_region(ResidentBuffer kind) noexcept {
  switch (kind) {
  case ResidentBuffer::Vertex:
    return {0, uint32_t(g_bufferSizes.residentVertex)};
  case ResidentBuffer::Index:
    return {0, uint32_t(g_bufferSizes.residentIndex)};
  case ResidentBuffer::Storage:
    return {0, uint32_t(g_bufferSizes.residentStorage)};
  }
  return {};
}

namespace detail {

Resources& resources() noexcept { return g_resources; }

const wgpu::Buffer& staging_buffer(size_t slot) { return g_stagingBuffers[slot]; }

std::optional<RegisteredDrawType> find_runtime_draw_type(DrawTypeId id) {
  std::lock_guard lock{g_runtimeTypeMutex};
  const auto* slot = find_runtime_draw_type_locked(id);
  if (slot == nullptr) {
    return std::nullopt;
  }
  return RegisteredDrawType{.draw = slot->draw, .userdata = slot->userdata};
}

std::optional<RegisteredEncoderTaskType> find_runtime_encoder_task_type(EncoderTaskId id) {
  std::lock_guard lock{g_runtimeTypeMutex};
  const auto* slot = find_runtime_encoder_task_type_locked(id);
  if (slot == nullptr) {
    return std::nullopt;
  }
  return RegisteredEncoderTaskType{
      .callback = slot->callback,
      .userdata = slot->userdata,
      .afterSubmit = slot->afterSubmit,
  };
}

void upload_fetch_texture_rows(const wgpu::CommandEncoder& encoder, StagingKind kind, uint64_t lo, uint64_t hi) {
  if (!g_vertexFetchTexture || hi <= lo || (kind != StagingKind::Vertex && kind != StagingKind::Storage)) {
    return;
  }
  const bool vertex = kind == StagingKind::Vertex;
  const wgpu::Texture& texture = vertex ? g_resources.vertexFetchTexture : g_resources.storageFetchTexture;
  if (!texture) {
    return;
  }
  const uint64_t firstRow = lo / FetchTextureRowBytes;
  const uint64_t lastRow = (hi + FetchTextureRowBytes - 1) / FetchTextureRowBytes;
  const wgpu::TexelCopyBufferInfo source{
      .layout =
          wgpu::TexelCopyBufferLayout{
              .offset = firstRow * FetchTextureRowBytes,
              .bytesPerRow = FetchTextureRowBytes,
              .rowsPerImage = static_cast<uint32_t>(lastRow - firstRow),
          },
      .buffer = vertex ? g_resources.vertexBuffer : g_resources.storageBuffer,
  };
  const wgpu::TexelCopyTextureInfo destination{
      .texture = texture,
      .origin =
          wgpu::Origin3D{
              .x = 0,
              .y = static_cast<uint32_t>(firstRow),
              .z = 0,
          },
  };
  const wgpu::Extent3D size{
      .width = FetchTextureWidth,
      .height = static_cast<uint32_t>(lastRow - firstRow),
      .depthOrArrayLayers = 1,
  };
  encoder.CopyBufferToTexture(&source, &destination, &size);
}

} // namespace detail

wgpu::Device device() noexcept { return g_device; }

wgpu::Queue queue() noexcept { return g_queue; }

wgpu::TextureFormat color_format() noexcept { return webgpu::g_graphicsConfig.surfaceConfiguration.format; }

wgpu::TextureFormat depth_format() noexcept { return webgpu::g_graphicsConfig.depthFormat; }

uint32_t sample_count() noexcept { return webgpu::g_graphicsConfig.msaaSamples; }

static RenderTargetLayout efb_render_target_layout(const webgpu::TextureWithSampler& color) noexcept {
  RenderTargetLayout layout{
      .colorAttachmentCount = 1,
      .colorAttachments = {{{
          .semantic = ColorAttachmentSemantic::SceneColor,
          .format = color.format,
          .width = color.size.width,
          .height = color.size.height,
      }}},
      .depthStencilFormat = webgpu::g_graphicsConfig.depthFormat,
      .sampleCount = webgpu::g_graphicsConfig.msaaSamples,
  };
  finalize_render_target_layout(layout);
  return layout;
}

// The target EFB passes draw into now: the HDR scene while it is active (set_scene_hdr), else the
// display EFB. color_format() stays the surface format either way.
RenderTargetLayout scene_render_target_layout() noexcept {
  return efb_render_target_layout(scene_hdr_active() ? webgpu::g_sceneHdr : webgpu::g_frameBuffer);
}

RenderTargetLayout display_render_target_layout() noexcept { return efb_render_target_layout(webgpu::g_frameBuffer); }

bool uses_reversed_z() noexcept { return gx::UseReversedZ; }

DrawTypeId register_draw_type(const DrawTypeDescriptor& desc) {
  if (desc.draw == nullptr) {
    Log.warn("register_draw_type: draw callback is null");
    return InvalidDrawType;
  }

  std::lock_guard lock{g_runtimeTypeMutex};
  uint32_t idx;
  if (!g_freeDrawTypeSlots.empty()) {
    idx = g_freeDrawTypeSlots.back();
    g_freeDrawTypeSlots.pop_back();
  } else {
    idx = static_cast<uint32_t>(g_runtimeDrawTypes.size());
    g_runtimeDrawTypes.emplace_back();
  }
  auto& slot = g_runtimeDrawTypes[idx];
  slot.label = desc.label != nullptr ? desc.label : "";
  slot.draw = desc.draw;
  slot.userdata = desc.userdata;
  return make_draw_type_id(idx, slot.generation);
}

void unregister_draw_type(DrawTypeId type) noexcept {
  std::lock_guard lock{g_runtimeTypeMutex};
  if (find_runtime_draw_type_locked(type) == nullptr) {
    return;
  }
  const auto idx = draw_type_index(type);
  auto& slot = g_runtimeDrawTypes[idx];
  slot.label.clear();
  slot.draw = nullptr;
  slot.userdata = nullptr;
  ++slot.generation;
  g_freeDrawTypeSlots.push_back(idx);
}

EncoderTaskId register_encoder_task_type(const EncoderTaskDescriptor& desc) {
  if (desc.callback == nullptr) {
    Log.warn("register_encoder_task_type: callback is null");
    return InvalidEncoderTask;
  }

  std::lock_guard lock{g_runtimeTypeMutex};
  uint32_t idx;
  if (!g_freeEncoderTaskTypeSlots.empty()) {
    idx = g_freeEncoderTaskTypeSlots.back();
    g_freeEncoderTaskTypeSlots.pop_back();
  } else {
    idx = static_cast<uint32_t>(g_runtimeEncoderTaskTypes.size());
    g_runtimeEncoderTaskTypes.emplace_back();
  }
  auto& slot = g_runtimeEncoderTaskTypes[idx];
  slot.label = desc.label != nullptr ? desc.label : "";
  slot.callback = desc.callback;
  slot.userdata = desc.userdata;
  slot.afterSubmit = desc.afterSubmit;
  return make_draw_type_id(idx, slot.generation);
}

void unregister_encoder_task_type(EncoderTaskId type) noexcept {
  std::lock_guard lock{g_runtimeTypeMutex};
  if (find_runtime_encoder_task_type_locked(type) == nullptr) {
    return;
  }
  const auto idx = draw_type_index(type);
  auto& slot = g_runtimeEncoderTaskTypes[idx];
  slot.label.clear();
  slot.callback = nullptr;
  slot.userdata = nullptr;
  slot.afterSubmit = nullptr;
  ++slot.generation;
  g_freeEncoderTaskTypeSlots.push_back(idx);
}

void initialize() {
  g_frameIndex = 0;
  g_processEventsQueued.store(false, std::memory_order_release);
  g_lastPresentNs.store(0, std::memory_order_release);
  g_presentPeriodNs.store(0, std::memory_order_release);
  g_cpuFrameTimeNs.store(0, std::memory_order_release);
  g_cpuFrameStart = {};
  {
    std::lock_guard lock{g_presentStatsMutex};
    g_presentTimes.clear();
  }
  render_worker::initialize();
  // This appears to take a while and blocks the render thread for periods of time
  // render_worker::set_event_pump([] {
  //   if (g_instance) {
  //     g_instance.ProcessEvents();
  //   }
  // });
  depth_peek::initialize();
  tex_copy_conv::initialize();
  tex_palette_conv::initialize();

  // For uniform & storage buffer offset alignments
  g_device.GetLimits(&g_resources.limits);

  // The vertex and array buffers are bound whole as storage, and one staging buffer holds
  // a frame of everything, so the scale stops where the device does.
  {
    const uint64_t maxBinding = g_resources.limits.maxStorageBufferBindingSize;
    const uint64_t maxBuffer = g_resources.limits.maxBufferSize;
    uint32_t scale = std::max<uint32_t>(g_config.frameBufferScale, 1);
    for (;; --scale) {
      g_bufferSizes = {
          .vertex = VertexBufferSize * scale,
          .uniform = scale > 1 ? UniformBufferSize * 2 : UniformBufferSize,
          // Not scaled: the heaviest room measured drew 0.1 MiB of indices, and each
          // scaled MiB here costs one per staging buffer plus the device copy.
          .index = IndexBufferSize,
          .storage = StorageBufferSize * scale,
      };
      if (scale == 1 || (g_bufferSizes.vertex <= maxBinding && g_bufferSizes.storage <= maxBinding &&
                         g_bufferSizes.staging() <= maxBuffer)) {
        break;
      }
    }
    if (scale != std::max<uint32_t>(g_config.frameBufferScale, 1)) {
      Log.warn("Frame buffer scale {} is beyond this device, using {}", g_config.frameBufferScale, scale);
    }
    // The arrays can be scaled apart from the rest: frameStorageScale 0 follows the scale.
    uint32_t storageScale = g_config.frameStorageScale != 0 ? g_config.frameStorageScale : scale;
    // Test hook: start the growable storage smaller to force an overflow.
    if (g_config.frameStorageScale == 0) {
      if (const char* start = std::getenv("AURORA_STORAGE_START_SCALE")) {
        storageScale = std::max<uint32_t>(1, std::min<uint32_t>(storageScale, std::strtoul(start, nullptr, 10)));
      }
    }
    const uint32_t wantedStorageScale = storageScale;
    while (storageScale > scale) {
      g_bufferSizes.storage = StorageBufferSize * storageScale;
      if (g_bufferSizes.storage <= maxBinding && g_bufferSizes.staging() <= maxBuffer) {
        break;
      }
      --storageScale;
    }
    g_bufferSizes.storage = StorageBufferSize * storageScale;
    if (storageScale != wantedStorageScale) {
      Log.warn("Frame storage scale {} is beyond this device, using {}", wantedStorageScale, storageScale);
    }
    // Unless the scale was pinned, an overflow can double the staging storage later, up to
    // 6x. The device buffer sets that much aside now so the resident region stays put.
    g_bufferSizes.storageReserve = g_bufferSizes.storage;
    g_bufferSizes.storageCap = 0;
    if (g_config.frameStorageScale == 0 && g_config.residentGeometryMiB != 0) {
      uint32_t capScale = StorageGrowthCapScale;
      while (capScale > storageScale) {
        FrameBufferSizes probe = g_bufferSizes;
        probe.storage = StorageBufferSize * capScale;
        if (probe.storage <= maxBinding && probe.staging() <= maxBuffer) {
          break;
        }
        --capScale;
      }
      if (capScale > storageScale) {
        g_bufferSizes.storageCap = StorageBufferSize * capScale;
        g_bufferSizes.storageReserve = g_bufferSizes.storageCap;
      }
    }
    // The vertices likewise: a frame drawing skinned models twice more (the Remastered suit
    // shell) went past 2x with the room geometry kept resident. Only the limit moves; the
    // device and staging buffers still grow to it when a frame needs more.
    uint64_t vertexReserve = g_bufferSizes.vertex;
    if (g_config.frameStorageScale == 0 && g_config.residentGeometryMiB != 0) {
      for (uint32_t capScale = StorageGrowthCapScale; capScale > scale; --capScale) {
        FrameBufferSizes probe = g_bufferSizes;
        probe.vertex = VertexBufferSize * capScale;
        probe.storage = g_bufferSizes.storageReserve;
        if (probe.vertex <= maxBinding && probe.staging() <= maxBuffer) {
          vertexReserve = probe.vertex;
          break;
        }
      }
    }
    for (auto& want : g_stagingGrowthWant) {
      want.store(0, std::memory_order_release);
    }
    // The resident regions follow a frame's data in the device buffers, which are bound
    // whole, so they take what the device leaves after it: two fifths each to vertices and
    // arrays, a fifth to indices.
    const uint64_t resident = uint64_t(g_config.residentGeometryMiB) * 1024 * 1024;
    if (resident != 0) {
      const auto fit = [](uint64_t want, uint64_t frame, uint64_t limit) -> uint64_t {
        const uint64_t room = limit > frame ? limit - frame : 0;
        // Offsets into it are u32, and every allocation is 256-aligned.
        return std::min({want, room, uint64_t(UINT32_MAX) - frame}) & ~uint64_t(255);
      };
      const uint64_t bound = std::min(maxBinding, maxBuffer);
      g_bufferSizes.residentVertex = fit(resident / 5 * 2, vertexReserve, bound);
      g_bufferSizes.residentStorage = fit(resident / 5 * 2, g_bufferSizes.storageReserve, bound);
      g_bufferSizes.residentIndex = fit(resident / 5, g_bufferSizes.index, maxBuffer);
      Log.info("Resident geometry: {} MiB of vertices, {} MiB of arrays, {} MiB of indices",
               g_bufferSizes.residentVertex >> 20, g_bufferSizes.residentStorage >> 20,
               g_bufferSizes.residentIndex >> 20);
    }
    // What a frame can hold is what is sized above (the device buffers' per-frame part). The
    // staging buffers, which there are StagingBufferCount of, start smaller unless the scale
    // was pinned: a frame that needs more spills (see SpillBuffer), and the arrays' staging
    // storage grows after an overflow where it can.
    g_bufferSizes.maxVertex = vertexReserve;
    g_bufferSizes.maxUniform = g_bufferSizes.uniform;
    g_bufferSizes.maxIndex = g_bufferSizes.index;
    g_bufferSizes.maxStorage = g_bufferSizes.storageReserve;
    g_bufferSizes.scaledStorage = g_bufferSizes.storage;
    g_bufferSizes.scaledVertex = g_bufferSizes.vertex;
    g_bufferSizes.textureUpload = TextureUploadSize;
    if (!g_config.frameBuffersPinned) {
      g_bufferSizes.vertex = std::min(g_bufferSizes.vertex, StartVertexSize);
      g_bufferSizes.uniform = std::min(g_bufferSizes.uniform, StartUniformSize);
      g_bufferSizes.index = std::min(g_bufferSizes.index, StartIndexSize);
      g_bufferSizes.storage = std::min(g_bufferSizes.storage, StartStorageSize);
      g_bufferSizes.textureUpload = StartTextureUploadSize;
    }
    // Test hook: AURORA_STAGING_START_KIB=<n> sets the vertex, uniform, index and storage
    // staging buffers to n KiB (at most their frame limit) to force the spill path.
    if (const char* kib = std::getenv("AURORA_STAGING_START_KIB")) {
      const uint64_t size = std::max<uint64_t>(256, (std::strtoull(kib, nullptr, 10) << 10) & ~uint64_t(255));
      const char* maskEnv = std::getenv("AURORA_STAGING_START_MASK"); // bits: 1 vertex, 2 uniform, 4 index, 8 storage
      const unsigned mask = maskEnv != nullptr ? unsigned(std::strtoul(maskEnv, nullptr, 0)) : 0xFu;
      if (mask & 1u) g_bufferSizes.vertex = std::min(g_bufferSizes.maxVertex, size);
      if (mask & 2u) g_bufferSizes.uniform = std::min(g_bufferSizes.maxUniform, size);
      if (mask & 4u) g_bufferSizes.index = std::min(g_bufferSizes.maxIndex, size);
      if (mask & 8u) g_bufferSizes.storage = std::min(g_bufferSizes.maxStorage, size);
    }
    // The growable cap is relative to the staging size, so it keeps its meaning; but a cap the
    // staging storage already has is no cap.
    if (g_bufferSizes.storageCap != 0 && g_bufferSizes.storageCap <= g_bufferSizes.storage) {
      g_bufferSizes.storageCap = 0;
    }
    // The device buffers' per-frame part starts where staging does, unless the scale was pinned,
    // and grows like it when a frame needs more (grow_frame_buffer). The uniform one holds at
    // least the bound window. Android takes the full frame limit up front: a mid-frame grow
    // there was followed by a device loss on an Adreno 618 (issue #165), and with robustness
    // off nothing catches a bad access.
    // AURORA_FULL_DEVICE_BUFFERS=1 takes the Android path elsewhere, to test it.
#if defined(__ANDROID__)
    const bool fullDeviceBuffers = true;
#else
    const bool fullDeviceBuffers = std::getenv("AURORA_FULL_DEVICE_BUFFERS") != nullptr;
#endif
    // Taking those limits whole costs memory a game without room geometry never uses, so
    // frameBuffersDeferred runs at what a scale of 1 takes until aurora_reserve_frame_buffers()
    // (reserve_held_frame_buffers, between two frames, never mid-frame). Once taken they stay
    // taken over a device loss.
    if (fullDeviceBuffers && g_config.frameBuffersDeferred && !g_config.frameBuffersPinned &&
        !g_heldFrameBuffersTaken && g_config.residentGeometryMiB == 0 &&
        std::getenv("AURORA_STAGING_START_KIB") == nullptr) {
      auto& s = g_bufferSizes;
      if (s.maxVertex > VertexBufferSize || s.maxUniform > UniformBufferSize || s.maxIndex > IndexBufferSize ||
          s.maxStorage > StorageBufferSize) {
        s.heldVertex = s.maxVertex;
        s.heldUniform = s.maxUniform;
        s.heldIndex = s.maxIndex;
        s.heldStorage = s.maxStorage;
        s.maxVertex = std::min(s.maxVertex, VertexBufferSize);
        s.maxUniform = std::min(s.maxUniform, UniformBufferSize);
        s.maxIndex = std::min(s.maxIndex, IndexBufferSize);
        s.maxStorage = std::min(s.maxStorage, StorageBufferSize);
        s.vertex = std::min(s.vertex, s.maxVertex);
        s.uniform = std::min(s.uniform, s.maxUniform);
        s.index = std::min(s.index, s.maxIndex);
        s.storage = std::min(s.storage, s.maxStorage);
        Log.info("Frame buffers: scaled limits held back until room geometry asks for them");
      }
    }
    if (g_config.frameBuffersPinned || (fullDeviceBuffers && std::getenv("AURORA_STAGING_START_KIB") == nullptr)) {
      g_bufferSizes.deviceVertex = g_bufferSizes.maxVertex;
      g_bufferSizes.deviceUniform = g_bufferSizes.maxUniform;
      g_bufferSizes.deviceIndex = g_bufferSizes.maxIndex;
      g_bufferSizes.deviceStorage = g_bufferSizes.maxStorage;
    } else {
      g_bufferSizes.deviceVertex = g_bufferSizes.vertex;
      g_bufferSizes.deviceUniform =
          std::min(g_bufferSizes.maxUniform, std::max<uint64_t>(g_bufferSizes.uniform, MinDeviceUniformSize));
      g_bufferSizes.deviceIndex = g_bufferSizes.index;
      g_bufferSizes.deviceStorage = g_bufferSizes.storage;
    }
    Log.info("Frame buffers: staging {}/{}/{}/{}+{} MiB (vertex/uniform/index/storage+textures), device {}/{}/{}/{} "
             "KiB, frame limit {}/{}/{}/{} MiB",
             g_bufferSizes.vertex >> 20, g_bufferSizes.uniform >> 20, g_bufferSizes.index >> 20,
             g_bufferSizes.storage >> 20, g_bufferSizes.textureUpload >> 20, g_bufferSizes.deviceVertex >> 10,
             g_bufferSizes.deviceUniform >> 10, g_bufferSizes.deviceIndex >> 10, g_bufferSizes.deviceStorage >> 10,
             g_bufferSizes.maxVertex >> 20, g_bufferSizes.maxUniform >> 20, g_bufferSizes.maxIndex >> 20,
             g_bufferSizes.maxStorage >> 20);
  }

  const auto createBuffer = [](wgpu::Buffer& out, wgpu::BufferUsage usage, uint64_t size, const char* label) {
    if (size <= 0) {
      return;
    }
    const wgpu::BufferDescriptor descriptor{
        .label = label,
        .usage = usage,
        .size = size,
    };
    out = g_device.CreateBuffer(&descriptor);
  };
  for (const auto& buffer : SharedBuffers) {
    createBuffer(g_resources.*buffer.member, buffer.usage,
                 fetch_aligned_buffer_size(buffer.kind,
                                           g_bufferSizes.base(buffer.kind) + g_bufferSizes.device(buffer.kind)),
                 buffer.label);
  }
  if (g_vertexFetchTexture) {
    const uint64_t vertexBytes = g_bufferSizes.base(StagingKind::Vertex) + g_bufferSizes.deviceVertex;
    const uint64_t storageBytes = g_bufferSizes.base(StagingKind::Storage) + g_bufferSizes.deviceStorage;
    create_fetch_texture(StagingKind::Vertex, vertexBytes);
    create_fetch_texture(StagingKind::Storage, storageBytes);
    // The new buffers are zeroed, and a read past the data has to see that (the storage buffers
    // read 0 there), so the textures start as their copies rather than as undefined content.
    const wgpu::CommandEncoderDescriptor encoderDescriptor{
        .label = "Vertex fetch textures",
    };
    auto encoder = g_device.CreateCommandEncoder(&encoderDescriptor);
    upload_fetch_texture_rows(encoder, StagingKind::Vertex, 0, vertexBytes);
    upload_fetch_texture_rows(encoder, StagingKind::Storage, 0, storageBytes);
    auto commands = encoder.Finish();
    g_queue.Submit(1, &commands);
    Log.info("Texture vertex fetch: {} rows of vertices, {} rows of arrays", vertexBytes / FetchTextureRowBytes,
             storageBytes / FetchTextureRowBytes);
  }
  for (size_t i = 0; i < g_stagingBuffers.size(); ++i) {
    const auto label = fmt::format("Staging Buffer {}", i);
    createBuffer(g_stagingBuffers[i], wgpu::BufferUsage::MapWrite | wgpu::BufferUsage::CopySrc, g_bufferSizes.staging(),
                 label.c_str());
  }
  for (auto& state : g_mappingStates) {
    state.store(BufferMapState::Unmapped, std::memory_order_release);
  }
  for (size_t slot = 0; slot < g_stagingBuffers.size(); ++slot) {
    map_staging_buffer(slot);
  }

  {
    // The vertex and storage data as either whole read-only storage buffers or, in texture vertex
    // fetch mode, textures the shaders textureLoad words from (resources.hpp).
    const auto makeEntry = [](uint32_t binding, wgpu::ShaderStage visibility) {
      wgpu::BindGroupLayoutEntry entry{
          .binding = binding,
          .visibility = visibility,
      };
      if (g_vertexFetchTexture) {
        entry.texture =
            wgpu::TextureBindingLayout{
                .sampleType = wgpu::TextureSampleType::Uint,
                .viewDimension = wgpu::TextureViewDimension::e2D,
            };
      } else {
        entry.buffer =
            wgpu::BufferBindingLayout{
                .type = wgpu::BufferBindingType::ReadOnlyStorage,
            };
      }
      return entry;
    };
    // Vertex data, then storage data.
    const std::array layoutEntries{makeEntry(0, wgpu::ShaderStage::Vertex),
                                   makeEntry(1, wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment)};
    const wgpu::BindGroupLayoutDescriptor layoutDesc{
        .label = "Static bind group layout",
        .entryCount = layoutEntries.size(),
        .entries = layoutEntries.data(),
    };
    g_resources.staticBindGroupLayout = g_device.CreateBindGroupLayout(&layoutDesc);
    create_static_bind_group();
  }

  {
    constexpr std::array layoutEntries{
        // Uniform buffer (dynamic offset)
        wgpu::BindGroupLayoutEntry{
            .binding = 0,
            .visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment,
            .buffer =
                wgpu::BufferBindingLayout{
                    .type = wgpu::BufferBindingType::Uniform,
                    .hasDynamicOffset = true,
                },
        },
    };
    const wgpu::BindGroupLayoutDescriptor layoutDesc{
        .label = "Uniform bind group layout",
        .entryCount = layoutEntries.size(),
        .entries = layoutEntries.data(),
    };
    g_resources.uniformBindGroupLayout = g_device.CreateBindGroupLayout(&layoutDesc);
    create_uniform_bind_group();
  }

  gx::initialize();
#ifdef AURORA_ENABLE_RMLUI
  rmlui::initialize_pipeline();
#endif
  initialize_pipeline_cache();
}

void shutdown() {
  render_worker::synchronize();
  render_worker::shutdown();
  g_processEventsQueued.store(false, std::memory_order_release);
  g_lastPresentNs.store(0, std::memory_order_release);
  g_presentPeriodNs.store(0, std::memory_order_release);
  g_cpuFrameTimeNs.store(0, std::memory_order_release);
  g_cpuFrameStart = {};
  {
    std::lock_guard lock{g_presentStatsMutex};
    g_presentTimes.clear();
  }
  shutdown_pipeline_cache();
  depth_peek::shutdown();
  tex_copy_conv::shutdown();
  probe::shutdown();
  bloom::shutdown();
  volfog::shutdown();
  xray::shutdown();
  thermal::shutdown();
  shadow::shutdown();
  actor_shadow::shutdown();
  vfx::shutdown();
  water::shutdown();
  envfx::shutdown();
  tex_palette_conv::shutdown();
  texture_replacement::shutdown();
  gx::shutdown();
#ifdef AURORA_ENABLE_RMLUI
  rmlui::shutdown_pipeline();
#endif

  shutdown_resource_cache();
  {
    std::lock_guard lock{g_runtimeTypeMutex};
    g_runtimeDrawTypes.clear();
    g_freeDrawTypeSlots.clear();
    g_runtimeEncoderTaskTypes.clear();
    g_freeEncoderTaskTypeSlots.clear();
  }
  g_resources.vertexBuffer = {};
  g_resources.uniformBuffer = {};
  g_resources.indexBuffer = {};
  g_resources.storageBuffer = {};
  g_resources.vertexFetchTexture = {};
  g_resources.vertexFetchTextureView = {};
  g_resources.storageFetchTexture = {};
  g_resources.storageFetchTextureView = {};
  g_stagingBuffers.fill({});
  for (auto& packet : g_framePackets) {
    packet = {};
  }
  shutdown_recording();
  g_resources.staticBindGroup = {};
  g_resources.staticBindGroupLayout = {};
  g_resources.uniformBindGroup = {};
  g_resources.uniformBindGroupLayout = {};
  g_frameIndex = UINT32_MAX;
  g_frameSlots.reset();
  g_stagingSlots.reset();
  for (auto& state : g_mappingStates) {
    state.store(BufferMapState::Unmapped, std::memory_order_release);
  }
}

bool wait_for_staging_buffer(size_t slot) {
  ZoneScopedN("Wait for buffer map");
  map_staging_buffer(slot);
  while (true) {
    const auto mappingState = g_mappingStates[slot].load(std::memory_order_acquire);
    if (mappingState == BufferMapState::Mapped) {
      return true;
    }
    if (mappingState == BufferMapState::Unmapped) {
      return false;
    }
    wait_for_gpu_progress(std::chrono::milliseconds{1});
  }
}

size_t acquire_frame_slot() {
  ZoneScopedN("Acquire frame slot");
  const auto waitStart = PresentClock::now();
  while (true) {
    if (const auto slot = g_frameSlots.try_acquire()) {
      const auto waitDuration = PresentClock::now() - waitStart;
      const double waitMs = std::chrono::duration<double, std::milli>{waitDuration}.count();
      TracyPlot("aurora: frameSlotWaitMs", waitMs);
      return *slot;
    }
    wait_for_gpu_progress(std::chrono::microseconds{100});
  }
}

std::optional<size_t> acquire_mapped_staging_buffer() {
  ZoneScopedN("Acquire mapped staging buffer");
  while (true) {
    if (auto slot = g_stagingSlots.try_acquire()) {
      if (wait_for_staging_buffer(*slot)) {
        return *slot;
      }
      g_stagingSlots.release(*slot);
      return std::nullopt;
    }
    wait_for_gpu_progress(std::chrono::microseconds{100});
  }
}

void request_staging_growth(StagingKind kind, uint64_t peak) noexcept {
  auto& want = g_stagingGrowthWant[static_cast<size_t>(kind)];
  uint64_t seen = want.load(std::memory_order_relaxed);
  while (peak > seen && !want.compare_exchange_weak(seen, peak, std::memory_order_acq_rel)) {
  }
}

void grow_frame_buffer(StagingKind kind, uint64_t need, const wgpu::CommandEncoder& encoder) {
  ZoneScopedN("Grow frame buffer");
  // On the render worker, after everything queued before: what this frame encoded so far reads
  // the old buffer, then the copy makes the new one hold all of it (resident region included,
  // which stays at the same offsets) before anything encoded later reads or writes it.
  render_worker::enqueue_work([kind, need, &encoder] {
    const auto& shared = SharedBuffers[static_cast<size_t>(kind)];
    uint64_t& device = kind == StagingKind::Vertex    ? g_bufferSizes.deviceVertex
                       : kind == StagingKind::Uniform ? g_bufferSizes.deviceUniform
                       : kind == StagingKind::Index   ? g_bufferSizes.deviceIndex
                                                      : g_bufferSizes.deviceStorage;
    if (need <= device) {
      return;
    }
    const uint64_t base = g_bufferSizes.base(kind);
    const uint64_t grown = std::min(std::bit_ceil(need), g_bufferSizes.limit(kind));
    const wgpu::BufferDescriptor descriptor{
        .label = shared.label,
        .usage = shared.usage,
        .size = fetch_aligned_buffer_size(kind, base + grown),
    };
    wgpu::Buffer buffer = g_device.CreateBuffer(&descriptor);
    wgpu::Buffer& current = g_resources.*shared.member;
    encoder.CopyBufferToBuffer(current, 0, buffer, 0, base + device);
    Log.info("frame {} device buffer grown from {} to {} KiB during frame {} (needed {} KiB)", shared.name,
             device >> 10, grown >> 10, g_frameIndex, need >> 10);
    current = std::move(buffer);
    device = grown;
    if (g_vertexFetchTexture && (kind == StagingKind::Vertex || kind == StagingKind::Storage)) {
      // The fetch texture is the new buffer, so it is the size of the buffer rather than of the
      // part in use; the buffer copy above already put the old contents at the same offsets.
      create_fetch_texture(kind, base + grown);
      upload_fetch_texture_rows(encoder, kind, 0, base + grown);
    }
    if (kind == StagingKind::Uniform) {
      create_uniform_bind_group();
    } else if (kind != StagingKind::Index) {
      create_static_bind_group();
    }
    // Cached bind groups that hold the old buffer are no longer looked up; drop them soon so
    // the old buffer goes too.
    expire_unused_bind_groups_soon();
  });
  render_worker::synchronize();
}

namespace {
// Grows the staging buffers a frame spilled out of, once every frame in flight is done with them.
void grow_staging(const std::array<uint64_t, 4>& want) {
  ZoneScopedN("Grow staging");
  render_worker::synchronize();
  // Holding every staging slot means no frame or pending map uses a buffer.
  for (size_t n = 0; n < StagingBufferCount; ++n) {
    while (true) {
      if (const auto slot = g_stagingSlots.try_acquire()) {
        (void)slot;
        break;
      }
      wait_for_gpu_progress(std::chrono::microseconds{100});
    }
  }
  struct Kind {
    const char* name;
    uint64_t& size;
    uint64_t limit;
  };
  const Kind kinds[] = {
      {"vertex", g_bufferSizes.vertex, g_bufferSizes.maxVertex},
      {"uniform", g_bufferSizes.uniform, g_bufferSizes.maxUniform},
      {"index", g_bufferSizes.index, g_bufferSizes.maxIndex},
      {"storage", g_bufferSizes.storage, g_bufferSizes.maxStorage},
  };
  for (size_t k = 0; k < 4; ++k) {
    if (want[k] <= kinds[k].size) {
      continue;
    }
    const uint64_t grown = std::min(std::bit_ceil(want[k]), kinds[k].limit);
    Log.info("frame {} staging buffers grown from {} to {} KiB after a frame spilled out of them (peak {} KiB)",
             kinds[k].name, kinds[k].size >> 10, grown >> 10, want[k] >> 10);
    kinds[k].size = grown;
  }
  for (size_t i = 0; i < g_stagingBuffers.size(); ++i) {
    // A buffer still mapping completes with an error; wait it out before replacing it.
    while (g_mappingStates[i].load(std::memory_order_acquire) == BufferMapState::Mapping) {
      wait_for_gpu_progress(std::chrono::milliseconds{1});
    }
    if (g_mappingStates[i].load(std::memory_order_acquire) == BufferMapState::Mapped) {
      g_stagingBuffers[i].Unmap();
    }
    g_mappingStates[i].store(BufferMapState::Unmapped, std::memory_order_release);
    const wgpu::BufferDescriptor descriptor{
        .label = "Staging Buffer",
        .usage = wgpu::BufferUsage::MapWrite | wgpu::BufferUsage::CopySrc,
        .size = g_bufferSizes.staging(),
    };
    g_stagingBuffers[i] = g_device.CreateBuffer(&descriptor);
  }
  for (size_t i = 0; i < g_stagingBuffers.size(); ++i) {
    g_stagingSlots.release(i);
  }
}

// Takes the limits frameBuffersDeferred held back. With every staging slot held no frame is in
// flight, so the device buffers are replaced whole at the new limits between two frames, with
// nothing of any frame to copy: the mid-frame grow lost an Adreno 618's device (issue #165).
void reserve_held_frame_buffers() {
  ZoneScopedN("Reserve frame buffers");
  auto& s = g_bufferSizes;
  if (s.heldVertex == 0) {
    return;
  }
  render_worker::synchronize();
  for (size_t n = 0; n < StagingBufferCount; ++n) {
    while (true) {
      if (const auto slot = g_stagingSlots.try_acquire()) {
        (void)slot;
        break;
      }
      wait_for_gpu_progress(std::chrono::microseconds{100});
    }
  }
  s.maxVertex = s.deviceVertex = s.heldVertex;
  s.maxUniform = s.deviceUniform = s.heldUniform;
  s.maxIndex = s.deviceIndex = s.heldIndex;
  s.maxStorage = s.deviceStorage = s.heldStorage;
  s.heldVertex = s.heldUniform = s.heldIndex = s.heldStorage = 0;
  g_heldFrameBuffersTaken = true;
  render_worker::enqueue_work([] {
    for (const auto& buffer : SharedBuffers) {
      const wgpu::BufferDescriptor descriptor{
          .label = buffer.label,
          .usage = buffer.usage,
          .size = fetch_aligned_buffer_size(buffer.kind,
                                            g_bufferSizes.base(buffer.kind) + g_bufferSizes.device(buffer.kind)),
      };
      g_resources.*buffer.member = g_device.CreateBuffer(&descriptor);
    }
    if (g_vertexFetchTexture) {
      const uint64_t vertexBytes = g_bufferSizes.base(StagingKind::Vertex) + g_bufferSizes.deviceVertex;
      const uint64_t storageBytes = g_bufferSizes.base(StagingKind::Storage) + g_bufferSizes.deviceStorage;
      create_fetch_texture(StagingKind::Vertex, vertexBytes);
      create_fetch_texture(StagingKind::Storage, storageBytes);
      const wgpu::CommandEncoderDescriptor encoderDescriptor{
          .label = "Vertex fetch textures",
      };
      auto encoder = g_device.CreateCommandEncoder(&encoderDescriptor);
      upload_fetch_texture_rows(encoder, StagingKind::Vertex, 0, vertexBytes);
      upload_fetch_texture_rows(encoder, StagingKind::Storage, 0, storageBytes);
      auto commands = encoder.Finish();
      g_queue.Submit(1, &commands);
    }
    create_uniform_bind_group();
    create_static_bind_group();
    // Cached bind groups that hold the old buffers are no longer looked up.
    expire_unused_bind_groups_soon();
  });
  render_worker::synchronize();
  for (size_t i = 0; i < g_stagingBuffers.size(); ++i) {
    g_stagingSlots.release(i);
  }
  Log.info("Frame buffers: took the held limits, device {}/{}/{}/{} MiB (vertex/uniform/index/storage)",
           s.deviceVertex >> 20, s.deviceUniform >> 20, s.deviceIndex >> 20, s.deviceStorage >> 20);
}
} // namespace

bool begin_frame() {
  ZoneScoped;
  {
    std::array<uint64_t, 4> want{};
    bool grow = false;
    const uint64_t sizes[] = {g_bufferSizes.vertex, g_bufferSizes.uniform, g_bufferSizes.index, g_bufferSizes.storage};
    for (size_t k = 0; k < 4; ++k) {
      want[k] = g_stagingGrowthWant[k].exchange(0, std::memory_order_acq_rel);
      grow = grow || want[k] > sizes[k];
    }
    if (grow) {
      grow_staging(want);
    }
  }
  if (g_reserveHeldFrameBuffers.exchange(false, std::memory_order_acq_rel)) {
    reserve_held_frame_buffers();
  }
  // pace_frame_start();
  const size_t frameSlot = acquire_frame_slot();
  const auto stagingSlot = acquire_mapped_staging_buffer();
  if (!stagingSlot) {
    g_frameSlots.release(frameSlot);
    return false;
  }

  auto& frame = g_framePackets[frameSlot];
  frame = {};
  frame.frameId = g_nextFrameId++;
  frame.frameIndex = g_frameIndex;
  frame.stagingBuffer = *stagingSlot;
  size_t bufferOffset = 0;
  const auto& stagingBuf = g_stagingBuffers[*stagingSlot];
  const auto mapBuffer = [&](ByteBuffer& buf, uint64_t size) {
    if (size <= 0) {
      return;
    }
    buf = ByteBuffer{static_cast<u8*>(stagingBuf.GetMappedRange(bufferOffset, size)), static_cast<size_t>(size)};
    bufferOffset += size;
  };
  mapBuffer(frame.verts, g_bufferSizes.vertex);
  mapBuffer(frame.uniforms, g_bufferSizes.uniform);
  mapBuffer(frame.indices, g_bufferSizes.index);
  mapBuffer(frame.storage, g_bufferSizes.storage);
  if constexpr (UseTextureBuffer) {
    mapBuffer(frame.textureUpload, g_bufferSizes.textureUpload);
  }

  begin_recording(frame, frameSlot);
  begin_pipeline_frame();
  render_worker::enqueue_begin_frame(frame.frameId, [frameSlot] {
    constexpr wgpu::CommandEncoderDescriptor EncoderDescriptor{.label = "Redraw encoder"};
    g_framePackets[frameSlot].encoder = g_device.CreateCommandEncoder(&EncoderDescriptor);
    webgpu::gpu_prof::frame_begin(g_framePackets[frameSlot].encoder);
    overdraw::frame_begin();
  });
  g_cpuFrameStart = PresentClock::now();
  return true;
}

void end_frame(EndFrameCallback callback) {
  ZoneScoped;
  if (g_cpuFrameStart.time_since_epoch().count() != 0) {
    const auto cpuFrameTime = PresentClock::now() - g_cpuFrameStart;
    update_ema(g_cpuFrameTimeNs, duration_ns(cpuFrameTime));
    const double cpuFrameTimeMs = std::chrono::duration<double, std::milli>{cpuFrameTime}.count();
    TracyPlot("aurora: cpuFrameTimeMs", cpuFrameTimeMs);
  }
  const auto recorded = end_recording();
  auto& frame = *recorded.packet;
  const size_t frameSlot = recorded.frameSlot;
  const uint64_t frameId = frame.frameId;
  end_pipeline_frame();
  ++g_frameIndex;

  const size_t stagingSlot = frame.stagingBuffer;
  render_worker::enqueue_end_frame(frameId, [frameSlot, stagingSlot, callback = std::move(callback)]() mutable {
    auto& packet = g_framePackets[frameSlot];
    const uint32_t finishedFrame = packet.frameIndex;
    g_stagingBuffers[stagingSlot].Unmap();
    g_mappingStates[stagingSlot].store(BufferMapState::Unmapped, std::memory_order_release);
    auto encoder = std::move(packet.encoder);
    const auto stats = packet.stats;
    auto afterSubmitCallbacks = std::move(packet.afterSubmitCallbacks);
    packet = {};
    g_resources.stats.drawCallCount = stats.drawCallCount;
    g_resources.stats.mergedDrawCallCount = stats.mergedDrawCallCount;
    g_resources.stats.renderPassCount = stats.renderPassCount;
    g_resources.stats.lastVertSize = stats.lastVertSize;
    g_resources.stats.lastUniformSize = stats.lastUniformSize;
    g_resources.stats.lastIndexSize = stats.lastIndexSize;
    g_resources.stats.lastStorageSize = stats.lastStorageSize;
    g_resources.stats.lastTextureUploadSize = stats.lastTextureUploadSize;
    if (callback) {
      callback(encoder, std::move(afterSubmitCallbacks));
    }
    g_frameSlots.release(frameSlot);
    expire_cached_bind_groups(finishedFrame);
    map_staging_buffer(stagingSlot, true);
    process_events();
  });
}

uint32_t current_frame() noexcept { return g_frameIndex; }

void after_submit() noexcept {
  depth_peek::after_submit();
  bloom::after_submit();
}

void gpu_synchronize() { render_worker::synchronize(); }

void synchronize() { render_worker::synchronize(); }

void after_present() noexcept {
  const auto now = PresentClock::now();
  const int64_t nowNs = timestamp_ns(now);
  const int64_t previousPresentNs = g_lastPresentNs.exchange(nowNs, std::memory_order_acq_rel);
  if (previousPresentNs != 0) {
    update_ema(g_presentPeriodNs, nowNs - previousPresentNs);
    const double presentPeriodMs = static_cast<double>(g_presentPeriodNs.load(std::memory_order_acquire)) / 1'000'000.0;
    TracyPlot("aurora: presentPeriodMs", presentPeriodMs);
  }
  std::lock_guard lock{g_presentStatsMutex};
  g_presentTimes.push_back(now);
  prune_present_times(now);
}

float calculate_fps() noexcept {
  const auto now = PresentClock::now();
  std::lock_guard lock{g_presentStatsMutex};
  prune_present_times(now);
  if (g_presentTimes.size() < 2) {
    return 0.f;
  }

  const auto elapsed = std::chrono::duration<float>(g_presentTimes.back() - g_presentTimes.front()).count();
  if (elapsed <= 0.f) {
    return 0.f;
  }
  return static_cast<float>(g_presentTimes.size() - 1) / elapsed;
}
} // namespace aurora::gfx

const AuroraStats* aurora_get_stats() { return &aurora::gfx::detail::resources().stats; }

uint32_t aurora_get_frame_buffer_scale() {
  return static_cast<uint32_t>(aurora::gfx::frame_buffer_sizes().scaledVertex / aurora::gfx::VertexBufferSize);
}
void aurora_reserve_frame_buffers() {
  aurora::gfx::g_reserveHeldFrameBuffers.store(true, std::memory_order_release);
}
uint32_t aurora_get_frame_storage_scale() {
  return static_cast<uint32_t>(aurora::gfx::frame_buffer_sizes().scaledStorage / aurora::gfx::StorageBufferSize);
}
uint32_t aurora_get_resident_geometry_mib() {
  const auto& sizes = aurora::gfx::frame_buffer_sizes();
  return static_cast<uint32_t>((sizes.residentVertex + sizes.residentIndex + sizes.residentStorage) >> 20);
}
float aurora_get_fps() { return aurora::gfx::calculate_fps(); }
void aurora_get_texture_support(bool* bc, bool* astc) {
  *bc = aurora::webgpu::g_bcTexturesSupported;
  *astc = aurora::webgpu::g_astcTexturesSupported;
}
