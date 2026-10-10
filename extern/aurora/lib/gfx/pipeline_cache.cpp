#include "pipeline_cache.hpp"

#include "clear.hpp"
#include "fx_pipelines.hpp"
#include "resources.hpp"
#include "hash.hpp"
#include "../gx/pipeline.hpp"
#include "../io.hpp"
#ifdef AURORA_ENABLE_RMLUI
#include "../rmlui/pipeline.hpp"
#endif
#include "../sqlite_utils.hpp"
#include "../webgpu/gpu.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <SDL3/SDL_iostream.h>
#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <fmt/format.h>
#include <tracy/Tracy.hpp>

namespace aurora::gfx {
static Module Log("aurora::gfx::pipeline_cache");

constexpr int PipelineCacheSchema = 1;
constexpr const char* InitialPipelineCacheName = "initial_pipeline_cache.db";
// The name the seed VFS maps to AuroraConfig::pipelineCacheSeedData instead of a file.
constexpr const char* EmbeddedSeedName = ":embedded-initial-pipeline-cache:";
constexpr const char* SdlVfsName = "aurora_pipeline_cache_sdl_vfs";

struct CachedPipeline {
  wgpu::RenderPipeline pipeline;
  uint32_t firstFrameUsed = UINT32_MAX;
};

struct PendingPipeline {
  PipelineRef hash;
  uint32_t firstFrameUsed = UINT32_MAX;
  NewPipelineCallback create;
};

struct PipelineCacheWrite {
  ShaderType type;
  PipelineRef hash;
  uint32_t configVersion;
  ByteBuffer config;
  uint32_t firstFrameUsed = UINT32_MAX;
};

struct SdlVfsSqliteFile {
  sqlite3_file base;
  SDL_IOStream* io = nullptr;
};

static std::mutex g_pipelineMutex;
static bool g_hasPipelineThread = false;
static size_t g_pipelinesPerFrame = 0;
// For synchronous pipeline fallback (OpenGL)
#ifdef NDEBUG
constexpr size_t BuildPipelinesPerFrame = 5;
#else
constexpr size_t BuildPipelinesPerFrame = 1;
#endif
static std::vector<std::thread> g_pipelineThreads;
static std::atomic_bool g_pipelineThreadEnd = false;
static std::condition_variable g_pipelineQueueCv;
static std::condition_variable g_pipelineReadyCv;
static absl::flat_hash_map<PipelineRef, CachedPipeline> g_pipelines;
static std::deque<PendingPipeline> g_pipelineQueue;
static std::deque<PendingPipeline> g_backgroundPipelineQueue;
// Set by set_background_pipelines_paused: background builds wait, lookups still compile.
static bool g_backgroundPipelinesPaused = false;
static absl::flat_hash_set<PipelineRef> g_pendingPipelines;
static std::atomic_bool g_gpuCachePrunePending = false;
static std::chrono::steady_clock::time_point g_pipelineLoadStart;

static sqlite3* g_pipelineCacheDb = nullptr;
static sqlite3_stmt* g_pipelineCacheLoadStmt = nullptr;
static sqlite3_stmt* g_pipelineCacheUpsertStmt = nullptr;
static bool g_pipelineCacheBroken = false;
// A failed operation found the file damaged; pipeline_cache_abort() deletes it.
static std::atomic_bool g_pipelineCacheCorrupt = false;
static std::thread g_pipelineCacheWriterThread;
static std::condition_variable g_pipelineCacheWriterCv;
static std::mutex g_pipelineCacheWriterMutex;
static std::deque<PipelineCacheWrite> g_pipelineCacheWriteQueue;
static bool g_pipelineCacheWriterStop = false;
static int g_sdlVfsRegisterResult = SQLITE_ERROR;

static SdlVfsSqliteFile* sdl_vfs_file(sqlite3_file* file) { return reinterpret_cast<SdlVfsSqliteFile*>(file); }

static sqlite3_vfs* default_vfs(sqlite3_vfs* vfs) { return static_cast<sqlite3_vfs*>(vfs->pAppData); }

static int sdl_vfs_close(sqlite3_file* file) {
  auto* vfsFile = sdl_vfs_file(file);
  if (vfsFile->io != nullptr) {
    SDL_CloseIO(vfsFile->io);
    vfsFile->io = nullptr;
  }
  return SQLITE_OK;
}

static int sdl_vfs_read(sqlite3_file* file, void* buffer, int amount, sqlite3_int64 offset) {
  auto* vfsFile = sdl_vfs_file(file);
  if (vfsFile->io == nullptr || offset < 0 || amount < 0) {
    return SQLITE_IOERR_READ;
  }
  if (SDL_SeekIO(vfsFile->io, offset, SDL_IO_SEEK_SET) < 0) {
    return SQLITE_IOERR_SEEK;
  }

  auto* dst = static_cast<uint8_t*>(buffer);
  int total = 0;
  while (total < amount) {
    const size_t read = SDL_ReadIO(vfsFile->io, dst + total, static_cast<size_t>(amount - total));
    if (read == 0) {
      if (SDL_GetIOStatus(vfsFile->io) == SDL_IO_STATUS_EOF) {
        std::memset(dst + total, 0, static_cast<size_t>(amount - total));
        return SQLITE_IOERR_SHORT_READ;
      }
      return SQLITE_IOERR_READ;
    }
    if (read > static_cast<size_t>(std::numeric_limits<int>::max() - total)) {
      return SQLITE_IOERR_READ;
    }
    total += static_cast<int>(read);
  }
  return SQLITE_OK;
}

static int sdl_vfs_write(sqlite3_file*, const void*, int, sqlite3_int64) { return SQLITE_READONLY; }

static int sdl_vfs_truncate(sqlite3_file*, sqlite3_int64) { return SQLITE_READONLY; }

static int sdl_vfs_sync(sqlite3_file*, int) { return SQLITE_OK; }

static int sdl_vfs_file_size(sqlite3_file* file, sqlite3_int64* size) {
  auto* vfsFile = sdl_vfs_file(file);
  if (vfsFile->io == nullptr || size == nullptr) {
    return SQLITE_IOERR_FSTAT;
  }

  const auto ioSize = SDL_GetIOSize(vfsFile->io);
  if (ioSize < 0) {
    return SQLITE_IOERR_FSTAT;
  }
  *size = static_cast<sqlite3_int64>(ioSize);
  return SQLITE_OK;
}

static int sdl_vfs_lock(sqlite3_file*, int) { return SQLITE_OK; }

static int sdl_vfs_unlock(sqlite3_file*, int) { return SQLITE_OK; }

static int sdl_vfs_check_reserved_lock(sqlite3_file*, int* reserved) {
  if (reserved != nullptr) {
    *reserved = 0;
  }
  return SQLITE_OK;
}

static int sdl_vfs_file_control(sqlite3_file*, int op, void* arg) {
  switch (op) {
  case SQLITE_FCNTL_LOCKSTATE:
    *static_cast<int*>(arg) = SQLITE_LOCK_NONE;
    return SQLITE_OK;
  case SQLITE_FCNTL_HAS_MOVED:
    *static_cast<int*>(arg) = 0;
    return SQLITE_OK;
  case SQLITE_FCNTL_SIZE_HINT:
    return SQLITE_OK;
  default:
    return SQLITE_NOTFOUND;
  }
}

static int sdl_vfs_sector_size(sqlite3_file*) { return 4096; }

static int sdl_vfs_device_characteristics(sqlite3_file*) {
  return SQLITE_IOCAP_IMMUTABLE | SQLITE_IOCAP_UNDELETABLE_WHEN_OPEN;
}

static constexpr sqlite3_io_methods SdlVfsIoMethods{
    .iVersion = 1,
    .xClose = sdl_vfs_close,
    .xRead = sdl_vfs_read,
    .xWrite = sdl_vfs_write,
    .xTruncate = sdl_vfs_truncate,
    .xSync = sdl_vfs_sync,
    .xFileSize = sdl_vfs_file_size,
    .xLock = sdl_vfs_lock,
    .xUnlock = sdl_vfs_unlock,
    .xCheckReservedLock = sdl_vfs_check_reserved_lock,
    .xFileControl = sdl_vfs_file_control,
    .xSectorSize = sdl_vfs_sector_size,
    .xDeviceCharacteristics = sdl_vfs_device_characteristics,
};

static int sdl_vfs_open(sqlite3_vfs*, sqlite3_filename name, sqlite3_file* file, int flags, int* outFlags) {
  auto* vfsFile = sdl_vfs_file(file);
  vfsFile->base.pMethods = nullptr;
  vfsFile->io = nullptr;

  if (name == nullptr || (flags & SQLITE_OPEN_READWRITE) != 0 || (flags & SQLITE_OPEN_READONLY) == 0) {
    return SQLITE_CANTOPEN;
  }

  if (std::strcmp(name, EmbeddedSeedName) == 0) {
    vfsFile->io = g_config.pipelineCacheSeedData != nullptr
                      ? SDL_IOFromConstMem(g_config.pipelineCacheSeedData, g_config.pipelineCacheSeedSize)
                      : nullptr;
  } else {
    vfsFile->io = SDL_IOFromFile(name, "rb");
  }
  if (vfsFile->io == nullptr) {
    return SQLITE_CANTOPEN;
  }

  vfsFile->base.pMethods = &SdlVfsIoMethods;
  if (outFlags != nullptr) {
    *outFlags = SQLITE_OPEN_READONLY;
  }
  return SQLITE_OK;
}

static int sdl_vfs_delete(sqlite3_vfs*, const char*, int) { return SQLITE_READONLY; }

static int sdl_vfs_access(sqlite3_vfs*, const char* name, int flags, int* result) {
  if (result == nullptr) {
    return SQLITE_IOERR_ACCESS;
  }
  if (name == nullptr || flags == SQLITE_ACCESS_READWRITE) {
    *result = 0;
    return SQLITE_OK;
  }

  auto* io = SDL_IOFromFile(name, "rb");
  *result = io != nullptr ? 1 : 0;
  if (io != nullptr) {
    SDL_CloseIO(io);
  }
  return SQLITE_OK;
}

static int sdl_vfs_full_pathname(sqlite3_vfs*, const char* name, int outSize, char* out) {
  if (name == nullptr || out == nullptr || outSize <= 0) {
    return SQLITE_CANTOPEN;
  }
  sqlite3_snprintf(outSize, out, "%s", name);
  return SQLITE_OK;
}

static void* sdl_vfs_dl_open(sqlite3_vfs*, const char*) { return nullptr; }

static void sdl_vfs_dl_error(sqlite3_vfs*, int bytes, char* message) {
  if (message != nullptr && bytes > 0) {
    sqlite3_snprintf(bytes, message, "%s", "Dynamic loading is unsupported");
  }
}

static void (*sdl_vfs_dl_sym(sqlite3_vfs*, void*, const char*))(void) { return nullptr; }

static void sdl_vfs_dl_close(sqlite3_vfs*, void*) {}

static int sdl_vfs_randomness(sqlite3_vfs* vfs, int bytes, char* out) {
  if (auto* base = default_vfs(vfs); base != nullptr && base->xRandomness != nullptr) {
    return base->xRandomness(base, bytes, out);
  }
  if (out != nullptr && bytes > 0) {
    std::memset(out, 0, static_cast<size_t>(bytes));
  }
  return bytes;
}

static int sdl_vfs_sleep(sqlite3_vfs* vfs, int microseconds) {
  if (auto* base = default_vfs(vfs); base != nullptr && base->xSleep != nullptr) {
    return base->xSleep(base, microseconds);
  }
  return microseconds;
}

static int sdl_vfs_current_time(sqlite3_vfs* vfs, double* time) {
  if (auto* base = default_vfs(vfs); base != nullptr && base->xCurrentTime != nullptr) {
    return base->xCurrentTime(base, time);
  }
  if (time != nullptr) {
    *time = 2440587.5;
  }
  return SQLITE_OK;
}

static int sdl_vfs_get_last_error(sqlite3_vfs*, int, char*) { return SQLITE_OK; }

static bool register_sdl_vfs() {
  static std::once_flag registerOnce;
  std::call_once(registerOnce, [] {
    auto* baseVfs = sqlite3_vfs_find(nullptr);
    static sqlite3_vfs sdlVfs{
        .iVersion = 1,
        .szOsFile = static_cast<int>(sizeof(SdlVfsSqliteFile)),
        .mxPathname = 4096,
        .pNext = nullptr,
        .zName = SdlVfsName,
        .pAppData = baseVfs,
        .xOpen = sdl_vfs_open,
        .xDelete = sdl_vfs_delete,
        .xAccess = sdl_vfs_access,
        .xFullPathname = sdl_vfs_full_pathname,
        .xDlOpen = sdl_vfs_dl_open,
        .xDlError = sdl_vfs_dl_error,
        .xDlSym = sdl_vfs_dl_sym,
        .xDlClose = sdl_vfs_dl_close,
        .xRandomness = sdl_vfs_randomness,
        .xSleep = sdl_vfs_sleep,
        .xCurrentTime = sdl_vfs_current_time,
        .xGetLastError = sdl_vfs_get_last_error,
    };
    g_sdlVfsRegisterResult = sqlite3_vfs_register(&sdlVfs, 0);
  });
  return g_sdlVfsRegisterResult == SQLITE_OK;
}

#if defined(__cpp_lib_atomic_ref)
static std::atomic_ref queuedPipelines{detail::resources().stats.queuedPipelines};
static std::atomic_ref createdPipelines{detail::resources().stats.createdPipelines};
#else
struct AtomicStatRef {
  uint32_t& ref;
  uint32_t operator++() { return __atomic_add_fetch(&ref, 1, __ATOMIC_RELAXED); }
  uint32_t operator--() { return __atomic_sub_fetch(&ref, 1, __ATOMIC_RELAXED); }
  uint32_t operator++(int) { return __atomic_fetch_add(&ref, 1, __ATOMIC_RELAXED); }
  uint32_t operator--(int) { return __atomic_fetch_sub(&ref, 1, __ATOMIC_RELAXED); }
  uint32_t operator=(uint32_t val) {
    __atomic_store_n(&ref, val, __ATOMIC_RELAXED);
    return val;
  }
};
static AtomicStatRef queuedPipelines{detail::resources().stats.queuedPipelines};
static AtomicStatRef createdPipelines{detail::resources().stats.createdPipelines};
#endif

template <typename PipelineConfig>
static PipelineCacheWrite make_pipeline_cache_write(ShaderType type, PipelineRef hash, const PipelineConfig& config,
                                                    uint32_t firstFrameUsed) {
  static_assert(std::has_unique_object_representations_v<PipelineConfig>);

  PipelineCacheWrite write{
      .type = type,
      .hash = hash,
      .configVersion = config.version,
      .config = ByteBuffer(sizeof(config)),
      .firstFrameUsed = firstFrameUsed,
  };
  std::memcpy(write.config.data(), &config, sizeof(config));
  return write;
}

static void enqueue_pipeline_cache_write(PipelineCacheWrite write) {
  if (g_pipelineCacheBroken || g_pipelineCacheDb == nullptr) {
    return;
  }

  {
    std::lock_guard lock{g_pipelineCacheWriterMutex};
    g_pipelineCacheWriteQueue.emplace_back(std::move(write));
  }
  g_pipelineCacheWriterCv.notify_one();
}

template <typename Queue>
static auto find_pending_pipeline(Queue& queue, PipelineRef hash) {
  return std::find_if(queue.begin(), queue.end(), [=](const PendingPipeline& pending) { return pending.hash == hash; });
}

enum class PipelinePriority {
  Background, // loaded from cache
  Normal,     // async skip draw
  Blocking,   // block until compiled
};

static PendingPipeline* touch_pending_pipeline(PipelineRef hash, PipelinePriority priority) {
  auto priorityIt = find_pending_pipeline(g_pipelineQueue, hash);
  if (priorityIt != g_pipelineQueue.end()) {
    return &*priorityIt;
  }

  auto backgroundIt = find_pending_pipeline(g_backgroundPipelineQueue, hash);
  if (backgroundIt == g_backgroundPipelineQueue.end()) {
    return nullptr;
  }
  switch (priority) {
  case PipelinePriority::Background:
    return &*backgroundIt;
  case PipelinePriority::Normal:
    g_pipelineQueue.emplace_back(std::move(*backgroundIt));
    g_backgroundPipelineQueue.erase(backgroundIt);
    return &g_pipelineQueue.back();
  case PipelinePriority::Blocking:
    g_pipelineQueue.emplace_front(std::move(*backgroundIt));
    g_backgroundPipelineQueue.erase(backgroundIt);
    return &g_pipelineQueue.front();
  }
  return nullptr;
}

static std::optional<PendingPipeline> take_pending_pipeline(PipelineRef hash) {
  auto priorityIt = find_pending_pipeline(g_pipelineQueue, hash);
  if (priorityIt != g_pipelineQueue.end()) {
    PendingPipeline pending = std::move(*priorityIt);
    g_pipelineQueue.erase(priorityIt);
    g_pendingPipelines.erase(hash);
    return pending;
  }

  auto backgroundIt = find_pending_pipeline(g_backgroundPipelineQueue, hash);
  if (backgroundIt != g_backgroundPipelineQueue.end()) {
    PendingPipeline pending = std::move(*backgroundIt);
    g_backgroundPipelineQueue.erase(backgroundIt);
    g_pendingPipelines.erase(hash);
    return pending;
  }

  return std::nullopt;
}

// Slow-pipeline diagnostics (issue #8 hangs in pipeline creation): every GX
// render pipeline creation is timed here, slow ones are named, and a summary
// lands in the log as the count crosses 100, 500 and 1000.
static std::atomic<uint64_t> g_compileCount = 0;
static std::atomic<uint64_t> g_compileTotalMs = 0;
static std::atomic<uint64_t> g_compileSlowestMs = 0;
static std::atomic<uint64_t> g_compileFailures = 0;
constexpr int64_t kSlowPipelineMs = 200;

static wgpu::RenderPipeline create_pipeline_timed(PipelineRef hash, NewPipelineCallback&& create) {
  const auto start = std::chrono::steady_clock::now();
  wgpu::RenderPipeline pipeline = create();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
  const uint64_t count = g_compileCount.fetch_add(1, std::memory_order_relaxed) + 1;
  g_compileTotalMs.fetch_add(static_cast<uint64_t>(ms.count()), std::memory_order_relaxed);
  uint64_t slowest = g_compileSlowestMs.load(std::memory_order_relaxed);
  while (static_cast<uint64_t>(ms.count()) > slowest &&
         !g_compileSlowestMs.compare_exchange_weak(slowest, static_cast<uint64_t>(ms.count()),
                                                   std::memory_order_relaxed)) {
  }
  if (!pipeline) {
    g_compileFailures.fetch_add(1, std::memory_order_relaxed);
    Log.error("pipeline: creation failed (hash {:016x})", hash);
  } else if (ms.count() > kSlowPipelineMs) {
    Log.warn("pipeline: compile took {} ms (hash {:016x})", ms.count(), hash);
  }
  if (count == 100 || count == 500 || count == 1000) {
    Log.info("pipelines: {} compiled, total {} ms, slowest {} ms, {} failed", count,
             g_compileTotalMs.load(std::memory_order_relaxed), g_compileSlowestMs.load(std::memory_order_relaxed),
             g_compileFailures.load(std::memory_order_relaxed));
  }
  return pipeline;
}

static void notify_pipeline_ready(bool queued) {
  ++createdPipelines;
  if (queued && --queuedPipelines == 0 && g_gpuCachePrunePending.exchange(false, std::memory_order_acq_rel)) {
    Log.info("pipeline cache: {} pipelines ready {} ms after startup", static_cast<uint32_t>(createdPipelines),
             std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                   g_pipelineLoadStart)
                 .count());
    // Prune GPU cache entries after fully loading the pipeline cache.
    webgpu::cache_prune();
  }
  g_pipelineReadyCv.notify_all();
}

static PipelineRef g_lastPipelineRef = std::numeric_limits<PipelineRef>::max();
static std::atomic_bool g_dropPipelines = false;

// drop_pipelines() only raises a flag; the next lookup (on the thread that builds draws) does the
// work, so no draw loses its pipeline in the middle of one.
static void apply_pipeline_drop() {
  std::scoped_lock guard{g_pipelineMutex};
  g_pipelines.clear();
  g_pipelineQueue.clear();
  g_backgroundPipelineQueue.clear();
  g_pendingPipelines.clear();
  g_lastPipelineRef = std::numeric_limits<PipelineRef>::max();
}

template <typename PipelineConfig>
static PipelineRef find_pipeline_impl(ShaderType type, const PipelineConfig& config, NewPipelineCallback&& cb,
                                      PipelinePriority priority = PipelinePriority::Normal,
                                      std::optional<uint32_t> firstFrameUsedOverride = std::nullopt,
                                      bool renderThread = false) {
  ZoneScoped;

  // The pipeline drop and g_lastPipelineRef belong to the thread that records draws.
  if (!renderThread && g_dropPipelines.exchange(false, std::memory_order_acq_rel)) {
    apply_pipeline_drop();
  }
  const PipelineRef hash = xxh3_hash(config, static_cast<HashType>(type));
  const bool blocking = priority == PipelinePriority::Blocking;
  if (!blocking && hash == g_lastPipelineRef) {
    return g_lastPipelineRef;
  }
  if (!renderThread) {
    g_lastPipelineRef = hash;
  }
  const uint32_t firstFrameUsed = firstFrameUsedOverride.value_or(current_frame());
  bool notifyWorker = false;
  bool persist = priority != PipelinePriority::Background;
  bool pipelineReady = false;
  bool createdPipeline = false;
  bool queued = false;
  std::optional<PipelineCacheWrite> cacheWrite;
  {
    std::scoped_lock guard{g_pipelineMutex};
    auto pipelineIt = g_pipelines.find(hash);
    if (pipelineIt != g_pipelines.end()) {
      pipelineReady = true;
      if (persist && firstFrameUsed < pipelineIt->second.firstFrameUsed) {
        pipelineIt->second.firstFrameUsed = firstFrameUsed;
        cacheWrite = make_pipeline_cache_write(type, hash, config, firstFrameUsed);
      }
    } else if (g_pendingPipelines.contains(hash)) {
      if (blocking && !g_hasPipelineThread) {
        auto pending = take_pending_pipeline(hash);
        if (pending) {
          if (firstFrameUsed < pending->firstFrameUsed) {
            pending->firstFrameUsed = firstFrameUsed;
            if (persist) {
              cacheWrite = make_pipeline_cache_write(type, hash, config, firstFrameUsed);
            }
          }
          g_pipelines.try_emplace(hash, CachedPipeline{
                                            .pipeline = create_pipeline_timed(hash, std::move(pending->create)),
                                            .firstFrameUsed = pending->firstFrameUsed,
                                        });
          pipelineReady = true;
          ++g_pipelinesPerFrame;
          createdPipeline = true;
          queued = true;
        }
      } else {
        auto* pending = touch_pending_pipeline(hash, priority);
        if (pending != nullptr && firstFrameUsed < pending->firstFrameUsed) {
          pending->firstFrameUsed = firstFrameUsed;
          if (persist) {
            cacheWrite = make_pipeline_cache_write(type, hash, config, firstFrameUsed);
          }
        } else if (pending == nullptr && persist) {
          cacheWrite = make_pipeline_cache_write(type, hash, config, firstFrameUsed);
        }
        notifyWorker = priority != PipelinePriority::Background;
      }
    } else if (!g_hasPipelineThread && (blocking || g_pipelinesPerFrame < BuildPipelinesPerFrame)) {
      g_pipelines.try_emplace(hash, CachedPipeline{
                                        .pipeline = create_pipeline_timed(hash, std::move(cb)),
                                        .firstFrameUsed = firstFrameUsed,
                                    });
      pipelineReady = true;
      if (persist) {
        cacheWrite = make_pipeline_cache_write(type, hash, config, firstFrameUsed);
      }
      ++g_pipelinesPerFrame;
      createdPipeline = true;
    } else {
      PendingPipeline pending{
          .hash = hash,
          .firstFrameUsed = firstFrameUsed,
          .create = std::move(cb),
      };
      switch (priority) {
      case PipelinePriority::Background:
        g_backgroundPipelineQueue.emplace_back(std::move(pending));
        break;
      case PipelinePriority::Normal:
        g_pipelineQueue.emplace_back(std::move(pending));
        break;
      case PipelinePriority::Blocking:
        g_pipelineQueue.emplace_front(std::move(pending));
        break;
      }
      g_pendingPipelines.insert(hash);
      if (persist) {
        cacheWrite = make_pipeline_cache_write(type, hash, config, firstFrameUsed);
      }
      ++queuedPipelines;
      notifyWorker = true;
    }
  }

  if (cacheWrite) {
    enqueue_pipeline_cache_write(std::move(*cacheWrite));
    cacheWrite.reset();
  }

  if (createdPipeline) {
    notify_pipeline_ready(queued);
  }

  if (notifyWorker) {
    g_pipelineQueueCv.notify_one();
  }

  if (blocking && !pipelineReady) {
    std::unique_lock lock{g_pipelineMutex};
    g_pipelineReadyCv.wait(lock, [=] { return g_pipelines.contains(hash) || g_pipelineThreadEnd; });
    auto pipelineIt = g_pipelines.find(hash);
    if (pipelineIt != g_pipelines.end() && persist && firstFrameUsed < pipelineIt->second.firstFrameUsed) {
      pipelineIt->second.firstFrameUsed = firstFrameUsed;
      cacheWrite = make_pipeline_cache_write(type, hash, config, firstFrameUsed);
    }
  }

  if (cacheWrite) {
    enqueue_pipeline_cache_write(std::move(*cacheWrite));
  }

  return hash;
}

static std::string pipeline_cache_path() {
  return io::fs_path_to_string(io::fs_path_from_string(g_config.cachePath) / "pipeline_cache.db");
}

static void note_pipeline_cache_corrupt() {
  if (g_pipelineCacheDb != nullptr && sqlite::is_corrupt(g_pipelineCacheDb)) {
    g_pipelineCacheCorrupt = true;
  }
}

// Stops using the cache for this session. A damaged database is deleted so the
// next start rebuilds it.
static void pipeline_cache_abort() {
  g_pipelineCacheBroken = true;
  note_pipeline_cache_corrupt();
  if (g_pipelineCacheLoadStmt != nullptr) {
    sqlite3_finalize(g_pipelineCacheLoadStmt);
    g_pipelineCacheLoadStmt = nullptr;
  }
  if (g_pipelineCacheUpsertStmt != nullptr) {
    sqlite3_finalize(g_pipelineCacheUpsertStmt);
    g_pipelineCacheUpsertStmt = nullptr;
  }
  if (g_pipelineCacheDb != nullptr) {
    sqlite3_close(g_pipelineCacheDb);
    g_pipelineCacheDb = nullptr;
  }
  if (g_pipelineCacheCorrupt.exchange(false)) {
    Log.warn("Pipeline cache is damaged; deleting it");
    sqlite::delete_db_files(pipeline_cache_path(), Log);
  }
}

static bool write_pipeline_cache_record(const PipelineCacheWrite& write);

static std::string pipeline_cache_seed_path() {
  if (g_config.pipelineCacheSeedData != nullptr && g_config.pipelineCacheSeedSize != 0) {
    return EmbeddedSeedName;
  }
  if (g_config.resourcesPath == nullptr || g_config.resourcesPath[0] == '\0') {
    return InitialPipelineCacheName;
  }

  std::string path{g_config.resourcesPath};
  if (path.back() != '/' && path.back() != '\\') {
    path += '/';
  }
  path += InitialPipelineCacheName;
  return path;
}

static sqlite3* open_pipeline_cache_seed_db(const std::string& path) {
  if (!register_sdl_vfs()) {
    Log.warn("Failed to register SDL pipeline cache seed VFS");
    return nullptr;
  }

  sqlite3* seedDb = nullptr;
  const auto ret = sqlite3_open_v2(path.c_str(), &seedDb, SQLITE_OPEN_READONLY | SQLITE_OPEN_PRIVATECACHE, SdlVfsName);
  if (ret != SQLITE_OK) {
    if (seedDb != nullptr) {
      sqlite3_close(seedDb);
    }
    Log.info("No bundled initial pipeline cache found at '{}'", path);
    return nullptr;
  }

  bool schemaMatch = false;
  const auto schemaQuery = fmt::format("SELECT 1 FROM aurora_schema WHERE value = {}", PipelineCacheSchema);
  const auto schemaRet =
      sqlite::exec(seedDb, schemaQuery.c_str(), [&schemaMatch](int, char**, char**) { schemaMatch = true; });
  if (schemaRet != SQLITE_OK) {
    Log.warn("Failed to read bundled pipeline cache schema from '{}': {}", path, sqlite3_errmsg(seedDb));
    sqlite3_close(seedDb);
    return nullptr;
  }
  if (!schemaMatch) {
    Log.warn("Bundled pipeline cache '{}' does not use schema version {}", path, PipelineCacheSchema);
    sqlite3_close(seedDb);
    return nullptr;
  }

  return seedDb;
}

static void seed_pipeline_cache() {
  if (g_config.pipelineCacheNoSeed || g_pipelineCacheBroken || g_pipelineCacheDb == nullptr ||
      g_pipelineCacheUpsertStmt == nullptr) {
    return;
  }

  const auto seedPath = pipeline_cache_seed_path();
  sqlite3* seedDb = open_pipeline_cache_seed_db(seedPath);
  if (seedDb == nullptr) {
    return;
  }

  sqlite3_stmt* seedStmt = nullptr;
  auto closeSeed = [&] {
    if (seedStmt != nullptr) {
      sqlite3_finalize(seedStmt);
      seedStmt = nullptr;
    }
    sqlite3_close(seedDb);
  };

  auto ret = sqlite3_prepare_v3(seedDb,
                                "SELECT type, hash, config_version, config_size, config, first_frame_used "
                                "FROM pipeline_cache",
                                -1, 0, &seedStmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.warn("Failed to read bundled pipeline cache rows from '{}': {}", seedPath, sqlite3_errmsg(seedDb));
    closeSeed();
    return;
  }

  bool writeFailed = false;
  bool readFailed = false;
  uint32_t mergedRows = 0;
  uint32_t skippedRows = 0;
  {
    sqlite::Transaction tx(g_pipelineCacheDb, Log, true);
    if (!tx) {
      Log.error("Failed to begin pipeline cache seed transaction");
      closeSeed();
      pipeline_cache_abort();
      return;
    }

    while ((ret = sqlite3_step(seedStmt)) == SQLITE_ROW) {
      const auto typeValue = sqlite3_column_int(seedStmt, 0);
      const auto hashValue = sqlite3_column_int64(seedStmt, 1);
      const auto configVersionValue = sqlite3_column_int(seedStmt, 2);
      const auto configSizeValue = sqlite3_column_int(seedStmt, 3);
      const auto* configBlob = static_cast<const uint8_t*>(sqlite3_column_blob(seedStmt, 4));
      const auto configBlobSize = sqlite3_column_bytes(seedStmt, 4);
      const auto firstFrameUsedValue = sqlite3_column_int64(seedStmt, 5);
      constexpr auto MaxShaderTypeValue = std::numeric_limits<std::underlying_type_t<ShaderType>>::max();
      if (typeValue < 0 || typeValue > MaxShaderTypeValue || configVersionValue < 0 || configSizeValue < 0 ||
          configSizeValue != configBlobSize || (configBlobSize > 0 && configBlob == nullptr) ||
          firstFrameUsedValue < 0 || firstFrameUsedValue > std::numeric_limits<uint32_t>::max()) {
        ++skippedRows;
        continue;
      }

      PipelineCacheWrite write{
          .type = static_cast<ShaderType>(typeValue),
          .hash = static_cast<PipelineRef>(hashValue),
          .configVersion = static_cast<uint32_t>(configVersionValue),
          .config = ByteBuffer(static_cast<size_t>(configBlobSize)),
          .firstFrameUsed = static_cast<uint32_t>(firstFrameUsedValue),
      };
      if (configBlobSize > 0) {
        std::memcpy(write.config.data(), configBlob, static_cast<size_t>(configBlobSize));
      }

      if (!write_pipeline_cache_record(write)) {
        writeFailed = true;
        break;
      }
      ++mergedRows;
    }

    if (!writeFailed && ret != SQLITE_DONE) {
      Log.warn("Failed while reading bundled pipeline cache rows from '{}': {}", seedPath, sqlite3_errmsg(seedDb));
      readFailed = true;
    }

    if (!writeFailed && !readFailed) {
      tx.commit();
    }
  }

  closeSeed();

  if (writeFailed) {
    pipeline_cache_abort();
    return;
  }

  if (!readFailed) {
    Log.info("Seeded pipeline cache from '{}' ({} rows merged, {} rows skipped)", seedPath, mergedRows, skippedRows);
  }
}

static bool prepare_pipeline_cache_db() {
  if (g_pipelineCacheBroken) {
    return false;
  }
  if (g_pipelineCacheDb != nullptr) {
    return true;
  }

  const auto path = pipeline_cache_path();
  auto ret = sqlite::open_cache_db(path, &g_pipelineCacheDb, Log);
  if (ret != SQLITE_OK) {
    Log.error("Failed to open pipeline cache database {}", path);
    pipeline_cache_abort();
    return false;
  }

  ret = sqlite::exec(g_pipelineCacheDb, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
  if (ret != SQLITE_OK) {
    Log.error("Failed to set pipeline cache pragmas: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return false;
  }

  bool schemaFailed = false;
  {
    sqlite::Transaction tx(g_pipelineCacheDb, Log, true);
    if (!tx) {
      Log.error("Failed to begin pipeline cache schema transaction");
      schemaFailed = true;
    } else {
      ret = sqlite::exec(g_pipelineCacheDb, "CREATE TABLE IF NOT EXISTS aurora_schema(value INTEGER);");
      if (ret != SQLITE_OK) {
        Log.error("Failed to create pipeline cache schema table: {}", sqlite3_errmsg(g_pipelineCacheDb));
        schemaFailed = true;
      }
    }

    bool schemaMatch = false;
    if (!schemaFailed) {
      const auto schemaQuery = fmt::format("SELECT 1 FROM aurora_schema WHERE value = {}", PipelineCacheSchema);
      ret = sqlite::exec(g_pipelineCacheDb, schemaQuery.c_str(),
                         [&schemaMatch](int, char**, char**) { schemaMatch = true; });
      if (ret != SQLITE_OK) {
        Log.error("Failed to read pipeline cache schema version: {}", sqlite3_errmsg(g_pipelineCacheDb));
        schemaFailed = true;
      }
    }

    if (!schemaFailed && !schemaMatch) {
      const auto schemaSql = fmt::format(
          R"(DROP TABLE IF EXISTS pipeline_cache;
CREATE TABLE pipeline_cache (
  type INTEGER NOT NULL,
  hash INTEGER NOT NULL,
  config_version INTEGER NOT NULL,
  config_size INTEGER NOT NULL,
  config BLOB NOT NULL,
  first_frame_used INTEGER NOT NULL,
  PRIMARY KEY (type, hash)
);
CREATE INDEX pipeline_cache_load_order_idx
  ON pipeline_cache(type, config_version, first_frame_used);
DELETE FROM aurora_schema;
INSERT INTO aurora_schema VALUES ({});)",
          PipelineCacheSchema);
      ret = sqlite::exec(g_pipelineCacheDb, schemaSql.c_str());
      if (ret != SQLITE_OK) {
        Log.error("Failed to initialize pipeline cache schema: {}", sqlite3_errmsg(g_pipelineCacheDb));
        schemaFailed = true;
      }
    }

    if (!schemaFailed) {
      tx.commit();
    }
  }

  if (schemaFailed) {
    pipeline_cache_abort();
    return false;
  }

  ret = sqlite3_prepare_v3(g_pipelineCacheDb,
                           "SELECT config, first_frame_used FROM pipeline_cache "
                           "WHERE type = ? AND config_version = ? "
                           "ORDER BY first_frame_used ASC, rowid ASC",
                           -1, SQLITE_PREPARE_PERSISTENT, &g_pipelineCacheLoadStmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.error("Failed to prepare pipeline cache load statement: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return false;
  }

  ret = sqlite3_prepare_v3(
      g_pipelineCacheDb,
      "INSERT INTO pipeline_cache (type, hash, config_version, config_size, config, first_frame_used) "
      "VALUES (?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(type, hash) DO UPDATE SET "
      "config_version = excluded.config_version, "
      "config_size = excluded.config_size, "
      "config = excluded.config, "
      "first_frame_used = MIN(pipeline_cache.first_frame_used, excluded.first_frame_used)",
      -1, SQLITE_PREPARE_PERSISTENT, &g_pipelineCacheUpsertStmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.error("Failed to prepare pipeline cache upsert statement: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return false;
  }

  seed_pipeline_cache();
  if (g_pipelineCacheBroken) {
    return false;
  }

  return true;
}

static void prune_old_pipeline_cache_versions() {
  if (!prepare_pipeline_cache_db()) {
    return;
  }

  const auto clearDelete = fmt::format("DELETE FROM pipeline_cache WHERE type = {} AND config_version < {}",
                                       underlying(ShaderType::Clear), clear::ClearPipelineConfigVersion);
  auto ret = sqlite::exec(g_pipelineCacheDb, clearDelete.c_str());
  if (ret != SQLITE_OK) {
    Log.error("Failed to prune clear pipeline cache rows: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return;
  }

  const auto gxDelete = fmt::format("DELETE FROM pipeline_cache WHERE type = {} AND config_version < {}",
                                    underlying(ShaderType::GX), gx::GXPipelineConfigVersion);
  ret = sqlite::exec(g_pipelineCacheDb, gxDelete.c_str());
  if (ret != SQLITE_OK) {
    Log.error("Failed to prune GX pipeline cache rows: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return;
  }

#ifdef AURORA_ENABLE_RMLUI
  const auto rmlDelete = fmt::format("DELETE FROM pipeline_cache WHERE type = {} AND config_version < {}",
                                     underlying(ShaderType::Rml), rmlui::RmlPipelineConfigVersion);
  ret = sqlite::exec(g_pipelineCacheDb, rmlDelete.c_str());
  if (ret != SQLITE_OK) {
    Log.error("Failed to prune RmlUi pipeline cache rows: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return;
  }
#endif

  const auto vfxDelete = fmt::format("DELETE FROM pipeline_cache WHERE type = {} AND config_version < {}",
                                     underlying(ShaderType::Vfx), vfx::VfxPipelineConfigVersion);
  ret = sqlite::exec(g_pipelineCacheDb, vfxDelete.c_str());
  if (ret != SQLITE_OK) {
    Log.error("Failed to prune VFX pipeline cache rows: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return;
  }

  const auto waterDelete = fmt::format("DELETE FROM pipeline_cache WHERE type = {} AND config_version < {}",
                                       underlying(ShaderType::Water), water::WaterPipelineConfigVersion);
  ret = sqlite::exec(g_pipelineCacheDb, waterDelete.c_str());
  if (ret != SQLITE_OK) {
    Log.error("Failed to prune water pipeline cache rows: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
  }
}

static bool write_pipeline_cache_record(const PipelineCacheWrite& write) {
  const auto fail = [&]() {
    note_pipeline_cache_corrupt();
    sqlite3_reset(g_pipelineCacheUpsertStmt);
    sqlite3_clear_bindings(g_pipelineCacheUpsertStmt);
    return false;
  };

  auto ret = sqlite3_bind_int(g_pipelineCacheUpsertStmt, 1, underlying(write.type));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache type: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }
  ret = sqlite3_bind_int64(g_pipelineCacheUpsertStmt, 2, static_cast<sqlite3_int64>(write.hash));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache hash: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }
  ret = sqlite3_bind_int(g_pipelineCacheUpsertStmt, 3, static_cast<int>(write.configVersion));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache config version: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }
  ret = sqlite3_bind_int(g_pipelineCacheUpsertStmt, 4, static_cast<int>(write.config.size()));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache config size: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }
  ret = sqlite3_bind_blob64(g_pipelineCacheUpsertStmt, 5, write.config.data(), write.config.size(), SQLITE_TRANSIENT);
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache config blob: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }
  ret = sqlite3_bind_int64(g_pipelineCacheUpsertStmt, 6, static_cast<sqlite3_int64>(write.firstFrameUsed));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache first-frame-used: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }

  ret = sqlite3_step(g_pipelineCacheUpsertStmt);
  if (ret != SQLITE_DONE) {
    Log.error("Failed to upsert pipeline cache row: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }

  sqlite3_reset(g_pipelineCacheUpsertStmt);
  sqlite3_clear_bindings(g_pipelineCacheUpsertStmt);
  return true;
}

static void pipeline_cache_writer() {
#ifdef TRACY_ENABLE
  tracy::SetThreadName("Pipeline cache writer thread");
#endif

  while (true) {
    std::deque<PipelineCacheWrite> batch;
    {
      std::unique_lock lock{g_pipelineCacheWriterMutex};
      g_pipelineCacheWriterCv.wait(lock,
                                   [] { return g_pipelineCacheWriterStop || !g_pipelineCacheWriteQueue.empty(); });
      if (g_pipelineCacheWriterStop && g_pipelineCacheWriteQueue.empty()) {
        return;
      }
      batch.swap(g_pipelineCacheWriteQueue);
    }

    bool writeFailed = false;
    {
      sqlite::Transaction tx(g_pipelineCacheDb, Log, true);
      if (!tx) {
        Log.error("Failed to begin pipeline cache write transaction");
        writeFailed = true;
      } else {
        for (const auto& write : batch) {
          if (!write_pipeline_cache_record(write)) {
            writeFailed = true;
            break;
          }
        }

        if (!writeFailed) {
          tx.commit();
        }
      }
    }

    if (writeFailed) {
      pipeline_cache_abort();
      return;
    }
  }
}

static void pipeline_worker() {
#ifdef TRACY_ENABLE
  tracy::SetThreadName("Pipeline compilation thread");
#endif

  while (g_hasPipelineThread || g_pipelinesPerFrame < BuildPipelinesPerFrame) {
    PendingPipeline pending;
    {
      std::unique_lock lock{g_pipelineMutex};
      if (g_hasPipelineThread) {
        // Several workers share the queues, so check them again under the lock every time.
        g_pipelineQueueCv.wait(lock, [] {
          return !g_pipelineQueue.empty() || (!g_backgroundPipelinesPaused && !g_backgroundPipelineQueue.empty()) ||
                 g_pipelineThreadEnd;
        });
      } else if (g_pipelineQueue.empty() && (g_backgroundPipelinesPaused || g_backgroundPipelineQueue.empty())) {
        return;
      }
      if (g_pipelineThreadEnd) {
        break;
      }
      auto& source = !g_pipelineQueue.empty() ? g_pipelineQueue : g_backgroundPipelineQueue;
      pending = std::move(source.front());
      source.pop_front();
    }
    auto result = create_pipeline_timed(pending.hash, std::move(pending.create));
    {
      std::lock_guard lock{g_pipelineMutex};
      g_pipelines.try_emplace(pending.hash, CachedPipeline{
                                                .pipeline = std::move(result),
                                                .firstFrameUsed = pending.firstFrameUsed,
                                            });
      g_pendingPipelines.erase(pending.hash);
    }
    if (!g_hasPipelineThread) {
      ++g_pipelinesPerFrame;
    }
    notify_pipeline_ready(true);
  }
}

// Pipelines compile on a pool: Tint and the driver's compiler are single threaded, so one
// worker left a cold cache (a new install, a driver update) compiling for over a minute on
// phones. GL keeps one worker, since its contexts are per thread. MP_PIPELINE_THREADS
// overrides the count.
static size_t pipeline_thread_count() {
  if (webgpu::g_backendType == wgpu::BackendType::OpenGL || webgpu::g_backendType == wgpu::BackendType::OpenGLES) {
    return 1;
  }
  if (const char* env = std::getenv("MP_PIPELINE_THREADS"); env != nullptr && *env != '\0') {
    return std::clamp<size_t>(std::strtoul(env, nullptr, 10), 1, 16);
  }
  // D3D11 is only picked where D3D12 and Vulkan failed: old GPUs, whose drivers often can't
  // create objects concurrently, so the runtime runs every Create* under one lock. More workers
  // only queue on it, and the game thread's buffer and texture creation waits behind them all.
  if (webgpu::g_backendType == wgpu::BackendType::D3D11) {
    return 1;
  }
  return std::clamp<size_t>(std::thread::hardware_concurrency() / 2, 1, 8);
}

template <typename PipelineConfig, typename CreateFn>
static size_t load_pipeline_cache_entries(ShaderType type, uint32_t configVersion, CreateFn&& create) {
  if (!prepare_pipeline_cache_db()) {
    return 0;
  }

  auto ret = sqlite3_bind_int(g_pipelineCacheLoadStmt, 1, underlying(type));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache load type: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return 0;
  }
  ret = sqlite3_bind_int(g_pipelineCacheLoadStmt, 2, static_cast<int>(configVersion));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache load config version: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return 0;
  }

  size_t acceptedRows = 0;
  while ((ret = sqlite3_step(g_pipelineCacheLoadStmt)) == SQLITE_ROW) {
    const auto* configBlob = static_cast<const uint8_t*>(sqlite3_column_blob(g_pipelineCacheLoadStmt, 0));
    const auto configSize = sqlite3_column_bytes(g_pipelineCacheLoadStmt, 0);
    const auto firstFrameUsed = static_cast<uint32_t>(sqlite3_column_int64(g_pipelineCacheLoadStmt, 1));
    if (configSize != static_cast<int>(sizeof(PipelineConfig)) || (configSize != 0 && configBlob == nullptr)) {
      continue;
    }

    PipelineConfig config;
    std::memcpy(&config, configBlob, sizeof(config));
    if (config.version != configVersion) {
      continue;
    }

    find_pipeline_impl(type, config, [=] { return create(config); }, PipelinePriority::Background, firstFrameUsed);
    ++acceptedRows;
  }

  if (ret != SQLITE_DONE) {
    Log.error("Failed to read pipeline cache rows: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
  }

  sqlite3_reset(g_pipelineCacheLoadStmt);
  sqlite3_clear_bindings(g_pipelineCacheLoadStmt);
  return acceptedRows;
}

static size_t load_pipeline_cache() {
  if (!prepare_pipeline_cache_db()) {
    return 0;
  }
  prune_old_pipeline_cache_versions();
  if (g_pipelineCacheBroken) {
    return 0;
  }

  size_t acceptedRows = 0;
#ifdef AURORA_ENABLE_RMLUI
  acceptedRows += load_pipeline_cache_entries<rmlui::PipelineConfig>(ShaderType::Rml, rmlui::RmlPipelineConfigVersion,
                                                                     rmlui::create_pipeline);
#endif
  acceptedRows += load_pipeline_cache_entries<clear::PipelineConfig>(
      ShaderType::Clear, clear::ClearPipelineConfigVersion, clear::create_pipeline);
  acceptedRows +=
      load_pipeline_cache_entries<gx::PipelineConfig>(ShaderType::GX, gx::GXPipelineConfigVersion, gx::create_pipeline);
  acceptedRows += load_pipeline_cache_entries<vfx::PipelineConfig>(ShaderType::Vfx, vfx::VfxPipelineConfigVersion,
                                                                   vfx::create_pipeline);
  acceptedRows += load_pipeline_cache_entries<water::PipelineConfig>(
      ShaderType::Water, water::WaterPipelineConfigVersion, water::create_pipeline);
  return acceptedRows;
}

static void start_pipeline_cache_writer() {
  if (!prepare_pipeline_cache_db()) {
    return;
  }

  g_pipelineCacheWriterStop = false;
  g_pipelineCacheWriterThread = std::thread(pipeline_cache_writer);
}

static void stop_pipeline_cache_writer() {
  if (g_pipelineCacheWriterThread.joinable()) {
    {
      std::lock_guard lock{g_pipelineCacheWriterMutex};
      g_pipelineCacheWriterStop = true;
    }
    g_pipelineCacheWriterCv.notify_one();
    g_pipelineCacheWriterThread.join();
  } else {
    g_pipelineCacheWriterStop = false;
  }

  g_pipelineCacheWriteQueue.clear();
}

template <>
PipelineRef find_pipeline(ShaderType type, const clear::PipelineConfig& config, NewPipelineCallback&& cb) {
  return find_pipeline_impl(type, config, std::move(cb));
}

template <>
PipelineRef find_pipeline(ShaderType type, const gx::PipelineConfig& config, NewPipelineCallback&& cb) {
  return find_pipeline_impl(type, config, std::move(cb));
}

bool touch_pipeline(PipelineRef ref) {
  if (g_dropPipelines.exchange(false, std::memory_order_acq_rel)) {
    apply_pipeline_drop();
  }
  if (ref == g_lastPipelineRef) {
    return true;
  }
  std::scoped_lock guard{g_pipelineMutex};
  if (!g_pipelines.contains(ref)) {
    if (!g_pendingPipelines.contains(ref)) {
      return false;
    }
    touch_pending_pipeline(ref, PipelinePriority::Normal);
  }
  g_lastPipelineRef = ref;
  return true;
}

template <typename Config>
static wgpu::RenderPipeline require_pipeline_impl(ShaderType type, const Config& config, NewPipelineCallback&& cb) {
  const PipelineRef ref =
      find_pipeline_impl(type, config, std::move(cb), PipelinePriority::Blocking, std::nullopt, true);
  wgpu::RenderPipeline pipeline;
  get_pipeline(ref, pipeline);
  return pipeline;
}

template <>
wgpu::RenderPipeline require_pipeline(ShaderType type, const vfx::PipelineConfig& config, NewPipelineCallback&& cb) {
  return require_pipeline_impl(type, config, std::move(cb));
}

template <>
wgpu::RenderPipeline require_pipeline(ShaderType type, const water::PipelineConfig& config, NewPipelineCallback&& cb) {
  return require_pipeline_impl(type, config, std::move(cb));
}

#ifdef AURORA_ENABLE_RMLUI
template <>
PipelineRef find_pipeline(ShaderType type, const rmlui::PipelineConfig& config, NewPipelineCallback&& cb) {
  return find_pipeline_impl(type, config, std::move(cb), PipelinePriority::Blocking, 0);
}
#endif

void initialize_pipeline_cache() {
  g_pipelineCacheBroken = false;
  g_compileCount.store(0, std::memory_order_relaxed);
  g_compileTotalMs.store(0, std::memory_order_relaxed);
  g_compileSlowestMs.store(0, std::memory_order_relaxed);
  g_compileFailures.store(0, std::memory_order_relaxed);
  g_pipelineCacheWriterStop = false;
  g_pipelineThreadEnd = false;
  g_gpuCachePrunePending = false;
  // See pipeline_thread_count: under D3D11 a build blocks the game thread's buffer and texture
  // creation, so the seed waits until the app resumes background builds (in game, for the port).
  g_backgroundPipelinesPaused = webgpu::g_backendType == wgpu::BackendType::D3D11;

  g_pipelineLoadStart = std::chrono::steady_clock::now();
  if (webgpu::g_backendType == wgpu::BackendType::WebGPU) {
    g_hasPipelineThread = false;
  } else {
    g_hasPipelineThread = true;
    const size_t threads = pipeline_thread_count();
    for (size_t i = 0; i < threads; ++i) {
      g_pipelineThreads.emplace_back(pipeline_worker);
    }
    Log.info("pipeline cache: {} compile thread(s)", threads);
  }

  const size_t loadedCount = load_pipeline_cache();
  if (!g_pipelineCacheBroken && loadedCount > 0) {
    g_gpuCachePrunePending = true;
  }

  if (!g_pipelineCacheBroken) {
    start_pipeline_cache_writer();
  }
}

void shutdown_pipeline_cache() {
  if (g_hasPipelineThread) {
    g_pipelineThreadEnd = true;
    g_pipelineQueueCv.notify_all();
    g_pipelineReadyCv.notify_all();
    for (auto& thread : g_pipelineThreads) {
      thread.join();
    }
    g_pipelineThreads.clear();
  }
  g_hasPipelineThread = false;

  stop_pipeline_cache_writer();
  pipeline_cache_abort();
  g_pipelineCacheBroken = false;
  g_pipelinesPerFrame = 0;
  g_gpuCachePrunePending = false;
  g_pipelines.clear();
  g_pipelineQueue.clear();
  g_backgroundPipelineQueue.clear();
  g_pendingPipelines.clear();

  queuedPipelines = 0;
  createdPipelines = 0;
}

void drop_pipelines() { g_dropPipelines.store(true, std::memory_order_release); }

void set_background_pipelines_paused(bool paused) {
  {
    std::lock_guard lock{g_pipelineMutex};
    if (g_backgroundPipelinesPaused == paused) {
      return;
    }
    g_backgroundPipelinesPaused = paused;
  }
  Log.info("pipeline cache: background builds {}", paused ? "paused" : "resumed");
  if (!paused) {
    g_pipelineQueueCv.notify_all();
  }
}

void begin_pipeline_frame() {
  if (!g_hasPipelineThread) {
    g_pipelinesPerFrame = 0;
  }
}

void end_pipeline_frame() {
  if (!g_hasPipelineThread) {
    pipeline_worker();
  }
}

bool get_pipeline(PipelineRef ref, wgpu::RenderPipeline& pipeline) {
  std::lock_guard guard{g_pipelineMutex};
  const auto it = g_pipelines.find(ref);
  if (it == g_pipelines.end()) {
    return false;
  }
  pipeline = it->second.pipeline;
  return true;
}

} // namespace aurora::gfx
