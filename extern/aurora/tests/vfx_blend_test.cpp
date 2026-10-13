#include <gtest/gtest.h>

#include <aurora/vfx.hpp>
#include <webgpu/webgpu_cpp.h>

namespace aurora::gfx::vfx {
// Defined in lib/gfx/vfx.cpp: the fixed-function blend every VFX pipeline is built with.
wgpu::BlendState blend_state(Blend blend);
} // namespace aurora::gfx::vfx

namespace {
using aurora::gfx::vfx::Blend;
using aurora::gfx::vfx::blend_state;
using wgpu::BlendFactor;

struct Factors {
  BlendFactor cs, cd, as, ad;
};

void expect_factors(Blend blend, const Factors& f) {
  const wgpu::BlendState s = blend_state(blend);
  EXPECT_EQ(s.color.operation, wgpu::BlendOperation::Add);
  EXPECT_EQ(s.alpha.operation, wgpu::BlendOperation::Add);
  EXPECT_EQ(s.color.srcFactor, f.cs) << "mode " << uint32_t(blend);
  EXPECT_EQ(s.color.dstFactor, f.cd) << "mode " << uint32_t(blend);
  EXPECT_EQ(s.alpha.srcFactor, f.as) << "mode " << uint32_t(blend);
  EXPECT_EQ(s.alpha.dstFactor, f.ad) << "mode " << uint32_t(blend);
}

TEST(VfxBlend, AdditiveOneIsOneOneWithZeroOneAlpha) {
  expect_factors(Blend::AdditiveOne, {BlendFactor::One, BlendFactor::One, BlendFactor::Zero, BlendFactor::One});
}

TEST(VfxBlend, AdditiveStaysSrcAlphaOne) {
  expect_factors(Blend::Additive, {BlendFactor::SrcAlpha, BlendFactor::One, BlendFactor::Zero, BlendFactor::One});
}

TEST(VfxBlend, OlderModesUnchanged) {
  expect_factors(Blend::Alpha,
                 {BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha, BlendFactor::One, BlendFactor::OneMinusSrcAlpha});
  expect_factors(Blend::Premultiplied,
                 {BlendFactor::One, BlendFactor::OneMinusSrcAlpha, BlendFactor::Zero, BlendFactor::One});
  expect_factors(Blend::Opaque, {BlendFactor::One, BlendFactor::Zero, BlendFactor::One, BlendFactor::Zero});
  expect_factors(Blend::Multiply, {BlendFactor::Dst, BlendFactor::Zero, BlendFactor::Zero, BlendFactor::One});
}

TEST(VfxBlend, SerialisedValuesAreStable) {
  EXPECT_EQ(uint32_t(Blend::Alpha), 0u);
  EXPECT_EQ(uint32_t(Blend::Premultiplied), 1u);
  EXPECT_EQ(uint32_t(Blend::Additive), 2u);
  EXPECT_EQ(uint32_t(Blend::Opaque), 3u);
  EXPECT_EQ(uint32_t(Blend::Multiply), 4u);
  EXPECT_EQ(uint32_t(Blend::AdditiveOne), 5u);
  EXPECT_EQ(aurora::gfx::vfx::MaxBlend, 5u);
}

TEST(VfxBlend, OnlyAdditiveOneForcesDepthWriteOff) {
  using aurora::gfx::vfx::forces_depth_write_off;
  EXPECT_TRUE(forces_depth_write_off(Blend::AdditiveOne));
  for (Blend b : {Blend::Alpha, Blend::Premultiplied, Blend::Additive, Blend::Opaque, Blend::Multiply})
    EXPECT_FALSE(forces_depth_write_off(b)) << uint32_t(b);
}

TEST(VfxBlend, ModelDepthSelection) {
  using aurora::gfx::vfx::DepthSelection;
  using aurora::gfx::vfx::select_model_depth;
  // AdditiveOne: compare on and write off for every generator flag combination, zTest false included.
  for (int m = 0; m < 8; ++m) {
    const bool z = m & 1, aaph = m & 2, zbuf = m & 4;
    EXPECT_EQ(select_model_depth(Blend::AdditiveOne, z, aaph, zbuf), (DepthSelection{true, false})) << m;
    // Every older mode keeps the generator's own state exactly.
    for (Blend b : {Blend::Alpha, Blend::Premultiplied, Blend::Additive, Blend::Opaque, Blend::Multiply})
      EXPECT_EQ(select_model_depth(b, z, aaph, zbuf), (DepthSelection{z, !aaph && zbuf})) << m << " mode " << uint32_t(b);
  }
}
} // namespace
