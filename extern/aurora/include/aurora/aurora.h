#ifndef AURORA_AURORA_H
#define AURORA_AURORA_H

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>

extern "C" {
#else
#include "stdbool.h"
#include "stddef.h"
#include "stdint.h"
#endif

typedef enum {
  SAMPLER_BILINEAR,
  SAMPLER_AREA,
} AuroraSampler;

typedef enum {
  BACKEND_AUTO,
  BACKEND_D3D11,
  BACKEND_D3D12,
  BACKEND_METAL,
  BACKEND_VULKAN,
  BACKEND_OPENGL,
  BACKEND_OPENGLES,
  BACKEND_WEBGPU,
  BACKEND_NULL,
} AuroraBackend;

typedef enum {
  LOG_DEBUG,
  LOG_INFO,
  LOG_WARNING,
  LOG_ERROR,
  LOG_FATAL,
} AuroraLogLevel;

typedef struct {
  int32_t x;
  int32_t y;
} AuroraWindowPos;

typedef struct {
  uint32_t width;
  uint32_t height;

  /**
   * Width of the main GX framebuffer.
   */
  uint32_t fb_width;

  /**
   * Height of the main GX framebuffer.
   */
  uint32_t fb_height;

  /**
   * The size of the framebuffer used to present to the operating system.
   * May differ from fb_width if Aurora is instructed to force an aspect ratio or resolution configuration.
   */
  uint32_t native_fb_width;

  /**
   * The size of the framebuffer used to present to the operating system.
   * May differ from fb_height if Aurora is instructed to force an aspect ratio or resolution configuration.
   */
  uint32_t native_fb_height;
  float scale;
} AuroraWindowSize;

typedef struct SDL_Window SDL_Window;
typedef struct AuroraEvent AuroraEvent;

typedef void (*AuroraLogCallback)(AuroraLogLevel level, const char* module, const char* message, unsigned int len);
typedef void (*AuroraImGuiInitCallback)(const AuroraWindowSize* size);

#define MEM1_DEFAULT_SIZE (24 * 1024 * 1024)
#define ARAM_DEFAULT_SIZE (16 * 1024 * 1024)

typedef struct {
  const char* appName;
  const char* userPath;
  const char* cachePath;
  const char* resourcesPath;
  AuroraBackend desiredBackend;
  uint32_t msaa;
  uint16_t maxTextureAnisotropy;
  bool vsync;
  bool startFullscreen;
  bool allowJoystickBackgroundEvents;
  bool pauseOnFocusLost;
  bool allowTextureDumps;
  bool allowCpuAdapter;
  int32_t windowPosX;
  int32_t windowPosY;
  uint32_t windowWidth;
  uint32_t windowHeight;
  void* iconRGBA8;
  uint32_t iconWidth;
  uint32_t iconHeight;
  AuroraLogCallback logCallback;
  AuroraLogLevel logLevel;
  AuroraImGuiInitCallback imGuiInitCallback;

  /*
   * The size of the GameCube's main memory, or MEM1 on the Wii.
   * Note that it will not be allocated at the exact 0x80000000 address, as that cannot be guaranteed.
   * This can be set to 0 to disable allocating this region.
   */
  uint32_t mem1Size;

  /*
   * The size of the GameCube's ARAM, or MEM2 on the Wii.
   * This can be set to 0 to disable allocating this region.
   */
  uint32_t mem2Size;

  /*
   * Multiplies what one frame can hold of vertex, index and array data (5, 2 and 8 MiB at 1),
   * and doubles the 24 MiB of uniforms when above 1. 0 means 1. A frame that outgrows
   * its buffers aborts, so raise this for content denser than the original game's.
   */
  uint32_t frameBufferScale;

  /*
   * MiB of the shared vertex, index and array buffers set aside for data kept across frames
   * (GXPortRetainResident), on top of what a frame holds. 0 sets none aside; the device's
   * limits may allow less.
   */
  uint32_t residentGeometryMiB;

  /*
   * An initial pipeline cache database held in memory (must stay valid for the whole run). When set
   * it is used instead of <resourcesPath>/initial_pipeline_cache.db, so a stale file left beside the
   * application cannot override it.
   */
  const uint8_t* pipelineCacheSeedData;
  size_t pipelineCacheSeedSize;
  /* Merge no initial pipeline cache at all (recording a new seed from an empty cache). */
  bool pipelineCacheNoSeed;

  /*
   * A directory (with a trailing slash) Dawn searches first for the Vulkan library, e.g. a shim
   * for a custom driver. Null searches the usual places only.
   */
  const char* vulkanLibraryDir;

  /*
   * When set and only the Null backend started (no GPU and no software adapter), this message is
   * shown in an error box and the process exits with status 1, instead of running with no picture.
   */
  const char* noGraphicsMessage;
} AuroraConfig;

typedef struct {
  AuroraBackend backend;
  const char* userPath;
  const char* cachePath;
  SDL_Window* window;
  AuroraWindowSize windowSize;
} AuroraInfo;

AuroraInfo aurora_initialize(int argc, char* argv[], const AuroraConfig* config);
void aurora_shutdown();
const AuroraEvent* aurora_update();
bool aurora_begin_frame();
void aurora_end_frame();
// Drops the swapchain if the window's surface went away, without starting a frame.
// For a main thread that waits outside the frame loop (a file dialog, a long copy):
// Android's surfaceDestroyed waits for this, and a swapchain left on a destroyed
// window can lose the device. Cheap when nothing changed.
void aurora_release_lost_surface();
// True while the app is backgrounded or has no surface (Android pause): the main loop
// may legitimately not run then. Atomics only, so any thread can ask.
bool aurora_is_suspended();

void aurora_set_log_level(AuroraLogLevel level);
void aurora_set_pause_on_focus_lost(bool value);
void aurora_set_background_input(bool value);
void aurora_set_resampler(AuroraSampler sampler);
/** Sets MSAA samples (1 or 4) and max texture anisotropy (1-16); applied at the next frame start. */
void aurora_set_graphics_quality(uint32_t msaa, uint16_t maxTextureAnisotropy);
/** Sets the clock timescale. Default 1.0f. 0.0f is paused. Range 0.0f-16.0f. */
void aurora_set_timescale(float scale);

/**
 * Holds background pipeline builds (the cache and shipped seed, compiled ahead of use) while paused.
 * Pipelines a draw looks up still compile. Starts paused under D3D11, where a build blocks every
 * other object creation; the app resumes them when it no longer loads much.
 */
void aurora_set_background_pipelines_paused(bool paused);

/**
 * Stops aurora writing the controller mapping, keyboard binding and controller port files for the
 * rest of the process. For an app that has just moved them away (a configuration reset).
 */
void aurora_disable_config_persistence();

AuroraBackend aurora_get_backend();
// The driver's description from the graphics adapter (e.g. "Turnip Mesa driver 25.1.0"); "" before init.
const char* aurora_get_gpu_driver();
// True when AuroraConfig::vulkanLibraryDir's Vulkan library failed and the system's was tried instead.
bool aurora_vulkan_library_failed();
const AuroraBackend* aurora_get_available_backends(size_t* count);
float aurora_get_timescale();

/**
 * GPU self-test: renders known patterns offscreen through the game's GX path, reads them back and logs
 * "gpu selftest: <case>: PASS/FAIL" lines. Call inside a frame (after aurora_begin_frame, before the frame's
 * own draws); it changes GX state, so the caller must reset its cached state afterwards. Returns false if a
 * run is already pending or the test could not start. Results arrive a frame or two later. Pipelines compile
 * asynchronously and skip draws until ready, so call once with warmup (nothing logged), wait for
 * queuedPipelines to drain, then call again for the real run.
 */
bool aurora_gpu_selftest_run(bool warmup);
bool aurora_gpu_selftest_pending();
/** Copies the last run's one-line summary ("X/Y passed ...", empty before the first run); returns its length. */
size_t aurora_gpu_selftest_summary(char* buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif
