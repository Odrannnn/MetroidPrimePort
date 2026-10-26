#include <gtest/gtest.h>

#include "frame_staging.hpp"
#include "gfx/frame_packet.hpp"
#include "gfx/fx_pipelines.hpp"
#include "gfx/hash.hpp"
#include "gfx/hdr_tone.hpp"
#include "gfx/recording.hpp"
#include "gfx/resources.hpp"
#include "gx/gx.hpp"
#include "gx/pipeline.hpp"
#include "gx/shader_info.hpp"
#include "webgpu/gpu.hpp"

#include <cmath>
#include <cstring>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// The HDR scene's writers (GX and VFX) without a GPU: the tone helpers the WGSL is held to, the
// sources each mode generates, and the config bits that keep LDR keys as they were.
namespace aurora::gx {
namespace {

using gfx::hdr::Tone;

// A room curve shaped like Remastered's: a cubic toe to 0.1, a line to 1.0, a shoulder to 1.0.
constexpr Tone RoomTone{{{20.f, 3.f, 0.5f, 1.5f}, {0.8f, 0.02f, 0.1f, 1.f}, {0.18f, 1.f, -1.f, 0.82f}}};
constexpr Tone NoTone{{{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f, 0.f}, {0.f, 0.f, 0.f, 0.f}}};
// Frozen rain frame 404: attribution-final/analysis.json (including the untouched exposure lane).
constexpr Tone RainTone{{{-25.720172882f, 9.259262085f, 0.555555463f, 1.189207077f},
                         {1.388888717f, 1.490116119e-8f, 0.180000007f, 0.261000037f},
                         {0.637499988f, 2.178648949f, -0.568627477f, 0.362500012f}}};

TEST(SceneHdrTone, SrcAlphaAdditiveWeightsDisplayBeforeTheInverse) {
  for (const Tone* tone : {&RoomTone, &RainTone, &NoTone}) {
    for (const float a : {0.f, 1e-20f, 1e-8f, 0.05f, 0.1f, 0.5f, 1.f}) {
      // Per-channel coloured endpoints, black, white and gain (>1) TEV input.
      for (const float c : {-0.2f, 0.f, 0.02f, 0.25f, 0.65f, 1.f, 4.f}) {
        const float h = gfx::hdr::tev_srcalpha_add(c, a, *tone);
        EXPECT_TRUE(std::isfinite(h)) << a << ": " << c;
        EXPECT_GE(h, 0.f);
        EXPECT_NEAR(gfx::hdr::post_tone(a * h, *tone), std::clamp(c, 0.f, 1.f) * a, 2e-3f);
        if (a == 0.f || c <= 0.f)
          EXPECT_EQ(h, 0.f);
        if (a == 1.f)
          EXPECT_FLOAT_EQ(h, gfx::hdr::exposed(c, *tone));
      }
    }
  }
  for (const float a : {0.05f, 0.1f}) {
    const float h = gfx::hdr::tev_srcalpha_add(1.f, a, RainTone);
    EXPECT_LT(h, 1.f); // no inverse-white 65504, nor its ~3276 / ~6550 contribution
    EXPECT_GT(gfx::hdr::exposed(1.f, RainTone) * a, 3000.f);
    EXPECT_NEAR(gfx::hdr::post_tone(h * a, RainTone), a, 1e-5f);
  }
  EXPECT_EQ(gfx::hdr::tev_srcalpha_add(1.f, -1.f, RainTone), 0.f);
  EXPECT_EQ(gfx::hdr::tev_srcalpha_add(1.f, 2.f, RainTone), gfx::hdr::exposed(1.f, RainTone));
}

TEST(SceneHdrTone, AlphaOverWeightsTheSourceLikeAdditive) {
  // SRCALPHA/INVSRCALPHA draws src * a + dst * (1 - a). Over black the converted source draws as LDR's
  // a * c; the plain inverse drew a white at alpha 0.3 as white.
  for (const float a : {0.1f, 0.3f, 0.5f}) {
    const float h = gfx::hdr::tev_srcalpha_add(1.f, a, RainTone);
    EXPECT_NEAR(gfx::hdr::post_tone(a * h, RainTone), a, 2e-3f) << a;
    EXPECT_GT(gfx::hdr::post_tone(a * gfx::hdr::exposed(1.f, RainTone), RainTone), 0.99f) << a;
  }
  // Over a nonblack background it is not LDR's display blend (the curve is nonlinear): a layer over its
  // own colour draws somewhat darker, never brighter.
  for (const float c : {0.25f, 0.5f, 0.8f}) {
    for (const float a : {0.3f, 0.5f, 0.7f}) {
      const float dst = gfx::hdr::exposed(c, RainTone);
      const float out = gfx::hdr::post_tone(a * gfx::hdr::tev_srcalpha_add(c, a, RainTone) + (1.f - a) * dst, RainTone);
      EXPECT_LE(out, c + 2e-3f) << c << " " << a;
      EXPECT_GT(out, c - 0.12f) << c << " " << a;
    }
  }
}

TEST(SceneHdrTone, DstFactorBlendsScaleTheSceneLinearly) {
  // A multiply factor is a gain on the light, never the inverse curve's white (65504).
  EXPECT_EQ(gfx::hdr::tev_dst_factor(1.f, 1), 1.f);
  EXPECT_EQ(gfx::hdr::tev_dst_factor(0.f, 1), 0.f);
  EXPECT_EQ(gfx::hdr::tev_dst_factor(-1.f, 1), 0.f);
  EXPECT_EQ(gfx::hdr::tev_dst_factor(4.f, 1), 1.f);
  EXPECT_NEAR(gfx::hdr::tev_dst_factor(0.5f, 1), 0.214f, 1e-3f);
  EXPECT_EQ(gfx::hdr::tev_dst_factor(0.f, 2), 0.f);
  EXPECT_NEAR(gfx::hdr::tev_dst_factor(1.f, 2), std::pow(2.f, 2.2f) - 1.f, 1e-5f);
  float prev = -1.f;
  for (const float c : {0.f, 0.1f, 0.25f, 0.5f, 0.75f, 1.f}) {
    const float f = gfx::hdr::tev_dst_factor(c, 1);
    EXPECT_GT(f, prev);
    prev = f;
  }
}

TEST(SceneHdrTone, TheCurveIsContinuousAndUntoneInvertsIt) {
  for (const float x : {0.1f, 1.f}) {
    EXPECT_NEAR(gfx::hdr::tone_fwd(x - 1e-5f, RoomTone), gfx::hdr::tone_fwd(x + 1e-5f, RoomTone), 1e-4f) << x;
  }
  for (float x = 0.f; x <= 20.f; x += x < 1.f ? 0.01f : 0.25f) {
    const float back = gfx::hdr::untone(gfx::hdr::tone_fwd(x, RoomTone), RoomTone);
    EXPECT_NEAR(back, x, std::max(1e-3f, x * 2e-3f)) << x;
  }
}

TEST(SceneHdrTone, ExposedDrawsAsTheDisplayColourAfterOnePostTone) {
  for (const Tone* tone : {&RoomTone, &NoTone}) {
    for (float c = 0.f; c <= 1.f; c += 1.f / 64.f) {
      EXPECT_NEAR(gfx::hdr::post_tone(gfx::hdr::exposed(c, *tone), *tone), c, 2e-3f) << c;
    }
  }
  EXPECT_EQ(gfx::hdr::exposed(0.f, RoomTone), 0.f);
  EXPECT_EQ(gfx::hdr::exposed(-0.5f, RoomTone), 0.f);
  EXPECT_EQ(gfx::hdr::exposed(-0.5f, NoTone), 0.f);
}

TEST(SceneHdrTone, NoToneIsTheDecodeAlone) {
  for (float c = 0.f; c <= 1.f; c += 1.f / 32.f) {
    EXPECT_FLOAT_EQ(gfx::hdr::exposed(c, NoTone), gfx::hdr::srgb_dec(c)) << c;
  }
  EXPECT_FLOAT_EQ(gfx::hdr::exposed(1.f, NoTone), 1.f);
}

TEST(SceneHdrTone, LevelsAboveOneAreKept) {
  // A display white is the top of the shoulder, so its exposed level is far above 1 (capped at the
  // RGBA16F limit, not bloom's 4.0), and the post tone still draws it white.
  EXPECT_GT(gfx::hdr::exposed(1.f, RoomTone), 4.f);
  EXPECT_GT(gfx::hdr::exposed(0.98f, RoomTone), 1.f);
  EXPECT_FLOAT_EQ(gfx::hdr::post_tone(35.f, RoomTone), gfx::hdr::post_tone(35.f, RoomTone));
  EXPECT_GT(gfx::hdr::post_tone(35.f, RoomTone), gfx::hdr::post_tone(1.f, RoomTone));
  EXPECT_FLOAT_EQ(gfx::hdr::post_tone(35.f, NoTone), 1.f);
}

// bloom.cpp's untone and exposed, as they were written: the inverse the HDR one must equal on the
// data it was written for (t2.z = -t2.y * t1.w, t2.w = the line's end).
float bloom_untone(float y, const Tone& t) {
  const float mid = t[1][2];
  const float lineStart = t[1][0] * mid + t[1][1];
  if (y < lineStart) {
    float x = y / std::max(lineStart, 1e-4f) * mid;
    for (int n = 0; n < 4; ++n) {
      const float slope = (3.f * t[0][0] * x + 2.f * t[0][1]) * x + t[0][2];
      x = std::clamp(x - (gfx::hdr::tone_fwd(x, t) - y) / std::max(slope, 1e-4f), 0.f, mid);
    }
    return x;
  }
  if (y < t[2][3] || t[2][1] <= 0.f) {
    return (y - t[1][1]) / t[1][0];
  }
  const float u = std::min((y - t[2][3]) / std::max(t[2][0], 1e-4f), 0.999f);
  return u / (1.f - u) / t[2][1] + t[1][3];
}

TEST(SceneHdrTone, ItIsBloomsInverseWhereThatOneHolds) {
  for (int i = 0; i <= 255; ++i) {
    const float c = i / 255.f;
    const float y = gfx::hdr::srgb_dec(c);
    const float bloom = bloom_untone(y, RoomTone);
    // Below bloom's 0.999 shoulder clamp the two are the same inverse; above it bloom's is already
    // far past its 4.0 cap, and so is this one.
    if (bloom < 900.f) {
      EXPECT_NEAR(gfx::hdr::untone(y, RoomTone), bloom, std::max(1e-5f, bloom * 1e-5f)) << c;
    }
    EXPECT_FLOAT_EQ(std::min(gfx::hdr::exposed(c, RoomTone), 4.f), std::min(bloom, 4.f)) << c;
  }
}

TEST(SceneHdrTone, ExposedLevelsAboveBloomsCapAreKept) {
  // Exposed 17 and 34 (beam-glow levels) survive a display round trip; bloom's cap would make them 4.
  for (const float x : {17.f, 34.f}) {
    const float c = gfx::hdr::srgb_enc(gfx::hdr::tone_fwd(x, RoomTone));
    EXPECT_NEAR(gfx::hdr::untone(gfx::hdr::tone_fwd(x, RoomTone), RoomTone), x, x * 1e-4f);
    EXPECT_NEAR(gfx::hdr::exposed(c, RoomTone), x, x * 2e-2f);
    EXPECT_GT(gfx::hdr::exposed(c, RoomTone), 4.f);
  }
}

TEST(SceneHdrTone, TheShoulderInverseUsesItsOwnOffset) {
  // A curve continuous at the shoulder start (1, 0.82) whose shoulder does not satisfy bloom's
  // t2.z = -t2.y * t1.w, nor t2.w = the line's end: st(1) = 0.5, 0.3 * 0.5 / 1.5 + 0.72 = 0.82.
  constexpr Tone General{{{20.f, 3.f, 0.5f, 1.5f}, {0.8f, 0.02f, 0.1f, 1.f}, {0.3f, 2.f, -1.5f, 0.72f}}};
  EXPECT_NEAR(gfx::hdr::tone_fwd(1.f - 1e-5f, General), gfx::hdr::tone_fwd(1.f + 1e-5f, General), 1e-4f);
  for (float x = 0.f; x <= 60.f; x += x < 2.f ? 0.01f : 0.5f) {
    const float y = gfx::hdr::tone_fwd(x, General);
    EXPECT_NEAR(gfx::hdr::untone(y, General), x, std::max(1e-3f, x * 2e-3f)) << x;
  }
  // The old form (u / (1 - u) / t2.y + t1.w) is a different number here: 5.25 for 5.
  EXPECT_NEAR(bloom_untone(gfx::hdr::tone_fwd(5.f, General), General), 5.25f, 1e-3f);
  // A line level between t2.w and the line's end is still the line, not the shoulder.
  EXPECT_NEAR(gfx::hdr::untone(gfx::hdr::tone_fwd(0.9f, General), General), 0.9f, 1e-4f);
}

bool finite_f16(float v) { return std::isfinite(v) && v >= 0.f && v <= gfx::hdr::HdrMax; }

TEST(SceneHdrTone, ExposedIsFiniteInHalfFloatForAnyCurve) {
  using gfx::hdr::MinSlope;
  std::vector<Tone> tones{RoomTone};
  // Line slopes at, around and below the guard, a subnormal one, zero and negative ones (0 and below: no
  // tone data).
  for (const float s : {MinSlope * 2.f, MinSlope, MinSlope / 2.f, 1e-12f, 1e-40f, 0.f, -0.5f, 1e30f}) {
    Tone t = RoomTone;
    t[1][0] = s;
    tones.push_back(t);
  }
  // Shoulder slopes and scales the same way, a shoulder offset far either way, a negative toe end.
  for (const float s : {MinSlope * 2.f, MinSlope, MinSlope / 2.f, 1e-12f, 0.f, -1.f, 1e30f}) {
    Tone a = RoomTone, b = RoomTone;
    a[2][1] = s;
    b[2][0] = s;
    tones.push_back(a);
    tones.push_back(b);
  }
  for (const float z : {1e30f, -1e30f, -1e6f}) {
    Tone t = RoomTone;
    t[2][2] = z;
    tones.push_back(t);
  }
  // A flat shoulder sends every level to the line; with a subnormal line slope too, only the guard keeps it finite.
  Tone flatBoth = RoomTone;
  flatBoth[1][0] = 1e-40f;
  flatBoth[2][1] = 0.f;
  tones.push_back(flatBoth);
  Tone negMid = RoomTone;
  negMid[1][2] = -0.5f;
  tones.push_back(negMid);
  Tone flatToe = RoomTone;
  flatToe[0] = {0.f, 0.f, 0.f, 1.f};
  tones.push_back(flatToe);
  for (const Tone& t : tones) {
    for (const float c : {-1.f, 0.f, 1e-7f, 1e-3f, 0.04045f, 0.5f, 0.9f, 254.f / 255.f, 0.9999f, 1.f, 2.f}) {
      const float e = gfx::hdr::exposed(c, t);
      EXPECT_TRUE(finite_f16(e)) << c << " -> " << e << " t1.x " << t[1][0] << " t2 " << t[2][0] << ","
                                 << t[2][1] << "," << t[2][2];
      if (t[1][0] > 0.f) {
        const float u = gfx::hdr::untone(gfx::hdr::srgb_dec(c), t);
        EXPECT_TRUE(std::isfinite(u)) << c << " -> " << u << " t1.x " << t[1][0];
        EXPECT_GE(u, 0.f) << c;
      }
    }
  }
}

TEST(SceneHdrTone, OnlyTheReconstructionIsCapped) {
  // White is the shoulder's asymptote: the inverse is a large finite f32 level, the exposed colour
  // its RGBA16F cap; just below white it is a level under the cap.
  EXPECT_GT(gfx::hdr::untone(1.f, RoomTone), gfx::hdr::HdrMax);
  EXPECT_TRUE(std::isfinite(gfx::hdr::untone(1.f, RoomTone)));
  EXPECT_EQ(gfx::hdr::exposed(1.f, RoomTone), gfx::hdr::HdrMax);
  const float nearWhite = gfx::hdr::exposed(254.f / 255.f, RoomTone);
  EXPECT_GT(nearWhite, 4.f);
  EXPECT_LT(nearWhite, gfx::hdr::HdrMax);
  EXPECT_NEAR(gfx::hdr::post_tone(nearWhite, RoomTone), 254.f / 255.f, 2e-3f);
  // Near black: small, nonnegative, monotonic.
  EXPECT_EQ(gfx::hdr::exposed(0.f, RoomTone), 0.f);
  float last = 0.f;
  for (int i = 1; i <= 255; ++i) {
    const float e = gfx::hdr::exposed(i / 255.f, RoomTone);
    EXPECT_GE(e, last) << i;
    last = e;
  }
  EXPECT_LT(gfx::hdr::exposed(1.f / 255.f, RoomTone), 1e-3f);
  // The WGSL clamps the same reconstruction at the same limit, and nothing else.
  const std::string_view wgsl = gfx::hdr::ToneWgsl;
  EXPECT_NE(wgsl.find("const hdr_f16_max = 65504.0;"), std::string_view::npos);
  EXPECT_NE(wgsl.find("return clamp(x, vec3f(0.0), vec3f(hdr_f16_max));"), std::string_view::npos);
  size_t uses = 0;
  for (size_t at = wgsl.find("hdr_f16_max"); at != std::string_view::npos; at = wgsl.find("hdr_f16_max", at + 1))
    ++uses;
  EXPECT_EQ(uses, 2u);
  EXPECT_NE(wgsl.find("return max((st - t2.z) / t2.y, 0.0);"), std::string_view::npos);
  EXPECT_NE(wgsl.find("if (y < t1.x * t1.w + t1.y || t2.y <= hdr_min_slope) {"), std::string_view::npos);
}

// --- GX ---------------------------------------------------------------------------------------------------------

ShaderConfig gxConfig(int pbr, int kind) {
  ShaderConfig config;
  config.pbr = static_cast<u8>(pbr);
  config.pbrKind = static_cast<u8>(kind);
  const bool screen = kind == 36 || kind == 37 || kind == 34;
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
    stage.texMapId = i == 0 || !screen ? GX_TEXMAP0 : GX_TEXMAP7;
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

std::vector<std::string> lines(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream in(s);
  for (std::string line; std::getline(in, line);)
    out.push_back(line);
  return out;
}

// pbr_lin, not the LDR curve's pbr_line.
bool has_pbr_lin(std::string_view s) {
  for (size_t at = s.find("pbr_lin"); at != std::string_view::npos; at = s.find("pbr_lin", at + 1))
    if (at + 7 == s.size() || s[at + 7] != 'e')
      return true;
  return false;
}

bool hdr_line(std::string_view line) {
  return line.find("hdr_") != std::string_view::npos || has_pbr_lin(line) ||
         line.find("HDR") != std::string_view::npos || line.find("pbr_keep") != std::string_view::npos ||
         line.find("pbr_vf") != std::string_view::npos;
}

// A line of an output statement the HDR branch rewrites (prev = vec4f(...), possibly wrapped).
bool output_line(std::string_view line) {
  return line.find("prev = vec4f(") != std::string_view::npos || line.ends_with("prev.a);");
}

// An LDR line with its map-7 screen reads made raw, as the HDR branch does (stage 3A).
std::string raw_screen(std::string line) {
  constexpr std::pair<std::string_view, std::string_view> Reads[] = {
      {"srgb_dec(pbr_gs)", "pbr_gs"},
      {"srgb_dec(pbr_hg)", "pbr_hg"},
      {"srgb_dec(pbr_sfb)", "pbr_sfb"},
      {"srgb_dec(textureSampleLevel(tex7, tex7_samp,", "(textureSampleLevel(tex7, tex7_samp,"},
  };
  for (const auto& [from, to] : Reads) {
    for (size_t at = line.find(from); at != std::string::npos; at = line.find(from, at + to.size())) {
      line.replace(at, from.size(), to);
    }
  }
  return line;
}

struct Kind {
  int pbr;
  int kind;
};
constexpr Kind Kinds[] = {{1, 0}, {1, 1}, {1, 34}, {1, 36}, {1, 37}};

TEST(SceneHdrGx, PbrSourcesSkipTheEncodeOnlyInHdr) {
  for (const auto [pbr, kind] : Kinds) {
    SCOPED_TRACE(kind);
    const ShaderConfig ldrConfig = gxConfig(pbr, kind);
    ShaderConfig hdrConfig = ldrConfig;
    hdrConfig.sceneHdr = true;
    const std::string ldr = build_shader_source(ldrConfig);
    const std::string hdr = build_shader_source(hdrConfig);
    ASSERT_FALSE(ldr.empty());
    EXPECT_EQ(ldr.find("hdr_tone_fwd"), std::string::npos);
    EXPECT_EQ(ldr.find("hdr_exposed"), std::string::npos);
    EXPECT_FALSE(has_pbr_lin(ldr));
    EXPECT_NE(ldr.find("prev = vec4f(srgb_enc(pbr_tm + pbr_pass), pbr_alpha);"), std::string::npos);
    EXPECT_NE(hdr.find("prev = vec4f(pbr_lin + pbr_pass + pbr_keep, pbr_alpha);"), std::string::npos);
    EXPECT_EQ(ldr.find("pbr_keep"), std::string::npos);
    EXPECT_EQ(ldr.find("pbr_vfw"), std::string::npos);
    EXPECT_EQ(hdr.find("srgb_enc(pbr_tm + pbr_pass)"), std::string::npos);
    EXPECT_NE(hdr.find("fn hdr_untone("), std::string::npos);
    EXPECT_NE(hdr.find("var pbr_lin = pbr_out;"), std::string::npos);
    if (kind == 36) {
      // C is in pbr_out, before the tone, so pbr_lin carries it without a second scale.
      EXPECT_NE(hdr.find("let pbr_out = max(pbr_lo + pbr_glow, vec3f(0.0)) * inv_c;"), std::string::npos);
      EXPECT_EQ(hdr.find("*= inv_c"), std::string::npos);
    }

    // Every line HDR adds is about HDR; every LDR line it drops is an output it replaced.
    const auto l = lines(ldr);
    const auto h = lines(hdr);
    const std::multiset<std::string> ls(l.begin(), l.end()), hs(h.begin(), h.end());
    std::multiset<std::string> screen;
    for (const auto& line : l) {
      if (const auto raw = raw_screen(line); raw != line) {
        screen.insert(raw);
      }
    }
    for (const auto& line : h) {
      if (!ls.contains(line) && !line.empty() && !hdr_line(line) && !output_line(line) && !screen.contains(line)) {
        // Lines of ToneWgsl itself.
        EXPECT_NE(std::string_view(gfx::hdr::ToneWgsl).find(line), std::string_view::npos) << line;
      }
    }
    for (const auto& line : l) {
      if (!hs.contains(line)) {
        EXPECT_TRUE(output_line(line) || (raw_screen(line) != line && hs.contains(raw_screen(line)))) << line;
      }
    }
    // The alpha (kind 34's coverage included) is the same expression in both.
    std::vector<std::string> la, ha;
    for (const auto& line : l)
      if (line.find("pbr_alpha") != std::string::npos && line.find("prev = vec4f(") == std::string::npos)
        la.push_back(line);
    for (const auto& line : h)
      if (line.find("pbr_alpha") != std::string::npos && line.find("prev = vec4f(") == std::string::npos &&
          line.find("pbr_vfw") == std::string::npos)
        ha.push_back(line);
    EXPECT_FALSE(la.empty());
    EXPECT_EQ(la, ha);
  }
}

TEST(SceneHdrGx, TevSourcesTakeTheDisplayColourBackThroughTheCurve) {
  const ShaderConfig ldrConfig = gxConfig(0, 0);
  ShaderConfig hdrConfig = ldrConfig;
  hdrConfig.sceneHdr = true;
  const std::string ldr = build_shader_source(ldrConfig);
  const std::string hdr = build_shader_source(hdrConfig);
  EXPECT_EQ(ldr.find("hdr_"), std::string::npos);
  EXPECT_NE(hdr.find("hdr_tone: array<vec4f, 3>,"), std::string::npos);
  EXPECT_NE(hdr.find("prev = vec4f(hdr_exposed(prev.rgb, ubuf.hdr_tone[0], ubuf.hdr_tone[1], ubuf.hdr_tone[2]), prev.a);"),
            std::string::npos);
  EXPECT_NE(hdr.find("fn hdr_exposed("), std::string::npos);
  EXPECT_FALSE(has_pbr_lin(hdr));
  // The conditional conversion is the last word on the colour; both branches preserve alpha.
  const size_t tail = hdr.find("// HDR scene");
  ASSERT_NE(tail, std::string::npos);
  const size_t fallback = hdr.find("prev = vec4f(hdr_exposed(prev.rgb,", tail);
  ASSERT_NE(fallback, std::string::npos);
  EXPECT_EQ(hdr.find("prev = vec4f(", fallback + 1), std::string::npos);
  EXPECT_NE(hdr.find("if (ubuf.hdr_tev_blend.x > 0.5)"), std::string::npos);
  EXPECT_NE(hdr.find("prev = vec4f(hdr_tev_srcalpha_add(prev.rgb, prev.a, ubuf.hdr_tone[0], ubuf.hdr_tone[1], "
                     "ubuf.hdr_tone[2]), prev.a);"),
            std::string::npos);
  EXPECT_NE(hdr.find("} else if (ubuf.hdr_tev_blend.y > 0.5) {\n"
                     "        prev = vec4f(hdr_tev_dst_factor(prev.rgb, ubuf.hdr_tev_blend.y), prev.a);"),
            std::string::npos);
  EXPECT_NE(hdr.find(gfx::hdr::TevBlendWgsl), std::string::npos);
  EXPECT_NE(gfx::hdr::TevBlendWgsl.find("if (a <= 0.0)"), std::string_view::npos);
  EXPECT_NE(gfx::hdr::TevBlendWgsl.find("hdr_exposed(clamp(c, vec3f(0.0), vec3f(1.0)) * a, t0, t1, t2) / a"),
            std::string_view::npos);
  for (const auto [pbr, kind] : Kinds) {
    auto config = gxConfig(pbr, kind);
    config.sceneHdr = true;
    EXPECT_EQ(build_shader_source(config).find("hdr_tev_"), std::string::npos);
  }
}

TEST(SceneHdrGx, TheModeBitIsTheOldPaddingSoLdrKeysKeepTheirBytes) {
  // ShaderConfig is hashed and cached as raw bytes: an LDR config must be the bytes it always was.
  static_assert(std::has_unique_object_representations_v<ShaderConfig>);
  ShaderConfig defaults;
  EXPECT_FALSE(defaults.sceneHdr);
  // Of all the bitfield byte's bits, only sceneHdr changes it.
  ShaderConfig hdr = defaults;
  hdr.sceneHdr = true;
  size_t differing = 0;
  for (size_t i = 0; i < sizeof(ShaderConfig); ++i)
    differing += reinterpret_cast<const u8*>(&hdr)[i] != reinterpret_cast<const u8*>(&defaults)[i];
  EXPECT_EQ(differing, 1u);
  EXPECT_NE(xxh3_hash(hdr), xxh3_hash(defaults));
  EXPECT_FALSE(hdr == defaults);
  // 16: pbrCut (also old padding) moved the cut tests into the key, so a cached cutout caster
  // (shadowPass 2) of 15 names an fs_shadow its shader no longer has.
  EXPECT_EQ(GXPipelineConfigVersion, 16u);
  ShaderConfig cut = defaults;
  cut.pbrCut = 1;
  differing = 0;
  for (size_t i = 0; i < sizeof(ShaderConfig); ++i)
    differing += reinterpret_cast<const u8*>(&cut)[i] != reinterpret_cast<const u8*>(&defaults)[i];
  EXPECT_EQ(differing, 1u);
  // The pipeline key around it has no padding either (pipeline.hpp asserts it too): every key byte
  // is a config field, so the xxh3 key of a config is fully determined by its values.
  static_assert(std::has_unique_object_representations_v<PipelineConfig>);
}

// The TEV HDR uniform: 3 tone rows (GXState::pbrTone), declared in the WGSL and packed in the UBO
// only for a non-PBR HDR draw; PBR reads its own pbr_tone and LDR has neither.
TEST(SceneHdrGx, TheTevToneRowsAreInTheUniformOnlyInHdr) {
  const auto saved = gfx::detail::resources().limits.minUniformBufferOffsetAlignment;
  gfx::detail::resources().limits.minUniformBufferOffsetAlignment = 256;
  const auto savedTone = g_gxState.pbrTone;
  gfx::detail::testing::StagingPool staging;
  gfx::detail::FramePacket frame;
  staging.attach(frame);
  gfx::detail::begin_recording(frame, 0);
  struct Cleanup {
    u32 alignment;
    std::array<Vec4<float>, 3> tone;
    ~Cleanup() {
      g_gxState.pbrTone = tone;
      gfx::detail::shutdown_recording();
      gfx::detail::resources().limits.minUniformBufferOffsetAlignment = alignment;
    }
  } cleanup{saved, savedTone};
  g_gxState.pbrTone = {Vec4<float>{4321.f, 1.f, 2.f, 3.f}, Vec4<float>{4.f, 5.f, 6.f, 7.f},
                       Vec4<float>{8.f, 9.f, 10.f, 11.f}};
  const float rows[12] = {4321.f, 1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f, 9.f, 10.f, 11.f};
  const auto packs_rows = [&](const ShaderInfo& info) {
    const auto range = build_uniform(info);
    const auto& bytes = frame.uniforms;
    EXPECT_GE(bytes.size(), range.offset + range.size);
    for (size_t at = range.offset; at + sizeof(rows) <= range.offset + range.size; at += 4)
      if (std::memcmp(bytes.data() + at, rows, sizeof(rows)) == 0)
        return true;
    return false;
  };
  for (const int pbr : {0, 1}) {
    SCOPED_TRACE(pbr);
    ShaderConfig ldr = gxConfig(pbr, 0);
    ShaderConfig hdr = ldr;
    hdr.sceneHdr = true;
    const ShaderInfo ldrInfo = build_shader_info(ldr);
    const ShaderInfo hdrInfo = build_shader_info(hdr);
    const std::string hdrSource = build_shader_source(hdr);
    EXPECT_FALSE(ldrInfo.usesHdrTone);
    EXPECT_EQ(build_shader_source(ldr).find("hdr_tone"), std::string::npos);
    if (pbr == 0) {
      EXPECT_TRUE(hdrInfo.usesHdrTone);
      // The size is rounded up to the uniform alignment, so the 3 rows may fit in LDR's padding; the packed
      // rows below are what proves they are there.
      EXPECT_GE(hdrInfo.uniformSize, ldrInfo.uniformSize);
      EXPECT_NE(hdrSource.find("\n    hdr_tone: array<vec4f, 3>,"), std::string::npos);
      EXPECT_TRUE(packs_rows(hdrInfo));
    } else {
      // PBR: pbr_tone carries the rows already (packed for LDR too); no second copy.
      EXPECT_FALSE(hdrInfo.usesHdrTone);
      EXPECT_EQ(hdrInfo.uniformSize, ldrInfo.uniformSize);
      EXPECT_EQ(hdrSource.find("hdr_tone:"), std::string::npos);
    }
    EXPECT_FALSE(pbr == 0 && packs_rows(ldrInfo));
  }
}

TEST(SceneHdrGx, ActualBlendFlagFollowsToneRowsWithinTheBindingWindow) {
  const auto savedTone = g_gxState.pbrTone;
  const auto savedMode = g_gxState.blendMode;
  const auto savedSrc = g_gxState.blendFacSrc;
  const auto savedDst = g_gxState.blendFacDst;
  const u32 alignment = gfx::detail::resources().limits.minUniformBufferOffsetAlignment;
  gfx::detail::resources().limits.minUniformBufferOffsetAlignment = 16;
  gfx::detail::testing::StagingPool staging;
  gfx::detail::FramePacket frame;
  staging.attach(frame);
  gfx::detail::begin_recording(frame, 0);
  struct Cleanup {
    std::array<Vec4<float>, 3> tone;
    GXBlendMode mode;
    GXBlendFactor src, dst;
    u32 alignment;
    ~Cleanup() {
      g_gxState.pbrTone = tone;
      g_gxState.blendMode = mode;
      g_gxState.blendFacSrc = src;
      g_gxState.blendFacDst = dst;
      gfx::detail::shutdown_recording();
      gfx::detail::resources().limits.minUniformBufferOffsetAlignment = alignment;
    }
  } cleanup{savedTone, savedMode, savedSrc, savedDst, alignment};
  std::memcpy(g_gxState.pbrTone.data(), RainTone.data(), sizeof(RainTone));
  auto ldr = gxConfig(0, 0);
  auto hdr = ldr;
  hdr.sceneHdr = true;
  const auto info = build_shader_info(hdr);
  EXPECT_EQ(info.uniformSize, build_shader_info(ldr).uniformSize + 4 * sizeof(Vec4<float>));
  EXPECT_LE(info.uniformSize, MaxUniformSize);
  for (const auto mode : {GX_BM_NONE, GX_BM_BLEND, GX_BM_SUBTRACT, GX_BM_LOGIC}) {
    for (const auto src : {GX_BL_ZERO, GX_BL_ONE, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA}) {
      for (const auto dst : {GX_BL_ZERO, GX_BL_ONE, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA}) {
        g_gxState.blendMode = mode;
        g_gxState.blendFacSrc = src;
        g_gxState.blendFacDst = dst;
        const auto range = build_uniform(info);
        ASSERT_GE(frame.uniforms.size(), range.offset + range.size);
        ASSERT_LE(range.size, info.uniformSize);
        size_t at = range.offset;
        for (; at + sizeof(RainTone) + 16 <= range.offset + range.size; at += 16)
          if (std::memcmp(frame.uniforms.data() + at, RainTone.data(), sizeof(RainTone)) == 0)
            break;
        ASSERT_LE(at + sizeof(RainTone) + 16, range.offset + range.size);
        std::array<float, 4> flag{};
        std::memcpy(flag.data(), frame.uniforms.data() + at + sizeof(RainTone), sizeof(flag));
        EXPECT_EQ(flag[0],
                  mode == GX_BM_BLEND && src == GX_BL_SRCALPHA && (dst == GX_BL_ONE || dst == GX_BL_INVSRCALPHA) ? 1.f
                                                                                                                : 0.f);
        EXPECT_EQ(flag[1], 0.f);
        EXPECT_EQ(flag[2], 0.f);
        EXPECT_EQ(flag[3], 0.f);
      }
    }
  }
}

} // namespace
} // namespace aurora::gx

namespace aurora::gfx::vfx {
namespace {

PipelineConfig vfxConfig(uint32_t blend, bool hdr) {
  PipelineConfig p;
  p.colorFormats[0] = hdr ? webgpu::SceneHdrFormat : wgpu::TextureFormat::RGBA8Unorm;
  p.depthStencilFormat = wgpu::TextureFormat::Depth24Plus;
  p.colorAttachmentCount = 1;
  p.features = 1; // ColorTex
  p.slotKey = 0x1ffff8u; // colour from slot 0, every other read unbound
  p.blend = blend | (hdr ? BlendSceneHdr : 0u);
  p.compare = static_cast<uint32_t>(wgpu::CompareFunction::LessEqual);
  return p;
}

TEST(SceneHdrVfx, LdrSourcesAreUnchanged) {
  for (uint32_t blend = 0; blend <= 5; ++blend) {
    const std::string ldr = pipeline_source(vfxConfig(blend, false));
    EXPECT_EQ(ldr.find("const HDR"), std::string::npos);
    EXPECT_EQ(ldr.find("if (HDR)"), std::string::npos);
    EXPECT_EQ(ldr.find("hdr_"), std::string::npos);
    EXPECT_NE(ldr.find("const BLEND: u32 = " + std::to_string(blend) + "u;"), std::string::npos);
    EXPECT_EQ(BlendSceneHdr & blend, 0u);
  }
}

TEST(SceneHdrVfx, OpacityGainBiasUsesSpareUniformLanesInBothForms) {
  DrawDesc desc;
  EXPECT_EQ(desc.opacityGain, 1.f);
  EXPECT_EQ(desc.opacityBias, 0.f);
  for (bool hdr : {false, true}) {
    auto config = vfxConfig(2, hdr);
    config.features = OpacityFade | OpacityFresnel;
    const std::string source = pipeline_source(config);
    // Exercise the actual generated source, not a parallel CPU formula. The gain is OUTSIDE sat.
    EXPECT_NE(source.find("x = x * u.misc2.y * sat(smooth3(select(select(0.0, 1.0, d >= fx), sat((d - fx) / q), q != 0.0)) - u.misc2.z);"), std::string::npos);
    EXPECT_NE(source.find("x = x * u.misc2.y * sat(smooth3(select(select(0.0, 1.0, fx <= 0.0), sat(-fx / q), q != 0.0)) - u.misc2.z);"), std::string::npos);
    EXPECT_NE(source.find("misc2: vec4f,"), std::string::npos);
  }
}

TEST(SceneHdrVfx, HdrSourcesWriteExposedLinear) {
  for (uint32_t blend = 0; blend <= 5; ++blend) {
    SCOPED_TRACE(blend);
    const std::string ldr = pipeline_source(vfxConfig(blend, false));
    const std::string hdr = pipeline_source(vfxConfig(blend, true));
    EXPECT_NE(hdr.find("const HDR: bool = true;"), std::string::npos);
    // The shader sees the blend mode alone, never the HDR bit.
    EXPECT_NE(hdr.find("const BLEND: u32 = " + std::to_string(blend) + "u;"), std::string::npos);
    EXPECT_NE(hdr.find("fn hdr_exposed("), std::string::npos);
    const size_t multiply = hdr.find("if (BLEND == 4u) {");
    const size_t tail = hdr.find("  if (HDR) {");
    const size_t tone = hdr.find("  var col = tone(rgb);");
    ASSERT_NE(multiply, std::string::npos);
    ASSERT_NE(tail, std::string::npos);
    EXPECT_LT(multiply, tail);
    EXPECT_LT(tail, tone);
    const std::string body = hdr.substr(tail, tone - tail);
    EXPECT_NE(body.find("var lin = max(rgb, vec3f(0.0));"), std::string::npos);
    EXPECT_NE(body.find("if (u.tone1.x > 0.0 && u.tone0.w > 0.0) { lin = lin * u.tone0.w; }"), std::string::npos);
    // The fog condition is wrapped over two lines in the generated WGSL; collapse whitespace to match it,
    // and pin every FEAT predicate: fogged (1 << 27) still fogs, while NoFog (1 << 20), the retail
    // unfogged bit (1 << 22) and the Remastered no-fog bits 29 (BarrierShield/BarrierGlow) and 30 exclude it.
    auto squeezed = [](const std::string& s) {
      std::string out;
      bool space = false;
      for (const char c : s) {
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
          space = true;
          continue;
        }
        if (space && !out.empty()) out += ' ';
        space = false;
        out += c;
      }
      return out;
    };
    EXPECT_NE(squeezed(body).find("if ((BLEND == 0u || BLEND == 3u || (FEAT & 134217728u) != 0u) && (FEAT & 1048576u) == 0u && (FEAT & 4194304u) == 0u && (FEAT & 536870912u) == 0u && (FEAT & 1073741824u) == 0u) {"), std::string::npos);
    EXPECT_NE(body.find("hdr_exposed(u.fogColor.rgb, u.tone0, u.tone1, u.tone2)"), std::string::npos);
    EXPECT_NE(body.find("return vec4f(lin, alpha);"), std::string::npos);
    EXPECT_EQ(body.find("tone("), std::string::npos);
    EXPECT_EQ(body.find("clamp(lin"), std::string::npos);
    // Genuine HDR light (33.8 after the exposure) is not capped; only hdr_exposed's display
    // reconstruction (the fog colour) is.
    EXPECT_EQ(body.find("hdr_f16_max"), std::string::npos);
    EXPECT_EQ(body.find("min(lin"), std::string::npos);
    // Past the HDR additions, the source is LDR's.
    std::string stripped = hdr;
    stripped.erase(tail, tone - tail);
    stripped.erase(stripped.find("const HDR: bool = true;\n"), 24);
    stripped.erase(stripped.size() - gfx::hdr::ToneWgsl.size());
    EXPECT_EQ(stripped, ldr);
  }
}

bool same(const wgpu::BlendComponent& a, const wgpu::BlendComponent& b) {
  return a.operation == b.operation && a.srcFactor == b.srcFactor && a.dstFactor == b.dstFactor;
}

TEST(SceneHdrVfx, TheHdrBitKeepsTheBlend) {
  for (uint32_t blend = 0; blend <= 5; ++blend) {
    SCOPED_TRACE(blend);
    const wgpu::BlendState ldr = pipeline_blend(vfxConfig(blend, false));
    const wgpu::BlendState hdr = pipeline_blend(vfxConfig(blend, true));
    EXPECT_TRUE(same(ldr.color, hdr.color));
    EXPECT_TRUE(same(ldr.alpha, hdr.alpha));
    EXPECT_NE(hdr.color.srcFactor, wgpu::BlendFactor::Undefined);
    EXPECT_NE(hdr.alpha.dstFactor, wgpu::BlendFactor::Undefined);
  }
  // Multiply stays the target times the source, unscaled.
  EXPECT_EQ(pipeline_blend(vfxConfig(4, true)).color.srcFactor, wgpu::BlendFactor::Dst);
  EXPECT_EQ(pipeline_blend(vfxConfig(4, true)).color.dstFactor, wgpu::BlendFactor::Zero);
}

TEST(SceneHdrVfx, AFormatMismatchIsCaught) {
  RenderTargetLayout ldr{};
  ldr.colorAttachmentCount = 1;
  ldr.colorAttachments[SceneColorAttachmentIndex].format = wgpu::TextureFormat::RGBA8Unorm;
  RenderTargetLayout hdr = ldr;
  hdr.colorAttachments[SceneColorAttachmentIndex].format = webgpu::SceneHdrFormat;
  RenderTargetLayout none{};
  for (uint32_t blend = 0; blend <= 5; ++blend) {
    EXPECT_TRUE(scene_format_matches(ldr, blend));
    EXPECT_FALSE(scene_format_matches(ldr, blend | BlendSceneHdr));
    EXPECT_TRUE(scene_format_matches(hdr, blend | BlendSceneHdr));
    EXPECT_FALSE(scene_format_matches(hdr, blend));
    EXPECT_TRUE(scene_format_matches(none, blend));
    EXPECT_FALSE(scene_format_matches(none, blend | BlendSceneHdr));
  }
}

TEST(SceneHdrVfx, TheHdrBitKeepsTheConfigLayout) {
  // Cached VFX records hold blend 0..5; the HDR bit is a new value, not a new field.
  static_assert(sizeof(PipelineConfig) == sizeof(wgpu::TextureFormat) * (MaxColorAttachments + 1) + 8 * 4);
  EXPECT_EQ(VfxPipelineConfigVersion, 1u);
  const auto a = vfxConfig(0, false), b = vfxConfig(0, true);
  EXPECT_NE(xxh3_hash(a), xxh3_hash(b));
}

TEST(SceneHdrVfx, IceModelCullIsExplicitAndSeparatesCachedConfigs) {
  auto plain = vfxConfig(0, true);
  auto ice = plain;
  ice.features = shader_features(plain.features | ModelBackCull);
  EXPECT_EQ(ice.features ^ plain.features, uint32_t(ModelBackCull));
  EXPECT_NE(xxh3_hash(plain), xxh3_hash(ice));
  const auto a = pipeline_primitive(plain), b = pipeline_primitive(ice);
  EXPECT_EQ(a.cullMode, wgpu::CullMode::None);
  EXPECT_EQ(a.frontFace, wgpu::FrontFace::CCW);
  EXPECT_EQ(b.cullMode, wgpu::CullMode::Back);
  EXPECT_EQ(b.frontFace, wgpu::FrontFace::CCW);
  EXPECT_EQ(b.topology, wgpu::PrimitiveTopology::TriangleList);
  EXPECT_TRUE(same(pipeline_blend(plain).color, pipeline_blend(ice).color));
  EXPECT_TRUE(same(pipeline_blend(plain).alpha, pipeline_blend(ice).alpha));
}

// The wave orb's exposure policy (DrawDesc::alreadyExposed, VMAT's AlreadyExposed bit) is a uniform
// value per draw, not a shader feature: no source, config or key changes with it.
TEST(SceneHdrVfx, ThePolicyBitIsNotAShaderFeature) {
  EXPECT_EQ(uint32_t(AlreadyExposed), 1u << 31);
  constexpr uint32_t shaderBits = 8191u & ~uint32_t(DepthSoften); // ColorTex..ColorRgbOnly but DepthSoften
  EXPECT_EQ(shaderBits & AlreadyExposed, 0u);
  EXPECT_EQ(shader_features(shaderBits), shaderBits);
  EXPECT_EQ(shader_features(shaderBits | AlreadyExposed), shaderBits);
  EXPECT_EQ(shader_features(ColorTex | DepthSoften | AlreadyExposed), uint32_t(ColorTex | DepthSoften));
  for (const bool hdr : {false, true}) {
    PipelineConfig flagged = vfxConfig(5, hdr);
    flagged.features = shader_features(flagged.features | AlreadyExposed);
    EXPECT_EQ(pipeline_source(flagged), pipeline_source(vfxConfig(5, hdr)));
    EXPECT_EQ(xxh3_hash(flagged), xxh3_hash(vfxConfig(5, hdr)));
  }
  EXPECT_FALSE(DrawDesc{}.alreadyExposed) << "every draw but a flagged material's keeps the room's exposure";
}

// What the HDR tail does with the uniform's exposure (tone0.w, tone1.x > 0 enables it).
float hdr_tail(float rgb, float exposure, float tone1x) { return tone1x > 0.f && exposure > 0.f ? rgb * exposure : rgb; }

TEST(SceneHdrVfx, ExplicitModelExposureDoesNotInheritThePriorModelMultiplier) {
  constexpr float inverse = 13.7331038f;
  constexpr float frameExposure = 0.07281675101006664f;
  DrawDesc desc;
  EXPECT_FALSE(desc.exposureOverride);
  desc.exposureOverride = frameExposure;
  for (const float inherited : {0.979701519f, 1.f, 0.125f, 8.f, 0.f}) {
    SCOPED_TRACE(inherited);
    const float uniform = draw_exposure(inherited, true, false, desc.exposureOverride);
    EXPECT_EQ(uniform, frameExposure);
    EXPECT_NEAR(inverse * uniform, 1.f, 1e-6f);
    EXPECT_NEAR(hdr_tail(inverse, uniform, 1.38888872f), 1.f, 1e-6f);
    EXPECT_EQ(draw_exposure(inherited, false, false, desc.exposureOverride), inherited);
    EXPECT_EQ(draw_exposure(inherited, false, true, desc.exposureOverride), inherited);
    EXPECT_EQ(draw_exposure(inherited, true, true, desc.exposureOverride), 1.f);
    EXPECT_EQ(draw_exposure(inherited, false, true, std::nullopt), inherited);
    EXPECT_EQ(draw_exposure(inherited, true, false, std::nullopt), inherited);
    EXPECT_EQ(draw_exposure(inherited, true, true, std::nullopt), 1.f);
  }
}

TEST(SceneHdrVfx, AnAlreadyExposedDrawIsExposureOneInHdrOnly) {
  constexpr float orb = 33.8f; // T 1 x COLR x ITEN of the source orb
  for (const float inherited : {0.125f, 1.f, 8.f, 0.f}) {
    SCOPED_TRACE(inherited);
    // HDR: the flagged draw writes its light at net exposure 1, whatever the last PBR draw left.
    EXPECT_EQ(draw_exposure(inherited, true, true), 1.f);
    EXPECT_EQ(hdr_tail(orb, draw_exposure(inherited, true, true), 0.8f), orb);
    // HDR, unflagged: the inherited exposure, as before.
    const float kept = draw_exposure(inherited, true, false);
    EXPECT_EQ(std::memcmp(&kept, &inherited, sizeof(float)), 0);
    EXPECT_EQ(hdr_tail(orb, kept, 0.8f), inherited > 0.f ? orb * inherited : orb);
    // LDR ignores the policy: the uniform's bytes are the GX state's.
    for (const bool flag : {false, true}) {
      const float ldr = draw_exposure(inherited, false, flag);
      EXPECT_EQ(std::memcmp(&ldr, &inherited, sizeof(float)), 0);
    }
  }
  const float negZero = -0.f;
  const float ldrNegZero = draw_exposure(negZero, false, true);
  EXPECT_EQ(std::memcmp(&ldrNegZero, &negZero, sizeof(float)), 0);
}

} // namespace
} // namespace aurora::gfx::vfx
