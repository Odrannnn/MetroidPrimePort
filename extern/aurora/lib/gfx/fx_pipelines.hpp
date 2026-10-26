#pragma once

#include <aurora/gfx.hpp>
#include <aurora/vfx.hpp>

#include <array>
#include <cstdint>
#include <string>
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
// Set in PipelineConfig::blend for a draw into the HDR scene (exposed linear, RGBA16F). Blend modes
// are 0..MaxBlend, so no LDR config, cached record or in-memory key can carry it.
constexpr uint32_t BlendSceneHdr = 1u << 31;

wgpu::RenderPipeline create_pipeline(const PipelineConfig& config);
// The blend a config's pipeline uses: its mode's, whatever the HDR bit.
wgpu::BlendState pipeline_blend(const PipelineConfig& config);
// ModelBackCull remains in features, so both cached config bytes and the live key distinguish it.
inline wgpu::PrimitiveState pipeline_primitive(const PipelineConfig& config) {
  const bool back = (config.features & ModelBackCull) != 0;
  // Native nonmirrored Ice: main 0x1ae378 selects NVN front-face 1; SDK 0x493890
  // emits Maxwell 0x191c/0x901 (CCW). Its origin compensation and the proper
  // imported basis preserve the same visible side as WebGPU CCW, not GX CW.
  return {.topology = wgpu::PrimitiveTopology::TriangleList,
          .frontFace = wgpu::FrontFace::CCW,
          .cullMode = back ? wgpu::CullMode::Back : wgpu::CullMode::None};
}
// The WGSL a config's pipeline is built from.
std::string pipeline_source(const PipelineConfig& config);
// Whether a draw's blend (BlendSceneHdr or not) fits the scene attachment it is drawn into.
bool scene_format_matches(const RenderTargetLayout& layout, uint32_t blend) noexcept;
// The features a draw's shader and pipeline key see: the AlreadyExposed policy bit is not a shader feature.
constexpr uint32_t shader_features(uint32_t features) noexcept {
  return features & ~uint32_t(AlreadyExposed);
}
// The exposure a draw's uniform carries (tone[0][3], which the HDR tail multiplies by): the GX tone
// row's, inherited from the last PBR draw, or 1 for an already-exposed material drawn into the HDR
// scene. An LDR draw keeps the inherited one whatever the policy. Only otherwise may an
// explicit forward exposure replace the inherited HDR value.
constexpr float draw_exposure(float inherited, bool hdrPass, bool alreadyExposed,
                              std::optional<float> exposureOverride = std::nullopt) noexcept {
  if (!hdrPass) return inherited;
  if (alreadyExposed) return 1.f;
  return exposureOverride.value_or(inherited);
}
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
// Set in PipelineConfig::flags of every surface: water is drawn only into the HDR scene (exposed
// linear, RGBA16F), its snapshot read raw and the blend's result written as light. Flags are 1 and 2,
// so no config recorded before the LDR blend was removed can carry it.
constexpr uint32_t WaterSceneHdr = 1u << 31;

wgpu::RenderPipeline create_pipeline(const PipelineConfig& config);
// The WGSL a config's pipeline is built from.
std::string pipeline_source(const PipelineConfig& config);
// Whether a draw's flags (WaterSceneHdr or not) fit the scene attachment it is drawn into.
bool scene_format_matches(const RenderTargetLayout& layout, uint32_t flags) noexcept;
} // namespace aurora::gfx::water
