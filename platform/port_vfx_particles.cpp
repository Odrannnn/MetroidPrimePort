#include "port_fx_debug.h"
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
    CColor c;
    e.color->GetValue(frame, c);
    out[0] = c.GetRed();
    out[1] = c.GetGreen();
    out[2] = c.GetBlue();
    out[3] = c.GetAlpha();
    return 4;
  }
  }
  return 0;
}

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

uint PortVfxEvalVsmt(const CPortVfxData& vfx, int frame, float vsmt[19]) {
  uint mask = 0;
  for (const CPortVfxElem& e : vfx.vsmt) {
    if (e.a >= 19) {
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
    a[s] = EvalReal(r[0], frame, 0.f);
    b[s] = EvalReal(r[1], frame, 0.f);
    c[s] = EvalReal(r[2], frame, 1.f);
    d[s] = EvalReal(r[3], frame, 1.f);
    const float e = EvalReal(r[4], frame, 0.f);
    cosE[s] = std::cos(e);
    sinE[s] = std::sin(e);
    f[s] = EvalReal(r[5], frame, 0.f);
  }
}

void CPortVfxUvXf::Apply(float qx, float qy, float uv[3][3]) const {
  for (int s = 0; s < 3; ++s) {
    const float dx = (qx - 0.5f) * c[s];
    const float dy = (qy - 0.5f) * d[s];
    uv[s][0] = a[s] + 0.5f + cosE[s] * dx - sinE[s] * dy;
    uv[s][1] = b[s] + 0.5f + sinE[s] * dx + cosE[s] * dy;
    uv[s][2] = f[s];
  }
}

void CElementGen::PortVfxUpdateSystem() {
  xPortVsmtMask = PortVfxEvalVsmt(*x28_loadedGenDesc->xPortVfx, x74_curFrame, xPortVsmt);
}

bool PortVfxBuildDesc(const CPortVfxData& vfx, const float* vsmt, uint vsmtMask,
                      aurora::gfx::vfx::DrawDesc& desc) {
  const CPortVfxMat& mat = vfx.mat;
  desc.features = mat.features;
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

  // The retail frame: the same model matrix as the non-ORNT path. Particles are placed in a
  // camera-aligned "frame space" (x right, y forward, z up) and the model matrix carries them to
  // the emitter's world position.
  CGraphics::SetCullMode(kCM_None);
  const CTransform4f systemViewCopy(CGraphics::GetViewMatrix()); // the camera transform
  CTransform4f systemModelMatrix(systemViewCopy);
  systemModelMatrix.SetTranslation(CVector3f::Zero());
  const CTransform4f systemCameraCopy(systemModelMatrix.GetQuickInverse() *
                                      x22c_globalOrientation);
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
    const CVector3f pos = (p.x4_pos - p.x10_prevPos) * x80_timeDeltaScale + p.x10_prevPos;
    order.emplace_back(static_cast< short >(i), systemCameraCopy * pos);
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
    CParticleGlobals::SetParticleLifetime(p.x0_endFrame - p.x28_startFrame);
    CParticleGlobals::UpdateParticleLifetimeTweenValues(partFrame);
    // As RenderModels: PAP1..8 read this particle's ADV values (NULL outside the update loop).
    if (x26d_28_enableADV) {
      CParticleGlobals::mParticleAccessParameters = x60_advValues[item.x0_partIdx].values;
    }

    // VTMT, one 6-element row per UV set: uv = (A, B) + 0.5 + R(E) diag(C, D) (q - 0.5), layer F.
    Tm tm[3];
    for (u32 s = 0; s < 3 && s < vfx.vtmtCount; ++s) {
      CRealElement* const* r = &vfx.vtmt[s * 6];
      tm[s].a = EvalReal(r[0], partFrame, 0.f);
      tm[s].b = EvalReal(r[1], partFrame, 0.f);
      tm[s].c = EvalReal(r[2], partFrame, 1.f);
      tm[s].d = EvalReal(r[3], partFrame, 1.f);
      const float e = EvalReal(r[4], partFrame, 0.f);
      tm[s].cosE = std::cos(e);
      tm[s].sinE = std::sin(e);
      tm[s].f = EvalReal(r[5], partFrame, 0.f);
    }

    float color[4] = {p.xPortColor[0] * p.xPortIten, p.xPortColor[1] * p.xPortIten,
                      p.xPortColor[2] * p.xPortIten, p.xPortColor[3]};
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
    float vec[3] = {0.f, 0.f, 1.f}; // [I] view-space quad normal, depth component in z
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
      const CVector3f a = systemCameraCopy.Rotate(dir);
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

    for (int k = 0; k < 4; ++k) {
      const float qx = kCorner[k][0];
      const float qy = kCorner[k][1];
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
        v.uv[s][0] = t.a + 0.5f + t.cosE * dx - t.sinE * dy;
        v.uv[s][1] = t.b + 0.5f + t.sinE * dx + t.cosE * dy;
        v.uv[s][2] = t.f;
      }
      for (int i = 0; i < 4; ++i) {
        v.color[i] = color[i];
        for (int j = 0; j < 4; ++j) {
          v.extra[i][j] = p.xPortVpmt[i][j];
        }
      }
      v.vec[0] = vec[0];
      v.vec[1] = vec[1];
      v.vec[2] = vec[2];
      verts.push_back(v);
    }
    if (verts.size() >= static_cast< size_t >(kMaxQuadsPerDraw) * 4) {
      flush();
    }
  }
  flush();

  CParticleGlobals::xPortIrndParticle = nullptr;
  CParticleGlobals::mCurrentParticle = savedParticle;
  CParticleGlobals::mParticleAccessParameters = nullptr;
  CGraphics::SetCullMode(kCM_Front);
  CGraphics::SetAlphaCompare(kAF_Always, 0, kAO_And, kAF_Always, 0);
}

void CPortVfxMeshBatch::Add(const CPortVfxData& vfx, const CTransform4f& model,
                            const CElementGen::CParticle& p, int partFrame,
                            const CColor& modulate) {
  const u32 nv = vfx.meshVerts;
  const bool wide = !vfx.meshIdx32.empty();
  const CTransform4f view(CGraphics::GetViewMatrix());

  Tm tm[3];
  for (u32 s = 0; s < 3 && s < vfx.vtmtCount; ++s) {
    CRealElement* const* r = &vfx.vtmt[s * 6];
    tm[s].a = EvalReal(r[0], partFrame, 0.f);
    tm[s].b = EvalReal(r[1], partFrame, 0.f);
    tm[s].c = EvalReal(r[2], partFrame, 1.f);
    tm[s].d = EvalReal(r[3], partFrame, 1.f);
    const float e = EvalReal(r[4], partFrame, 0.f);
    tm[s].cosE = std::cos(e);
    tm[s].sinE = std::sin(e);
    tm[s].f = EvalReal(r[5], partFrame, 0.f);
  }
  const float color[4] = {p.xPortColor[0] * p.xPortIten * modulate.GetRed(),
                          p.xPortColor[1] * p.xPortIten * modulate.GetGreen(),
                          p.xPortColor[2] * p.xPortIten * modulate.GetBlue(),
                          p.xPortColor[3] * modulate.GetAlpha()};

  // The vertices once, in world space; the triangles index them. One scratch buffer serves every
  // particle (the render thread is the only caller), so a frame allocates nothing per particle.
  static std::vector< aurora::gfx::vfx::Vertex > world;
  world.resize(nv);
  for (u32 i = 0; i < nv; ++i) {
    const float* m = &vfx.meshV[size_t(i) * 8];
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
      v.uv[s][0] = t.a + 0.5f + t.cosE * dx - t.sinE * dy;
      v.uv[s][1] = t.b + 0.5f + t.sinE * dx + t.cosE * dy;
      v.uv[s][2] = t.f;
    }
    for (int k = 0; k < 4; ++k) {
      v.color[k] = color[k];
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

void CElementGen::PortRenderMeshesVfx(CPortVfxMeshBatch& batch) {
  if (batch.verts.empty()) {
    return;
  }
  aurora::gfx::vfx::DrawDesc desc;
  if (!PortVfxBuildDesc(*x28_loadedGenDesc->xPortVfx, xPortVsmt, xPortVsmtMask, desc)) {
    batch.verts.clear();
    return;
  }
  // The vertices are in world space already.
  CGraphics::SetModelMatrix(CTransform4f::Identity());
  CGraphics::SetCullMode(kCM_None);
  CGraphics::SetDepthWriteMode(x26c_28_zTest, kE_LEqual, x26c_26_AAPH ? false : x26c_27_ZBUF);
  PortFx::gTriangles += static_cast< uint32_t >(batch.verts.size() / 3);
  ++PortFx::gDraws;
  aurora::gfx::vfx::draw_triangles(desc, batch.verts.data(), static_cast< uint32_t >(batch.verts.size() / 3));
  batch.verts.clear();
}
