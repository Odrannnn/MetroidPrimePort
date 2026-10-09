#include <aurora/aurora.h>
#include <aurora/phase.hpp>
#include <aurora/time.hpp>

#ifdef AURORA_ENABLE_GX
#include "gfx/resources.hpp"
#include "gfx/frame.hpp"
#include "gfx/recording.hpp"
#include "gfx/render_worker.hpp"
#include "gx/command_processor.hpp"
#include "gx/fifo.hpp"
#include "gx/gx.hpp"
#include "gx/texture.hpp"
#include "imgui.hpp"
#include "webgpu/gpu.hpp"
#include "webgpu/gpu_prof.hpp"
#include <webgpu/webgpu_cpp.h>
#endif

#ifdef AURORA_ENABLE_RMLUI
#include "rmlui.hpp"
#endif

#include "input.hpp"
#include "internal.hpp"
#include "screenshot.hpp"
#include "thread.hpp"
#include "window.hpp"

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_surface.h>
#include <magic_enum.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>

#include "system_info.hpp"
#include "tracy/Tracy.hpp"

namespace aurora {
AuroraConfig g_config;
// aurora_set_graphics_quality request: msaa << 16 | anisotropy, 0 = none.
std::atomic<uint32_t> g_pendingQuality{0};
uint32_t g_sdlCustomEventsStart;
char g_gameName[4];

namespace {
constexpr Module Log{"aurora"};

#ifdef AURORA_ENABLE_GX
std::atomic_bool g_screenshotRequested;

// GPU
using webgpu::g_device;
using webgpu::g_queue;
using webgpu::g_surface;

void record_screenshot(wgpu::CommandEncoder& encoder, std::vector<gfx::AfterSubmitCallback>& callbacks) {
  const auto& source = webgpu::present_source();
  const bool bgra = source.format == wgpu::TextureFormat::BGRA8Unorm ||
                    source.format == wgpu::TextureFormat::BGRA8UnormSrgb;
  const bool rgba = source.format == wgpu::TextureFormat::RGBA8Unorm ||
                    source.format == wgpu::TextureFormat::RGBA8UnormSrgb;
  if (!bgra && !rgba) {
    Log.warn("Screenshot unsupported for texture format {}", magic_enum::enum_name(source.format));
    return;
  }

  const uint32_t width = source.size.width;
  const uint32_t height = source.size.height;
  const uint32_t bytesPerRow = (width * 4 + 255) & ~255u;
  const uint64_t byteSize = static_cast<uint64_t>(bytesPerRow) * height;
  const wgpu::BufferDescriptor descriptor{
      .label = "Screenshot readback",
      .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
      .size = byteSize,
  };
  auto readback = g_device.CreateBuffer(&descriptor);
  const wgpu::TexelCopyTextureInfo src{.texture = source.texture};
  const wgpu::TexelCopyBufferInfo dst{
      .layout =
          wgpu::TexelCopyBufferLayout{
              .bytesPerRow = bytesPerRow,
              .rowsPerImage = height,
          },
      .buffer = readback,
  };
  encoder.CopyTextureToBuffer(&src, &dst, &source.size);

  callbacks.emplace_back([readback, width, height, bytesPerRow, byteSize, bgra]() {
    readback.MapAsync(
        wgpu::MapMode::Read, 0, byteSize, wgpu::CallbackMode::AllowSpontaneous,
        [readback, width, height, bytesPerRow, byteSize, bgra](wgpu::MapAsyncStatus status,
                                                               wgpu::StringView message) {
          if (status != wgpu::MapAsyncStatus::Success) {
            if (status != wgpu::MapAsyncStatus::CallbackCancelled && status != wgpu::MapAsyncStatus::Aborted) {
              Log.warn("Screenshot readback failed {}: {}", magic_enum::enum_name(status), message);
            }
            return;
          }

          const void* pixels = readback.GetConstMappedRange(0, byteSize);
          SDL_Surface* surface = SDL_CreateSurfaceFrom(
              static_cast<int>(width), static_cast<int>(height),
              bgra ? SDL_PIXELFORMAT_BGRA32 : SDL_PIXELFORMAT_RGBA32, const_cast<void*>(pixels),
              static_cast<int>(bytesPerRow));
          if (surface == nullptr) {
            Log.warn("Unable to create screenshot surface: {}", SDL_GetError());
          } else {
            std::error_code ec;
            std::filesystem::create_directories("screenshots", ec);
            const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
            const std::string path = "screenshots/metroid-prime-" + std::to_string(stamp) + ".bmp";
            if (!SDL_SaveBMP(surface, path.c_str())) {
              Log.warn("Unable to save screenshot {}: {}", path, SDL_GetError());
            } else {
              Log.info("Screenshot saved to {}", path);
            }
            SDL_DestroySurface(surface);
          }
          readback.Unmap();
        });
  });
}

uint32_t clamp_scissor_coord(double value, uint32_t maximum) noexcept {
  if (!std::isfinite(value)) {
    return 0;
  }
  return static_cast<uint32_t>(std::clamp(value, 0.0, static_cast<double>(maximum)));
}

void set_present_viewport(const wgpu::RenderPassEncoder& pass, const gfx::Viewport& viewport, uint32_t surfaceWidth,
                          uint32_t surfaceHeight) noexcept {
  pass.SetViewport(viewport.left, viewport.top, viewport.width, viewport.height, viewport.znear, viewport.zfar);
  const auto scissorX = clamp_scissor_coord(std::floor(viewport.left), surfaceWidth);
  const auto scissorY = clamp_scissor_coord(std::floor(viewport.top), surfaceHeight);
  const auto scissorRight = clamp_scissor_coord(std::ceil(viewport.left + viewport.width), surfaceWidth);
  const auto scissorBottom = clamp_scissor_coord(std::ceil(viewport.top + viewport.height), surfaceHeight);
  pass.SetScissorRect(scissorX, scissorY, scissorRight - scissorX, scissorBottom - scissorY);
}
#endif

#ifdef AURORA_ENABLE_GX
constexpr std::array PreferredBackendOrder{
#ifdef ENABLE_BACKEND_WEBGPU
    BACKEND_WEBGPU,
#endif
#ifdef DAWN_ENABLE_BACKEND_D3D12
    BACKEND_D3D12,
#endif
#ifdef DAWN_ENABLE_BACKEND_METAL
    BACKEND_METAL,
#endif
#ifdef DAWN_ENABLE_BACKEND_VULKAN
    BACKEND_VULKAN,
#endif
#ifdef DAWN_ENABLE_BACKEND_D3D11
    BACKEND_D3D11,
#endif
// #ifdef DAWN_ENABLE_BACKEND_DESKTOP_GL
//     BACKEND_OPENGL,
// #endif
#ifdef DAWN_ENABLE_BACKEND_OPENGLES
    BACKEND_OPENGLES,
#endif
#ifdef DAWN_ENABLE_BACKEND_NULL
    BACKEND_NULL,
#endif
};
#else
constexpr std::array<AuroraBackend, 0> PreferredBackendOrder{};
#endif

bool g_initialFrame = false;

AuroraInfo initialize(int argc, char* argv[], const AuroraConfig& config) noexcept {
  g_config = config;
  Log.info("Aurora initializing");
  log_system_information();
  if (g_config.appName == nullptr) {
    g_config.appName = "Aurora";
  } else {
    g_config.appName = strdup(g_config.appName);
  }
  if (g_config.userPath == nullptr) {
    g_config.userPath = SDL_GetPrefPath(nullptr, g_config.appName);
  } else {
    g_config.userPath = strdup(g_config.userPath);
  }
  if (g_config.cachePath == nullptr) {
    g_config.cachePath = SDL_GetPrefPath(nullptr, g_config.appName);
  } else {
    g_config.cachePath = strdup(g_config.cachePath);
  }
  if (g_config.resourcesPath == nullptr) {
    g_config.resourcesPath = SDL_GetBasePath();
  } else {
    g_config.resourcesPath = strdup(g_config.resourcesPath);
  }
  if (g_config.msaa == 0) {
    g_config.msaa = 1;
  }
  if (g_config.maxTextureAnisotropy == 0) {
    g_config.maxTextureAnisotropy = 16;
  }
  AURORA_ASSERT(window::initialize(), "Error initializing window");

  g_sdlCustomEventsStart = SDL_RegisterEvents(2);
  AURORA_ASSERT(g_sdlCustomEventsStart, "Failed to allocate user events: {}", SDL_GetError());
  AURORA_ASSERT(window::initialize_event_watch(), "Error initializing SDL event watch");

#ifdef AURORA_ENABLE_GX
  /* Attempt to create a window using the calling application's desired backend */
  AuroraBackend selectedBackend = config.desiredBackend;
  bool windowCreated = false;
  if (selectedBackend != BACKEND_AUTO && window::create_window(selectedBackend)) {
    if (webgpu::initialize(selectedBackend, config.allowCpuAdapter)) {
      windowCreated = true;
    } else {
      window::destroy_window();
    }
  }

  // Without a usable GPU (a virtual machine without 3D acceleration, a broken driver), a software
  // adapter such as llvmpipe or WARP still draws a picture, slowly; the Null backend draws nothing.
  // So unless CPU adapters are allowed from the start, a second pass takes them before Null.
  const int passes = config.allowCpuAdapter ? 1 : 2;
  for (int pass = 0; pass < passes && !windowCreated; ++pass) {
    const bool allowCpu = config.allowCpuAdapter || pass == 1;
    for (const auto backendType : PreferredBackendOrder) {
      if (pass + 1 < passes && backendType == BACKEND_NULL) {
        continue;
      }
      selectedBackend = backendType;
      if (!window::create_window(selectedBackend)) {
        continue;
      }
      if (webgpu::initialize(selectedBackend, allowCpu)) {
        windowCreated = true;
        break;
      } else {
        window::destroy_window();
      }
    }
  }

  AURORA_ASSERT(windowCreated, "Error creating window: {}", SDL_GetError());

  if (webgpu::g_backendType == wgpu::BackendType::Null && config.noGraphicsMessage != nullptr) {
    Log.error("No graphics device could be started; exiting");
    // No parent: the window isn't shown yet, and a box owned by a hidden window may not show either.
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, config.appName, config.noGraphicsMessage, nullptr);
    window::destroy_window();
    SDL_Quit();
    std::exit(1);
  }

  // Initialize SDL_Renderer for ImGui when we can't use a Dawn backend
  if (webgpu::g_backendType == wgpu::BackendType::Null) {
    AURORA_ASSERT(window::create_renderer(), "Failed to initialize SDL renderer: {}", SDL_GetError());
  }
#else
  AuroraBackend selectedBackend = BACKEND_NULL;
  AURORA_ASSERT(window::create_window(BACKEND_NULL), "Error creating window: {}", SDL_GetError());
  AURORA_ASSERT(window::create_renderer(), "Failed to initialize SDL renderer: {}", SDL_GetError());
#endif

  window::show_window();
  thread::set_current({
      .name = "Main thread",
      .affinity = thread::Affinity::SharedCache,
  });

#ifdef AURORA_ENABLE_GX
  gfx::initialize();
  gx::fifo::init();
  imgui::create_context();
#endif
  const auto size = window::get_window_size();
  Log.info("Using framebuffer size {}x{} scale {}", size.fb_width, size.fb_height, size.scale);
#ifdef AURORA_ENABLE_GX
  if (g_config.imGuiInitCallback != nullptr) {
    g_config.imGuiInitCallback(&size);
  }
  imgui::initialize();
#endif

#ifdef AURORA_ENABLE_RMLUI
  rmlui::initialize(size);
#endif

  g_initialFrame = true;
  g_config.desiredBackend = selectedBackend;
  return {
      .backend = selectedBackend,
      .userPath = g_config.userPath,
      .cachePath = g_config.cachePath,
      .window = window::get_sdl_window(),
      .windowSize = size,
  };
}

void shutdown() noexcept {
#ifdef AURORA_ENABLE_GX
  gx::fifo::shutdown();
  gfx::render_worker::synchronize();
#ifdef AURORA_ENABLE_RMLUI
  rmlui::shutdown();
#endif
  imgui::shutdown();
  gfx::shutdown();
  webgpu::shutdown();
#endif
  input::shutdown();
  window::shutdown();
}

const AuroraEvent* update() noexcept {
  ZoneScoped;
  phase::set(phase::Main, "window event pump");
  if (g_initialFrame) {
    g_initialFrame = false;
    input::initialize();
  }
#ifdef AURORA_ENABLE_GX
  gx::update();
#endif
  const AuroraEvent* events = window::poll_events();
  phase::set(phase::Main, "game update");
  return events;
}

#ifdef AURORA_ENABLE_GX
// Returns false (after dropping the surface where needed) when no frame can be drawn
// because the surface is changing or gone.
static bool service_surface(const char* caller) noexcept {
  // Android keeps the native window across surfaceChanged. Releasing the surface here
  // would rebuild it on that same window while the old swapchain is still connected,
  // which fails with VK_ERROR_NATIVE_WINDOW_IN_USE_KHR (a lost device). Skip the frame.
  if (window::is_surface_changing()) {
    return false;
  }
  const bool invalidated = window::consume_surface_invalidated();
  if (!window::is_presentable() || (invalidated && webgpu::surface_window_changed())) {
    static int sNotPresentable = 0;
    if (sNotPresentable++ < 3) {
      Log.warn("{}: not presentable - surfaceReady={} backgrounded={} invalidated={}", caller,
               window::is_surface_ready(), window::is_backgrounded(), invalidated);
    }
    webgpu::release_surface();
    return false;
  }
  return true;
}
#endif

void release_lost_surface() noexcept {
#ifdef AURORA_ENABLE_GX
  if (g_surface) {
    service_surface("release_lost_surface");
  }
#endif
}

bool begin_frame() noexcept {
  ZoneScoped;
#ifdef AURORA_ENABLE_GX
  {
    if (!service_surface("begin_frame")) {
      return false;
    }
    if (window::is_paused()) {
      static int sPaused = 0;
      if (sPaused++ < 3) {
        Log.warn("begin_frame: paused - surfaceReady={} backgrounded={}", window::is_surface_ready(),
                 window::is_backgrounded());
      }
      return false;
    }
    if (!g_surface) {
      webgpu::refresh_surface(true);
      if (!g_surface) {
        return false;
      }
    }
    if (const uint32_t quality = g_pendingQuality.exchange(0, std::memory_order_acq_rel); quality != 0) {
      webgpu::set_quality(quality >> 16, static_cast<uint16_t>(quality & 0xFFFF));
    }
  }

  phase::set(phase::Main, "begin_frame (waiting for a frame slot)");
  if (!gfx::begin_frame()) {
    return false;
  }
  // A failed staging-buffer acquisition must not leave an ImGui frame open.
  imgui::new_frame(window::get_window_size());
  gx::fifo::begin_frame();
#endif
  return true;
}

void end_frame() noexcept {
  ZoneScoped;
#ifdef AURORA_ENABLE_GX
  phase::set(phase::Main, "end_frame (recording and queueing the frame)");
  gx::fifo::drain();
  gx::fifo::end_frame();
  gx::texture::end_frame();
  gfx::finish();
  auto imguiDrawData = imgui::freeze();

  const auto& presentSource = webgpu::present_source();
  const auto viewport = webgpu::calculate_present_viewport(webgpu::g_graphicsConfig.surfaceConfiguration.width,
                                                           webgpu::g_graphicsConfig.surfaceConfiguration.height,
                                                           presentSource.size.width, presentSource.size.height);

  wgpu::BindGroup rmlBindGroup;
  bool rmlOverlay = false;
#if AURORA_ENABLE_RMLUI
  if (rmlui::is_initialized()) {
    auto rmlFrame = rmlui::record_frame(viewport);
    rmlBindGroup = std::move(rmlFrame.bindGroup);
    rmlOverlay = rmlFrame.overlay;
  }
#endif

  gfx::end_frame([rmlBindGroup = std::move(rmlBindGroup), rmlOverlay, viewport,
                  imguiDrawData = std::move(imguiDrawData)](
                     wgpu::CommandEncoder& encoder, std::vector<gfx::AfterSubmitCallback> afterSubmitCallbacks) {
    if (g_screenshotRequested.exchange(false, std::memory_order_acq_rel)) {
      record_screenshot(encoder, afterSubmitCallbacks);
    }
    wgpu::Texture currentTexture;
    wgpu::TextureView currentView;
    auto surfaceStatus = wgpu::SurfaceGetCurrentTextureStatus::Error;
    bool acquireAttempted = false;
    {
      window::SurfaceLock surfaceLock;
      if (window::is_presentable() && g_surface) {
        ZoneScopedN("Acquire texture");
        phase::set(phase::Render, "surface GetCurrentTexture");
        wgpu::SurfaceTexture surfaceTexture;
        g_surface.GetCurrentTexture(&surfaceTexture);
        phase::set(phase::Render, "present blit recording");
        acquireAttempted = true;
        surfaceStatus = surfaceTexture.status;
        if (surfaceStatus == wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal) {
          currentTexture = std::move(surfaceTexture.texture);
          currentView = currentTexture.CreateView();
        }
      }
    }

    const bool canPresent = currentTexture && currentView;
    if (canPresent) {
      wgpu::BindGroup presentBindGroup;
      if (rmlBindGroup && !rmlOverlay) {
        presentBindGroup = rmlBindGroup;
      } else {
        const auto& resampledSource = webgpu::resample_present_source(encoder, viewport);
        presentBindGroup = webgpu::create_copy_bind_group(resampledSource);
      }
      {
        const std::array attachments{
            wgpu::RenderPassColorAttachment{
                .view = currentView,
                .loadOp = wgpu::LoadOp::Clear,
                .storeOp = wgpu::StoreOp::Store,
            },
        };
        const wgpu::RenderPassDescriptor renderPassDescriptor{
            .label = "EFB copy render pass",
            .colorAttachmentCount = attachments.size(),
            .colorAttachments = attachments.data(),
            .timestampWrites = webgpu::gpu_prof::pass_writes("Present blit"),
        };
        const auto pass = encoder.BeginRenderPass(&renderPassDescriptor);
        // Copy EFB -> XFB (swapchain)
        pass.SetPipeline(webgpu::g_CopyPipeline);
        pass.SetBindGroup(0, presentBindGroup, 0, nullptr);
        set_present_viewport(pass, viewport, webgpu::g_graphicsConfig.surfaceConfiguration.width,
                             webgpu::g_graphicsConfig.surfaceConfiguration.height);

        pass.Draw(3);
        if (rmlBindGroup && rmlOverlay) {
          pass.SetPipeline(webgpu::g_CopyPremultipliedAlphaPipeline);
          pass.SetBindGroup(0, rmlBindGroup, 0, nullptr);
          pass.Draw(3);
        }
        // ImGui draws in the same pass: a second pass would write the whole swapchain image out
        // and read it back in, which costs a tile-based (mobile) GPU a full-screen round trip.
        pass.SetViewport(0.f, 0.f, static_cast<float>(webgpu::g_graphicsConfig.surfaceConfiguration.width),
                         static_cast<float>(webgpu::g_graphicsConfig.surfaceConfiguration.height), 0.f, 1.f);
        imgui::render(pass, imguiDrawData);
        pass.End();
      }
    } else {
      Log.info("Skipping present; window not presentable");
    }
    webgpu::gpu_prof::frame_end(encoder);
    const wgpu::CommandBufferDescriptor cmdBufDescriptor{.label = "Redraw command buffer"};
    const auto buffer = encoder.Finish(&cmdBufDescriptor);
    {
      ZoneScopedN("Queue Submit");
      phase::set(phase::Render, "queue submit");
      g_queue.Submit(1, &buffer);
    }
    phase::set(phase::Render, "after submit");
    webgpu::gpu_prof::after_submit();
    if (canPresent && g_surface) {
      ZoneScopedN("Present");
      wgpu::ConvertibleStatus status = wgpu::Status::Error;
      {
        window::SurfaceLock surfaceLock;
        if (window::is_presentable()) {
          phase::set(phase::Render, "surface Present");
          status = g_surface.Present();
        }
      }
      phase::set(phase::Render, "after present");
      if (status) {
        gfx::after_present();
      } else {
        Log.warn("Surface present failed");
        webgpu::release_surface();
      }
    } else if (g_surface && acquireAttempted) {
      // Without an attempt, surfaceStatus is only the Error placeholder: the window was briefly
      // not presentable (an Android surfaceChanged, say). Dropping the surface then made
      // begin_frame build a second VkSurface on the same ANativeWindow while the old swapchain
      // was still connected, which fails with VK_ERROR_NATIVE_WINDOW_IN_USE_KHR (device lost).
      switch (surfaceStatus) {
      case wgpu::SurfaceGetCurrentTextureStatus::Timeout:
        Log.warn("Surface texture acquisition timed out");
        break;
      case wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal:
      case wgpu::SurfaceGetCurrentTextureStatus::Outdated:
        Log.info("Surface texture is {}, reconfiguring swapchain", magic_enum::enum_name(surfaceStatus));
        window::push_custom_event(window::CustomEvent::RefreshSurface);
        break;
      case wgpu::SurfaceGetCurrentTextureStatus::Lost:
        Log.warn("Surface texture is {}, releasing surface", magic_enum::enum_name(surfaceStatus));
        webgpu::release_surface();
        break;
      case wgpu::SurfaceGetCurrentTextureStatus::Error:
        Log.warn("Surface texture is {}, dropping surface", magic_enum::enum_name(surfaceStatus));
        g_surface = {};
        window::set_surface_held(false);
        break;
      default:
        if (!window::is_presentable()) {
          webgpu::release_surface();
        } else {
          Log.error("Failed to get surface texture: {}", magic_enum::enum_name(surfaceStatus));
        }
        break;
      }
    }
    for (auto& callback : afterSubmitCallbacks) {
      if (callback) {
        callback();
      }
    }
    gfx::after_submit();

    TracyPlotConfig("aurora: lastVertSize", tracy::PlotFormatType::Memory, false, true, 0);
    TracyPlotConfig("aurora: lastUniformSize", tracy::PlotFormatType::Memory, false, true, 0);
    TracyPlotConfig("aurora: lastIndexSize", tracy::PlotFormatType::Memory, false, true, 0);
    TracyPlotConfig("aurora: lastStorageSize", tracy::PlotFormatType::Memory, false, true, 0);
    TracyPlotConfig("aurora: lastTextureUploadSize", tracy::PlotFormatType::Memory, false, true, 0);

    const auto& stats = gfx::detail::resources().stats;
    TracyPlot("aurora: queuedPipelines", static_cast<int64_t>(stats.queuedPipelines));
    TracyPlot("aurora: createdPipelines", static_cast<int64_t>(stats.createdPipelines));
    TracyPlot("aurora: drawCallCount", static_cast<int64_t>(stats.drawCallCount));
    TracyPlot("aurora: mergedDrawCallCount", static_cast<int64_t>(stats.mergedDrawCallCount));
    TracyPlot("aurora: lastVertSize", static_cast<int64_t>(stats.lastVertSize));
    TracyPlot("aurora: lastUniformSize", static_cast<int64_t>(stats.lastUniformSize));
    TracyPlot("aurora: lastIndexSize", static_cast<int64_t>(stats.lastIndexSize));
    TracyPlot("aurora: lastStorageSize", static_cast<int64_t>(stats.lastStorageSize));
    TracyPlot("aurora: lastTextureUploadSize", static_cast<int64_t>(stats.lastTextureUploadSize));
  });

#endif
}
} // namespace

void request_screenshot() noexcept {
#ifdef AURORA_ENABLE_GX
  g_screenshotRequested.store(true, std::memory_order_release);
#endif
}
} // namespace aurora

// C API bindings
AuroraInfo aurora_initialize(int argc, char* argv[], const AuroraConfig* config) {
  return aurora::initialize(argc, argv, *config);
}
void aurora_shutdown() { aurora::shutdown(); }
const AuroraEvent* aurora_update() { return aurora::update(); }
bool aurora_begin_frame() { return aurora::begin_frame(); }
void aurora_release_lost_surface() { aurora::release_lost_surface(); }
bool aurora_is_suspended() {
  return aurora::window::is_backgrounded() || !aurora::window::is_surface_ready() ||
         aurora::window::is_surface_changing();
}
void aurora_end_frame() { aurora::end_frame(); }
AuroraBackend aurora_get_backend() { return aurora::g_config.desiredBackend; }
const char* aurora_get_gpu_driver() {
  static std::string driver;
  const wgpu::StringView description = aurora::webgpu::g_adapterInfo.description;
  driver = description.IsUndefined() ? std::string() : std::string(description.data, description.length);
  return driver.c_str();
}
bool aurora_vulkan_library_failed() { return aurora::webgpu::g_vulkanLibraryFailed; }
const AuroraBackend* aurora_get_available_backends(size_t* count) {
  if (count != nullptr) {
    *count = aurora::PreferredBackendOrder.size();
  }
  return aurora::PreferredBackendOrder.data();
}
void aurora_set_log_level(AuroraLogLevel level) { aurora::g_config.logLevel = level; }
void aurora_set_pause_on_focus_lost(bool value) { aurora::g_config.pauseOnFocusLost = value; }
void aurora_set_background_input(bool value) {
  aurora::g_config.allowJoystickBackgroundEvents = value;
  aurora::window::set_background_input(value);
}
void aurora_set_resampler(AuroraSampler sampler) {
#ifdef AURORA_ENABLE_GX
  aurora::webgpu::set_resampler(sampler);
#else
  (void)sampler;
#endif
}
void aurora_set_graphics_quality(uint32_t msaa, uint16_t maxTextureAnisotropy) {
  aurora::g_pendingQuality.store(std::min(msaa, 4u) << 16 | maxTextureAnisotropy, std::memory_order_release);
}
void aurora_set_timescale(float scale) { aurora::time::set_scale(scale); }
float aurora_get_timescale() { return aurora::time::scale(); }
