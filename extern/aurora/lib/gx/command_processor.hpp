#pragma once

#include "../internal.hpp"

#include <atomic>
#include <cstdint>

namespace aurora::gx::fifo {

struct ProcessResult {
  uint32_t bytesProcessed;
  bool drawDone;
};

// Process GX FIFO commands until the next draw done event or end of buffer
ProcessResult process(const uint8_t* data, uint32_t size) noexcept;
void clear_draw_cache() noexcept;

} // namespace aurora::gx::fifo

namespace aurora::gx::testing {
// The vertex array overrun check (MP_GX_CHECK_ARRAYS, see command_processor.cpp): whether it runs,
// and how many overruns it has found. Tests set the flag rather than the environment.
extern std::atomic<bool> vertexArrayCheck;
extern std::atomic<uint32_t> vertexArrayOverruns;
void set_vertex_array_check(bool enabled) noexcept;
} // namespace aurora::gx::testing
