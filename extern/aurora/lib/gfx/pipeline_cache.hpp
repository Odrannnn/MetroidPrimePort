#pragma once

#include "types.hpp"

#include <functional>

namespace aurora::gfx::clear {
struct PipelineConfig;
} // namespace aurora::gfx::clear

namespace aurora::gfx::vfx {
struct PipelineConfig;
} // namespace aurora::gfx::vfx

namespace aurora::gfx::water {
struct PipelineConfig;
} // namespace aurora::gfx::water

namespace aurora::gx {
struct PipelineConfig;
} // namespace aurora::gx

namespace aurora::rmlui {
struct PipelineConfig;
} // namespace aurora::rmlui

namespace aurora::gfx {

enum class ShaderType : uint8_t {
  Clear = 0,
  GX = 1,
  Rml = 2,
  Vfx = 3,
  Water = 4,
};

using NewPipelineCallback = std::function<wgpu::RenderPipeline()>;

void initialize_pipeline_cache();
void shutdown_pipeline_cache();
void begin_pipeline_frame();
// Forgets every built and queued pipeline; the next lookup rebuilds them (shader reload).
void drop_pipelines();
void end_pipeline_frame();

template <typename Config>
PipelineRef find_pipeline(ShaderType type, const Config& config, NewPipelineCallback&& cb);

bool get_pipeline(PipelineRef ref, wgpu::RenderPipeline& pipeline);

// Render thread (custom draw callbacks): the pipeline for `config`, waiting for it if the cache
// hasn't built it yet (a queued one moves to the front). Null if creation failed.
template <typename Config>
wgpu::RenderPipeline require_pipeline(ShaderType type, const Config& config, NewPipelineCallback&& cb);

} // namespace aurora::gfx
