#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "gx/gx.hpp"

namespace aurora::gx {
// Defined in lib/gx/shader.cpp: the scalar expressions the inventory visor shaders (kinds 36, 37) emit.
std::string_view inventory_visor_expr(std::string_view name) noexcept;

namespace {

// A tiny evaluator for the emitted scalar WGSL expressions: numbers, names, + - * /, parentheses, clamp/min/max/mix
// and a one-value vec3f splat (each channel is computed alone, so a splat is its scalar).
class Eval {
public:
  Eval(std::string_view text, const std::map<std::string, double>& vars) : m_s(text), m_vars(vars) {}
  double run() {
    const double v = sum();
    skip();
    EXPECT_EQ(m_i, m_s.size()) << "trailing text in " << m_s;
    return v;
  }

private:
  std::string_view m_s;
  const std::map<std::string, double>& m_vars;
  size_t m_i = 0;
  void skip() {
    while (m_i < m_s.size() && std::isspace(static_cast<unsigned char>(m_s[m_i])))
      ++m_i;
  }
  bool eat(char c) {
    skip();
    if (m_i < m_s.size() && m_s[m_i] == c) {
      ++m_i;
      return true;
    }
    return false;
  }
  double sum() {
    double v = product();
    for (;;) {
      if (eat('+'))
        v += product();
      else if (eat('-'))
        v -= product();
      else
        return v;
    }
  }
  double product() {
    double v = unary();
    for (;;) {
      if (eat('*'))
        v *= unary();
      else if (eat('/'))
        v /= unary();
      else
        return v;
    }
  }
  double unary() {
    if (eat('-'))
      return -unary();
    return atom();
  }
  double atom() {
    skip();
    if (eat('(')) {
      const double v = sum();
      EXPECT_TRUE(eat(')'));
      return v;
    }
    const size_t start = m_i;
    if (m_i < m_s.size() && (std::isdigit(static_cast<unsigned char>(m_s[m_i])) || m_s[m_i] == '.')) {
      while (m_i < m_s.size() && (std::isdigit(static_cast<unsigned char>(m_s[m_i])) || m_s[m_i] == '.'))
        ++m_i;
      return std::stod(std::string(m_s.substr(start, m_i - start)));
    }
    while (m_i < m_s.size() && (std::isalnum(static_cast<unsigned char>(m_s[m_i])) || m_s[m_i] == '_'))
      ++m_i;
    const std::string name(m_s.substr(start, m_i - start));
    EXPECT_FALSE(name.empty()) << "bad token in " << m_s;
    if (eat('(')) {
      double a[3];
      int n = 0;
      do {
        a[n++] = sum();
      } while (n < 3 && eat(','));
      EXPECT_TRUE(eat(')'));
      if (name == "clamp")
        return std::min(std::max(a[0], a[1]), a[2]);
      if (name == "min")
        return std::min(a[0], a[1]);
      if (name == "max")
        return std::max(a[0], a[1]);
      if (name == "mix")
        return a[0] + (a[1] - a[0]) * a[2];
      // A splat of one value: every channel is the same, so it evaluates as that value.
      if (name == "vec3f" && n == 1)
        return a[0];
      ADD_FAILURE() << "unknown function " << name;
      return 0.0;
    }
    const auto it = m_vars.find(name);
    EXPECT_TRUE(it != m_vars.end()) << "unbound " << name << " in " << m_s;
    return it == m_vars.end() ? 0.0 : it->second;
  }
};

double alpha(double ba, double da) {
  return Eval(inventory_visor_expr("alpha"), {{"inv_ba", ba}, {"inv_da", da}}).run();
}
double weight(double c, double b, double a) {
  return Eval(inventory_visor_expr("weight"), {{"inv_c", c}, {"inv_b", b}, {"inv_a", a}}).run();
}

ShaderConfig inventoryConfig(int kind, bool screen) {
  ShaderConfig config;
  config.pbr = 1;
  config.pbrKind = static_cast<u8>(kind);
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

size_t count(const std::string& s, std::string_view what) {
  size_t n = 0;
  for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + what.size()))
    ++n;
  return n;
}
size_t pos(const std::string& s, std::string_view what) { return s.find(what); }

// --- CPU reference edge cases, run on the emitted expressions -----------------------------------------------

TEST(GxInventoryVisor, AlphaSquaresBaseOnly) {
  EXPECT_NEAR(alpha(0.5, 1.0), 0.25, 1e-12);
  EXPECT_NEAR(alpha(0.5, 0.5), 0.125, 1e-12); // DIFC alpha is not squared
  EXPECT_NEAR(alpha(1.0, 0.0), 0.0, 1e-12);
  EXPECT_NEAR(alpha(2.0, 1.0), 1.0, 1e-12); // clamped
}

TEST(GxInventoryVisor, SceneWeightEdgeCases) {
  EXPECT_NEAR(weight(0.0, 0.7, 0.3), 1.0, 1e-12);          // coverage 0 keeps the scene
  EXPECT_NEAR(weight(1.0, 0.5, 0.25), 0.5 * 0.75, 1e-12);  // coverage 1: scene * b * (1 - A)
  EXPECT_NEAR(weight(1.0, 0.9, 1.0), 0.0, 1e-12);          // opaque, full coverage: scene suppressed
  EXPECT_NEAR(weight(0.5, 0.5, 0.25), 0.5 + 0.5 * 0.375, 1e-12);
  // DIFC alpha and C are independent: A only enters through the expression's own input.
  EXPECT_NEAR(weight(0.5, 0.4, alpha(0.5, 0.5)), 1.0 - 0.5 + 0.5 * 0.4 * (1.0 - 0.125), 1e-12);
  EXPECT_NEAR(weight(1.0, 0.0, 0.0), 0.0, 1e-12); // black colour at A=0 blocks the scene (O2 multiplicative)
}

// --- generated source wiring ----------------------------------------------------------------------------------

TEST(GxInventoryVisor, Kind36Wiring) {
  const std::string src = build_shader_source(inventoryConfig(36, true));
  ASSERT_NE(pos(src, "pbr_pass += pbr_sdst * (" + std::string(inventory_visor_expr("weight")) + ");"), std::string::npos);
  // The weight is a vec3f throughout: its scalar term is splat, not added to the vec3 directly.
  EXPECT_EQ(inventory_visor_expr("weight").substr(0, 6), "vec3f(");
  EXPECT_NE(pos(src, "let inv_a = " + std::string(inventory_visor_expr("alpha")) + ";"), std::string::npos);
  EXPECT_NE(pos(src, "let inv_c = ubuf.pbr_model_coverage.x;"), std::string::npos);
  // Modulation happens before F0 and the diffuse, and the diffuse is premultiplied by A.
  const size_t b = pos(src, "let inv_b = max(sampled0.rgb, vec3f(0.0)) * ubuf.pbr_model_diffuse.rgb;");
  const size_t f0 = pos(src, "let pbr_f0 =");
  const size_t diff = pos(src, "let pbr_diff =");
  ASSERT_NE(b, std::string::npos);
  EXPECT_LT(b, f0);
  EXPECT_LT(b, diff);
  EXPECT_NE(pos(src, "var pbr_base = inv_b;"), std::string::npos);
  EXPECT_NE(src.find("* inv_a * ubuf.pbr_light_scale.x", diff), std::string::npos);
  // ICMC (rgb only) is added linearly before the tone curve, and C scales that light once, before the tone
  // (source (lit + ICMC) * C); the composition precedes the encode. The generic model fade is gone.
  const size_t icmc = pos(src, "pbr_glow += ubuf.pbr_model_incan.rgb;");
  const size_t out = pos(src, "let pbr_out = max(pbr_lo + pbr_glow, vec3f(0.0)) * inv_c;");
  const size_t tone = pos(src, "pbr_tm = clamp(pbr_tm, vec3f(0.0), vec3f(1.0));");
  const size_t comp = pos(src, "pbr_pass += pbr_sdst");
  const size_t enc = pos(src, "prev = vec4f(srgb_enc(pbr_tm + pbr_pass), pbr_alpha);");
  ASSERT_NE(icmc, std::string::npos);
  ASSERT_NE(out, std::string::npos);
  EXPECT_LT(icmc, out);
  EXPECT_LT(out, tone);
  EXPECT_LT(tone, comp);
  EXPECT_LT(comp, enc);
  // C is applied exactly once; ICMC alpha is not read.
  EXPECT_EQ(count(src, "inv_c;"), 1u);
  EXPECT_EQ(src.find("*= inv_c"), std::string::npos);
  EXPECT_EQ(src.find("pbr_model_incan.w"), std::string::npos);
  EXPECT_EQ(src.find("pbr_model_incan.a"), std::string::npos);
  EXPECT_EQ(src.find("pbr_light_scale.w - 1.0"), std::string::npos);
  EXPECT_EQ(src.find("-ubuf.pbr_light_scale.w - 1.0"), std::string::npos);
  EXPECT_NE(src.find("pbr_alpha = 1.0;", comp), std::string::npos);
  EXPECT_NE(pos(src, "pbr_scr: vec4f"), std::string::npos);
  EXPECT_EQ(count(src, "pbr_sdst * "), 1u);
}

TEST(GxInventoryVisor, Kind37Wiring) {
  const std::string src = build_shader_source(inventoryConfig(37, false));
  // ICMC rgb only (DMGI is the bounded default 1); varying ICMC.a cannot change the term, as it is never read.
  EXPECT_NE(pos(src, "pbr_glow += ubuf.pbr_model_incan.rgb;"), std::string::npos);
  EXPECT_EQ(src.find("pbr_model_incan.w"), std::string::npos);
  EXPECT_EQ(src.find("pbr_model_incan.a"), std::string::npos);
  EXPECT_EQ(src.find("pbr_tm *= "), std::string::npos);
  // No COVG: the field is only in the struct declaration.
  EXPECT_EQ(count(src, "pbr_model_coverage"), 1u);
  EXPECT_EQ(count(src, "inv_c"), 0u);
  EXPECT_EQ(src.find("pbr_sdst"), std::string::npos);
  EXPECT_EQ(src.find("pbr_light_scale.w - 1.0"), std::string::npos);
  EXPECT_NE(pos(src, "let inv_a = " + std::string(inventory_visor_expr("alpha")) + ";"), std::string::npos);
  EXPECT_LT(pos(src, "let inv_b ="), pos(src, "let pbr_f0 ="));
  // The diffuse is not premultiplied for 37.
  EXPECT_EQ(src.find("* inv_a * ubuf.pbr_light_scale.x"), std::string::npos);
}

// Kinds 0 and 3 read the model tint now (GxModelTint); the others still do not.
TEST(GxInventoryVisor, OtherKindsDoNotReadModulation) {
  for (const int kind : {34, 35}) {
    const std::string src = build_shader_source(inventoryConfig(kind, true));
    // Only the uniform struct mentions the fields.
    EXPECT_EQ(count(src, "pbr_model_diffuse"), 1u) << kind;
    EXPECT_EQ(count(src, "pbr_model_incan"), 1u) << kind;
    EXPECT_EQ(count(src, "pbr_model_coverage"), 1u) << kind;
    EXPECT_EQ(count(src, "inv_"), 0u) << kind;
    EXPECT_NE(src.find("pbr_light_scale.w - 1.0"), std::string::npos) << kind; // generic fade stays
  }
  // Kind 34 keeps its own composition untouched.
  const std::string k34 = build_shader_source(inventoryConfig(34, true));
  EXPECT_NE(k34.find("pbr_pass += pbr_sdst * vec3f(1.0 - pbr_alpha);"), std::string::npos);
}


// --- model tint outside the visor (kb topic/model-flags-fade-tint.md) -------------------------------------------

TEST(GxModelTint, StdKindsApplyDiffuseAndIncan) {
  for (const int kind : {0, 3}) {
    const std::string src = build_shader_source(inventoryConfig(kind, false));
    const size_t diff = pos(src, "let pbr_diff = select(pbr_base * ubuf.pbr_model_diffuse.rgb * (1.0 - pbr_metal)");
    EXPECT_NE(diff, std::string::npos) << "kind " << kind;
    const size_t glow = pos(src, "pbr_glow += ubuf.pbr_model_incan.rgb;");
    const size_t out = pos(src, "let pbr_out = max(pbr_lo + pbr_glow, vec3f(0.0));");
    ASSERT_NE(glow, std::string::npos) << "kind " << kind;
    EXPECT_LT(glow, out); // after the exposure scaling, before the output
    EXPECT_EQ(count(src, "pbr_model_incan.rgb"), 1u);
  }
}

TEST(GxModelTint, GlassTakesNoTint) {
  const std::string src = build_shader_source(inventoryConfig(8, false));
  EXPECT_EQ(pos(src, "pbr_model_diffuse.rgb"), std::string::npos);
  EXPECT_EQ(pos(src, "pbr_model_incan.rgb"), std::string::npos);
}

TEST(GxModelTint, InventoryKindsNotDoubleApplied) {
  for (const int kind : {36, 37}) {
    const std::string src = build_shader_source(inventoryConfig(kind, true));
    EXPECT_EQ(count(src, "pbr_model_diffuse.rgb"), 1u) << kind; // inv_b only
    EXPECT_EQ(count(src, "pbr_model_incan.rgb"), 1u) << kind;
  }
}

// Model_ThermalOverride replaces the shading only in the HDR scene and only with the bit; its grey is
// emitted after the HDR output, and it takes no GX fog.
TEST(GxPbrThermal, OverrideOnlyInHdrSceneWithTheBit) {
  ShaderConfig config = inventoryConfig(0, false);
  config.sceneHdr = 1;
  config.pbrThermal = 1;
  config.fogType = GX_FOG_PERSP_LIN;
  const std::string src = build_shader_source(config);
  const size_t hdr = pos(src, "prev = vec4f(pbr_lin + pbr_pass + pbr_keep, pbr_alpha);");
  const size_t thmN = pos(src, "let thm_n = ");
  const size_t thm = pos(src, "let thm_g = (thm_l * 3.75 + 0.25) * abs(dot(thm_n, pbr_v)) * ubuf.pbr_thermal.x +");
  ASSERT_NE(hdr, std::string::npos);
  ASSERT_NE(thmN, std::string::npos);
  ASSERT_NE(thm, std::string::npos);
  EXPECT_LT(hdr, thmN);
  EXPECT_LT(thmN, thm);
  // Its own normal: a quarter of the map's tilt, unnormalised (02c254b6 048_0), never the lit pbr_n.
  if (pos(src, "let thm_ts = ") != std::string::npos) {
    EXPECT_NE(pos(src, " * 0.498046875 - 0.25;"), std::string::npos);
  }
  EXPECT_NE(pos(src, "ubuf.pbr_thermal.z > 0.5 && !pbr_cut"), std::string::npos);
  EXPECT_EQ(pos(src, "mix(prev.rgb, hdr_exposed(ubuf.fog.color.rgb"), std::string::npos);

  ShaderConfig off = config;
  off.pbrThermal = 0;
  const std::string offSrc = build_shader_source(off);
  EXPECT_EQ(pos(offSrc, "thm_g"), std::string::npos);
  EXPECT_NE(pos(offSrc, "mix(prev.rgb, hdr_exposed(ubuf.fog.color.rgb"), std::string::npos);

  ShaderConfig ldr = config;
  ldr.sceneHdr = 0;
  EXPECT_EQ(pos(build_shader_source(ldr), "thm_g"), std::string::npos);
}

} // namespace
} // namespace aurora::gx
