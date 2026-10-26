#include <array>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "frame_staging.hpp"
#include "gfx/frame_packet.hpp"
#include "gfx/recording.hpp"
#include "gfx/resources.hpp"
#include "gx/gx.hpp"
#include "gx/shader_info.hpp"

namespace aurora::gx {
namespace {

// gx::MaxUniformSize is one shared number: shader_info.cpp refuses to build a
// shader above it, frame.cpp binds exactly that much out of the uniform buffer
// and recording.cpp pads each frame's uniform packet by it. A legitimate PBR
// material did not fit the old 3840 and aborted the game.
class GxUniformLimitTest : public ::testing::Test {
protected:
  void SetUp() override { saved = setAlignment(256); }
  void TearDown() override { setAlignment(saved); }

  static u32 setAlignment(u32 alignment) {
    const auto previous = gfx::detail::resources().limits.minUniformBufferOffsetAlignment;
    gfx::detail::resources().limits.minUniformBufferOffsetAlignment = alignment;
    return previous;
  }

  // Everything that can add uniform data at once: 4 TEV stages each reading a
  // texture, a colour channel and a constant register, all 8 texcoords with a
  // post matrix, 4 indirect stages driving all 3 indirect matrices, fog,
  // lighting with register-sourced material/ambient, and PBR.
  static ShaderConfig maximal(bool pbr) {
    ShaderConfig config;
    config.pbr = pbr ? 1 : 0;
    config.fogType = GX_FOG_LIN;
    config.numIndStages = 4;
    config.tevStageCount = 4;
    constexpr std::array kc{GX_TEV_KCSEL_K0, GX_TEV_KCSEL_K1, GX_TEV_KCSEL_K2, GX_TEV_KCSEL_K3};
    constexpr std::array is{GX_INDTEXSTAGE0, GX_INDTEXSTAGE1, GX_INDTEXSTAGE2, GX_INDTEXSTAGE3};
    for (u32 i = 0; i < config.tevStageCount; ++i) {
      auto& stage = config.tevStages[i];
      stage.colorPass.a = GX_CC_CPREV;
      stage.colorPass.b = GX_CC_TEXC;
      stage.colorPass.c = GX_CC_RASC;
      stage.colorPass.d = GX_CC_KONST;
      stage.alphaPass.a = GX_CA_APREV;
      stage.alphaPass.b = GX_CA_TEXA;
      stage.alphaPass.c = GX_CA_RASA;
      stage.alphaPass.d = GX_CA_KONST;
      stage.kcSel = kc[i];
      stage.kaSel = static_cast<GXTevKAlphaSel>(GX_TEV_KASEL_K0_R + i * 4);
      stage.texCoordId = static_cast<GXTexCoordID>(i);
      stage.texMapId = static_cast<GXTexMapID>(i);
      stage.channelId = i < 2 ? GX_COLOR0A0 : GX_COLOR1A1;
      stage.indTexStage = is[i];
      stage.indTexMtxId = static_cast<GXIndTexMtxID>(i % 3);
      config.indStages[i].texMapId = static_cast<GXTexMapID>(i + 4);
      config.indStages[i].texCoordId = static_cast<GXTexCoordID>(i + 4);
    }
    for (auto& cc : config.colorChannels) {
      cc.lightingEnabled = true;
      cc.matSrc = GX_SRC_REG;
      cc.ambSrc = GX_SRC_REG;
    }
    for (u32 i = 0; i < MaxTexCoord; ++i) {
      config.tcgs[i].postMtx = static_cast<GXPTTexMtx>(GX_PTTEXMTX0 + 3 * (i % 8));
    }
    return config;
  }

  u32 saved = 0;
};

TEST_F(GxUniformLimitTest, MaximalPbrConfigNoLongerAborts) {
  constexpr u32 OldLimit = 3840; // the limit a legitimate PBR material did not fit
  const auto plain = build_shader_info(maximal(false));
  const auto pbr = build_shader_info(maximal(true));
  ASSERT_TRUE(pbr.usesPbr);
  EXPECT_TRUE(pbr.lightingEnabled);
  // The worst TEV-only config is what the old limit was sized for...
  EXPECT_LE(plain.uniformSize, OldLimit);
  // ...and PBR's Mat3x4 + 24 vec4 push it past it, which is the abort.
  EXPECT_GT(pbr.uniformSize, OldLimit);
  EXPECT_GE(pbr.uniformSize - plain.uniformSize, sizeof(Mat3x4<float>) + sizeof(Vec4<float>) * 24);
  EXPECT_LE(pbr.uniformSize, MaxUniformSize);
  auto hdrConfig = maximal(false);
  hdrConfig.sceneHdr = true;
  const auto hdr = build_shader_info(hdrConfig);
  EXPECT_TRUE(hdr.usesHdrTone);
  EXPECT_LE(hdr.uniformSize, MaxUniformSize) << "the extra blend vec4 must fit the bound window";
}

// The three model modulation vectors follow pbr_room_lights in the PBR uniform, in the order the WGSL
// struct declares them, so a PBR draw's block grew by exactly 3 vec4.
TEST_F(GxUniformLimitTest, PbrModelModulationIsPackedAfterRoomLights) {
  ShaderConfig config;
  config.pbr = 1;
  // Shader source generation needs a position attribute.
  config.attrs[GX_VA_POS].attrType = GX_DIRECT;
  config.attrs[GX_VA_POS].cnt = GX_POS_XYZ;
  config.attrs[GX_VA_POS].compType = GX_F32;
  config.tevStageCount = 1;
  const std::string source = build_shader_source(config);
  EXPECT_NE(source.find("pbr_room_lights: vec4f,\n    pbr_model_diffuse: vec4f,\n    pbr_model_incan: vec4f,\n"
                        "    pbr_model_coverage: vec4f,"),
            std::string::npos);
  ShaderConfig plainConfig = config;
  plainConfig.pbr = 0;
  const std::string plainSource = build_shader_source(plainConfig);
  EXPECT_EQ(plainSource.find("pbr_model_"), std::string::npos);

  const auto info = build_shader_info(config);
  ASSERT_TRUE(info.usesPbr);

  gfx::detail::testing::StagingPool staging;
  gfx::detail::FramePacket frame;
  staging.attach(frame);
  gfx::detail::begin_recording(frame, 0);
  // Restores the global state and ends the recording even when an ASSERT_* returns early.
  struct Cleanup {
    Vec4<float> room = g_gxState.pbrRoomLights;
    Vec4<float> diffuse = g_gxState.pbrModelDiffuse;
    Vec4<float> incan = g_gxState.pbrModelIncan;
    Vec4<float> coverage = g_gxState.pbrModelCoverage;
    ~Cleanup() {
      g_gxState.pbrRoomLights = room;
      g_gxState.pbrModelDiffuse = diffuse;
      g_gxState.pbrModelIncan = incan;
      g_gxState.pbrModelCoverage = coverage;
      // No pass is encoded here: shutdown drops the packet, which ends the session.
      gfx::detail::shutdown_recording();
    }
  } cleanup;
  g_gxState.pbrRoomLights = {1234.5f, 0.f, 0.f, 0.f};
  g_gxState.pbrModelDiffuse = {0.1f, 0.2f, 0.3f, 0.4f};
  g_gxState.pbrModelIncan = {5.f, 6.f, 7.f, 0.8f};
  g_gxState.pbrModelCoverage = {0.9f, 0.f, 0.f, 0.f};
  const auto range = build_uniform(info);
  const auto& bytes = frame.uniforms;
  ASSERT_GE(bytes.size(), range.offset + range.size);
  // Find the sentinel room-lights vec4, then the three vectors right after it.
  const float sentinel[4] = {1234.5f, 0.f, 0.f, 0.f};
  size_t at = range.offset;
  bool found = false;
  for (; at + sizeof(sentinel) + 48 <= range.offset + range.size; at += 4) {
    if (std::memcmp(bytes.data() + at, sentinel, sizeof(sentinel)) == 0) {
      found = true;
      break;
    }
  }
  ASSERT_TRUE(found);
  float tail[12];
  std::memcpy(tail, bytes.data() + at + sizeof(sentinel), sizeof(tail));
  const float expected[12] = {0.1f, 0.2f, 0.3f, 0.4f, 5.f, 6.f, 7.f, 0.8f, 0.9f, 0.f, 0.f, 0.f};
  for (u32 i = 0; i < 12; ++i) {
    EXPECT_EQ(tail[i], expected[i]) << i;
  }
}

// The thermal override's vec4 follows pbr_model_coverage, and only a config with the bit has it.
TEST_F(GxUniformLimitTest, PbrThermalIsPackedAfterModelCoverage) {
  ShaderConfig config;
  config.pbr = 1;
  config.sceneHdr = 1;
  config.pbrThermal = 1;
  config.attrs[GX_VA_POS].attrType = GX_DIRECT;
  config.attrs[GX_VA_POS].cnt = GX_POS_XYZ;
  config.attrs[GX_VA_POS].compType = GX_F32;
  config.tevStageCount = 1;
  EXPECT_NE(build_shader_source(config).find("pbr_model_coverage: vec4f,\n    pbr_thermal: vec4f,"), std::string::npos);
  ShaderConfig plainConfig = config;
  plainConfig.pbrThermal = 0;
  EXPECT_EQ(build_shader_source(plainConfig).find("pbr_thermal"), std::string::npos);
  EXPECT_FALSE(build_shader_info(plainConfig).usesPbrThermal);

  const auto info = build_shader_info(config);
  ASSERT_TRUE(info.usesPbrThermal);

  gfx::detail::testing::StagingPool staging;
  gfx::detail::FramePacket frame;
  staging.attach(frame);
  gfx::detail::begin_recording(frame, 0);
  struct Cleanup {
    Vec4<float> coverage = g_gxState.pbrModelCoverage;
    Vec4<float> thermal = g_gxState.pbrThermal;
    ~Cleanup() {
      g_gxState.pbrModelCoverage = coverage;
      g_gxState.pbrThermal = thermal;
      gfx::detail::shutdown_recording();
    }
  } cleanup;
  g_gxState.pbrModelCoverage = {4321.5f, 0.f, 0.f, 0.f};
  g_gxState.pbrThermal = {0.4f, 0.6f, 1.f, 1.f};
  const auto range = build_uniform(info);
  const auto& bytes = frame.uniforms;
  ASSERT_GE(bytes.size(), range.offset + range.size);
  const float sentinel[4] = {4321.5f, 0.f, 0.f, 0.f};
  size_t at = range.offset;
  bool found = false;
  for (; at + sizeof(sentinel) + 16 <= range.offset + range.size; at += 4) {
    if (std::memcmp(bytes.data() + at, sentinel, sizeof(sentinel)) == 0) {
      found = true;
      break;
    }
  }
  ASSERT_TRUE(found);
  float tail[4];
  std::memcpy(tail, bytes.data() + at + sizeof(sentinel), sizeof(tail));
  const float expected[4] = {0.4f, 0.6f, 1.f, 1.f};
  for (u32 i = 0; i < 4; ++i) {
    EXPECT_EQ(tail[i], expected[i]) << i;
  }
}

TEST_F(GxUniformLimitTest, MaximalConfigFitsAtEveryAlignment) {
  // Adapters report minUniformBufferOffsetAlignment of 64 or 256, and the
  // uniform is padded up to it, so the limit has to hold for both.
  for (const u32 alignment : {64u, 256u}) {
    const auto previous = setAlignment(alignment);
    const auto plain = build_shader_info(maximal(false));
    const auto pbr = build_shader_info(maximal(true));
    EXPECT_EQ(plain.uniformSize % alignment, 0u) << "alignment " << alignment;
    EXPECT_EQ(pbr.uniformSize % alignment, 0u) << "alignment " << alignment;
    EXPECT_LE(pbr.uniformSize, MaxUniformSize) << "alignment " << alignment;
    setAlignment(previous);
  }
}

TEST_F(GxUniformLimitTest, LimitIsBindable) {
  // WebGPU's default maxUniformBufferBindingSize is 64 KiB; the spec's minimum
  // guarantee is 16 KiB, and we never request more, so the default applies.
  static_assert(MaxUniformSize <= 16384);
  static_assert(MaxUniformSize % 256 == 0); // no partial binding of the last texel
  // frame.cpp binds MaxUniformSize out of the uniform buffer.
  static_assert(MaxUniformSize <= gfx::UniformBufferSize);
  EXPECT_EQ(MaxUniformSize, 8192u);
}

} // namespace
} // namespace aurora::gx
