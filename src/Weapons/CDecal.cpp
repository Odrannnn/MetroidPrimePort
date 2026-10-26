#include "Weapons/CDecal.hpp"

#include "Kyoto/Graphics/CGX.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Graphics/CModel.hpp"
#include "Kyoto/Graphics/CModelFlags.hpp"
#include "Kyoto/Graphics/CTexture.hpp"
#include "Kyoto/Math/CRelAngle.hpp"
#include "Kyoto/Particles/CParticleGlobals.hpp"

#include "dolphin/gx/GXTev.h"
#include "dolphin/gx/GXVert.h"

#ifdef TARGET_PC
#include "aurora/vfx.hpp"
#include "port_fx_debug.h"
#include "port_projected_decal.h"
#include "port_vfx_particles.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#endif

CRandom16 CDecal::sDecalRandom(99);
bool CDecal::sMoveRedToAlphaBuffer = false;

void CDecal::SetGlobalSeed(ushort seed) { sDecalRandom.SetSeed(seed); }

CDecal::CDecal(const TToken< CDecalDescription >& desc, const CTransform4f& xf)
: x0_description(desc)
, xc_transform(xf)
, x54_modelLifetime(0)
, x58_frameIdx(0)
, x5c_flags(0)
, x60_rotation(CVector3f::Zero())
, x6c_elapsedTime(0.f)
#ifdef TARGET_PC
, xPortRemastered(false)
, xPortBaseTransform(xf)
, xPortTransform(xf)
, xPortWidth(1.f)
, xPortHeight(1.f)
, xPortBoxDepth(0.5f)
, xPortLifetime(0)
, xPortModelLifetime(0)
, xPortFrame(0)
, xPortTime(0.0)
, xPortAngle(0.f)
, xPortFlags(0)
#endif
{
  CGlobalRandom gr(sDecalRandom);

  InitQuad(x3c_quad1, x0_description->x0_quad1, 1);
  InitQuad(x48_quad2, x0_description->x1c_quad2, 2);

  if (x0_description->x38_DMDL) {
    if (!x0_description->x48_DLFT.null()) {
      x0_description->x48_DLFT->GetValue(0, x54_modelLifetime);
    } else {
      x54_modelLifetime = 0x7FFFFF;
    }

    if (!x0_description->x50_DMRT.null()) {
      x0_description->x50_DMRT->GetValue(0, x60_rotation);
    }
  } else {
    x5c_flags |= 4;
  }
}

#ifdef TARGET_PC
namespace {
// Remastered's degrees-to-radians factor in CDecal (0xb7ca04 / 0xb7ce44).
const float kPortDegToRad = 0.017453292f;

// Remastered evaluates a decal's elements in a CVarContext on the decal's own random; its IRND
// draws PortFloat28 at frame 0 and writes nothing at later frames.
class CPortDecalEval {
public:
  explicit CPortDecalEval(CRandom16& random) : mRandom(random), mPrev(CParticleGlobals::xPortIrnd28) {
    CParticleGlobals::xPortIrnd28 = true;
  }
  ~CPortDecalEval() { CParticleGlobals::xPortIrnd28 = mPrev; }

private:
  CGlobalRandom mRandom;
  bool mPrev;
};
// Explode 0x10121cc builds the decal's transform with Remastered's LookAt (forward is local Z, the
// hit normal) where retail's has forward on Y: same normal, same position, Remastered's basis.
CTransform4f PortRemasteredBase(const CTransform4f& xf) {
  const CVector3f y = xf.GetColumn(kDY);
  const PortProjectedDecal::Basis b = PortProjectedDecal::LookAt({y.GetX(), y.GetY(), y.GetZ()});
  const CVector3f t = xf.GetTranslation();
  return CTransform4f(b.x.x, b.y.x, b.z.x, t.GetX(), b.x.y, b.y.y, b.z.y, t.GetY(), b.x.z, b.y.z, b.z.z,
                      t.GetZ());
}
} // namespace

CDecal::CDecal(const TToken< CDecalDescription >& desc, uint seed, const CTransform4f& xf)
: x0_description(desc)
, xc_transform(xf)
, x54_modelLifetime(0)
, x58_frameIdx(0)
, x5c_flags(0)
, x60_rotation(CVector3f::Zero())
, x6c_elapsedTime(0.f)
, xPortRemastered(true)
, xPortRandom(seed)
, xPortBaseTransform(PortRemasteredBase(xf))
, xPortTransform(xPortBaseTransform)
// Default extents (1, 1) (static init 0xb7df60 -> 0x459abf0) and the box height 0.5 (0x1d0e0b0).
, xPortWidth(1.f)
, xPortHeight(1.f)
, xPortBoxDepth(0.5f)
, xPortLifetime(0)
, xPortModelLifetime(0)
, xPortFrame(0)
, xPortTime(0.0)
, xPortAngle(0.f)
, xPortFlags(0) {
  // Outside the HDR scene a Remastered decal draws retail's quads (the HDR scene draws its
  // clipped mesh, PortRenderProjected); they keep their own lifetimes and random, as retail's.
  {
    CGlobalRandom gr(sDecalRandom);
    InitQuad(x3c_quad1, x0_description->x0_quad1, 1);
    InitQuad(x48_quad2, x0_description->x1c_quad2, 2);
  }
  // No Remastered decal has a model (the importer rejects DMDL), so the model bit is preset.
  x5c_flags |= 4;

  CPortDecalEval eval(xPortRandom);
  const CDecalDescription& d = *x0_description.operator->();
  if (CIntElement* mstl = d.xPortMSTL.get()) {
    mstl->GetValue(0, xPortLifetime);
  }
  CParticleGlobals::SetEmitterTime(0);
  CParticleGlobals::SetParticleLifetime(xPortLifetime);
  CParticleGlobals::UpdateParticleLifetimeTweenValues(0);
  if (CRealElement* mstr = d.xPortMSTR.get()) {
    float angle = 0.f;
    mstr->GetValue(0, angle);
    xPortAngle = angle * kPortDegToRad;
    xPortTransform = xPortBaseTransform * CTransform4f::RotateZ(CRelAngle::FromRadians(xPortAngle));
  }
  if (CRealElement* mstw = d.xPortMSTW.get()) {
    mstw->GetValue(0, xPortWidth);
  }
  if (CRealElement* msth = d.xPortMSTH.get()) {
    msth->GetValue(0, xPortHeight);
  } else {
    xPortHeight = xPortWidth;
  }
  // No model: model life stays 0 and bit 0 is preset.
  xPortFlags |= 1;
}

// CDecal::Update 0xb7ce44: dt in 1/256-frame ticks, stepped one whole frame at a time.
void CDecal::PortRemasteredUpdate(float dt) {
  double frames = 0.00390625 * static_cast< double >(static_cast< long long >(15360.0 * dt + 0.5));
  if (!(0.0 < frames)) {
    return;
  }
  double counted = static_cast< double >(xPortFrame);
  double time = xPortTime;
  do {
    const double step = frames > 1.0 ? 1.0 : frames;
    if (1.0 <= (time - counted) + step) {
      const int frame = xPortFrame;
      if (frame < xPortModelLifetime) {
        if (xPortLifetime <= frame) {
          xPortFlags |= 2;
        }
      } else {
        xPortFlags |= 1;
        if (frame >= xPortLifetime) {
          xPortFlags |= 2;
        }
      }
      xPortFrame = frame + 1;
      if (xPortFlags == 3) {
        return;
      }
      CPortDecalEval eval(xPortRandom);
      CParticleGlobals::SetEmitterTime(xPortFrame);
      CParticleGlobals::SetParticleLifetime(xPortLifetime);
      CParticleGlobals::UpdateParticleLifetimeTweenValues(xPortFrame);
      if (CRealElement* mstr = x0_description->xPortMSTR.get()) {
        // An IRND rotation writes nothing past frame 0, so the transform loses its rotation here
        // while xPortAngle keeps the frame-0 angle (as Remastered does).
        float angle = 0.f;
        mstr->GetValue(xPortFrame, angle);
        xPortTransform =
            xPortBaseTransform * CTransform4f::RotateZ(CRelAngle::FromRadians(angle * kPortDegToRad));
      }
      counted += 1.0;
      time = xPortTime;
    }
    frames -= step;
    time += step;
    xPortTime = time;
  } while (0.0 < frames);
}

// UpdateMaterialData 0xb7d07c, run per frame by UpdateDecalRenderable 0x10106c4 in a CVarContext
// on the decal's random (lifetime MSTL, frame +0x90): BuildUniformMaterialData seeds the MATI's
// parameters and an override alpha of 1, the DUL sets the alpha, and only without a DUL or with
// an alpha above 0 do FillInUniformMaterialData (SMTR) and the TMTR run.
void CDecal::PortUpdateMaterialData(int slot, SPortSlotData& out) const {
  const CDecalDescription::SPortMaterial& material = x0_description->xPortMaterial[slot];
  std::memcpy(out.xParams, material.xDefaults, sizeof(out.xParams));
  out.xAlpha = 1.f;
  // The TRSS default (0x20e9d0): no offset, unit scale, no rotation, layer 0.
  static const float kIdentityTrss[6] = {0.f, 0.f, 1.f, 1.f, 0.f, 0.f};
  std::memcpy(out.xTrss, kIdentityTrss, sizeof(out.xTrss));
  CPortDecalEval eval(const_cast< CRandom16& >(xPortRandom));
  CParticleGlobals::SetEmitterTime(xPortFrame);
  CParticleGlobals::SetParticleLifetime(xPortLifetime);
  CParticleGlobals::UpdateParticleLifetimeTweenValues(xPortFrame);
  CRealElement* dul = x0_description->xPortDUL[slot].get();
  if (dul != nullptr) {
    dul->GetValue(xPortFrame, out.xAlpha);
    if (!(out.xAlpha > 0.f)) {
      return;
    }
  }
  // A colour fills its whole row (unclamped), a real its one component.
  for (size_t i = 0; i < material.xSmtr.size(); ++i) {
    const CDecalDescription::SPortParam& param = material.xSmtr[i];
    float* row = out.xParams[param.xRow];
    if (param.xColor) {
      param.xColor->GetValueHdr(xPortFrame, row);
    } else {
      param.xReal->GetValue(xPortFrame, row[param.xComp]);
    }
  }
  for (int i = 0; i < 6; ++i) {
    if (material.xTmtr[i]) {
      material.xTmtr[i]->GetValue(xPortFrame, out.xTrss[i]);
    }
  }
}

void CDecal::PortRenderProjected(const PortProjectedDecal::Mesh& mesh, int pass) const {
  const uint32_t triCount = static_cast< uint32_t >(mesh.Triangles());
  if (triCount == 0) {
    return;
  }
  // SDecalSystemParams c4[4..5]: the box plane through the centre along the normalized OBB column 2,
  // and 5 / ext.z; the VS's fade is sat(1 - |dot(n, p) - d| * 5 / ext.z), per vertex.
  const CVector3f normal = xPortTransform.GetColumn(kDZ).AsNormalized();
  const float planeD = CVector3f::Dot(normal, xPortTransform.GetTranslation());
  const float fadeScale = 5.f / xPortBoxDepth;
  static std::vector< aurora::gfx::vfx::Vertex > sVerts;
  for (int slot = 0; slot < 2; ++slot) {
    const CDecalDescription::SPortMaterial& material = x0_description->xPortMaterial[slot];
    if (!material.xPresent) {
      continue;
    }
    // Pass 0 takes the materials whose blend is not 1 or 6, pass 1 the additive 1/6 ones.
    const bool additive = material.xBlend == 1 || material.xBlend == 6;
    if ((pass != 1) == additive) {
      continue;
    }
    aurora::gfx::vfx::DrawDesc desc;
    switch (material.xShader) {
    case 0xe37ab789u:
      desc.decal = aurora::gfx::vfx::Decal::ColorErosion;
      break;
    case 0x05ae8d81u:
      desc.decal = aurora::gfx::vfx::Decal::Ice;
      break;
    case 0xa19fec01u:
      desc.decal = aurora::gfx::vfx::Decal::Scorch;
      break;
    default:
      continue;
    }
    desc.features = aurora::gfx::vfx::Unfogged;
    // The decal MATIs carry blend 2 (SrcAlpha/InvSrcAlpha) or 10 (dual source); the importer
    // passes no other.
    switch (material.xBlend) {
    case 2:
      desc.blend = aurora::gfx::vfx::Blend::Alpha;
      break;
    case 10:
      desc.blend = aurora::gfx::vfx::Blend::DualSource;
      break;
    default:
      continue;
    }
    SPortSlotData data;
    PortUpdateMaterialData(slot, data);
    if (!(data.xAlpha > 0.f)) {
      continue;
    }
    if (!material.xTextureToken) {
      continue;
    }
    TCachedToken< CTexture >& token = const_cast< TCachedToken< CTexture >& >(*material.xTextureToken);
    if (!token.TryCache()) {
      continue; // the MATI's texture is not loaded yet
    }
    CTexture* tex = token.GetObject();
    tex->PortLoad(GX_TEXMAP0, static_cast< CTexture::EClampMode >(material.xWrapS),
                  static_cast< CTexture::EClampMode >(material.xWrapT));
    aurora::gfx::vfx::Texture& t = desc.tex[0];
    t.obj = tex->PortTexObj();
    t.wrapS = static_cast< GXTexWrapMode >(material.xWrapS);
    t.wrapT = static_cast< GXTexWrapMode >(material.xWrapT);
    t.linear = material.xLinear != 0;
    desc.colorSlot = 0;
    std::memcpy(desc.params, data.xParams, sizeof(data.xParams));
    // MATI cull 0xff -> 2: back faces.
    desc.cullBack = true;

    // The VS's TMTR (c4[1] as pack_texture_transform leaves it) on the box UV.
    const float offU = PortVfxTrssOffset(data.xTrss[0]);
    const float offV = PortVfxTrssOffset(data.xTrss[1]);
    const float scaleU = data.xTrss[2];
    const float scaleV = data.xTrss[3];
    const float angle = PortVfxTrssAngle(data.xTrss[4]);
    const float cosE = std::cos(angle);
    const float sinE = std::sin(angle);
    // c4[0] = ScaleRGB(info+0xec, alpha), info+0xec white.
    const float a = data.xAlpha;
    sVerts.resize(mesh.vertices.size());
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
      const PortProjectedDecal::Vertex& src = mesh.vertices[i];
      aurora::gfx::vfx::Vertex& v = sVerts[i];
      v = aurora::gfx::vfx::Vertex{};
      v.pos[0] = src.pos.x;
      v.pos[1] = src.pos.y;
      v.pos[2] = src.pos.z;
      const float u = src.u - 0.5f;
      const float w = src.v - 0.5f;
      v.uv[0][0] = cosE * scaleU * u + sinE * scaleV * w + offU + 0.5f;
      v.uv[0][1] = -sinE * scaleU * u + cosE * scaleV * w + offV + 0.5f;
      v.color[0] = a;
      v.color[1] = a;
      v.color[2] = a;
      v.color[3] = 1.f;
      const float dist = normal.GetX() * src.pos.x + normal.GetY() * src.pos.y +
                         normal.GetZ() * src.pos.z - planeD;
      const float fade = 1.f - std::fabs(dist) * fadeScale;
      v.extra[0][0] = fade < 0.f ? 0.f : fade > 1.f ? 1.f : fade;
    }
    CGraphics::SetModelMatrix(CTransform4f::Identity());
    CGraphics::SetCullMode(kCM_None);
    // Depth test on (compare 4, LEqual), no write.
    CGraphics::SetDepthWriteMode(true, kE_LEqual, false);
    PortFx::gTriangles += triCount;
    ++PortFx::gDraws;
    aurora::gfx::vfx::draw_triangles(desc, sVerts.data(), triCount);
  }
}
#endif

void CDecal::RenderQuad(CQuadDecal& decal, const CDecalDescription::SQuadDescr& desc) const {
  CColor color = CColor::White();
  float size = 1.f;
  CVector3f offset = CVector3f::Zero();
  if (CColorElement* clr = desc.x10_CLR.get()) {
    clr->GetValue(x58_frameIdx, color);
  }
  if (CRealElement* sze = desc.x4_SZE.get()) {
    sze->GetValue(x58_frameIdx, size);
    size *= 0.5f;
  }
  if (CRealElement* rot = desc.x8_ROT.get()) {
    rot->GetValue(x58_frameIdx, decal.x8_rotation);
  }
  if (CVectorElement* off = desc.xc_OFF.get()) {
    off->GetValue(x58_frameIdx, offset);
    offset.SetY(0.f);
  }

  CTransform4f modXf = xc_transform;
  modXf.AddTranslation(offset);
  CGraphics::SetModelMatrix(modXf);
  CGraphics::SetAlphaCompare(kAF_Always, 0, kAO_And, kAF_Always, 0);

  bool redToAlpha = CDecal::sMoveRedToAlphaBuffer && desc.x18_ADD && !desc.x14_TEX.null();
  if (desc.x18_ADD) {
    CGraphics::SetDepthWriteMode(true, kE_LEqual, false);
    if (redToAlpha) {
      CGraphics::SetBlendMode(kBM_Blend, kBF_One, kBF_One, kLO_Clear);
    } else {
      CGraphics::SetBlendMode(kBM_Blend, kBF_SrcAlpha, kBF_One, kLO_Clear);
    }
  } else {
    CGraphics::SetDepthWriteMode(true, kE_LEqual, false);
    CGraphics::SetBlendMode(kBM_Blend, kBF_SrcAlpha, kBF_InvSrcAlpha, kLO_Clear);
  }

  SUVElementSet uvSet;
  uvSet.xMin = 0.f;
  uvSet.xMax = 1.f;
  uvSet.yMin = 0.f;
  uvSet.yMax = 1.f;
  if (!desc.x14_TEX.null()) {
    TToken< CTexture > tex = desc.x14_TEX->GetValueTexture(x58_frameIdx);
    if (!tex.IsLoaded()) {
      return;
    }
    tex->Load(GX_TEXMAP0, CTexture::kCM_Repeat);
    tex.GetObj(); // ?
    CGraphics::SetTevOp(kTS_Stage0, CGraphics::kEnvModulate);
    desc.x14_TEX->GetValueUV(x58_frameIdx, uvSet);
    if (redToAlpha) {
      CGX::SetNumTevStages(2);
      CGX::SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_APREV, GX_CC_ZERO);
      CGX::SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_TEXA, GX_CA_APREV, GX_CA_ZERO);
      CGX::SetStandardTevColorAlphaOp(GX_TEVSTAGE1);
      CGX::SetAlphaCompare(GX_GREATER, 0, GX_AOP_OR, GX_NEVER, 0);
      GXSetTevSwapMode(GX_TEVSTAGE1, GX_TEV_SWAP0, GX_TEV_SWAP1);
    } else {
      CGraphics::SetTevOp(kTS_Stage1, CGraphics::kEnvPassthru);
    }
  } else {
    CGraphics::SetTevOp(kTS_Stage0, CGraphics::kEnvPassthru);
    CGraphics::SetTevOp(kTS_Stage1, CGraphics::kEnvPassthru);
  }

  CGX::SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
  CGX::SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
  CGX::SetNumTexGens(1);
  CGX::SetNumChans(1);
  CGX::SetNumIndStages(0);
  CGX::SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, false, GX_PTIDENTITY);
  CGX::SetChanCtrl(CGX::Channel0, false, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE,
                   GX_AF_NONE);
  static const GXVtxDescList vtxDesc[4] = {
      {GX_VA_POS, GX_DIRECT},
      {GX_VA_CLR0, GX_DIRECT},
      {GX_VA_TEX0, GX_DIRECT},
      {GX_VA_NULL, GX_NONE},
  };
  CGX::SetVtxDescv(vtxDesc);
  CGX::Begin(GX_TRIANGLESTRIP, GX_VTXFMT0, 4);

  if (decal.x8_rotation == 0.f) {
    // Vertex 0
    GXPosition3f32(-size, 0.001f, size);
    GXColor1u32(color.GetColor_u32());
    GXTexCoord2f32(uvSet.xMin, uvSet.yMax);
    // Vertex 1
    GXPosition3f32(size, 0.001f, size);
    GXColor1u32(color.GetColor_u32());
    GXTexCoord2f32(uvSet.xMax, uvSet.yMax);
    // Vertex 2
    GXPosition3f32(-size, 0.001f, -size);
    GXColor1u32(color.GetColor_u32());
    GXTexCoord2f32(uvSet.xMin, uvSet.yMin);
    // Vertex 3
    GXPosition3f32(size, 0.001f, -size);
    GXColor1u32(color.GetColor_u32());
    GXTexCoord2f32(uvSet.xMax, uvSet.yMin);
  } else {
    const CRelAngle ang = CRelAngle::FromDegrees(decal.x8_rotation);
    const float sinSize = sine(ang) * size;
    const float cosSize = cosine(ang) * size;
    // Vertex 0
    GXPosition3f32(sinSize - cosSize, 0.001f, cosSize + sinSize);
    GXColor1u32(color.GetColor_u32());
    GXTexCoord2f32(uvSet.xMin, uvSet.yMax);
    // Vertex 1
    GXPosition3f32(sinSize + cosSize, 0.001f, cosSize - sinSize);
    GXColor1u32(color.GetColor_u32());
    GXTexCoord2f32(uvSet.xMax, uvSet.yMax);
    // Vertex 2
    GXPosition3f32(-(sinSize + cosSize), 0.001f, -(cosSize - sinSize));
    GXColor1u32(color.GetColor_u32());
    GXTexCoord2f32(uvSet.xMin, uvSet.yMin);
    // Vertex 3
    GXPosition3f32(-sinSize + cosSize, 0.001f, -cosSize - sinSize);
    GXColor1u32(color.GetColor_u32());
    GXTexCoord2f32(uvSet.xMax, uvSet.yMin);
  }

  CGX::End();
  if (redToAlpha) {
    GXSetTevSwapMode(GX_TEVSTAGE1, GX_TEV_SWAP0, GX_TEV_SWAP0);
    CGX::SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
  }
}

void CDecal::RenderMdl() const {
  CColor color = CColor::White();
  CVector3f offset = CVector3f::Zero();
  CTransform4f rotXf(CTransform4f::Identity());
  if (!x0_description->x5c_25_DMOO) {
    rotXf = xc_transform.GetRotation();
  }

  bool dmrtIsConst = false;
  if (CVectorElement* off = x0_description->x50_DMRT.get()) {
    if (off->IsFastConstant()) {
      dmrtIsConst = true;
    }
  }

  CTransform4f dmrtXf = CTransform4f::Identity();
  if (dmrtIsConst) {
    x0_description->x50_DMRT->GetValue(x58_frameIdx, x60_rotation);
    dmrtXf = CTransform4f::RotateZ(CRelAngle::FromDegrees(x60_rotation.GetZ()));
    dmrtXf.RotateLocalY(CRelAngle::FromDegrees(x60_rotation.GetY()));
    dmrtXf.RotateLocalX(CRelAngle::FromDegrees(x60_rotation.GetX()));
  }
  dmrtXf = rotXf * dmrtXf;

  if (CVectorElement* off = x0_description->x4c_DMOP.get()) {
    off->GetValue(x58_frameIdx, offset);
  }

  CTransform4f worldXf = CTransform4f::Translate(xc_transform.GetTranslation() + rotXf * offset);
  if (dmrtIsConst) {
    worldXf *= dmrtXf;
  } else if (CVectorElement* dmrt = x0_description->x50_DMRT.get()) {
    CVector3f rotation(0.f, 0.f, 0.f);
    dmrt->GetValue(x58_frameIdx, rotation);
    dmrtXf = CTransform4f::RotateZ(CRelAngle::FromDegrees(rotation.GetZ()));
    dmrtXf.RotateLocalY(CRelAngle::FromDegrees(rotation.GetY()));
    dmrtXf.RotateLocalX(CRelAngle::FromDegrees(rotation.GetX()));
    worldXf *= rotXf * dmrtXf;
  } else {
    worldXf *= dmrtXf;
  }

  if (CVectorElement* dmsc = x0_description->x54_DMSC.get()) {
    CVector3f scale(0.f, 0.f, 0.f);
    dmsc->GetValue(x58_frameIdx, scale);
    worldXf *= CTransform4f::Scale(scale.GetX(), scale.GetY(), scale.GetZ());
  }

  if (CColorElement* dmcl = x0_description->x58_DMCL.get()) {
    dmcl->GetValue(x58_frameIdx, color);
  }

  CGraphics::SetModelMatrix(worldXf);

  if (x0_description->x5c_24_DMAB) {
    const CModelFlags flags = CModelFlags::Additive(color).DepthCompareUpdate(true, false);
    (*x0_description->x38_DMDL)->Draw(flags);
  } else if (color.GetAlpha() == 1.f) {
    (*x0_description->x38_DMDL)->Draw(CModelFlags::Normal());
  } else {
    (*x0_description->x38_DMDL)
        ->Draw(CModelFlags::AlphaBlendedDepthCompareUpdate(color, true, false));
  }

  CGraphics::SetCullMode(kCM_Front);
  CTevCombiners::ResetStates();
}

void CDecal::Render() const {
  CGlobalRandom gr(sDecalRandom);
  if (IsDone()) {
    return;
  }
  // A Remastered decal reaches this only outside the HDR scene (CDecalManager::AddToRenderer): it
  // draws retail's flat quads at its Remastered frame.

  CGraphics::DisableAllLights();
  CParticleGlobals::SetEmitterTime(x58_frameIdx);

  ProcessQuad(const_cast< CQuadDecal& >(x3c_quad1), x0_description->x0_quad1, 1);
  ProcessQuad(const_cast< CQuadDecal& >(x48_quad2), x0_description->x1c_quad2, 2);
  if (x0_description->x38_DMDL && (x5c_flags & 4) == 0) {
    CParticleGlobals::SetParticleLifetime(x54_modelLifetime);
    CParticleGlobals::UpdateParticleLifetimeTweenValues(x58_frameIdx);
    RenderMdl();
  }
}

void CDecal::Update(float dt) {
#ifdef TARGET_PC
  if (xPortRemastered) {
    PortRemasteredUpdate(dt);
    // Drives the retail quads (ProcessQuad) a Remastered decal draws outside the HDR scene.
    x58_frameIdx = xPortFrame;
    if (x58_frameIdx >= x3c_quad1.GetLifetime()) {
      x5c_flags |= 1;
    }
    if (x58_frameIdx >= x48_quad2.GetLifetime()) {
      x5c_flags |= 2;
    }
    return;
  }
#endif
  // Age by real time and map onto the 60 Hz frame units the tables use.
  x6c_elapsedTime += dt;
  x58_frameIdx = static_cast< int >(x6c_elapsedTime * 60.f);

  if (x58_frameIdx >= x3c_quad1.GetLifetime()) {
    x5c_flags |= 1;
  }

  if (x58_frameIdx >= x48_quad2.GetLifetime()) {
    x5c_flags |= 2;
  }

  if (x58_frameIdx >= x54_modelLifetime) {
    x5c_flags |= 4;
  }
}
