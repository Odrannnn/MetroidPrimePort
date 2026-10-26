#pragma once

#include "../gx/pipeline.hpp"
#include "types.hpp"

#include <webgpu/webgpu_cpp.h>

// Diagnostic: with AURORA_OVERDRAW=<file>, one frame in Interval wraps every GX draw of its EFB
// passes in an occlusion query and rewrites <file> as a TSV of the samples each draw passed
// (depth and stencil tests), with its pass and pipeline. Off (no variable) costs one branch per
// draw. Encode-side calls run on the encoding thread; note_pipeline on any.
namespace aurora::gfx::overdraw {
bool enabled();
void note_pipeline(PipelineRef ref, const gx::PipelineConfig& config);

void frame_begin();
// The query set for a pass's descriptor, null when this frame is not captured.
wgpu::QuerySet pass_begin(uint32_t passIndex, const wgpu::Extent3D& size, uint32_t samples);
void pass_end();
// Returns the query to end after the draw, or UINT32_MAX outside a pass_begin/pass_end.
uint32_t draw_begin(PipelineRef ref, const wgpu::RenderPassEncoder& pass);
void draw_end(uint32_t query, const wgpu::RenderPassEncoder& pass);
void frame_end(const wgpu::CommandEncoder& encoder);
void after_submit();
} // namespace aurora::gfx::overdraw
