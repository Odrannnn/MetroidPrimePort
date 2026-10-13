#pragma once

#include <aurora/aurora.h>
#include <aurora/event.h>

struct SDL_Window;
struct SDL_Renderer;

namespace aurora::window {

enum class CustomEvent {
  FutureResize,
  RefreshSurface,
  End,
};
static Uint32 operator+(Uint32 lhs, CustomEvent rhs) { return lhs + static_cast<Uint32>(rhs); }

// On Android in particular, we need to hold a mutex around critical areas like surface creation
// and presentation, so that the SDLActivity doesn't destroy the surface out from underneath us.
class SurfaceLock {
public:
  SurfaceLock() noexcept;
  ~SurfaceLock();

  SurfaceLock(const SurfaceLock&) = delete;
  SurfaceLock& operator=(const SurfaceLock&) = delete;
};

bool initialize();
bool initialize_event_watch();
bool push_custom_event(CustomEvent eventType);
void shutdown();
bool create_window(AuroraBackend backend);
bool create_renderer();
void destroy_window();
void show_window();
AuroraWindowSize get_window_size();
// The frame buffer size a render scale would give, without applying it.
bool get_frame_buffer_size_for_scale(float scale, uint32_t* width, uint32_t* height);
const AuroraEvent* poll_events();
SDL_Window* get_sdl_window();
SDL_Renderer* get_sdl_renderer();
const void* get_native_window_handle() noexcept;
bool is_paused() noexcept;
bool is_presentable() noexcept;
bool is_backgrounded() noexcept;
bool is_surface_ready() noexcept;
bool consume_surface_invalidated() noexcept;
void set_surface_ready(bool ready) noexcept;
bool is_surface_changing() noexcept;
void set_surface_changing(bool changing) noexcept;
// Tracks whether the renderer holds a surface for the platform window. Android's
// surfaceDestroyed waits for it to drop, since the native window must not be used
// after that callback returns.
void set_surface_held(bool held) noexcept;
bool wait_surface_released(int timeoutMs) noexcept;
void set_title(const char* title);
void set_fullscreen(bool fullscreen);
bool get_fullscreen();
void set_window_size(uint32_t width, uint32_t height);
void set_window_position(uint32_t x, uint32_t y);
void center_window();
void request_frame_buffer_resize();
void set_frame_buffer_scale(float scale);
void set_frame_buffer_aspect_fit(bool fit);
void set_background_input(bool value);
}; // namespace aurora::window
