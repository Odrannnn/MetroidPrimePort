#include <gtest/gtest.h>

#include "gfx/fx_pipelines.hpp"
#include "gfx/hash.hpp"
#include "gfx/resources.hpp"
#include "gfx/volfog.hpp"
#include "gx/gx.hpp"
#include "webgpu/gpu.hpp"

#include <string>
#include <string_view>

// The HDR scene's water, full-screen fog and per-draw fog readers without a GPU (stage 3B): the
// sources they generate, and the config bits that key them.
namespace aurora::gfx {
namespace {

size_t count(std::string_view s, std::string_view what) {
  size_t n = 0;
  for (size_t at = s.find(what); at != std::string_view::npos; at = s.find(what, at + what.size()))
    ++n;
  return n;
}

// The text after a function's opening line up to its closing brace at column 0.
std::string_view body(std::string_view s, std::string_view signature) {
  const size_t start = s.find(signature);
  if (start == std::string_view::npos)
    return {};
  const size_t end = s.find("\n}", start);
  return s.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
}

// --- Water ------------------------------------------------------------------------------------------------------

// water.cpp's blend tail: the snapshot read raw, the result left as light.
constexpr std::string_view WaterHdrBlend = R"(  let x = max(o0 + snap.rgb * (1.0 - o1), vec3f(0.0));
  return vec4f(x, snap.a);
)";

water::PipelineConfig waterConfig(uint32_t flags) {
  water::PipelineConfig config{};
  config.colorAttachmentCount = 1;
  config.colorFormats[0] = (flags & water::WaterSceneHdr) != 0 ? webgpu::SceneHdrFormat
                                                               : wgpu::TextureFormat::RGBA8Unorm;
  config.depthStencilFormat = wgpu::TextureFormat::Depth24Plus;
  config.flags = flags;
  return config;
}

TEST(SceneHdrWater, SourcesBlendRawLight) {
  for (uint32_t flags = 0; flags < 4; ++flags) {
    SCOPED_TRACE(flags);
    const std::string hdr = water::pipeline_source(waterConfig(flags | water::WaterSceneHdr));
    const std::string prefix = std::string("const BOTTOM: bool = ") + ((flags & 1) ? "true" : "false") +
                               ";\nconst RAIN: bool = " + ((flags & 2) ? "true" : "false") + ";\n";
    EXPECT_TRUE(hdr.starts_with(prefix));
    EXPECT_EQ(count(hdr, WaterHdrBlend), 1u);
    // No decode, inverse curve, clip, curve or encode anywhere.
    EXPECT_EQ(count(hdr, "srgb_dec("), 0u);
    EXPECT_EQ(count(hdr, "srgb_enc("), 0u);
    EXPECT_EQ(count(hdr, "untone("), 0u);
    EXPECT_EQ(count(hdr, "tone("), 0u);
    EXPECT_EQ(count(hdr, "vec3f(4.0)"), 0u);
  }
}

TEST(SceneHdrWater, TheHdrBitIsANewKeyNotANewField) {
  static_assert(sizeof(water::PipelineConfig) == sizeof(wgpu::TextureFormat) * (MaxColorAttachments + 1) + 6 * 4);
  EXPECT_EQ(water::WaterPipelineConfigVersion, 1u);
  for (uint32_t flags = 0; flags < 4; ++flags) {
    auto hdr = waterConfig(flags | water::WaterSceneHdr);
    hdr.colorFormats[0] = wgpu::TextureFormat::RGBA8Unorm; // the same layout bytes but for the bit
    EXPECT_NE(xxh3_hash(waterConfig(flags)), xxh3_hash(hdr));
  }
}

TEST(SceneHdrWater, AFormatMismatchIsCaught) {
  RenderTargetLayout ldr{};
  ldr.colorAttachmentCount = 1;
  ldr.colorAttachments[SceneColorAttachmentIndex].format = wgpu::TextureFormat::RGBA8Unorm;
  RenderTargetLayout hdr = ldr;
  hdr.colorAttachments[SceneColorAttachmentIndex].format = webgpu::SceneHdrFormat;
  RenderTargetLayout none{};
  for (uint32_t flags = 0; flags < 4; ++flags) {
    EXPECT_TRUE(water::scene_format_matches(ldr, flags));
    EXPECT_FALSE(water::scene_format_matches(ldr, flags | water::WaterSceneHdr));
    EXPECT_TRUE(water::scene_format_matches(hdr, flags | water::WaterSceneHdr));
    EXPECT_FALSE(water::scene_format_matches(hdr, flags));
    EXPECT_TRUE(water::scene_format_matches(none, flags));
    EXPECT_FALSE(water::scene_format_matches(none, flags | water::WaterSceneHdr));
  }
}

// --- Full-screen volumetric fog -------------------------------------------------------------------------------

constexpr std::string_view FogHdrTail = R"(  let x = f.rgb * fog.a + fog.rgb;
  return vec4f(x, f.a);
)";

TEST(SceneHdrVolFog, ApplyFogsRawLight) {
  for (const uint32_t samples : {1u, 4u}) {
    SCOPED_TRACE(samples);
    const std::string hdr = volfog::apply_source(samples);
    EXPECT_EQ(count(hdr, FogHdrTail), 1u);
    EXPECT_EQ(count(hdr, "DEPTH_TYPE"), 0u);
    EXPECT_EQ(count(hdr, "texture_depth_multisampled_2d") > 0, samples > 1);
    const std::string_view apply = body(hdr, "fn fs_apply(");
    ASSERT_FALSE(apply.empty());
    EXPECT_EQ(count(apply, "exposed("), 0u);
    EXPECT_EQ(count(apply, "tone("), 0u);
    EXPECT_EQ(count(apply, "srgb_enc("), 0u);
    EXPECT_EQ(count(apply, "srgb_dec("), 0u);
    EXPECT_EQ(count(apply, "vec3f(4.0)"), 0u);
    // The in-scatter's exposure once, in the fog both applies share.
    const std::string_view at = body(hdr, "fn fog_at(");
    ASSERT_FALSE(at.empty());
    EXPECT_EQ(count(at, "p.colorA.w"), 1u);
    EXPECT_EQ(count(apply, "fog_at("), 1u);
    const std::string_view blend = body(hdr, "fn fs_apply_blend(");
    ASSERT_FALSE(blend.empty());
    EXPECT_EQ(count(blend, "fog_at("), 1u);
    EXPECT_EQ(count(blend, "p.colorA.w"), 0u);
  }
}

} // namespace
} // namespace aurora::gfx

// --- Per-draw volumetric fog (GX vf_apply) ------------------------------------------------------------------------

namespace aurora::gx {
namespace {

using gfx::count;

ShaderConfig fogConfig(int pbr, u8 volFog, bool hdr) {
  ShaderConfig config;
  config.pbr = static_cast<u8>(pbr);
  config.volFog = volFog;
  config.sceneHdr = hdr;
  config.tevStageCount = 1;
  auto& stage = config.tevStages[0];
  stage.colorPass = {GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO};
  stage.alphaPass = {GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO};
  stage.texCoordId = GX_TEXCOORD0;
  stage.texMapId = GX_TEXMAP0;
  stage.channelId = GX_COLOR0A0;
  for (const auto attr : {GX_VA_POS, GX_VA_NRM, GX_VA_TEX0}) {
    auto& a = config.attrs[attr];
    a.attrType = GX_DIRECT;
    a.cnt = attr == GX_VA_TEX0 ? 2 : 3;
    a.compType = GX_F32;
    a.stride = 12;
  }
  config.colorChannels[GX_COLOR0].lightingEnabled = true;
  config.colorChannels[GX_COLOR0].matSrc = GX_SRC_REG;
  config.colorChannels[GX_COLOR0].ambSrc = GX_SRC_REG;
  return config;
}

constexpr u8 FogModes[] = {VolFogBlended, VolFogAdditive, VolFogOpaque, VolFogPremultiplied};
constexpr std::string_view Convert = "prev = vec4f(hdr_exposed(prev.rgb,";

TEST(SceneHdrGxFog, LdrDrawsHaveNoFog) {
  for (const u8 mode : FogModes) {
    SCOPED_TRACE(static_cast<int>(mode));
    const std::string ldr = build_shader_source(fogConfig(0, mode, false));
    EXPECT_EQ(count(ldr, "vf_apply"), 0u);
    EXPECT_EQ(count(ldr, "hdr_exposed("), 0u);
  }
}

TEST(SceneHdrGxFog, TevDrawsFogTheirLightAfterOneConversion) {
  for (const u8 mode : FogModes) {
    SCOPED_TRACE(static_cast<int>(mode));
    const std::string hdr = build_shader_source(fogConfig(0, mode, true));
    EXPECT_EQ(count(hdr, "prev = vf_apply_hdr(prev,"), 1u);
    EXPECT_EQ(count(hdr, Convert), 1u) << "converted once";
    EXPECT_EQ(count(hdr, "if (ubuf.hdr_tev_blend.x > 0.5)"), 1u);
    EXPECT_EQ(count(hdr, "prev = vec4f(hdr_tev_srcalpha_add(prev.rgb, prev.a,"), 1u);
    EXPECT_LT(hdr.find("prev = vec4f(hdr_tev_srcalpha_add(prev.rgb, prev.a,"), hdr.find("prev = vf_apply_hdr(prev,"));
    EXPECT_LT(hdr.find(Convert), hdr.find("prev = vf_apply_hdr(prev,")) << "the fog applies in the light";
    // The HDR fog itself: transmittance plus the in-scatter, no curve, decode, clip or encode.
    const std::string_view fn = gfx::body(hdr, "fn vf_apply_hdr(");
    ASSERT_FALSE(fn.empty());
    EXPECT_NE(fn.find("return vec4f(c.rgb * fog.a + light, c.a);"), std::string_view::npos);
    for (const std::string_view banned : {"vf_exposed(", "vf_tone(", "vf_srgb_enc(", "vf_srgb_dec(", "untone(", "4.0"}) {
      EXPECT_EQ(count(fn, banned), 0u) << banned;
    }
    for (const std::string_view banned : {"fn vf_exposed(", "fn vf_tone(", "fn vf_untone(", "fn vf_apply(c"}) {
      EXPECT_EQ(count(hdr, banned), 0u) << banned;
    }
  }
}

TEST(SceneHdrGxFog, TheDisplayFogStaysBeforeTheConversion) {
  for (const u8 mode : FogModes) {
    SCOPED_TRACE(static_cast<int>(mode));
    ShaderConfig config = fogConfig(0, mode, true);
    config.fogType = GX_FOG_PERSP_LIN;
    const std::string hdr = build_shader_source(config);
    const size_t displayFog = hdr.find("var fogZ");
    ASSERT_NE(displayFog, std::string::npos);
    EXPECT_LT(displayFog, hdr.find(Convert));
    EXPECT_EQ(count(hdr, Convert), 1u);
  }
}

TEST(SceneHdrGxFog, PbrDrawsFogOnce) {
  for (const u8 mode : FogModes) {
    SCOPED_TRACE(static_cast<int>(mode));
    const std::string ldr = build_shader_source(fogConfig(1, mode, false));
    const std::string hdr = build_shader_source(fogConfig(1, mode, true));
    for (const std::string* s : {&ldr, &hdr}) {
      EXPECT_EQ(count(*s, "prev = vf_apply(prev,"), 0u);
      EXPECT_EQ(count(*s, "prev = vf_apply_hdr(prev,"), 0u);
      EXPECT_EQ(count(*s, "pbr_out = pbr_out * pbr_vf.a +"), 1u);
      EXPECT_EQ(count(*s, "pbr_pass *= pbr_vf.a;"), 1u);
    }
    EXPECT_EQ(count(hdr, Convert), 0u) << "PBR light is exposed already";
  }
}

TEST(SceneHdrGxFog, NoFogNoHdrFunction) {
  const std::string hdr = build_shader_source(fogConfig(0, VolFogNone, true));
  EXPECT_EQ(count(hdr, "vf_apply"), 0u);
  EXPECT_EQ(count(hdr, Convert), 1u);
}

} // namespace
} // namespace aurora::gx
