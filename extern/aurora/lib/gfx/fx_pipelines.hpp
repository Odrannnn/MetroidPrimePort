#pragma once

#include <aurora/gfx.hpp>

#include <array>
#include <cstdint>
#include <type_traits>

#include <webgpu/webgpu_cpp.h>

// Pipeline configs of the custom draw types (Remastered VFX particles, water). They go through
// the pipeline cache like GX pipelines, so the ones met in earlier runs compile on the pool at
// startup instead of on the render thread the first time an effect or a water surface is drawn.
namespace aurora::gfx::vfx {
constexpr uint32_t VfxPipelineConfigVersion = 1;
struct PipelineConfig {
  std::array<wgpu::TextureFormat, MaxColorAttachments> colorFormats{};
  wgpu::TextureFormat depthStencilFormat = wgpu::TextureFormat::Undefined;
  uint32_t version = VfxPipelineConfigVersion;
  uint32_t colorAttachmentCount = 0;
  uint32_t msaaSamples = 1;
  uint32_t features = 0;
  uint32_t slotKey = 0;
  uint32_t blend = 0;
  uint32_t compare = 0; // wgpu::CompareFunction
  uint32_t depthWrite = 0;
};
static_assert(std::has_unique_object_representations_v<PipelineConfig>);

wgpu::RenderPipeline create_pipeline(const PipelineConfig& config);
} // namespace aurora::gfx::vfx

namespace aurora::gfx::water {
constexpr uint32_t WaterPipelineConfigVersion = 1;
struct PipelineConfig {
  std::array<wgpu::TextureFormat, MaxColorAttachments> colorFormats{};
  wgpu::TextureFormat depthStencilFormat = wgpu::TextureFormat::Undefined;
  uint32_t version = WaterPipelineConfigVersion;
  uint32_t colorAttachmentCount = 0;
  uint32_t msaaSamples = 1;
  uint32_t flags = 0; // 1 bottom, 2 rain
  uint32_t cull = 0;  // wgpu::CullMode
  uint32_t compare = 0;
};
static_assert(std::has_unique_object_representations_v<PipelineConfig>);

wgpu::RenderPipeline create_pipeline(const PipelineConfig& config);
} // namespace aurora::gfx::water
