// The room environment's loading: reading and parsing a room's file off the render thread,
// registering its tiles, and the worker that decodes its cubes, volumes and lightmap and
// hands them to the GPU. See port_room_env_internal.h.

#include "port_room_env_internal.h"

namespace PortRoomEnv {
namespace EnvDetail {

// MP_ROOMENV_TIMING=1 logs where Load's time goes.
bool LoadTiming() {
  static const bool on = port::EnvFloat("MP_ROOMENV_TIMING", 0.f) != 0.f;
  return on;
}

double MsSince(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

// Reads and parses a room's file, and averages its cubes, on a thread of its own: the file
// is up to 40 MB, which stalled the render thread for 140 ms when a room loaded cold.
Loaded ReadRoomFile(const std::string& path) {
  Loaded loaded;
  std::ifstream in(PortGci::PathFromString(path), std::ios::binary);
  std::vector<uint8_t> data = ReadAll(in);
  if (!in || !Parse(std::move(data), loaded.file, loaded.error)) {
    if (loaded.error.empty()) {
      loaded.error = "cannot read";
    }
    return loaded;
  }
  const size_t cubes = loaded.file.cubes.size();
  loaded.average.resize(cubes);
  loaded.peak.resize(cubes);
  for (size_t i = 0; i < cubes; ++i) {
    loaded.average[i] = CubeAverage(loaded.file, loaded.file.cubes[i], loaded.peak[i]);
  }
  loaded.ok = true;
  return loaded;
}

// Starts the area's load; Finish completes it on the render thread.
void Load(uint32_t mrea, Area& area) {
  const std::string path = PortMods::RoomEnvPath(mrea);
  if (path.empty()) {
    return;
  }
  area.pendingFrames = 0;
  area.pendingFluid = sPlayerFluid;
  area.pendingWater = sCameraWater;
  area.queued.clear();
  area.loadSeq = ++sTileSeq;
  try {
    area.pending = std::async(std::launch::async, ReadRoomFile, path);
  } catch (...) {
    area.pending = std::async(std::launch::deferred, ReadRoomFile, path);
  }
}

// Has the worker compose the world texture from the tiles mapped now: every mapped key of
// every registered area, in one grid (a version 21 file). Called once the tables settle.
void SubmitWorld() {
  sWorld.dirty = false;
  auto input = std::make_shared<VolumeInput>();
  input->world = true;
  const PortRoomEnvTiles::TileSet* first = nullptr;
  for (const auto& [key, node] : sTiles.Mapped()) {
    const auto set = sTileSets.find(node);
    if (set == sTileSets.end() || set->second->Find(key) == nullptr) {
      continue;
    }
    if (first == nullptr) {
      first = set->second.get();
      std::copy(std::begin(first->worldToPoint), std::end(first->worldToPoint), input->worldToPoint);
    } else if (!std::equal(std::begin(first->worldToPoint), std::end(first->worldToPoint),
                           std::begin(set->second->worldToPoint))) {
      static bool warned = false;
      if (!warned) {
        warned = true;
        PortLog::Write("room env: area %08X has a different tile space than the others; its tiles are placed "
                       "as if it had the same one\n", node);
      }
    }
    input->mapped.emplace(key, set->second);
  }
  ++sWorld.gen;
  sWorker.DropVolumes(0);
  if (first == nullptr) {
    // Nothing is mapped: nothing to sample.
    if (sWorld.volume != 0) {
      GXDestroyPBRVolume(sWorld.volume);
      sWorld.volume = 0;
    }
    sWorld.grid = {};
    Invalidate();
    return;
  }
  Job job{JobKind::Volume, 0, 0, 0, &sNoFile};
  job.input = std::move(input);
  job.gen = sWorld.gen;
  sWorker.Submit(job);
}

// Registers the tiles of the areas that have loaded, in the order Load was called (the order
// Remastered loads its areas in: the first copy of a tile stays mapped). An area waits for
// the ones asked for before it, so the order does not depend on which file reads first.
void RegisterReady() {
  for (;;) {
    uint32_t nextMrea = 0;
    Area* next = nullptr;
    uint64_t waiting = UINT64_MAX; // the earliest area still loading
    for (auto& [mrea, area] : sAreas) {
      if (area.loadSeq == 0) {
        continue;
      }
      if (!area.settled) {
        waiting = std::min(waiting, area.loadSeq);
      } else if (!area.registered && area.file.tiles != nullptr && (next == nullptr || area.loadSeq < next->loadSeq)) {
        next = &area;
        nextMrea = mrea;
      }
    }
    if (next == nullptr || next->loadSeq > waiting) {
      break;
    }
    std::vector<PortRoomEnvTiles::TileKey> keys;
    for (const PortRoomEnvTiles::Tile& tile : next->file.tiles->tiles) {
      keys.push_back(tile.key);
    }
    sTileSets[nextMrea] = next->file.tiles;
    sTiles.Register(nextMrea, keys);
    next->registered = true;
    sWorld.dirty = true;
  }
  if (sWorld.dirty) {
    SubmitWorld();
  }
}

// Completes a load: takes the file from the loader thread (waiting for it if need be), then
// does what Load did on the render thread, and sends the area the states it was sent since.
void Finish(uint32_t mrea, Area& area) {
  if (!area.pending.valid()) {
    return;
  }
  const auto t0 = std::chrono::steady_clock::now();
  Loaded loaded = area.pending.get();
  const double waitMs = MsSince(t0);
  if (!loaded.ok) {
    PortLog::Write("room env: %s: %s\n", PortMods::RoomEnvPath(mrea).c_str(), loaded.error.c_str());
    area.file = {};
    area.queued.clear();
    area.settled = true;
    RegisterReady();
    return;
  }
  const auto t1 = std::chrono::steady_clock::now();
  area.file = std::move(loaded.file);
  if (area.file.tiles != nullptr) {
    area.ownAverage = PortRoomEnvTiles::OwnAverage(*area.file.tiles);
  }
  area.cubes.resize(area.file.cubes.size());
  for (size_t i = 0; i < area.cubes.size(); ++i) {
    area.cubes[i].average = loaded.average[i];
    area.cubes[i].peak = loaded.peak[i];
    // Black: nothing to reflect, and no exposure to set by it.
    area.cubes[i].failed = !(area.cubes[i].average > 1e-6f);
  }
  area.volumes.resize(area.file.tiles != nullptr ? 1 : area.file.grids.size());
  // Before the grades start: what drives them drives the exposure hints too.
  area.hintSnap = area.hints.Load(area.file.fluidHints, area.pendingWater, sExposureSwap);
  StartGrades(mrea, area, area.pendingFluid, area.pendingWater);
  ReplayWorldVolumes(mrea, area);
  LoadEnvVars(mrea, area);
  area.exposure = RoomExposure(area);
  if (LoadTiming()) {
    PortLog::Write("room env timing: %08X finish: waited %.2f ms for the loader, then %.2f ms (%zu cubes, %zu grids, "
                   "pending %u frames)\n",
                   mrea, waitMs, MsSince(t1), area.file.cubes.size(), area.file.grids.size(), area.pendingFrames);
  }
  const float* const t = area.file.tonemap;
  if (area.exposure > 0.f && t[1] > 0.f && t[1] < 1.f) {
    BuildTone(t[1], area.file.contrast, t[2], t[3], area.tone);
  }
  PortLog::Write("room env: %08X exposure %g (EV %g, hint %g..%g bias %g), tone mid %g contrast %g toe %g shoulder %g\n",
                 mrea, area.exposure, area.exposure > 0.f ? 3.f - std::log2(area.exposure) : 0.f,
                 area.file.exposure[0], area.file.exposure[1], area.file.exposureBias, t[1], area.file.contrast, t[2],
                 t[3]);
  area.hasFile = true;
  area.serial = sNextSerial++;
  if (sNextSerial == 0) {
    sNextSerial = 1;
  }
  area.settled = true;
  // The worker makes them all now, so no draw waits for one. The volumes first: until its
  // area's are made, room geometry is lit as without volumes (see HasVolume). A version 21
  // file's grid is composed once its tiles are registered (RegisterReady, below).
  for (size_t i = 0; i < area.file.grids.size(); ++i) {
    if (area.file.grids[i].average > 0.f) {
      sWorker.Submit({JobKind::Volume, mrea, area.serial, i, &area.file});
    }
  }
  for (size_t i = 0; i < area.cubes.size(); ++i) {
    if (!area.cubes[i].failed) {
      sWorker.Submit({JobKind::Cube, mrea, area.serial, i, &area.file});
    }
  }
  const Lightmap& lightmap = area.file.lightmap;
  const bool lightmapFits = lightmap.width != 0 && lightmap.height != 0 && lightmap.layers >= 4 &&
                            lightmap.offset <= area.file.data.size() &&
                            lightmap.length <= area.file.data.size() - lightmap.offset;
  if (lightmapFits && Lightmaps()) {
    // BC6H wants whole blocks; a device without it gets the colours.
    const bool raw = GXPBRLightmapBC6HSupported() && lightmap.width % 4 == 0 && lightmap.height % 4 == 0;
    sWorker.Submit({JobKind::Lightmap, mrea, area.serial, 0, &area.file, raw});
  }
  // Drops the cubes' and lightmap's blocks once they are made.
  if (!area.file.cubes.empty() || lightmapFits) {
    sWorker.Submit({JobKind::Keep, mrea, area.serial, 0, &area.file});
  }
  std::vector<std::pair<uint32_t, int>> queued;
  queued.swap(area.queued);
  for (const auto& [sender, state] : queued) {
    DriveGrades(mrea, area, sender, state);
  }
  RegisterReady();
  Invalidate();
}

// Completes the loads that are ready, and any that must be: the room the camera is in (it
// draws this frame), and one that has waited so long a draw may need it.
void FinishLoads() {
  constexpr uint32_t kMaxPendingFrames = 8;
  for (auto& [mrea, area] : sAreas) {
    if (!area.pending.valid()) {
      continue;
    }
    const bool ready = area.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    if (ready || mrea == sViewArea || area.pendingFrames >= kMaxPendingFrames) {
      Finish(mrea, area);
    } else {
      ++area.pendingFrames;
    }
  }
}

// What an area keeps of its file once its cubes and lightmap are made: the grids' points, which
// SampleGrid reads for models every frame, and the grades' LUTs, handed to Aurora when one
// is first shown; with where each starts in the copy. The cubes' and lightmap's blocks are left behind.
void KeepData(const File& file, std::vector<uint8_t>& kept, std::vector<size_t>& offsets) {
  constexpr size_t kPoint = 24;
  size_t bytes = 0;
  for (const Grid& grid : file.grids) {
    bytes += size_t(grid.size[0]) * grid.size[1] * grid.size[2] * kPoint;
  }
  bytes += file.grades.size() * kGradeLutBytes;
  kept.clear();
  kept.reserve(bytes);
  offsets.clear();
  const auto keep = [&](size_t offset, size_t length) {
    offsets.push_back(kept.size());
    kept.insert(kept.end(), file.data.begin() + offset, file.data.begin() + offset + length);
  };
  for (const Grid& grid : file.grids) {
    keep(grid.offset, size_t(grid.size[0]) * grid.size[1] * grid.size[2] * kPoint);
  }
  for (const Grade& grade : file.grades) {
    keep(grade.offset, kGradeLutBytes);
  }
}

// Decodes a cube into RGBA16Float, every mip of face 0, then face 1 (GXCreatePBRCube); the
// file has every face of mip 0, then mip 1. On the worker; false when cancelled.
bool DecodeCube(const File& file, const Cube& cube, std::vector<uint16_t>& texels, const std::atomic<bool>& cancel) {
  std::vector<size_t> mipOffset(cube.mipCount);
  size_t perFace = 0;
  for (uint32_t mip = 0; mip < cube.mipCount; ++mip) {
    const size_t edge = std::max(cube.size >> mip, 1u);
    mipOffset[mip] = perFace;
    perFace += edge * edge * 8;
  }
  texels.assign(perFace * 6 / 2, 0);
  const uint8_t* blocks = file.data.data() + cube.offset;
  for (uint32_t mip = 0; mip < cube.mipCount; ++mip) {
    const uint32_t edge = std::max(cube.size >> mip, 1u);
    const size_t blockBytes = size_t((edge + 3) / 4) * ((edge + 3) / 4) * 16;
    for (uint32_t face = 0; face < 6; ++face) {
      if (cancel.load(std::memory_order_relaxed)) {
        return false;
      }
      uint16_t* out = texels.data() + (perFace * face + mipOffset[mip]) / 2;
      PortRemastered::DecodeBc6hFace(blocks, edge, cube.isSigned, out);
      blocks += blockBytes;
    }
  }
  return true;
}

// A room's lightmap as GXCreatePBRLightmap takes it: its blocks as they are when `raw`, else
// decoded to RGBA16F and halved until a layer has at most kMaxLightmapTexels (a 2888 square
// of four layers is 267 MB at full size). On the worker; false when cancelled.
constexpr size_t kMaxLightmapTexels = size_t(1) << 21;

bool MakeLightmap(const File& file, bool raw, Result& out, const std::atomic<bool>& cancel) {
  const Lightmap& map = file.lightmap;
  const uint8_t* const blocks = file.data.data() + map.offset;
  out.raw = raw;
  out.width = map.width;
  out.height = map.height;
  if (raw) {
    out.volume.assign(blocks, blocks + map.length);
    return true;
  }
  uint32_t shift = 0;
  while (size_t(std::max(map.width >> shift, 1u)) * std::max(map.height >> shift, 1u) > kMaxLightmapTexels) {
    ++shift;
  }
  // Rounded up, so the edge texels count too (the box loop clamps).
  const uint32_t width = std::max((map.width + (1u << shift) - 1) >> shift, 1u);
  const uint32_t height = std::max((map.height + (1u << shift) - 1) >> shift, 1u);
  const size_t layerBytes = map.length / map.layers;
  std::vector<uint16_t> full(size_t(map.width) * map.height * 4);
  out.width = width;
  out.height = height;
  out.cube.assign(size_t(width) * height * 4 * map.layers, 0);
  for (uint32_t layer = 0; layer < map.layers; ++layer) {
    if (cancel.load(std::memory_order_relaxed)) {
      return false;
    }
    PortRemastered::DecodeBc6hImage(blocks + layerBytes * layer, map.width, map.height, map.isSigned, full.data());
    uint16_t* const dst = out.cube.data() + size_t(width) * height * 4 * layer;
    if (shift == 0) {
      std::copy(full.begin(), full.end(), dst);
      continue;
    }
    // Each texel the average of those under it at full size.
    const uint32_t step = 1u << shift;
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        float sum[4] = {};
        uint32_t count = 0;
        for (uint32_t sy = y * step; sy < std::min((y + 1) * step, map.height); ++sy) {
          for (uint32_t sx = x * step; sx < std::min((x + 1) * step, map.width); ++sx) {
            const uint16_t* t = full.data() + (size_t(sy) * map.width + sx) * 4;
            for (int c = 0; c < 4; ++c) {
              sum[c] += HalfToFloat(t[c]);
            }
            ++count;
          }
        }
        for (int c = 0; c < 4; ++c) {
          dst[(size_t(y) * width + x) * 4 + c] = FloatToHalf(sum[c] / float(count));
        }
      }
    }
  }
  return true;
}

Worker::~Worker() {
  {
    std::lock_guard<std::mutex> lock(mutex);
    stop = true;
    cancel = true;
  }
  wake.notify_all();
  if (thread.joinable()) {
    thread.join();
  }
}

void Worker::Submit(const Job& job) {
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!thread.joinable() && !noThread) {
      try {
        thread = std::thread(&Worker::Run, this);
      } catch (...) {
        noThread = true;
        PortLog::Write("room env: no worker thread; cubes and volumes are made as areas load\n");
      }
    }
    if (!noThread) {
      jobs.push_back(job);
      wake.notify_one();
      return;
    }
  }
  Result result;
  if (Do(job, result)) {
    std::lock_guard<std::mutex> lock(mutex);
    results.push_back(std::move(result));
  }
}

void Worker::Cancel(uint32_t serial) {
  std::unique_lock<std::mutex> lock(mutex);
  jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [serial](const Job& job) { return job.serial == serial; }),
             jobs.end());
  results.erase(std::remove_if(results.begin(), results.end(),
                               [serial](const Result& result) { return result.serial == serial; }),
                results.end());
  if (running && runningSerial == serial) {
    cancel = true;
    done.wait(lock, [this, serial] { return !running || runningSerial != serial; });
  }
  wake.notify_all();
}

void Worker::DropVolumes(uint32_t serial) {
  std::lock_guard<std::mutex> lock(mutex);
  jobs.erase(std::remove_if(jobs.begin(), jobs.end(),
                            [serial](const Job& job) { return job.serial == serial && job.kind == JobKind::Volume; }),
             jobs.end());
  results.erase(std::remove_if(results.begin(), results.end(),
                               [serial](const Result& result) {
                                 return result.serial == serial && result.kind == JobKind::Volume;
                               }),
                results.end());
  wake.notify_all();
}

bool Worker::Take(Result& out, bool gpu) {
  std::lock_guard<std::mutex> lock(mutex);
  if (results.empty() || (!gpu && results.front().kind != JobKind::Keep)) {
    return false;
  }
  out = std::move(results.front());
  results.pop_front();
  wake.notify_all();
  return true;
}

void Worker::Run() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex);
      wake.wait(lock, [this] { return stop || (!jobs.empty() && results.size() < kMaxResults); });
      if (stop) {
        return;
      }
      job = jobs.front();
      jobs.pop_front();
      running = true;
      runningSerial = job.serial;
      cancel = false;
    }
    Result result;
    const bool finished = Do(job, result);
    {
      std::lock_guard<std::mutex> lock(mutex);
      running = false;
      if (finished && !cancel) {
        results.push_back(std::move(result));
      }
    }
    done.notify_all();
  }
}

bool Worker::Do(const Job& job, Result& out) {
  out.kind = job.kind;
  out.area = job.area;
  out.serial = job.serial;
  out.index = job.index;
  out.gen = job.gen;
  switch (job.kind) {
  case JobKind::Cube:
    return DecodeCube(*job.file, job.file->cubes[job.index], out.cube, cancel);
  case JobKind::Volume:
    if (job.input != nullptr) {
      return ResolveVolume(*job.input, job.file, out, cancel);
    }
    return FillVolume(*job.file, job.file->grids[job.index], out.volume, cancel);
  case JobKind::Lightmap:
    return MakeLightmap(*job.file, job.raw, out, cancel);
  case JobKind::Keep:
    KeepData(*job.file, out.volume, out.offsets);
    return true;
  }
  return false;
}

// Hands the worker's decoded cubes and filled volumes to the GPU, one a frame: each is
// megabytes for Aurora to copy and upload, and an area brings several at once. While the
// room environment is off they wait (nothing would use them), and so does the worker.
// The world texture's result: a volume made from tiles that have changed hands since is dropped.
void TakeWorld(Result& result) {
  if (result.gen != sWorld.gen) {
    return;
  }
  if (result.empty) {
    if (sWorld.volume != 0) {
      GXDestroyPBRVolume(sWorld.volume);
      sWorld.volume = 0;
    }
    sWorld.grid = {};
    return;
  }
  sWorld.grid = std::move(result.grid);
  const Grid& grid = sWorld.grid;
  // The old volume is destroyed after the new one is created: aurora erases it when the destroy
  // command is processed (not deferred), so commands already queued that name it still run first.
  const uint32_t oldVolume = sWorld.volume;
  sWorld.volume = sNextVolume++;
  if (sNextVolume == 0) {
    sNextVolume = 1;
  }
  const auto tv = std::chrono::steady_clock::now();
  const size_t volumeBytes = result.volume.size();
  GXCreatePBRVolumeOwned(sWorld.volume, grid.size[0], grid.size[1], grid.size[2],
                         new std::vector<uint8_t>(std::move(result.volume)));
  if (LoadTiming()) {
    PortLog::Write("room env timing: world GXCreatePBRVolume %.2f ms (%zu bytes)\n", MsSince(tv), volumeBytes);
  }
  PortLog::Write("room env: world volume %u %ux%ux%u average %g (%zu mapped tiles)\n", sWorld.volume, grid.size[0],
                 grid.size[1], grid.size[2], grid.average, sTiles.KeyCount());
  if (oldVolume != 0) {
    GXDestroyPBRVolume(oldVolume);
  }
}

void TakeResults() {
  bool gpu = Enabled();
  Result result;
  while (sWorker.Take(result, gpu)) {
    if (result.kind == JobKind::Volume && result.serial == 0) {
      TakeWorld(result);
      Invalidate();
      gpu = false;
      continue;
    }
    const auto found = sAreas.find(result.area);
    if (found == sAreas.end() || found->second.serial != result.serial) {
      continue; // Free drops an area's results, so this does not happen
    }
    Area& area = found->second;
    File& file = area.file;
    if (result.kind == JobKind::Keep) {
      // The worker has finished with the old bytes: Keep is the area's last job.
      file.data.swap(result.volume);
      for (size_t i = 0; i < file.grids.size(); ++i) {
        file.grids[i].offset = result.offsets[i];
      }
      for (size_t i = 0; i < file.grades.size(); ++i) {
        file.grades[i].offset = result.offsets[file.grids.size() + i];
      }
      for (Cube& cube : file.cubes) {
        cube.offset = cube.length = 0; // its blocks are gone
      }
      continue;
    }
    if (result.kind == JobKind::Lightmap) {
      const Lightmap& lightmap = file.lightmap;
      const GXPBRLightmapFormat format = !result.raw         ? GX_PBR_LIGHTMAP_RGBA16F
                                         : lightmap.isSigned ? GX_PBR_LIGHTMAP_BC6H_SFLOAT
                                                             : GX_PBR_LIGHTMAP_BC6H_UFLOAT;
      const void* data = result.raw ? static_cast<const void*>(result.volume.data()) : result.cube.data();
      const size_t bytes = result.raw ? result.volume.size() : result.cube.size() * 2;
      if (area.lightmap != 0) {
        GXDestroyPBRLightmap(area.lightmap);
      }
      area.lightmap = GXCreatePBRLightmap(result.width, result.height, lightmap.layers, format, data, uint32_t(bytes));
      PortLog::Write("room env: %08X lightmap %u %ux%u x %u layers%s\n", result.area, area.lightmap, result.width,
                     result.height, lightmap.layers, result.raw ? " (BC6H)" : " (RGBA16F)");
      Invalidate();
      gpu = false;
      continue;
    }
    if (result.kind == JobKind::Cube) {
      const Cube& cube = file.cubes[result.index];
      GpuCube& gpuCube = area.cubes[result.index];
      gpuCube.id = sNextCube++;
      if (sNextCube == 0) {
        sNextCube = 1;
      }
      gpuCube.mipCount = cube.mipCount;
      GXCreatePBRCube(gpuCube.id, cube.size, cube.mipCount, result.cube.data(), uint32_t(result.cube.size() * 2));
    } else {
      const Grid& grid = file.grids[result.index];
      GpuVolume& gpuVolume = area.volumes[result.index];
      gpuVolume.id = sNextVolume++;
      if (sNextVolume == 0) {
        sNextVolume = 1;
      }
      const auto tv = std::chrono::steady_clock::now();
      const size_t volumeBytes = result.volume.size();
      GXCreatePBRVolumeOwned(gpuVolume.id, grid.size[0], grid.size[1], grid.size[2],
                             new std::vector<uint8_t>(std::move(result.volume)));
      if (LoadTiming()) {
        PortLog::Write("room env timing: %08X GXCreatePBRVolume %.2f ms (%zu bytes)\n", result.area, MsSince(tv),
                       volumeBytes);
      }
      PortLog::Write("room env: %08X volume %u %ux%ux%u average %g\n", result.area, gpuVolume.id, grid.size[0],
                     grid.size[1], grid.size[2], grid.average);
    }
    // What Select finds changes with what is on the GPU.
    Invalidate();
    gpu = false;
  }
}

}  // namespace EnvDetail
}  // namespace PortRoomEnv
