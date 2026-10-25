#include "port_fx_debug.h"
#include "port_room_env.h"
#include "port_vfx_opacity.h"
#include "port_vfx_particles.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "Kyoto/Graphics/CColor.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Graphics/CTexture.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "Kyoto/Particles/CParticleGlobals.hpp"
#include "Kyoto/Particles/PortParticleMath.hpp"
#include "Kyoto/Particles/IElement.hpp"

#include "aurora/vfx.hpp"

namespace {
constexpr uint kMaxQuadsPerDraw = 16384;
constexpr float kEps = 1.1920929e-7f; // FLT_EPSILON, retail's "can be normalised" bound

// Sprite pivot table (x0, y0, x1, y1), indexed by VMAT.spriteCenter.
constexpr float kPivot[9][4] = {
    {-.5f, -.5f, .5f, .5f}, {0.f, 0.f, 1.f, 1.f},  {-.5f, 0.f, .5f, 1.f},
    {-1.f, 0.f, 0.f, 1.f},  {-1.f, -.5f, 0.f, .5f}, {-1.f, -1.f, 0.f, 0.f},
    {-.5f, -1.f, .5f, 0.f}, {0.f, -1.f, 1.f, 0.f},  {0.f, -.5f, 1.f, .5f},
};

// Quad corners in draw_quads order; (qx, qy) is the corner's position in the unit quad.
constexpr float kCorner[4][2] = {{0.f, 0.f}, {1.f, 0.f}, {1.f, 1.f}, {0.f, 1.f}};

// Remastered's sprite meshes for SSHP 3-6 (CGraphicsUtilCache::BuildParticleInstancingBuffers; kb
// format/SSHP.md): the corner (qx, qy) is both the position in the unit quad and the UV. Each
// triangle goes out as a degenerate quad (a, b, c, c). Shapes 0-2 are plain quad grids.
struct PortSpriteShape {
  const float (*pts)[2];
  const uint8_t* idx;
  int triangles;
};
constexpr float kTriPts[3][2] = {{0.f, 1.f}, {1.f, 1.f}, {.5f, 0.f}};
constexpr uint8_t kTriIdx[] = {0, 1, 2};
constexpr float kPentPts[5][2] = {{.2f, 0.f}, {.8f, 0.f}, {1.f, .6f}, {.5f, 1.f}, {0.f, .6f}};
constexpr uint8_t kPentIdx[] = {0, 3, 4, 0, 1, 3, 1, 2, 3};
constexpr float kHexPts[6][2] = {{.24f, 0.f}, {.76f, 0.f}, {1.f, .5f}, {.76f, 1.f}, {.24f, 1.f}, {0.f, .5f}};
constexpr uint8_t kHexIdx[] = {0, 1, 5, 1, 2, 3, 3, 4, 5, 1, 3, 5};
constexpr float kOctPts[8][2] = {{0.2928932f, 0.f}, {0.7071068f, 0.f}, {1.f, 0.2928932f}, {1.f, 0.7071068f},
                                 {0.7071068f, 1.f}, {0.2928932f, 1.f}, {0.f, 0.7071068f}, {0.f, 0.2928932f}};
constexpr uint8_t kOctIdx[] = {0, 1, 7, 1, 2, 3, 1, 3, 4, 1, 4, 7, 7, 4, 5, 7, 5, 6};
constexpr PortSpriteShape kShapes[4] = {
    {kTriPts, kTriIdx, 1}, {kPentPts, kPentIdx, 3}, {kHexPts, kHexIdx, 4}, {kOctPts, kOctIdx, 6}};

// One VTMT row's evaluated terms.
struct Tm {
  float a = 0.f, b = 0.f, c = 1.f, d = 1.f, cosE = 1.f, sinE = 0.f, f = 0.f;
};

float EvalReal(CRealElement* e, int frame, float def) {
  float v = def;
  if (e != nullptr) {
    e->GetValue(frame, v);
  }
  return v;
}

// One TMTR TRSS(A..F) as the shipped vertex shaders see it: GetValue 0x2de7dc packs it with
// pack_texture_transform 0x20e8f0 (A, B and E as 16-bit fields), and the VS unpacks them.
Tm EvalTrss(CRealElement* const* r, int frame) {
  Tm t;
  t.a = PortVfxTrssOffset(EvalReal(r[0], frame, 0.f));
  t.b = PortVfxTrssOffset(EvalReal(r[1], frame, 0.f));
  t.c = EvalReal(r[2], frame, 1.f);
  t.d = EvalReal(r[3], frame, 1.f);
  const float e = PortVfxTrssAngle(EvalReal(r[4], frame, 0.f));
  t.cosE = std::cos(e);
  t.sinE = std::sin(e);
  t.f = EvalReal(r[5], frame, 0.f);
  return t;
}

// Evaluates a VPMT/VSMT element into up to four floats; returns the count.
int EvalElem(const CPortVfxElem& e, int frame, float out[4]) {
  switch (e.kind) {
  case CPortVfxElem::kReal: {
    if (e.real == nullptr) {
      return 0;
    }
    float v = 0.f;
    e.real->GetValue(frame, v);
    out[0] = v;
    return 1;
  }
  case CPortVfxElem::kInt: {
    if (e.integer == nullptr) {
      return 0;
    }
    int v = 0;
    e.integer->GetValue(frame, v);
    out[0] = static_cast< float >(v);
    return 1;
  }
  case CPortVfxElem::kVector: {
    if (e.vec == nullptr) {
      return 0;
    }
    CVector3f v = CVector3f::Zero();
    e.vec->GetValue(frame, v);
    out[0] = v.GetX();
    out[1] = v.GetY();
    out[2] = v.GetZ();
    return 3;
  }
  case CPortVfxElem::kColor: {
    if (e.color == nullptr) {
      return 0;
    }
    e.color->GetValueHdr(frame, out);
    return 4;
  }
  }
  return 0;
}

// 3x3 helpers for the IceChargeBeam feed (row-major, float as the GPU computes them).
struct Mat3 {
  float m[3][3];
  CVector3f operator*(const CVector3f& v) const {
    return CVector3f(m[0][0] * v.GetX() + m[0][1] * v.GetY() + m[0][2] * v.GetZ(),
                     m[1][0] * v.GetX() + m[1][1] * v.GetY() + m[1][2] * v.GetZ(),
                     m[2][0] * v.GetX() + m[2][1] * v.GetY() + m[2][2] * v.GetZ());
  }
  float Det() const {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  }
  // The cofactor matrix: det x the inverse transpose.
  Mat3 Cofactor() const {
    Mat3 r;
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        const int i1 = (i + 1) % 3, i2 = (i + 2) % 3, j1 = (j + 1) % 3, j2 = (j + 2) % 3;
        r.m[i][j] = m[i1][j1] * m[i2][j2] - m[i1][j2] * m[i2][j1];
      }
    }
    return r;
  }
  // The cofactor matrix over the determinant: the inverse transpose.
  Mat3 InverseTranspose() const {
    Mat3 r = Cofactor();
    const float k = 1.f / Det();
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        r.m[i][j] *= k;
      }
    }
    return r;
  }
  Mat3 Transposed() const {
    Mat3 r;
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        r.m[i][j] = m[j][i];
      }
    }
    return r;
  }
};

// VFX_IceChargeBeam's VS (9e0605cd perm 000_1, ICECHARGE.md section 2): one rotation per whole
// step of t4 = time x CCH0.w + CCH2.x, from three hashes of floor(t4), and the shake amplitude
// mix(CCH0.y, CCH0.z, a fourth hash) x 0.1.
struct IceHash {
  Mat3 r;
  float amp;
};
float IceH(float x) {
  const float v = std::sin(x) * 43758.5469f;
  return v - std::floor(v);
}
IceHash IceHashAt(float time, const float params[7][4]) {
  constexpr float kPi = 3.14159012f;
  const float t4 = time * params[0][3] + params[2][0];
  const float t7 = std::floor(t4);
  const float s7 = std::sin(t7), c7 = std::cos(t7), k = t7 * 12.9898f;
  const float a = IceH(s7 * 78.233f + k) * kPi;
  const float b = IceH(c7 * 78.233f + k) * kPi;
  const float c = IceH(s7 * -78.233f + k) * kPi;
  const float h3 = IceH((t7 + 10.f) * 78.233f + k);
  const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b), cc = std::cos(c),
              sc = std::sin(c);
  IceHash h;
  h.r = {{{cc * cb, sb * sa - sc * ca * cb, sb * ca + sa * sc * cb},
          {sc, ca * cc, -sa * cc},
          {-sb * cc, sa * cb + sc * sb * ca, ca * cb - sa * sc * sb}}};
  const float lo = params[0][1] * 0.1f, hi = params[0][2] * 0.1f;
  h.amp = lo + (hi - lo) * h3;
  return h;
}
float IceSat(float x) { return std::min(std::max(x, 0.f), 1.f); }

CVector3f Normalised(const CVector3f& v, const CVector3f& fallback) {
  float m = v.MagSquared();
  if (m > kEps) {
    return v * (1.f / std::sqrt(m));
  }
  return fallback;
}

// A unit vector perpendicular to `a` (used when a cross product degenerates).
CVector3f AnyPerpendicular(const CVector3f& a) {
  CVector3f axis = std::fabs(a.GetX()) < 0.9f ? CVector3f(1.f, 0.f, 0.f) : CVector3f(0.f, 1.f, 0.f);
  return Normalised(CVector3f::Cross(a, axis), CVector3f(1.f, 0.f, 0.f));
}
} // namespace

CPortVfxData::~CPortVfxData() {
  for (CRealElement* e : vtmt) {
    delete e;
  }
  for (const CPortVfxElem& e : vpmt) {
    delete e.real;
    delete e.vec;
    delete e.integer;
    delete e.color;
  }
  for (const CPortVfxElem& e : vsmt) {
    delete e.real;
    delete e.vec;
    delete e.integer;
    delete e.color;
  }
  delete ssze;
  delete iten;
}

void PortVfxEvalParticle(const CGenDescription& desc, CElementGen::CParticle& particle,
                         int frame) {
  const CPortVfxData& vfx = *desc.xPortVfx;
  particle.xPortSsze = EvalReal(vfx.ssze, frame, particle.x2c_lineLengthOrSize);
  particle.xPortIten = EvalReal(vfx.iten, frame, 1.f);
  for (const CPortVfxElem& e : vfx.vpmt) {
    if (e.a >= 4) {
      continue;
    }
    float v[4] = {};
    int n = EvalElem(e, frame, v);
    for (int i = 0; i < n && e.b + i < 4; ++i) {
      particle.xPortVpmt[e.a][e.b + i] = v[i];
    }
  }
}

void PortVfxSetLaunchDir(CElementGen::CParticle& particle) {
  float m = particle.x1c_vel.MagSquared();
  particle.xPortLaunchDir =
      m > kEps ? particle.x1c_vel * (1.f / std::sqrt(m)) : CVector3f::Zero();
}

uint PortVfxEvalVsmt(const CPortVfxData& vfx, int frame, float vsmt[kPortVfxVsmtCount]) {
  uint mask = 0;
  for (const CPortVfxElem& e : vfx.vsmt) {
    if (e.a >= kPortVfxVsmtCount) {
      continue;
    }
    float v[4] = {};
    if (EvalElem(e, frame, v) > 0) {
      vsmt[e.a] = v[0];
      mask |= 1u << e.a;
    }
  }
  return mask;
}

void PortVfxEvalParams(const CPortVfxData& vfx, int frame, float params[7][4]) {
  for (u32 r = 0; r < kPortVfxParamCount; ++r) {
    for (int c = 0; c < 4; ++c) {
      params[r][c] = r < vfx.paramCount ? vfx.params[r][c] : 0.f;
    }
  }
  for (const CPortVfxElem& e : vfx.vsmt) {
    if (e.a < kPortVfxParamTarget || e.a >= kPortVfxParamTarget + 4 * kPortVfxParamCount) {
      continue;
    }
    float v[4] = {};
    if (EvalElem(e, frame, v) > 0) {
      params[(e.a - kPortVfxParamTarget) / 4][(e.a - kPortVfxParamTarget) % 4] = v[0];
    }
  }
}

void PortVfxEvalPoint(const CPortVfxData& vfx, int frame, float& iten, float vpmt[4][4]) {
  iten = EvalReal(vfx.iten, frame, 1.f);
  for (const CPortVfxElem& e : vfx.vpmt) {
    if (e.a >= 4) {
      continue;
    }
    float v[4] = {};
    const int n = EvalElem(e, frame, v);
    for (int i = 0; i < n && e.b + i < 4; ++i) {
      vpmt[e.a][e.b + i] = v[i];
    }
  }
}

void CPortVfxUvXf::Eval(const CPortVfxData& vfx, int frame) {
  for (u32 s = 0; s < 3 && s < vfx.vtmtCount; ++s) {
    CRealElement* const* r = &vfx.vtmt[s * 6];
    const Tm t = EvalTrss(r, frame);
    a[s] = t.a;
    b[s] = t.b;
    c[s] = t.c;
    d[s] = t.d;
    cosE[s] = t.cosE;
    sinE[s] = t.sinE;
    f[s] = t.f;
  }
}

void CPortVfxUvXf::Apply(float qx, float qy, float uv[3][3]) const {
  for (int s = 0; s < 3; ++s) {
    const float dx = (qx - 0.5f) * c[s];
    const float dy = (qy - 0.5f) * d[s];
    uv[s][0] = a[s] + 0.5f + cosE[s] * dx + sinE[s] * dy;
    uv[s][1] = b[s] + 0.5f - sinE[s] * dx + cosE[s] * dy;
    uv[s][2] = f[s];
  }
}

void CElementGen::PortVfxUpdateSystem() {
  xPortVsmtMask = PortVfxEvalVsmt(*x28_loadedGenDesc->xPortVfx, x74_curFrame, xPortVsmt);
}

float PortVfxExposureGain(const CGenDescription& desc, bool model) {
  if (!desc.xPortExposurePolicy || !desc.xPortInverseExposure || !desc.xPortVfx ||
      (!model && (desc.xPortVfx->mat.features & aurora::gfx::vfx::ParticleLit) != 0)) {
    return 1.f;
  }
  return PortRoomEnv::ParticleInverseExposure(
      desc.xPortVfx->mat.blend != uint32_t(aurora::gfx::vfx::Blend::Opaque));
}

bool PortVfxBuildDesc(const CPortVfxData& vfx, const float* vsmt, uint vsmtMask,
                      aurora::gfx::vfx::DrawDesc& desc) {
  const CPortVfxMat& mat = vfx.mat;
  desc.features = mat.features & ~uint32_t(aurora::gfx::vfx::AlreadyExposed);
  desc.alreadyExposed = (mat.features & aurora::gfx::vfx::AlreadyExposed) != 0;
  desc.blend = static_cast< aurora::gfx::vfx::Blend >(mat.blend);
  desc.colorSlot = static_cast< int8_t >(mat.colorSlot);
  desc.opacitySlot = static_cast< int8_t >(mat.opacitySlot);
  desc.rampSlot = static_cast< int8_t >(mat.rampSlot);
  desc.ramp2Slot = static_cast< int8_t >(mat.ramp2Slot);
  desc.thresholdSlot = static_cast< int8_t >(mat.thresholdSlot);
  desc.indirectSlot = static_cast< int8_t >(mat.indirectSlot);
  desc.paletteSlot = static_cast< int8_t >(mat.paletteSlot);
  desc.modulate = mat.modulate;
  desc.depthSoften = mat.depthSoften;
  desc.opacityGain = (vsmtMask & (1u << kPortVfxOpacityGainTarget)) ? vsmt[kPortVfxOpacityGainTarget] : mat.opacityGain;
  desc.opacityBias = (vsmtMask & (1u << kPortVfxOpacityBiasTarget)) ? vsmt[kPortVfxOpacityBiasTarget] : mat.opacityBias;
  desc.rampRow[0] = static_cast< int8_t >(mat.rampRow[0]);
  desc.rampRow[1] = static_cast< int8_t >(mat.rampRow[1]);
  desc.addRow = static_cast< int8_t >(mat.addRow);

  aurora::gfx::vfx::Src* srcs[11] = {&desc.erosion,   &desc.thrX,       &desc.thrY,
                                     &desc.thrW,      &desc.fresnelX,   &desc.fresnelY,
                                     &desc.fadeX,     &desc.fadeY,      &desc.indexScale,
                                     &desc.indexOffset, &desc.indexRow};
  for (int i = 0; i < 11; ++i) {
    srcs[i]->row = static_cast< int8_t >(mat.src[i].row);
    srcs[i]->comp = static_cast< int8_t >(mat.src[i].comp);
    srcs[i]->value = (vsmtMask & (1u << i)) ? vsmt[i] : mat.src[i].value;
  }

  const int texCount = static_cast< int >(std::min< size_t >(mat.tex.size(), 4));
  for (int i = 0; i < texCount; ++i) {
    const CPortVfxMat::Tex& t = mat.tex[i];
    CTexture* tex = t.token.GetObject();
    if (tex == nullptr) {
      return false; // not streamed in yet
    }
    tex->PortLoad(static_cast< GXTexMapID >(GX_TEXMAP0 + i),
                  static_cast< CTexture::EClampMode >(t.wrapS),
                  static_cast< CTexture::EClampMode >(t.wrapT));
    aurora::gfx::vfx::Texture& d = desc.tex[i];
    d.obj = tex->PortTexObj();
    d.uvSet = t.uvSet;
    d.cols = t.cols;
    d.rows = t.rows;
    d.layers = t.layers;
    d.wrapS = static_cast< GXTexWrapMode >(t.wrapS);
    d.wrapT = static_cast< GXTexWrapMode >(t.wrapT);
    d.linear = t.linear != 0;
    d.warped = t.warped != 0;
    for (int k = 0; k < 2; ++k) {
      const int slot = 11 + 2 * i + k;
      d.warpScale[k] = (vsmtMask & (1u << slot)) ? vsmt[slot] : t.warpScale[k];
    }
  }
  return true;
}

namespace {
void PortVfxClusterView(const float* pos, aurora::gfx::vfx::DrawDesc& desc);
} // namespace

// Primitive 2: UpdateRenderData_PlanarSquareModel 0x2ba3f8 serializes the
// SolveParticleTransform matrix, and Render 0x2b23e8 uses the sprite material.
// Unlike a billboard, SIZE is not multiplied into this matrix (0x246f14).
void CElementGen::PortRenderPlanarVfx(const CParticle& particle, int frame) {
  const CPortVfxData& vfx = *x28_loadedGenDesc->xPortVfx;
  aurora::gfx::vfx::DrawDesc desc;
  if (!PortVfxBuildDesc(vfx, xPortVsmt, xPortVsmtMask, desc)) return;
  PortVfxEvalParams(vfx, x74_curFrame, desc.params);
  if (x28_loadedGenDesc->xPortExposurePolicy) desc.alreadyExposed = false;
  const bool lit = (desc.features & aurora::gfx::vfx::ParticleLit) != 0;
  const CTransform4f& model = CGraphics::GetModelMatrix();
  if (lit) {
    const CVector3f pos = model.GetTranslation();
    const float probe[3] = {pos.GetX(), pos.GetY(), pos.GetZ()};
    PortVfxClusterView(x28_loadedGenDesc->xPortBakedLighting ? probe : nullptr, desc);
    if (x28_loadedGenDesc->xPortBakedLighting) PortRoomEnv::BakedLightModulation(desc.lit.modulate);
  }
  CGraphics::SetCullMode(kCM_None);
  CGraphics::SetDepthWriteMode(x26c_28_zTest, kE_LEqual, x26c_26_AAPH ? false : x26c_27_ZBUF);
  CPortVfxUvXf uv;
  uv.Eval(vfx, frame);
  const float gain = lit ? 1.f : particle.xPortIten * PortVfxExposureGain(*x28_loadedGenDesc);
  const float modulation[4] = {x338_moduColor.GetRed(), x338_moduColor.GetGreen(),
                                x338_moduColor.GetBlue(), x338_moduColor.GetAlpha()};
  const CVector3f normal = Normalised(CGraphics::GetViewMatrix().TransposeRotate(
      CVector3f::Cross(model.Rotate(CVector3f(-1.f, 0.f, 0.f)),
                       model.Rotate(CVector3f(0.f, 0.f, 1.f)))), CVector3f::Zero());
  float splatInv[4] = {};
  if ((desc.features & aurora::gfx::vfx::VisorSplat) != 0) {
    const auto viewAxis = [&](const CVector3f& axis) {
      const CVector3f v = CGraphics::GetViewMatrix().TransposeRotate(model.Rotate(axis));
      return CVector3f(v.GetX(), v.GetZ(), -v.GetY());
    };
    const CVector3f a = viewAxis(CVector3f(-1.f, 0.f, 0.f));
    const CVector3f b = viewAxis(CVector3f(0.f, 0.f, 1.f));
    const CVector3f c = CVector3f::Cross(a, b);
    const CVector3f bc = CVector3f::Cross(b, c);
    const CVector3f ca = CVector3f::Cross(c, a);
    const float determinant = CVector3f::Dot(a, bc);
    if (determinant != 0.f) {
      splatInv[0] = bc.GetX() / determinant;
      splatInv[1] = bc.GetY() / determinant;
      splatInv[2] = ca.GetX() / determinant;
      splatInv[3] = ca.GetY() / determinant;
    }
  }
  aurora::gfx::vfx::Vertex vertices[4]{};
  for (int i = 0; i < 4; ++i) {
    auto& vertex = vertices[i];
    const float u = kCorner[i][0], v = kCorner[i][1];
    const CVector3f corner = PortParticleMath::PlanarSquareCorner(u, v);
    vertex.pos[0] = corner.GetX();
    vertex.pos[2] = corner.GetZ();
    uv.Apply(u, v, vertex.uv);
    for (int c = 0; c < 4; ++c) {
      vertex.color[c] = particle.xPortColor[c] * modulation[c] * (c == 3 ? 1.f : gain);
      for (int j = 0; j < 4; ++j) vertex.extra[c][j] = particle.xPortVpmt[c][j];
    }
    if ((desc.features & aurora::gfx::vfx::VisorSplat) != 0) {
      const CVector3f world = model * corner;
      vertex.extra[2][0] = world.GetX();
      vertex.extra[2][1] = world.GetY();
      vertex.extra[2][2] = world.GetZ();
      vertex.extra[2][3] = 0.f;
      for (int j = 0; j < 4; ++j) vertex.extra[3][j] = splatInv[j];
    }
    vertex.vec[0] = normal.GetX();
    vertex.vec[1] = normal.GetZ();
    vertex.vec[2] = normal.GetY();
  }
  aurora::gfx::vfx::draw_quads(desc, vertices, 1);
  ++PortFx::gQuads;
  ++PortFx::gDraws;
}

void CElementGen::PortRenderParticlesVfx() {
  const CPortVfxData& vfx = *x28_loadedGenDesc->xPortVfx;
  const CPortVfxMat& mat = vfx.mat;
  const int particleCount = static_cast< int >(x30_particles.size());
  if (particleCount == 0) {
    return;
  }

  aurora::gfx::vfx::DrawDesc desc;
  if (!PortVfxBuildDesc(vfx, xPortVsmt, xPortVsmtMask, desc)) {
    return; // a texture is not streamed in yet
  }
  if (x28_loadedGenDesc->xPortExposurePolicy) {
    desc.alreadyExposed = false;
  }
  const float inverseExposure = PortVfxExposureGain(*x28_loadedGenDesc);
  // VisorRunOff is a PMDL model particle (the mesh path below), but a sprite generator sharing its
  // material still reads CCH0..4: evaluate them rather than draw from zeroed uniforms. VisorRain is a
  // sprite whose CCH0 is the drop mask's scale (SMTR-animated), so it needs the same rows.
  if ((desc.features & (aurora::gfx::vfx::VisorRunOff | aurora::gfx::vfx::VisorRunOffLit |
                        aurora::gfx::vfx::VisorRain)) != 0) {
    PortVfxEvalParams(vfx, x74_curFrame, desc.params);
  }
  // A VFX_*_Lit shader under LIT_ 1 (ParticleLit): AMBS and the clustered gather, and with BLIT 1 (PBLT)
  // the room's baked probe. CParticleSceneNode::AppendBakedLightingIfRequested (0x2b12a8) appends the probe
  // texture (BPTX) and BLCM only for a BLIT generator; CSceneRenderContext+0x990, the baked-lighting manager
  // it reads, is set every main frame (RenderPrimaryPass). Remastered samples the texture per fragment
  // (20e149b2 084_0..087_0); the port takes the room's grid at the emitter. Without BLIT, the probe-less lit
  // perm (088_0..091_0): the probe terms stay zero.
  const bool lit = (desc.features & aurora::gfx::vfx::ParticleLit) != 0;
  if (lit) {
    const float probePos[3] = {xe8_globalTranslation.GetX(), xe8_globalTranslation.GetY(),
                               xe8_globalTranslation.GetZ()};
    PortVfxClusterView(x28_loadedGenDesc->xPortBakedLighting ? probePos : nullptr, desc);
    // BLCM (c6[7]) comes with the probe: CBakedLightingManager::GetModulationColor, the colour the room's
    // models take too (power-bomb flash, X-ray). Without BLIT it is not appended and stays white.
    if (x28_loadedGenDesc->xPortBakedLighting) {
      PortRoomEnv::BakedLightModulation(desc.lit.modulate);
    }
  }

  // The retail frame: the same model matrix as the non-ORNT path. Particles are placed in a
  // camera-aligned "frame space" (x right, y forward, z up) and the model matrix carries them to
  // the emitter's world position.
  CGraphics::SetCullMode(kCM_None);
  const CTransform4f systemViewCopy(CGraphics::GetViewMatrix()); // the camera transform
  CTransform4f systemModelMatrix(systemViewCopy);
  systemModelMatrix.SetTranslation(CVector3f::Zero());
  const bool followsEmitter = PortFollowsEmitter();
  const CTransform4f systemCameraCopy(systemModelMatrix.GetQuickInverse() *
                                      x22c_globalOrientation);
  const CTransform4f cameraRotationInverse(systemModelMatrix.GetQuickInverse());
  systemModelMatrix = CTransform4f::Translate(xe8_globalTranslation) * x10c_globalScaleTransform *
                      systemModelMatrix * x178_localScaleTransform;
  CGraphics::SetModelMatrix(systemModelMatrix);
  CGraphics::SetDepthWriteMode(x26c_28_zTest, kE_LEqual, x26c_26_AAPH ? false : x26c_27_ZBUF);

  // The camera in frame space, for the oriented quads.
  const CVector3f camFrame = systemModelMatrix.GetInverse() * systemViewCopy.GetTranslation();

  const bool hasModu = x338_moduColor.GetColor_u32() != 0xFFFFFFFF;
  const float modu[4] = {x338_moduColor.GetRed(), x338_moduColor.GetGreen(),
                         x338_moduColor.GetBlue(), x338_moduColor.GetAlpha()};
  const bool hasRota = x28_loadedGenDesc->x3c_ROTA != nullptr;
  const float* pivot = kPivot[std::min< u32 >(mat.spriteCenter, 8)];

  // Retail's SORT: farthest first (view y descending).
  std::vector< CParticleListItem > order;
  order.reserve(particleCount);
  for (int i = 0; i < particleCount; ++i) {
    const CParticle& p = x30_particles[i];
    order.emplace_back(static_cast< short >(i),
                       PortParticleMath::ParticlePositionForSort(
                           cameraRotationInverse, systemCameraCopy, followsEmitter, p.x4_pos,
                           p.x10_prevPos, x80_timeDeltaScale));
  }
  if (x28_loadedGenDesc->x30_28_SORT) {
    std::stable_sort(order.begin(), order.end(),
                     [](const CParticleListItem& a, const CParticleListItem& b) {
                       return a.x4_viewPoint.GetY() > b.x4_viewPoint.GetY();
                     });
  }

  CParticleGlobals::SetEmitterTime(x74_curFrame);
  CParticle* savedParticle = CParticleGlobals::mCurrentParticle;

  std::vector< aurora::gfx::vfx::Vertex > verts;
  verts.reserve(static_cast< size_t >(std::min< uint >(particleCount, kMaxQuadsPerDraw)) * 4);
  auto flush = [&]() {
    if (!verts.empty()) {
      PortFx::gQuads += static_cast< uint32_t >(verts.size() / 4);
      ++PortFx::gDraws;
      aurora::gfx::vfx::draw_quads(desc, verts.data(), static_cast< uint32_t >(verts.size() / 4));
      verts.clear();
    }
  };

  for (const CParticleListItem& item : order) {
    CParticle& p = x30_particles[item.x0_partIdx];
    const CVector3f& vp = item.x4_viewPoint;

    // Retail's UV evaluation context.
    const int partFrame = x74_curFrame - p.x28_startFrame - 1;
    CParticleGlobals::mCurrentParticle = &p;
    CParticleGlobals::xPortIrndParticle = x28_loadedGenDesc->xPortIrnd ? &p : nullptr;
    CPortKeepGlobals::sCurrent = x28_loadedGenDesc->xPortIrnd ? &p.xPortKeep : nullptr;
    CParticleGlobals::SetParticleLifetime(p.x0_endFrame - p.x28_startFrame);
    CParticleGlobals::UpdateParticleLifetimeTweenValues(partFrame);
    // As RenderModels: PAP1..8 read this particle's ADV values (NULL outside the update loop).
    if (x26d_28_enableADV) {
      CParticleGlobals::mParticleAccessParameters = x60_advValues[item.x0_partIdx].values;
    }

    // VTMT, one 6-element row per UV set: uv = (A, B) + 0.5 + R(-E) diag(C, D) (q - 0.5), layer F.
    Tm tm[3];
    for (u32 s = 0; s < 3 && s < vfx.vtmtCount; ++s) {
      CRealElement* const* r = &vfx.vtmt[s * 6];
      tm[s] = EvalTrss(r, partFrame);
    }

    // A lit perm (ParticleLit) reads no ITEN: Remastered's 080_1 passes the colour through without it.
    const float iten = lit ? 1.f : p.xPortIten * inverseExposure;
    float color[4] = {p.xPortColor[0] * iten, p.xPortColor[1] * iten, p.xPortColor[2] * iten, p.xPortColor[3]};
    if (hasModu) {
      for (int i = 0; i < 4; ++i) {
        color[i] *= modu[i];
      }
    }

    const float sx = p.x2c_lineLengthOrSize;
    const float sy = p.xPortSsze;
    // Oriented quads keep their direction: a spin would turn velocity streaks into random lines
    // (Plasma2nd_1's sparks have ROTA -360..360), and retail's ORNT path never rotates either.
    const float theta =
        hasRota && vfx.vorn == 0 ? p.x30_lineWidthOrRota * (3.14159265358979f / 180.f) : 0.f;
    const float cosT = std::cos(theta);
    const float sinT = std::sin(theta);

    // Corner offsets: sprites span the pivot row in the camera plane; vector-oriented quads span
    // width B (perpendicular to the direction and to the view) and length C.
    CVector3f center = vp;
    CVector3f axisU(1.f, 0.f, 0.f), axisW(0.f, 0.f, 1.f);
    // [I] view-space quad normal, depth component in z: toward the eye (camera-local -y), as the
    // vector-oriented normal below is; the fresnel reads only |z|, ParticleLit the direction.
    float vec[3] = {0.f, 0.f, -1.f};
    if (vfx.vorn != 0) {
      CVector3f dir;
      if (vfx.vorn == 1) {
        dir = p.xPortLaunchDir;
      } else {
        dir = Normalised(p.x1c_vel, CVector3f::Zero());
      }
      if (dir.MagSquared() <= kEps) {
        dir = Normalised(p.x4_pos - p.x10_prevPos, CVector3f(0.f, 0.f, 1.f));
      }
      const CVector3f a = PortParticleMath::ParticleDirectionInCamera(systemCameraCopy, dir);
      const CVector3f toCam = camFrame - vp;
      CVector3f b = Normalised(CVector3f::Cross(a, toCam), CVector3f::Zero());
      if (b.MagSquared() <= kEps) {
        b = AnyPerpendicular(a);
      }
      const CVector3f c = Normalised(CVector3f::Cross(toCam, b), CVector3f(0.f, 0.f, 1.f));
      axisU = b;
      axisW = c;
      // Frame -> world -> camera-local; retail's depth axis is y, which goes to z.
      const CVector3f nw = systemModelMatrix.Rotate(CVector3f::Cross(b, c));
      const CVector3f nv = systemViewCopy.TransposeRotate(nw);
      const CVector3f n = Normalised(nv, CVector3f(0.f, 1.f, 0.f));
      vec[0] = n.GetX();
      vec[1] = n.GetZ();
      vec[2] = n.GetY();
    }

    // VisorSplat: rows 0 and 1 of the inverse of the quad's axes in view space (the VS's o5, o6: a view
    // offset in units of the corner extents). A, B are the corner's derivatives; C = A x B stands in for
    // the instance's third axis, which leaves the rows exact while A and B lie in the view plane (sprites).
    float splatInv[4] = {};
    if ((desc.features & aurora::gfx::vfx::VisorSplat) != 0) {
      const auto toView = [&](const CVector3f& f) {
        const CVector3f c = systemViewCopy.TransposeRotate(systemModelMatrix.Rotate(f));
        return CVector3f(c.GetX(), c.GetZ(), -c.GetY()); // retail camera (x right, y forward, z up) -> view
      };
      const CVector3f a = toView((axisU * cosT + axisW * sinT) * sx);
      const CVector3f b = toView((axisW * cosT - axisU * sinT) * sy);
      const CVector3f c = CVector3f::Cross(a, b);
      const CVector3f bc = CVector3f::Cross(b, c);
      const CVector3f ca = CVector3f::Cross(c, a);
      const float det = CVector3f::Dot(a, bc);
      if (std::fabs(det) > 1e-20f) {
        splatInv[0] = bc.GetX() / det;
        splatInv[1] = bc.GetY() / det;
        splatInv[2] = ca.GetX() / det;
        splatInv[3] = ca.GetY() / det;
      }
    }

    const u8 shapeId = x28_loadedGenDesc->xPortSpriteShape;
    const PortSpriteShape* const shape = shapeId >= 3 && shapeId <= 6 ? &kShapes[shapeId - 3] : nullptr;
    const int polyQuads = shape != nullptr ? shape->triangles : 1;
    for (int pq = 0; pq < polyQuads * 4; ++pq) {
      float qx, qy;
      if (shape != nullptr) {
        const uint8_t* tri = shape->idx + (pq / 4) * 3;
        const float* pt = shape->pts[tri[std::min(pq % 4, 2)]];
        qx = pt[0];
        qy = pt[1];
      } else {
        qx = kCorner[pq][0];
        qy = kCorner[pq][1];
      }
      const float ex = pivot[0] + (pivot[2] - pivot[0]) * qx;
      const float ey = pivot[1] + (pivot[3] - pivot[1]) * qy;
      const float ox = ex * sx;
      const float oy = ey * sy;
      const float u = ox * cosT - oy * sinT;
      const float w = ox * sinT + oy * cosT;
      const CVector3f pos = center + axisU * u + axisW * w;

      aurora::gfx::vfx::Vertex v{};
      v.pos[0] = pos.GetX();
      v.pos[1] = pos.GetY();
      v.pos[2] = pos.GetZ();
      for (int s = 0; s < 3; ++s) {
        const Tm& t = tm[s];
        const float dx = (qx - 0.5f) * t.c;
        const float dy = (qy - 0.5f) * t.d;
        v.uv[s][0] = t.a + 0.5f + t.cosE * dx + t.sinE * dy;
        v.uv[s][1] = t.b + 0.5f - t.sinE * dx + t.cosE * dy;
        v.uv[s][2] = t.f;
      }
      for (int i = 0; i < 4; ++i) {
        v.color[i] = color[i];
        for (int j = 0; j < 4; ++j) {
          v.extra[i][j] = p.xPortVpmt[i][j];
        }
      }
      if ((desc.features & aurora::gfx::vfx::VisorSplat) != 0) {
        // The shader reads PMTR rows 0 and 1 only; rows 2 and 3 carry the world position and the axes.
        const CVector3f world = systemModelMatrix * pos;
        v.extra[2][0] = world.GetX();
        v.extra[2][1] = world.GetY();
        v.extra[2][2] = world.GetZ();
        v.extra[2][3] = 0.f;
        for (int j = 0; j < 4; ++j) {
          v.extra[3][j] = splatInv[j];
        }
      }
      v.vec[0] = vec[0];
      v.vec[1] = vec[1];
      v.vec[2] = vec[2];
      verts.push_back(v);
    }
    if (verts.size() >= static_cast< size_t >(kMaxQuadsPerDraw - 8) * 4) {
      flush();
    }
  }
  flush();

  CParticleGlobals::xPortIrndParticle = nullptr;
  CPortKeepGlobals::sCurrent = nullptr;
  CParticleGlobals::mCurrentParticle = savedParticle;
  CParticleGlobals::mParticleAccessParameters = nullptr;
  CGraphics::SetCullMode(kCM_Front);
  CGraphics::SetAlphaCompare(kAF_Always, 0, kAO_And, kAF_Always, 0);
}

// IceChargeBeam's vertices into `world` (Add's scratch): the shader's VS on the CPU. In the
// Remastered mesh's own space (port = orient x Remastered + offset), the position and normal turn by
// the step's hash rotation R and the position moves along the turned normal by amp x CCH2.x x the
// shake of time - 1000 x COLOR.x. The instance matrix is the particle's model times orient; its
// inverse transpose carries the turned normal and the unturned tangent, both unnormalised, and the
// handedness takes the model's mirror. COLOR.z rides in uv[1].x.
static void AddIce(const CPortVfxData& vfx, const CTransform4f& model, const CElementGen::CParticle& p,
                   const Tm (&tm)[3], const float (&color)[4], std::vector< aurora::gfx::vfx::Vertex >& world) {
  const CTransform4f view(CGraphics::GetViewMatrix());
  float params[7][4];
  PortVfxEvalParams(vfx, CParticleGlobals::GetEmitterTime(), params);
  const float time = CGraphics::GetSimTime();
  const IceHash h = IceHashAt(time, params);
  Mat3 orient;
  for (int i = 0; i < 9; ++i) {
    orient.m[i / 3][i % 3] = vfx.meshOrient[i];
  }
  // Remastered = orient^-1 (port - offset); orient^-1 = the inverse transpose, transposed.
  const Mat3 unorient = orient.InverseTranspose().Transposed();
  const CVector3f offset(vfx.meshOffset[0], vfx.meshOffset[1], vfx.meshOffset[2]);
  Mat3 inst;
  const CVector3f cols[3] = {model.Rotate(CVector3f(1.f, 0.f, 0.f)), model.Rotate(CVector3f(0.f, 1.f, 0.f)),
                             model.Rotate(CVector3f(0.f, 0.f, 1.f))};
  Mat3 m3;
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      m3.m[r][c] = r == 0 ? cols[c].GetX() : r == 1 ? cols[c].GetY() : cols[c].GetZ();
    }
  }
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      inst.m[r][c] = m3.m[r][0] * orient.m[0][c] + m3.m[r][1] * orient.m[1][c] + m3.m[r][2] * orient.m[2][c];
    }
  }
  const Mat3 invT = inst.InverseTranspose();
  const float mirror = m3.Det() < 0.f ? -1.f : 1.f;
  const float cch2x = params[2][0];
  for (u32 i = 0; i < vfx.meshVerts; ++i) {
    const float* m = &vfx.meshV[size_t(i) * 16];
    const CVector3f pr = unorient * (CVector3f(m[0], m[1], m[2]) - offset);
    const CVector3f nr = unorient * CVector3f(m[3], m[4], m[5]);
    const CVector3f tr = unorient * CVector3f(m[8], m[9], m[10]);
    const float s = time - 1000.f * m[12];
    const float shake = 2.f * IceSat(20.f * std::sin((s + 8.f) * 50.f)) +
                        0.5f * IceSat(20.f * std::sin((s + 2.f) * 150.f)) + IceSat(20.f * std::sin((s + 4.f) * 100.f));
    const CVector3f n2 = h.r * nr;
    const CVector3f p2 = h.r * pr + n2 * (h.amp * shake * cch2x);
    const CVector3f pos = model * (orient * p2 + offset);
    const CVector3f nl = view.TransposeRotate(invT * n2);
    const CVector3f tl = view.TransposeRotate(invT * tr);
    aurora::gfx::vfx::Vertex& v = world[i];
    v.pos[0] = pos.GetX();
    v.pos[1] = pos.GetY();
    v.pos[2] = pos.GetZ();
    for (int st = 0; st < 3; ++st) {
      const Tm& t = tm[st];
      const float dx = (m[6] - 0.5f) * t.c;
      const float dy = (m[7] - 0.5f) * t.d;
      v.uv[st][0] = t.a + 0.5f + t.cosE * dx + t.sinE * dy;
      v.uv[st][1] = t.b + 0.5f - t.sinE * dx + t.cosE * dy;
      v.uv[st][2] = t.f;
    }
    v.uv[1][0] = m[14];
    for (int k = 0; k < 4; ++k) {
      v.color[k] = color[k];
      for (int j = 0; j < 4; ++j) {
        v.extra[k][j] = p.xPortVpmt[k][j];
      }
    }
    v.vec[0] = nl.GetX();
    v.vec[1] = nl.GetZ();
    v.vec[2] = nl.GetY();
    v.tan[0] = tl.GetX();
    v.tan[1] = tl.GetZ();
    v.tan[2] = tl.GetY();
    v.tan[3] = mirror * m[11];
  }
}

// IceChargeCollision's vertices into `world`: its VS (1c651f97 perm 000_1) on the CPU. No hash or
// shake: the position is the model's; the view normal and tangent are the instance matrix's (model x
// orient) cofactor times the Remastered-space ones, unnormalised (det x the inverse transpose: the FS
// normalises, but a mirrored instance flips both), and the handedness is sign(det inst) x v3.w, as
// ICECHARGE.md has it. det inst = det model x det orient, so the sign is the instance's, not the
// model's. The instance colour is in `color`.
static void AddIceCollision(const CPortVfxData& vfx, const CTransform4f& model, const CElementGen::CParticle& p,
                            const Tm (&tm)[3], const float (&color)[4],
                            std::vector< aurora::gfx::vfx::Vertex >& world) {
  const CTransform4f view(CGraphics::GetViewMatrix());
  Mat3 orient;
  for (int i = 0; i < 9; ++i) {
    orient.m[i / 3][i % 3] = vfx.meshOrient[i];
  }
  const Mat3 unorient = orient.InverseTranspose().Transposed();
  const CVector3f cols[3] = {model.Rotate(CVector3f(1.f, 0.f, 0.f)), model.Rotate(CVector3f(0.f, 1.f, 0.f)),
                             model.Rotate(CVector3f(0.f, 0.f, 1.f))};
  Mat3 m3;
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      m3.m[r][c] = r == 0 ? cols[c].GetX() : r == 1 ? cols[c].GetY() : cols[c].GetZ();
    }
  }
  Mat3 inst;
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      inst.m[r][c] = m3.m[r][0] * orient.m[0][c] + m3.m[r][1] * orient.m[1][c] + m3.m[r][2] * orient.m[2][c];
    }
  }
  const Mat3 cof = inst.Cofactor();
  const float mirror = inst.Det() < 0.f ? -1.f : 1.f;
  for (u32 i = 0; i < vfx.meshVerts; ++i) {
    const float* m = &vfx.meshV[size_t(i) * 16];
    const CVector3f pos = model * CVector3f(m[0], m[1], m[2]);
    const CVector3f nl = view.TransposeRotate(cof * (unorient * CVector3f(m[3], m[4], m[5])));
    const CVector3f tl = view.TransposeRotate(cof * (unorient * CVector3f(m[8], m[9], m[10])));
    aurora::gfx::vfx::Vertex& v = world[i];
    v.pos[0] = pos.GetX();
    v.pos[1] = pos.GetY();
    v.pos[2] = pos.GetZ();
    for (int st = 0; st < 3; ++st) {
      const Tm& t = tm[st];
      const float dx = (m[6] - 0.5f) * t.c;
      const float dy = (m[7] - 0.5f) * t.d;
      v.uv[st][0] = t.a + 0.5f + t.cosE * dx + t.sinE * dy;
      v.uv[st][1] = t.b + 0.5f - t.sinE * dx + t.cosE * dy;
      v.uv[st][2] = t.f;
    }
    for (int k = 0; k < 4; ++k) {
      v.color[k] = color[k];
      for (int j = 0; j < 4; ++j) {
        v.extra[k][j] = p.xPortVpmt[k][j];
      }
    }
    v.vec[0] = nl.GetX();
    v.vec[1] = nl.GetZ();
    v.vec[2] = nl.GetY();
    v.tan[0] = tl.GetX();
    v.tan[1] = tl.GetZ();
    v.tan[2] = tl.GetY();
    v.tan[3] = mirror * m[11];
  }
}

// VFX_BarrierShield's vertices into `world`: its VS (f197ec38's refraction perm, the MFC0 one) folds to
// a plain model-view transform of the mesh — that perm has no vertex displacement — so the position is
// the model's and the view normal is the model's inverse transpose times the mesh normal: a non-uniform
// particle scale must not slide the normal, whose fresnel is a dot of unit vectors in the fragment. The
// mesh's own vertex colour (VMSH v2 words 12..15) rides in Vertex::mesh, apart from the instance colour
// in Vertex::color: the alpha gain and the breakup amplitude read one, only the instance colour scales
// the result. The tangent is unused by this recipe.
static void AddBarrierShield(const CPortVfxData& vfx, const CTransform4f& model, const CElementGen::CParticle& p,
                             const Tm (&tm)[3], const float (&color)[4],
                             std::vector< aurora::gfx::vfx::Vertex >& world) {
  const CTransform4f view(CGraphics::GetViewMatrix());
  const CVector3f cols[3] = {model.Rotate(CVector3f(1.f, 0.f, 0.f)), model.Rotate(CVector3f(0.f, 1.f, 0.f)),
                             model.Rotate(CVector3f(0.f, 0.f, 1.f))};
  Mat3 m3;
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      m3.m[r][c] = r == 0 ? cols[c].GetX() : r == 1 ? cols[c].GetY() : cols[c].GetZ();
    }
  }
  // Cofactors alone reverse normals for negative determinants. Keep the true inverse transpose;
  // degenerate transforms have no defined normal, so feed zero rather than infinities.
  const Mat3 invT = std::fabs(m3.Det()) > 1e-12f ? m3.InverseTranspose() : Mat3{};
  for (u32 i = 0; i < vfx.meshVerts; ++i) {
    const float* m = &vfx.meshV[size_t(i) * 16];
    const CVector3f pos = model * CVector3f(m[0], m[1], m[2]);
    const CVector3f nl = view.TransposeRotate(invT * CVector3f(m[3], m[4], m[5]));
    aurora::gfx::vfx::Vertex& v = world[i];
    v.pos[0] = pos.GetX();
    v.pos[1] = pos.GetY();
    v.pos[2] = pos.GetZ();
    for (int st = 0; st < 3; ++st) {
      const Tm& t = tm[st];
      const float dx = (m[6] - 0.5f) * t.c;
      const float dy = (m[7] - 0.5f) * t.d;
      v.uv[st][0] = t.a + 0.5f + t.cosE * dx + t.sinE * dy;
      v.uv[st][1] = t.b + 0.5f - t.sinE * dx + t.cosE * dy;
      v.uv[st][2] = t.f;
    }
    for (int k = 0; k < 4; ++k) {
      v.color[k] = color[k];
      for (int j = 0; j < 4; ++j) {
        v.extra[k][j] = p.xPortVpmt[k][j];
      }
      v.mesh[k] = m[12 + k];
    }
    // Same convention as the sprites' vec: (x, z, y) of the camera-local normal.
    v.vec[0] = nl.GetX();
    v.vec[1] = nl.GetZ();
    v.vec[2] = nl.GetY();
  }
}

// VFX_BarrierGlow's vertices into `world`: BarrierShield's feed (the mesh's own colour in Vertex::mesh
// apart from the instance colour, the undisplaced normal through the inverse transpose) plus the
// b1eb2b67 VS's displacement, applied in the Remastered model coordinates before the instance
// transform: along the normal by amp = .065 (1 + 3 mc.r) max(sin(20 phase), -.33) with
// phase = time - 10 mc.b (mc is the mesh's own vertex colour: the VS reads it, not the instance's), and
// by .035 max(sin(20 (phase + 114)), -.33) on Remastered +Y. The port's model data is R2G, so that +Y is
// the port's model Z. The normal itself is not displaced, and the displaced position feeds both the
// projected position and the view position. The literal values are the folded VS's (b1eb-vs.txt,
// pool0xe00: -.33 and .065), not a guessed +-1 clamp.
static void AddBarrierGlow(const CPortVfxData& vfx, const CTransform4f& model, const CElementGen::CParticle& p,
                           const Tm (&tm)[3], const float (&color)[4],
                           std::vector< aurora::gfx::vfx::Vertex >& world) {
  AddBarrierShield(vfx, model, p, tm, color, world);
  const float time = CGraphics::GetSimTime();
  for (u32 i = 0; i < vfx.meshVerts; ++i) {
    // m[12..15] is the mesh's own colour (VMSH v2): its r lifts the amplitude, its b shifts the phase.
    const float* m = &vfx.meshV[size_t(i) * vfx.meshStride];
    const float phase = time - 10.f * m[14];
    const float amp = 0.0649999976f * (1.f + 3.f * m[12]) * std::max(std::sin(20.f * phase), -0.33f);
    const float liftY = 0.0350000001f * std::max(std::sin(20.f * (phase + 114.f)), -0.33f);
    const CVector3f pos = model * CVector3f(m[0] + m[3] * amp, m[1] + m[4] * amp, m[2] + m[5] * amp + liftY);
    aurora::gfx::vfx::Vertex& v = world[i];
    v.pos[0] = pos.GetX();
    v.pos[1] = pos.GetY();
    v.pos[2] = pos.GetZ();
  }
}

void CPortVfxMeshBatch::Add(const CPortVfxData& vfx, const CTransform4f& model,
                            const CElementGen::CParticle& p, int partFrame,
                            const CColor& modulate, float inverseExposure) {
  const u32 nv = vfx.meshVerts;
  const bool wide = !vfx.meshIdx32.empty();
  const CTransform4f view(CGraphics::GetViewMatrix());
  if (!probePosSet) {
    const CVector3f origin = model.GetTranslation();
    probePos[0] = origin.GetX();
    probePos[1] = origin.GetY();
    probePos[2] = origin.GetZ();
    probePosSet = true;
  }

  Tm tm[3];
  for (u32 s = 0; s < 3 && s < vfx.vtmtCount; ++s) {
    CRealElement* const* r = &vfx.vtmt[s * 6];
    tm[s] = EvalTrss(r, partFrame);
  }
  // Model instance writers 0x2bfd08/0x2bfe4c multiply ITEN * inverse exposure into RGB,
  // leaving alpha alone. The lit-sprite shader's ITEN exception does not apply to models.
  const float iten = p.xPortIten * inverseExposure;
  const float color[4] = {p.xPortColor[0] * iten * modulate.GetRed(),
                          p.xPortColor[1] * iten * modulate.GetGreen(),
                          p.xPortColor[2] * iten * modulate.GetBlue(),
                          p.xPortColor[3] * modulate.GetAlpha()};

  // The vertices once, in world space; the triangles index them. One scratch buffer serves every
  // particle (the render thread is the only caller), so a frame allocates nothing per particle.
  static std::vector< aurora::gfx::vfx::Vertex > world;
  world.resize(nv);
  const bool collision = (vfx.mat.features & aurora::gfx::vfx::IceChargeCollision) != 0;
  const bool ice = collision || (vfx.mat.features & aurora::gfx::vfx::IceChargeBeam) != 0;
  const bool barrier = (vfx.mat.features & aurora::gfx::vfx::BarrierShield) != 0;
  const bool glow = (vfx.mat.features & aurora::gfx::vfx::BarrierGlow) != 0;
  if (collision) {
    AddIceCollision(vfx, model, p, tm, color, world);
  } else if (ice) {
    AddIce(vfx, model, p, tm, color, world);
  } else if (barrier) {
    AddBarrierShield(vfx, model, p, tm, color, world);
  } else if (glow) {
    AddBarrierGlow(vfx, model, p, tm, color, world);
  }
  // VMSH flag 4: the mesh's colour tints the draw, as the shader's COLOR perm does (2 x c^2.2, alpha c.a).
  const bool tint = vfx.meshStride == 16 && (vfx.meshFlags & 4) != 0;
  for (u32 i = 0; !ice && !barrier && !glow && i < nv; ++i) {
    const float* m = &vfx.meshV[size_t(i) * vfx.meshStride];
    const CVector3f pos = model * CVector3f(m[0], m[1], m[2]);
    const CVector3f nw = Normalised(model.Rotate(CVector3f(m[3], m[4], m[5])), CVector3f(0.f, 0.f, 1.f));
    const CVector3f nv3 = Normalised(view.TransposeRotate(nw), CVector3f(0.f, 1.f, 0.f));
    aurora::gfx::vfx::Vertex& v = world[i];
    v.pos[0] = pos.GetX();
    v.pos[1] = pos.GetY();
    v.pos[2] = pos.GetZ();
    for (int s = 0; s < 3; ++s) {
      const Tm& t = tm[s];
      const float dx = (m[6] - 0.5f) * t.c;
      const float dy = (m[7] - 0.5f) * t.d;
      v.uv[s][0] = t.a + 0.5f + t.cosE * dx + t.sinE * dy;
      v.uv[s][1] = t.b + 0.5f - t.sinE * dx + t.cosE * dy;
      v.uv[s][2] = t.f;
    }
    for (int k = 0; k < 4; ++k) {
      v.color[k] = color[k];
      if (tint) {
        v.color[k] *= k < 3 ? 2.f * std::pow(std::abs(m[12 + k]), 2.2f) : m[15];
      }
      for (int j = 0; j < 4; ++j) {
        v.extra[k][j] = p.xPortVpmt[k][j];
      }
    }
    // Same convention as the sprites' vec: (x, z, y) of the camera-local normal.
    v.vec[0] = nv3.GetX();
    v.vec[1] = nv3.GetZ();
    v.vec[2] = nv3.GetY();
  }
  const size_t count = size_t(vfx.meshTris) * 3;
  for (size_t k = 0; k < count; ++k) {
    verts.push_back(world[wide ? vfx.meshIdx32[k] : vfx.meshIdx16[k]]);
  }
}

namespace {

// The camera Remastered's cluster grid and its cull are built from: GX's view matrix as rows (the same
// rows CStateManager hands the PBR lights) and the projection's half tangents, near and far.
void PortVfxClusterCamera(PortClusteredLights::Camera& camera) {
  const CTransform4f& view = CGraphics::GetViewMatrix();
  const CVector3f eye = view.GetTranslation();
  const float axes[3][3] = {
      {view.Get00(), view.Get10(), view.Get20()},
      {view.Get02(), view.Get12(), view.Get22()},
      {-view.Get01(), -view.Get11(), -view.Get21()},
  };
  float worldToView[3][4];
  for (int r = 0; r < 3; ++r) {
    worldToView[r][0] = axes[r][0];
    worldToView[r][1] = axes[r][1];
    worldToView[r][2] = axes[r][2];
    worldToView[r][3] = -(axes[r][0] * eye.GetX() + axes[r][1] * eye.GetY() + axes[r][2] * eye.GetZ());
  }
  const CGraphics::CProjectionState& proj = CGraphics::GetProjectionState();
  camera = PortClusteredLights::MakeCamera(worldToView, proj.GetLeft(), proj.GetRight(), proj.GetTop(),
                                          proj.GetBottom(), proj.GetNear(), proj.GetFar());
}

// The Lit material's baked ambient: Remastered indexes a BLPD probe record with the instance's BLPI, which
// the port has no equivalent of, so the room's grid at the particle stands in (unexposed, as the bake's
// record is). The record's lobe directions are view space, as the fragment's normal is.
void PortVfxClusterAmbient(const PortClusteredLights::Camera& camera, const float pos[3],
                           aurora::gfx::vfx::LitAmbient& lit) {
  PortRoomEnv::Ambient ambient;
  if (!PortRoomEnv::AmbientAt(pos, ambient)) {
    return;
  }
  for (int c = 0; c < 3; ++c) {
    lit.mean[c] = ambient.mean[c];
    lit.lobe[c] = ambient.lobe[c];
    for (int r = 0; r < 3; ++r) {
      const float* m = camera.worldToView[r];
      lit.dir[c][r] = m[0] * ambient.direction[c][0] + m[1] * ambient.direction[c][1] +
                      m[2] * ambient.direction[c][2];
    }
    lit.dir[c][3] = 1.f + 2.f * ambient.sharpness[c];
  }
}

// The clustered gather for this camera, from the port's room lights. Remastered masks every light scene node
// in the scene (muzzle, beam, ball and explosion lights included) and culls it on the GPU; the port has only
// the room's LightDynamic lights, which is what RoomLights walks. AMBS (the flat ambient c6[0].xyz) is the
// scene's ambient light-node sum (kb topic/ambient-constant.md); the port has no such nodes, so it is the
// fallback SFlatAmbientSourcePOD constant (0x1d0e184), black. The LIT_ 1 waterfall mist (GENP 1d3d2dfc) is
// lit by the baked probe instead: both its generators are BLIT 1.
// Without pos (a sprite without BLIT), no baked probe: desc.lit keeps its zero mean, lobe and directions.
void PortVfxClusterView(const float* pos, aurora::gfx::vfx::DrawDesc& desc) {
  PortClusteredLights::Camera camera;
  PortVfxClusterCamera(camera);
  static std::vector< PortClusteredLights::LightSource > sources;
  sources.clear();
  PortRoomEnv::RoomLights(sources);
  static std::vector< PortClusteredLights::RawLight > lights;
  lights.clear();
  for (const PortClusteredLights::LightSource& source : sources) {
    lights.push_back(PortClusteredLights::MakeLight(source, camera.worldToView));
  }
  const PortClusteredLights::Frame& frame = PortClusteredLights::Update(
      camera, uint32_t(CGraphics::GetViewportWidth()), uint32_t(CGraphics::GetViewportHeight()), lights.data(),
      int(lights.size()));
  desc.cluster.block = frame.header;
  if (!frame.mask.empty() && frame.grid.tilesX > 0 && frame.grid.tilesY > 0) {
    desc.cluster.mask = frame.mask.data();
    desc.cluster.maskWidth = uint32_t(frame.grid.tilesX * frame.grid.tilesY);
    desc.cluster.maskHeight = uint32_t(PortClusteredLights::kSlices);
  }
  if (pos != nullptr) {
    PortVfxClusterAmbient(camera, pos, desc.lit);
  }
  desc.lit.ambient[0] = 0.f;
  desc.lit.ambient[1] = 0.f;
  desc.lit.ambient[2] = 0.f;
}

} // namespace

void CElementGen::PortRenderMeshesVfx(CPortVfxMeshBatch& batch) {
  if (batch.verts.empty()) {
    return;
  }
  aurora::gfx::vfx::DrawDesc desc;
  if (!PortVfxBuildDesc(*x28_loadedGenDesc->xPortVfx, xPortVsmt, xPortVsmtMask, desc)) {
    batch.verts.clear();
    return;
  }
  const CPortVfxData& vfx = *x28_loadedGenDesc->xPortVfx;
  const uint32_t features = vfx.mat.features;
  if (x28_loadedGenDesc->xPortExposurePolicy) {
    desc.alreadyExposed = false;
  }
  if ((features & (aurora::gfx::vfx::IceChargeBeam | aurora::gfx::vfx::IceChargeCollision)) != 0) {
    desc.normalSlot = static_cast< int8_t >(vfx.normalSlot);
    desc.envSlot = static_cast< int8_t >(vfx.envSlot);
    desc.time = CGraphics::GetSimTime();
  } else if ((features & (aurora::gfx::vfx::BarrierShield | aurora::gfx::vfx::BarrierGlow)) != 0) {
    // BarrierShield: the pattern's four scroll reads and the sawtooth motion are driven by the modular
    // simulation time (the shader's c3[0]); the recipe reads no NMAP or env map. BarrierGlow: the same
    // time, for the sawtooth phase and the third pattern read's v.
    desc.time = CGraphics::GetSimTime();
  }
  // VPRM's CCHn rows, with the VSMT overrides, at this frame: IceCharge* and VisorRunOff's 5 rows.
  if ((features & (aurora::gfx::vfx::IceChargeBeam | aurora::gfx::vfx::IceChargeCollision |
                   aurora::gfx::vfx::VisorRunOff | aurora::gfx::vfx::VisorRunOffLit |
                   aurora::gfx::vfx::VisorRain | aurora::gfx::vfx::BarrierShield |
                   aurora::gfx::vfx::BarrierGlow)) != 0) {
    PortVfxEvalParams(vfx, x74_curFrame, desc.params);
  }
  // VFX_IceChargeCollision_Lit's lighting: the clustered gather (block and mask) and the baked probe.
  if ((features & (aurora::gfx::vfx::IceChargeCollisionLit | aurora::gfx::vfx::ParticleLit)) != 0) {
    PortVfxClusterView(batch.probePos, desc);
  }
  // This is a model draw, so a material whose mesh permutation was read against the shader takes its
  // fade from the view-space normal (aurora's OpacityFresnel); every other material keeps the form
  // its VMAT carries. port_vfx_opacity.h has the formula and why it is pinned to one recipe.
  PortVfxSelectModelOpacity(x28_loadedGenDesc->xPortVfx->mat.features, desc);
  // The vertices are in world space already.
  CGraphics::SetModelMatrix(CTransform4f::Identity());
  CGraphics::SetCullMode(kCM_None);
  const aurora::gfx::vfx::DepthSelection depth =
      aurora::gfx::vfx::select_model_depth(desc.blend, x26c_28_zTest, x26c_26_AAPH, x26c_27_ZBUF);
  CGraphics::SetDepthWriteMode(depth.compare, kE_LEqual, depth.write);
  PortFx::gTriangles += static_cast< uint32_t >(batch.verts.size() / 3);
  ++PortFx::gDraws;
  aurora::gfx::vfx::draw_triangles(desc, batch.verts.data(), static_cast< uint32_t >(batch.verts.size() / 3));
  batch.verts.clear();
  batch.probePosSet = false;
}
