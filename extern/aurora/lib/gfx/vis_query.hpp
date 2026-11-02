#pragma once

#include <webgpu/webgpu_cpp.h>

// Render-thread hooks of aurora/vis_query.hpp, beside overdraw's.
namespace aurora::gfx::vis_query::detail {
// The query set for a pass's descriptor (null before the first query, without a free slot, or
// when overdraw claimed the pass).
wgpu::QuerySet pass_begin(bool claimed);
void pass_end();
void frame_end(const wgpu::CommandEncoder& encoder);
void after_submit();
} // namespace aurora::gfx::vis_query::detail
