#pragma once

#include <cstdint>

namespace aurora {
void request_screenshot() noexcept;
// Saves the next frame's picture as a PNG at `path` (relative to the cwd); poll the result:
// 0 pending, 1 saved (size filled in), -1 failed.
void request_screenshot_png(const char* path) noexcept;
int screenshot_png_state(uint32_t* width, uint32_t* height) noexcept;
// The frame buffer size render scale `scale` would give (false before the window exists).
bool frame_buffer_size_for_scale(float scale, uint32_t* width, uint32_t* height) noexcept;
// The device's maxTextureDimension2D (0 when unknown).
uint32_t max_texture_dimension() noexcept;
}
