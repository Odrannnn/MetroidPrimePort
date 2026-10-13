#include <algorithm>
#include <array>
#include <string>

#include <gtest/gtest.h>

#include "gx/gx.hpp"

namespace aurora::gx {
namespace {

// Kind 34 (Remastered's 788360de / 4c37625c, MFC0 feature off): One / InvSrcAlpha with alpha = A, so the scene
// behind is kept by 1 - A, whatever the base colour. The old code kept it by 1 - base * (1 - A).
ShaderConfig kind34() {
  ShaderConfig config;
  config.pbr = 1;
  config.pbrKind = 34;
  config.tevStageCount = 2;
  for (u32 i = 0; i < 2; ++i) {
    auto& stage = config.tevStages[i];
    stage.colorPass.a = GX_CC_ZERO;
    stage.colorPass.b = GX_CC_TEXC;
    stage.colorPass.c = GX_CC_RASC;
    stage.colorPass.d = GX_CC_ZERO;
    stage.alphaPass.a = GX_CA_ZERO;
    stage.alphaPass.b = GX_CA_TEXA;
    stage.alphaPass.c = GX_CA_RASA;
    stage.alphaPass.d = GX_CA_ZERO;
    stage.texCoordId = static_cast<GXTexCoordID>(i);
    stage.texMapId = i == 0 ? GX_TEXMAP0 : GX_TEXMAP7;
    stage.channelId = GX_COLOR0A0;
  }
  for (const auto attr : {GX_VA_POS, GX_VA_NRM, GX_VA_TEX0, GX_VA_TEX1}) {
    auto& a = config.attrs[attr];
    a.attrType = GX_DIRECT;
    a.cnt = attr == GX_VA_TEX0 || attr == GX_VA_TEX1 ? 2 : 3;
    a.compType = GX_F32;
    a.stride = 12;
  }
  config.colorChannels[GX_COLOR0].lightingEnabled = true;
  config.colorChannels[GX_COLOR0].matSrc = GX_SRC_REG;
  config.colorChannels[GX_COLOR0].ambSrc = GX_SRC_REG;
  return config;
}

// The weight the emitted line applies to the scene behind (reference for the cases below).
constexpr float scene_weight(float alpha) { return 1.0f - alpha; }

TEST(GxSuitGlass, EmittedSourceUsesPremultipliedBackgroundWeight) {
  const std::string source = build_shader_source(kind34());
  ASSERT_NE(source.find("pbr_kind > 33.5 && pbr_kind < 34.5"), std::string::npos) << "kind 34 block missing";
  EXPECT_NE(source.find("pbr_pass += pbr_sdst * vec3f(1.0 - pbr_alpha);"), std::string::npos);
  // The old base-tinted transmission must be gone.
  EXPECT_EQ(source.find("pbr_sdst * (vec3f(1.0) - pbr_base * (1.0 - pbr_alpha))"), std::string::npos);
  EXPECT_EQ(source.find("pbr_base * (1.0 - pbr_alpha)"), std::string::npos);
  // Still drawn opaque, so no second framebuffer blend.
  EXPECT_NE(source.find("pbr_alpha = 1.0;"), std::string::npos);
}

TEST(GxSuitGlass, WeightEdgeCases) {
  const std::array<float, 3> tints{0.0f, 0.5f, 1.0f}; // base colour must not matter
  for (const float t : tints) {
    EXPECT_FLOAT_EQ(scene_weight(0.0f), 1.0f) << t; // A=0: background kept
    EXPECT_FLOAT_EQ(scene_weight(1.0f), 0.0f) << t; // A=1: background gone
    EXPECT_FLOAT_EQ(scene_weight(0.25f), 0.75f) << t;
  }
}

} // namespace
} // namespace aurora::gx
