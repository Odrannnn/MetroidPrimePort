#include <gtest/gtest.h>

#include <aurora/vfx.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// The clustered light gather Phase A builds (platform/port_clustered_lights.h) as the vfx pipeline takes it:
// the 0xa30 block copied into the uniform at ClusterBlockOffset, and the mask uploaded as an R32Uint texture.
#include <gfx/fx_pipelines.hpp>

namespace {
using namespace aurora::gfx::vfx;

// PortClusteredLights::Frame's layout: the 12-float header, then 32 records of 20 floats, contiguous.
struct Gather {
  float header[ClusterHeaderFloats] = {};
  float records[ClusterMaxLights * ClusterRecordFloats] = {};
  std::vector<uint32_t> mask;
};

// c5[0] = (1/pptX, 1/pptY, tilesX, tilesY), c5[1] = (1, 32/500, depthOrigin, 500),
// c5[2] = (tilesX*tilesY bits, tilesX bits, validity, mask source).
void fill_header(Gather& g, uint32_t tilesX, uint32_t tilesY, float depthOrigin, uint32_t validity) {
  const auto bits = [](uint32_t v) {
    float f;
    std::memcpy(&f, &v, sizeof(f));
    return f;
  };
  g.header[0] = 1.f / 48.f;
  g.header[1] = 1.f / 48.f;
  g.header[2] = float(tilesX);
  g.header[3] = float(tilesY);
  g.header[4] = 1.f;
  g.header[5] = 32.f / 500.f;
  g.header[6] = depthOrigin;
  g.header[7] = 500.f;
  g.header[8] = bits(tilesX * tilesY);
  g.header[9] = bits(tilesX);
  g.header[10] = bits(validity);
  g.header[11] = 0.f;
}

// A record: colour rgb x intensity at +0x00, widen +0x0c, view position (w = cone scale) +0x10, direction
// surface -> light (w = cone bias) +0x20, A/B +0x30, zeros +0x38, the falloff cubic +0x40.
void fill_record(float* record, float r, float gg, float b, float intensity) {
  record[0] = r;
  record[1] = gg;
  record[2] = b;
  record[3] = intensity;
  for (int i = 4; i < int(ClusterRecordFloats); ++i) {
    record[i] = 0.25f * float(i);
  }
}

std::vector<float> zeroed_uniform() { return std::vector<float>(VfxUniformSize / 4, 0.f); }

// The bytes of one c5 slot (4 floats) of a uniform.
const float* c5(const std::vector<float>& uniform, uint32_t slot) {
  return uniform.data() + ClusterBlockOffset / 4 + 4 * slot;
}

TEST(VfxCluster, UniformLayoutIsUnchangedBeforeTheBlock) {
  // The block goes after every field an earlier recipe reads: the uniform grew by exactly the block, and
  // the members before it keep their offsets (vfx.cpp's static_asserts pin the offsets themselves).
  EXPECT_EQ(ClusterBlockOffset, 768u);
  EXPECT_EQ(ClusterBlockFloats, 652u);
  EXPECT_EQ(VfxUniformSize, 3376u);
  EXPECT_EQ(ClusterHeaderFloats, 12u);
  EXPECT_EQ(ClusterRecordFloats, 20u);
  EXPECT_EQ(ClusterMaxLights, 32u);
  // The shader's c5[i] is u.cluster[i] (a vec4f each): the header is three slots, then the records.
  EXPECT_EQ(4 * ClusterHeaderFloats, 3 * 4 * 4);
  // Each record is 20 floats = 5 slots, so record i starts at c5[3 + 5 * i].
  EXPECT_EQ(ClusterRecordFloats % 4, 0u);
  EXPECT_EQ(3 + 5 * ClusterMaxLights, ClusterBlockFloats / 4);
}

TEST(VfxCluster, HeaderFloatsAndFirstRecordLandAtTheShaderSlots) {
  Gather g;
  fill_header(g, 34, 19, 0.f, 0xffffffffu);
  fill_record(g.records, 0.5f, 0.75f, 1.25f, 2.f);
  fill_record(g.records + ClusterRecordFloats, 1.f, 2.f, 3.f, 4.f);
  ClusterView view{};
  view.block = g.header;
  view.mask = g.mask.data();
  view.maskWidth = 34 * 19;
  view.maskHeight = ClusterMaskHeight;

  auto uniform = zeroed_uniform();
  write_cluster_block(view, uniform.data());

  // c5[0..2] are the header's twelve floats, unchanged (bit for bit: the validity mask is all ones, a NaN
  // as a float).
  for (uint32_t i = 0; i < ClusterHeaderFloats; ++i) {
    EXPECT_EQ(std::memcmp(&uniform[ClusterBlockOffset / 4 + i], &g.header[i], sizeof(float)), 0)
        << "header float " << i;
  }
  EXPECT_FLOAT_EQ(c5(uniform, 0)[0], 1.f / 48.f);
  EXPECT_FLOAT_EQ(c5(uniform, 0)[2], 34.f);
  EXPECT_FLOAT_EQ(c5(uniform, 1)[1], 32.f / 500.f);
  EXPECT_FLOAT_EQ(c5(uniform, 1)[3], 500.f);
  // c5[2].y is tilesX as int bits, which the shader reads with floatBitsToInt to get the row pitch.
  int32_t pitch = 0;
  std::memcpy(&pitch, &c5(uniform, 2)[1], sizeof(pitch));
  EXPECT_EQ(pitch, 34);
  EXPECT_EQ(std::memcmp(&c5(uniform, 2)[2], &g.header[10], sizeof(float)), 0); // validity, bit for bit
  // c5[3] is the first record's colour x intensity, and c5[8] the next record's.
  EXPECT_FLOAT_EQ(c5(uniform, 3)[0], 0.5f);
  EXPECT_FLOAT_EQ(c5(uniform, 3)[1], 0.75f);
  EXPECT_FLOAT_EQ(c5(uniform, 3)[2], 1.25f);
  EXPECT_FLOAT_EQ(c5(uniform, 3)[3], 2.f);
  EXPECT_FLOAT_EQ(c5(uniform, 8)[0], 1.f);
  EXPECT_FLOAT_EQ(c5(uniform, 8)[3], 4.f);
  // Every record float is there: 32 records of 20 floats = 160 c5 slots after the header.
  EXPECT_FLOAT_EQ(c5(uniform, 162)[3], g.records[639]);
  // Nothing of the block lands outside the block's bytes.
  for (uint32_t i = 0; i < ClusterBlockOffset / 4; ++i) {
    EXPECT_EQ(uniform[i], 0.f) << "uniform float " << i << " before the block";
  }
}

TEST(VfxCluster, BlockAndMaskAreDeclaredForTheLitFeature) {
  PipelineConfig config{};
  config.features = IceChargeCollisionLit;
  const std::string source = pipeline_source(config);
  // The block: 14 vec4f after the pre-block fields. lit0..lit6 (AMBS, the probe's mean/lobe/lobes and
  // BLCM) take the first seven, misc3 (the colour map's alpha gamma) the eighth, cm0..cm2 (GXSetColorMatrix's rows) the next three, the rest is padding, then 163 vec4f of cluster, which is
  // ClusterBlockFloats / 4 — the layout vfx.cpp's static_asserts pin to the C++ struct.
  EXPECT_NE(source.find("cluster: array<vec4f, 163>"), std::string::npos);
  EXPECT_NE(source.find("lit6: vec4f,\n  misc3: vec4f,\n  cm0: vec4f,\n  cm1: vec4f,\n  cm2: vec4f,\n  padCluster: array<vec4f, 3>"), std::string::npos);
  // The mask, at its own binding, R32Uint (texture_2d<u32>).
  EXPECT_NE(source.find("@binding(12) var clusterMask: texture_2d<u32>"), std::string::npos);
  EXPECT_NE(source.find("textureLoad(clusterMask"), std::string::npos);
  // The lit perm runs the unlit recipe's branch.
  EXPECT_NE(source.find("33554432u"), std::string::npos);
}

TEST(VfxCluster, LitTailIsTheReportedEquation) {
  // REPORT.md sections 3 to 5, as the shader writes them: the mask's lowest set bit indexes a record of
  // 5 vec4f at c5[3], the probe is three lobes of mean - lobe + lobe * (1 + s) * 2 * q^(1 + 2s), and the
  // tail is (vc.rgb * base * lit * BLCM) / pi + base * CCH5.z / exposure, x PCMD.
  PipelineConfig config{};
  config.features = IceChargeCollision | IceChargeCollisionLit | Unfogged;
  const std::string source = pipeline_source(config);
  // The loop, one bit at a time, and the record's stride.
  EXPECT_NE(source.find("let bit = firstTrailingBit(mask);"), std::string::npos);
  EXPECT_NE(source.find("mask = mask & (mask - 1u);"), std::string::npos);
  EXPECT_NE(source.find("let i = 3u + bit * 5u;"), std::string::npos);
  EXPECT_NE(source.find("if (mask == 0u) { break; }"), std::string::npos);
  // The per-light terms: N.L, the cone, and the cubic falloff at sat(d * A + B).
  EXPECT_NE(source.find("let cone = sat(dot(l, r2.xyz) * r1.w + r2.w);"), std::string::npos);
  EXPECT_NE(source.find("let t = sat(fma(d, r3.x, r3.y));"), std::string::npos);
  EXPECT_NE(source.find("let poly = ((r4.x * t + r4.y) * t + r4.z) * t + r4.w;"), std::string::npos);
  // The probe: the 1 + 2s lane is the record's w, and 1 + s is 0.5 * w + 0.5.
  EXPECT_NE(source.find("return fma(lobe * (sharp + 1.0), pow(q, sharp), mean - lobe);"), std::string::npos);
  EXPECT_NE(source.find("let q = sat(dot(n, dir) * 0.5 + 0.5);"), std::string::npos);
  // The tail: AMBS + probe + lights, BLCM, 1/pi, the exposed base's inverse exposure, PCMD (M).
  EXPECT_NE(source.find("u.lit0.xyz + probe_ambient(N) + cluster_lights(in, N)"), std::string::npos);
  EXPECT_NE(source.find("let shaded = vc.rgb * base * lighting * u.lit6.rgb;"), std::string::npos);
  EXPECT_NE(source.find("(shaded * 0.3183098862 + base * (c5.z * ie)) * M"), std::string::npos);
}

TEST(VfxCluster, ParticleLitScalesTheRecipeColourByTheLight) {
  // 20e149b2's lit perms (080_0..091_0): the unlit colour x (AMBS + probe + lights) x BLCM / pi, whatever
  // the recipe; the gather and the probe are the IceChargeCollisionLit ones.
  PipelineConfig config{};
  config.features = ColorTex | OpacityTex | DepthSoften | ParticleLit;
  const std::string source = pipeline_source(config);
  EXPECT_NE(source.find("if ((FEAT & 67108864u) != 0u && u.misc3.y < 0.5) {"), std::string::npos);
  EXPECT_NE(source.find("rgb = rgb * lighting * u.lit6.rgb * 0.3183098862;"), std::string::npos);
  // 52d26efe's per-vertex form (VS 040_1, misc3.y): the vertex colour x [(probe midpoint + AMBS) x
  // BLCM + the group-0 lights without N.L] / pi.
  EXPECT_NE(source.find("if ((FEAT & 67108864u) != 0u && u.misc3.y > 0.5) {"), std::string::npos);
  EXPECT_NE(source.find("let lighting = (u.lit0.xyz + u.lit1.xyz) * u.lit6.rgb + cluster_sum("), std::string::npos);
  EXPECT_NE(source.find("out.color = vec4f(in.color.rgb * lighting * 0.3183098862, in.color.w);"), std::string::npos);
  EXPECT_NE(source.find("let i = 3u + bit * 5u;"), std::string::npos);
  EXPECT_NE(source.find("@binding(12) var clusterMask: texture_2d<u32>"), std::string::npos);
  // Not a shader-side exposure policy: the feature word's AlreadyExposed stays off the pipeline key.
  EXPECT_EQ(shader_features(ParticleLit | AlreadyExposed), uint32_t(ParticleLit));
}

TEST(VfxCluster, MaskDimensionsAreValidated) {
  Gather g;
  fill_header(g, 34, 19, 0.f, 0xffffffffu);
  g.mask.assign(size_t(34 * 19) * ClusterMaskHeight, 0u);
  ClusterView view{};
  view.block = g.header;

  // No mask at all: legal, the draw binds the dummy.
  EXPECT_TRUE(valid_cluster_view(view));

  view.mask = g.mask.data();
  view.maskWidth = 34 * 19;
  view.maskHeight = ClusterMaskHeight;
  EXPECT_TRUE(valid_cluster_view(view));

  view.maskWidth = 1;
  view.maskHeight = 1;
  EXPECT_TRUE(valid_cluster_view(view));

  view.maskWidth = 0; // a mask needs a texel
  EXPECT_FALSE(valid_cluster_view(view));
  view.maskWidth = 34 * 19;
  view.maskHeight = 0;
  EXPECT_FALSE(valid_cluster_view(view));
  view.maskHeight = ClusterMaskHeight;
  view.maskWidth = ClusterMaskMaxWidth + 1; // wider than the grid's cap (34 tiles x 19)
  EXPECT_FALSE(valid_cluster_view(view));
  view.maskWidth = 34 * 19;
  view.maskHeight = ClusterMaskHeight + 1; // taller than the 32 slices
  EXPECT_FALSE(valid_cluster_view(view));

  // Dimensions without a mask are a mistake, not "no mask".
  view.mask = nullptr;
  EXPECT_FALSE(valid_cluster_view(view));
  view.maskWidth = 0;
  view.maskHeight = 0;
  EXPECT_TRUE(valid_cluster_view(view));

  // No block: nothing to copy.
  view.block = nullptr;
  EXPECT_FALSE(valid_cluster_view(view));
}

TEST(VfxCluster, InvalidViewLeavesTheUniformBlockZeroed) {
  // write_cluster_block with no block writes nothing (draw_prims zero-initialises the uniform).
  ClusterView view{};
  auto uniform = zeroed_uniform();
  write_cluster_block(view, uniform.data());
  for (float f : uniform) {
    EXPECT_EQ(f, 0.f);
  }
}
} // namespace
