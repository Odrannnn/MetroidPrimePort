#include "MetroidPrime/CModelData.hpp"

#include "Kyoto/Animation/CSkinnedModel.hpp"
#include "Kyoto/Animation/CVertexMorphEffect.hpp"
#include "Kyoto/CResFactory.hpp"
#include "Kyoto/Graphics/CCubeMaterial.hpp"
#include "Kyoto/Graphics/CCubeModel.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Graphics/CModel.hpp"
#include "Kyoto/Graphics/CModelFlags.hpp"
#include "Kyoto/Math/CFrustumPlanes.hpp"
#include "MetaRender/CCubeRenderer.hpp"
#include "MetroidPrime/CActorLights.hpp"
#include "MetroidPrime/CAnimRes.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Factories/CCharacterFactory.hpp"
#include "MetroidPrime/Factories/CCharacterFactoryBuilder.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#ifdef TARGET_PC
#include "port_retail_placement.h"
#endif

struct SThermalDrawContext {
  SThermalDrawContext(CSkinnedModel& model, const CColor& mulColor, const CColor& addColor,
                      const CModelFlags& flags)
  : model(model), mulColor(mulColor), addColor(addColor), flags(flags) {}
  CSkinnedModel& model;
  CColor mulColor;
  CColor addColor;
  const CModelFlags& flags;
};
struct SOneTextureDrawContext {
  SOneTextureDrawContext(CSkinnedModel& model, const CTexture& texture, const CColor& color,
                         float t)
  : model(model), texture(texture), color(color), t(t) {}
  CSkinnedModel& model;
  const CTexture& texture;
  CColor color;
  float t;
};
struct SFlatDrawContext {
  SFlatDrawContext(CSkinnedModel& model, bool unsortedOnly, const CModelFlags& flags)
  : model(model), unsortedOnly(unsortedOnly), flags(flags) {}
  CSkinnedModel& model;
  bool unsortedOnly;
  const CModelFlags& flags;
};
struct SMultiLightingDrawContext {
  SMultiLightingDrawContext(const CModel& model, const CActorLights* lights,
                            const CColor& alphaColor, const CColor& additiveColor)
  : model(model), lights(lights), alphaColor(alphaColor), additiveColor(additiveColor) {}
  const CModel& model;
  const CActorLights* lights;
  const CColor& alphaColor;
  const CColor& additiveColor;
};
struct SMultipassDrawContext {
  SMultipassDrawContext(CSkinnedModel& model, const CModelFlags* flags, int count)
  : model(model), flags(flags), count(count) {}
  CSkinnedModel& model;
  const CModelFlags* flags;
  int count;
};

static const CAdvancementDeltas skNullAdvance(CVector3f::Zero(), CQuaternion::NoRotation());

CModelData::CModelData(const CStaticRes& res)
: x0_scale(res.GetScale())
, x14_24_renderSorted(false)
, x14_25_sortThermal(false)
, x18_ambientColor(CColor::White())
, x1c_normalModel(gpSimplePool->GetObj(SObjectTag('CMDL', res.GetId()))) {}

CModelData::CModelData()
: x0_scale(1.f, 1.f, 1.f)
, x14_24_renderSorted(false)
, x14_25_sortThermal(false)
, x18_ambientColor(CColor::White()) {}

CModelData::CModelData(const CAnimRes& res)
: x0_scale(res.GetScale())
, x14_24_renderSorted(false)
, x14_25_sortThermal(false)
, x18_ambientColor(CColor::White()) {
  TLockedToken< CCharacterFactory > factory = gpCharacterFactoryBuilder->GetFactory(res);
  xc_animData =
      factory
          ->CreateCharacter(res.GetCharacterNodeId(), res.CanLoop(), factory, res.GetDefaultAnim())
          .release();
}

CModelData::~CModelData() {}

void CModelData::Render(EWhichModel which, const CTransform4f& xf, const CActorLights* lights,
                        const CModelFlags& flags) const {
#ifdef TARGET_PC
  // Remastered's CModelDataMP1::Render (0xfa6c0c): model 3 is always a thermal command with
  // SModelThermalParams::DefaultOverride (a = 1, b = 0), a = the flags' alpha for a sort-thermal
  // model; no sort-thermal test gates it. HDR scene only; without HDR it stays retail.
  if (which == kWM_ThermalHot && CCubeMaterial::sPortSceneHdr) {
    const float a = x14_25_sortThermal ? flags.GetColorRef().GetAlpha() : 1.f;
    bool outerOn;
    float outerX, outerY;
    CCubeModel::PortGetThermalOverride(outerOn, outerX, outerY);
    CCubeModel::PortSetThermalOverride(a, 0.f);
    RenderThermal(xf, CColor(a, a, a, a), CColor(uchar(0), uchar(0), uchar(0), uchar(0x40)), flags);
    CCubeModel::PortRestoreThermalOverride(outerOn, outerX, outerY);
    return;
  }
#endif
  if (x14_25_sortThermal && which == kWM_ThermalHot) {
    const float alpha = flags.GetColorRef().GetAlpha();
    const CColor mulColor(alpha, alpha, alpha, alpha);
    RenderThermal(xf, mulColor, CColor(uchar(0), uchar(0), uchar(0), uchar(0x40)), flags);
    return;
  }
  CTransform4f scaledXf = xf;
  scaledXf *= CTransform4f::Scale(x0_scale.GetX(), x0_scale.GetY(), x0_scale.GetZ());
#ifdef TARGET_PC
  scaledXf = PortScaledXf(which, xf);
#endif
  gpRender->SetModelMatrix(scaledXf);
  if (lights != nullptr) {
    lights->ActivateLights();
  } else {
    CGraphics::DisableAllLights();
    gpRender->SetAmbientColor(x18_ambientColor);
  }
  if (HasAnimation()) {
#ifdef TARGET_PC
    const CSkinnedModel* frozen =
        sPortDrawFrozen && (which == kWM_Normal || which == kWM_XRay || which == kWM_Thermal)
            ? PortFrozenModel()
            : nullptr;
    if (frozen != nullptr) {
      xc_animData->Render(*frozen, flags, rstl::optional_object< CVertexMorphEffect >(), nullptr);
    } else
#endif
    xc_animData->Render(PickAnimatedModel(which), flags,
                        rstl::optional_object< CVertexMorphEffect >(), nullptr);
  } else if (x1c_normalModel) {
    const CModel& model = **PickStaticModel(which);
    if (x14_24_renderSorted)
      model.DrawSortedParts(flags);
    else
      model.Draw(flags);
  }
  gpRender->SetAmbientColor(CColor::White());
  CGraphics::DisableAllLights();
  x14_24_renderSorted = false;
}

#ifdef TARGET_PC
bool CModelData::sPortDrawFrozen = false;

const CSkinnedModel* CModelData::PortFrozenModel() const {
  if (!HasAnimation()) {
    return nullptr;
  }
  const rstl::optional_object< TLockedToken< CSkinnedModelWithAvgNormals > >& ice =
      xc_animData->GetIceModel();
  if (!ice.valid()) {
    return nullptr;
  }
  const CSkinnedModel& model = (**ice)->GetSkinnedModel();
  const CCubeModel* cube = model.GetModel()->GetCubeModel();
  if (cube == nullptr || cube->PortMaterialCount() == 0) {
    return nullptr;
  }
  float values[19];
  return cube->PortReadPBRMaterial(0, values) > 0 ? &model : nullptr;
}
#endif

void CModelData::RenderUnsortedParts(EWhichModel which, const CTransform4f& xf,
                                     const CActorLights* lights, const CModelFlags& flags) const {
  if ((x14_25_sortThermal && which == kWM_ThermalHot) || HasAnimation() || !x1c_normalModel ||
      flags.GetTrans() > CModelFlags::kT_Four) {
    x14_24_renderSorted = false;
    return;
  }
  CTransform4f scaledXf =
      xf * CTransform4f::Scale(x0_scale.GetX(), x0_scale.GetY(), x0_scale.GetZ());
#ifdef TARGET_PC
  scaledXf = PortScaledXf(which, xf);
  // Remastered's RenderUnsortedParts (0xfa71c4) queues a non-sort-thermal model 3 as a normal
  // unsorted node; in the hot pass (RenderUnsorted(8)) only its bit-19 surfaces draw, through
  // the override at the world's global constants. HDR scene only.
  if (which == kWM_ThermalHot && CCubeMaterial::sPortSceneHdr) {
    gpRender->SetModelMatrix(scaledXf);
    CGraphics::DisableAllLights();
    const CModel& model = **PickStaticModel(which);
    model.UpdateLastFrame();
    if (const CCubeModel* cube = model.GetCubeModel()) {
      cube->PortDrawThermalHotNode(flags, kSS_Unsorted);
    }
    x14_24_renderSorted = true;
    return;
  }
#endif
  gpRender->SetModelMatrix(scaledXf);
  if (lights != nullptr) {
    lights->ActivateLights();
  } else {
    CGraphics::DisableAllLights();
    gpRender->SetAmbientColor(x18_ambientColor);
  }
  PickStaticModel(which)->DrawUnsortedParts(flags);
  gpRender->SetAmbientColor(CColor::White());
  CGraphics::DisableAllLights();
  x14_24_renderSorted = true;
}

void CModelData::ThermalDrawCallback(const float* positions, const float* normals,
                                     const SThermalDrawContext* ctx) {
  gpRender->DrawThermalModel(**ctx->model.GetModel(), ctx->mulColor, ctx->addColor, positions,
                             normals, ctx->flags);
}

void CModelData::DisintegrateDrawCallback(const float* positions, const float* normals,
                                          const SOneTextureDrawContext* ctx) {
  gpRender->DrawModelDisintegrate(**ctx->model.GetModel(), ctx->texture, ctx->color, positions,
                                  normals, ctx->t);
}

void CModelData::FlatDrawCallback(const float* positions, const float* normals,
                                  const SFlatDrawContext* ctx) {
  gpRender->DrawModelFlat(**ctx->model.GetModel(), ctx->flags, ctx->unsortedOnly, positions,
                          normals);
}

void CModelData::MultiLightingDrawCallback(const float* positions, const float* normals,
                                           SMultiLightingDrawContext* ctx) {
  CGraphics::DisableAllLights();
  CModelFlags depthFlags = CModelFlags::AlphaBlended(0.f);
  CModelFlags alphaFlags =
      CModelFlags::AlphaBlended(ctx->alphaColor).DepthCompareUpdate(true, false);
  CModelFlags additiveFlags =
      CModelFlags::Additive(ctx->additiveColor).DepthCompareUpdate(true, false);
  if (positions != nullptr) {
    ctx->model.Draw(positions, normals, depthFlags);
    if (ctx->lights != nullptr)
      ctx->lights->ActivateLights();
    ctx->model.Draw(positions, normals, alphaFlags);
    ctx->model.Draw(positions, normals, additiveFlags);
  } else {
    ctx->model.Draw(depthFlags);
    if (ctx->lights != nullptr)
      ctx->lights->ActivateLights();
    ctx->model.Draw(alphaFlags);
    ctx->model.Draw(additiveFlags);
  }
}

void CModelData::MultipassDrawCallback(const float* positions, const float* normals,
                                       const SMultipassDrawContext* ctx) {
  const CModel& model = **ctx->model.GetModel();
  for (int i = 0; i < ctx->count; ++i)
    model.Draw(positions, normals, ctx->flags[i]);
}

void CModelData::ThermalDraw(CSkinnedModel& model, const CColor& mulColor, const CColor& addColor,
                             const CModelFlags& flags) {
  SThermalDrawContext ctx(model, mulColor, addColor, flags);
  model.Draw(reinterpret_cast< TDrawFunc >(ThermalDrawCallback), &ctx);
}

void CModelData::ThermalDraw(CSkinnedModel& model, const float* positions, const float* normals,
                             const CColor& mulColor, const CColor& addColor,
                             const CModelFlags& flags) {
  gpRender->DrawThermalModel(**model.GetModel(), mulColor, addColor, positions, normals, flags);
}

void CModelData::RenderThermal(const CTransform4f& xf, const CColor& mulColor,
                               const CColor& addColor, const CModelFlags& flags) const {
  CTransform4f scaledXf = xf * CTransform4f::Scale(x0_scale);
#ifdef TARGET_PC
  scaledXf = PortScaledXf(kWM_ThermalHot, xf);
#endif
  gpRender->SetModelMatrix(scaledXf);
  CGraphics::DisableAllLights();
  if (HasAnimation()) {
    CSkinnedModel& model = PickAnimatedModel(kWM_ThermalHot);
    xc_animData->SetupRender(model, rstl::optional_object< CVertexMorphEffect >(), nullptr);
    ThermalDraw(model, mulColor, addColor, flags);
  } else {
    gpRender->DrawThermalModel(**PickStaticModel(kWM_ThermalHot), mulColor, addColor, nullptr,
                               nullptr, flags);
  }
}

void CModelData::DisintegrateDraw(EWhichModel which, const CTransform4f& xf,
                                  const CTexture& texture, const CColor& color, float t) const {
  CTransform4f scaledXf = xf * CTransform4f::Scale(x0_scale);
#ifdef TARGET_PC
  scaledXf = PortScaledXf(which, xf);
#endif
  gpRender->SetModelMatrix(scaledXf);
  CGraphics::DisableAllLights();
  CAABox bounds = GetBounds(scaledXf);
  if (HasAnimation()) {
    CSkinnedModel& model = PickAnimatedModel(which);
    xc_animData->SetupRender(model, rstl::optional_object< CVertexMorphEffect >(), nullptr);
    SOneTextureDrawContext ctx(model, texture, color, t);
    model.Draw(reinterpret_cast< TDrawFunc >(DisintegrateDrawCallback), &ctx);
  } else {
    gpRender->DrawModelDisintegrate(**PickStaticModel(which), texture, color, nullptr, nullptr, t);
  }
}

void CModelData::DisintegrateDraw(const CStateManager& mgr, const CTransform4f& xf,
                                  const CTexture& texture, const CColor& color, float t) const {
  DisintegrateDraw(GetRenderingModel(mgr), xf, texture, color, t);
}

void CModelData::FlatDraw(EWhichModel which, const CTransform4f& xf, bool unsortedOnly,
                          const CModelFlags& flags) const {
  CTransform4f scaledXf = xf * CTransform4f::Scale(x0_scale);
#ifdef TARGET_PC
  scaledXf = PortScaledXf(which, xf);
#endif
  gpRender->SetModelMatrix(scaledXf);
  CGraphics::DisableAllLights();
  if (HasAnimation()) {
    CSkinnedModel& model = PickAnimatedModel(which);
    xc_animData->SetupRender(model, rstl::optional_object< CVertexMorphEffect >(), nullptr);
    SFlatDrawContext ctx(model, unsortedOnly, flags);
    model.Draw(reinterpret_cast< TDrawFunc >(FlatDrawCallback), &ctx);
  } else {
    gpRender->DrawModelFlat(**PickStaticModel(which), flags, unsortedOnly, nullptr, nullptr);
  }
}

void CModelData::MultiLightingDraw(EWhichModel which, const CTransform4f& xf,
                                   const CActorLights* lights, const CColor& mulColor,
                                   const CColor& addColor) const {
  CTransform4f scaledXf = xf * CTransform4f::Scale(x0_scale);
#ifdef TARGET_PC
  scaledXf = PortScaledXf(which, xf);
#endif
  gpRender->SetModelMatrix(scaledXf);
  if (HasAnimation()) {
    CSkinnedModel& model = PickAnimatedModel(which);
    GetAnimationData()->SetupRender(model, rstl::optional_object< CVertexMorphEffect >(), nullptr);
    SMultiLightingDrawContext ctx(**model.GetModel(), lights, mulColor, addColor);
    model.Draw(reinterpret_cast< TDrawFunc >(MultiLightingDrawCallback), &ctx);
  } else {
    SMultiLightingDrawContext ctx(**PickStaticModel(which), lights, mulColor, addColor);
    MultiLightingDrawCallback(nullptr, nullptr, &ctx);
  }
}

void CModelData::MultipassDraw(EWhichModel which, const CTransform4f& xf,
                               const CActorLights* lights, const CModelFlags* flags,
                               int count) const {
  CTransform4f scaledXf = xf * CTransform4f::Scale(x0_scale);
#ifdef TARGET_PC
  scaledXf = PortScaledXf(which, xf);
#endif
  gpRender->SetModelMatrix(scaledXf);
  if (lights != nullptr) {
    lights->ActivateLights();
  } else {
    CGraphics::DisableAllLights();
    gpRender->SetAmbientColor(x18_ambientColor);
  }
  if (HasAnimation()) {
    CSkinnedModel& model = PickAnimatedModel(which);
    GetAnimationData()->SetupRender(model, rstl::optional_object< CVertexMorphEffect >(), nullptr);
    SMultipassDrawContext ctx(model, flags, count);
    model.Draw(reinterpret_cast< TDrawFunc >(MultipassDrawCallback), &ctx);
  } else {
    const CModel& model = **PickStaticModel(which);
    for (int i = 0; i < count; ++i)
      model.Draw(flags[i]);
  }
}

void CModelData::Touch(const CStateManager& mgr, int shaderIdx) const {
  Touch(GetRenderingModel(mgr), shaderIdx);
}

void CModelData::Touch(EWhichModel which, int shaderIdx) const {
  if (HasAnimation())
    xc_animData->Touch(PickAnimatedModel(which), shaderIdx);
  else
    PickStaticModel(which)->Touch(shaderIdx);
}

void CModelData::RenderParticles(const CFrustumPlanes& planes) const {
  if (HasAnimation())
    xc_animData->RenderAuxiliary(planes);
}

bool CModelData::IsInFrustum(const CTransform4f& xf, const CFrustumPlanes& planes) const {
  if (IsNull())
    return true;
  return planes.BoxInFrustumPlanes(GetBounds(xf));
}

bool CModelData::IsAnimating() const { return HasAnimation() && xc_animData->IsAnimating(); }

CAdvancementDeltas CModelData::AdvanceAnimation(float dt, CStateManager& mgr, TAreaId aid,
                                                bool advTree) {
  if (!HasAnimation())
    return skNullAdvance;
  return xc_animData->Advance(dt, ScaleCopy(), mgr, aid, advTree);
}

CAdvancementDeltas CModelData::AdvanceAnimationIgnoreParticles(float dt, CRandom16& rand,
                                                               bool advTree) {
  if (!HasAnimation())
    return skNullAdvance;
  return xc_animData->AdvanceIgnoreParticles(dt, rand, advTree);
}

CTransform4f CModelData::GetLocatorTransform(const rstl::string& name) const {
  if (!HasAnimation())
    return CTransform4f::Identity();
  return xc_animData->GetLocatorTransform(name, nullptr);
}

CTransform4f CModelData::GetLocatorTransformDynamic(const rstl::string& name,
                                                    const CCharAnimTime* time) const {
  if (!HasAnimation())
    return CTransform4f::Identity();
  return xc_animData->GetLocatorTransform(name, time);
}

CTransform4f CModelData::GetScaledLocatorTransform(const rstl::string& name) const {
  CTransform4f xf = GetLocatorTransform(name);
  const CVector3f pos = xf.GetTranslation();
  xf.SetTranslation(CVector3f(x0_scale.GetX() * pos.GetX(), x0_scale.GetY() * pos.GetY(),
                              x0_scale.GetZ() * pos.GetZ()));
  return xf;
}

CTransform4f CModelData::GetScaledLocatorTransformDynamic(const rstl::string& name,
                                                          const CCharAnimTime* time) const {
  CTransform4f xf = GetLocatorTransformDynamic(name, time);
  const CVector3f pos = xf.GetTranslation();
  xf.SetTranslation(CVector3f(x0_scale.GetX() * pos.GetX(), x0_scale.GetY() * pos.GetY(),
                              x0_scale.GetZ() * pos.GetZ()));
  return xf;
}

CAABox CModelData::GetBounds(const CTransform4f& xf) const {
  CTransform4f scaledXf =
      xf * CTransform4f::Scale(x0_scale.GetX(), x0_scale.GetY(), x0_scale.GetZ());
  if (HasAnimation())
    return xc_animData->GetBoundingBox(scaledXf);
#ifdef TARGET_PC
  if (xPortCorrected != 0)
    return PortLocalBounds().GetTransformedAABox(scaledXf);
#endif
  if (!x2c_xrayModel && !x3c_infraModel)
    return (*x1c_normalModel)->GetBoundingBox().GetTransformedAABox(scaledXf);
  CAABox bounds = (*x1c_normalModel)->GetBoundingBox();
  if (x3c_infraModel) {
    bounds.Include((*x3c_infraModel)->GetBoundingBox());
  }
  if (x2c_xrayModel) {
    bounds.Include((*x2c_xrayModel)->GetBoundingBox());
  }
  return bounds.GetTransformedAABox(scaledXf);
}

CAABox CModelData::GetBounds() const {
  if (HasAnimation())
    return xc_animData->GetBoundingBox(
        CTransform4f::Scale(x0_scale.GetX(), x0_scale.GetY(), x0_scale.GetZ()));
#ifdef TARGET_PC
  if (xPortCorrected != 0)
    return PortLocalBounds().GetTransformedAABox(
        CTransform4f::Scale(x0_scale.GetX(), x0_scale.GetY(), x0_scale.GetZ()));
#endif
  if (!x2c_xrayModel && !x3c_infraModel) {
    const CAABox& bounds = (*x1c_normalModel)->GetBoundingBox();
    return CAABox(CVector3f(bounds.GetMinPoint().GetX() * x0_scale.GetX(),
                            bounds.GetMinPoint().GetY() * x0_scale.GetY(),
                            bounds.GetMinPoint().GetZ() * x0_scale.GetZ()),
                  CVector3f(bounds.GetMaxPoint().GetX() * x0_scale.GetX(),
                            bounds.GetMaxPoint().GetY() * x0_scale.GetY(),
                            bounds.GetMaxPoint().GetZ() * x0_scale.GetZ()));
  }
  CAABox bounds = (*x1c_normalModel)->GetBoundingBox();
  if (x3c_infraModel) {
    bounds.Include((*x3c_infraModel)->GetBoundingBox());
  }
  if (x2c_xrayModel) {
    bounds.Include((*x2c_xrayModel)->GetBoundingBox());
  }
  return CAABox(CVector3f(bounds.GetMinPoint().GetX() * x0_scale.GetX(),
                          bounds.GetMinPoint().GetY() * x0_scale.GetY(),
                          bounds.GetMinPoint().GetZ() * x0_scale.GetZ()),
                CVector3f(bounds.GetMaxPoint().GetX() * x0_scale.GetX(),
                          bounds.GetMaxPoint().GetY() * x0_scale.GetY(),
                          bounds.GetMaxPoint().GetZ() * x0_scale.GetZ()));
}

void CModelData::AdvanceParticles(const CTransform4f& xf, float dt, CStateManager& mgr) {
  if (HasAnimation())
    xc_animData->AdvanceParticles(xf, dt, x0_scale, mgr);
}

void CModelData::EnableLooping(bool enable) {
  if (!HasAnimation())
    return;
  xc_animData->EnableLooping(enable);
}

float CModelData::GetAnimationDuration(int anim) const {
  if (xc_animData.null())
    return 0.f;
  return xc_animData->GetAnimationDuration(anim);
}

bool CModelData::GetIsLoop() const {
  if (xc_animData.null())
    return false;
  return xc_animData->GetIsLoop();
}

bool CModelData::IsDefinitelyOpaque(EWhichModel which) const {
  if (HasAnimation())
    return PickAnimatedModel(which).GetModel()->IsDefinitelyOpaque();
  if (x1c_normalModel)
    return PickStaticModel(which)->IsDefinitelyOpaque();
  return false;
}

void CModelData::SetInfraModel(const rstl::pair< CAssetId, CAssetId >& assets) {
  if (assets.first != 0) {
    if (HasAnimation() && assets.second != 0) {
      xc_animData->SetInfraModel(
          TToken< CModel >(gpSimplePool->GetObj(SObjectTag('CMDL', assets.first))),
          TToken< CSkinRules >(gpSimplePool->GetObj(SObjectTag('CSKR', assets.second))));
    } else {
      x3c_infraModel =
          TLockedToken< CModel >(gpSimplePool->GetObj(SObjectTag('CMDL', assets.first)));
    }
  }
}

void CModelData::SetXRayModel(const rstl::pair< CAssetId, CAssetId >& assets) {
  if (assets.first == 0 || gpResourceFactory->GetResourceTypeById(assets.first) != 'CMDL')
    return;
  if (HasAnimation() && assets.second != 0 &&
      gpResourceFactory->GetResourceTypeById(assets.second) == 'CSKR') {
    xc_animData->SetXRayModel(
        TToken< CModel >(gpSimplePool->GetObj(SObjectTag('CMDL', assets.first))),
        TToken< CSkinRules >(gpSimplePool->GetObj(SObjectTag('CSKR', assets.second))));
  } else {
    x2c_xrayModel = TLockedToken< CModel >(gpSimplePool->GetObj(SObjectTag('CMDL', assets.first)));
  }
}

const TLockedToken< CModel >& CModelData::PickStaticModel(EWhichModel which) const {
  switch (which) {
  case kWM_Thermal:
  case kWM_ThermalHot:
    if (x3c_infraModel)
      return *x3c_infraModel;
    break;
  case kWM_XRay:
    if (x2c_xrayModel)
      return *x2c_xrayModel;
    break;
  default:
    break;
  }
  return *x1c_normalModel;
}

CSkinnedModel& CModelData::PickAnimatedModel(EWhichModel which) const {
  CSkinnedModel* model = nullptr;
  switch (which) {
  case kWM_Thermal:
  case kWM_ThermalHot:
    model = xc_animData->GetInfraModel();
    break;
  case kWM_XRay:
    model = xc_animData->GetXRayModel();
    break;
  default:
    break;
  }
  if (model != nullptr)
    return *model;
  return **xc_animData->GetModelData();
}

CModelData::EWhichModel CModelData::GetRenderingModel(const CStateManager& mgr) {
  switch (mgr.GetPlayerState()->GetActiveVisor(mgr)) {
  case CPlayerState::kPV_XRay:
    return kWM_XRay;
  case CPlayerState::kPV_Thermal:
    if (mgr.GetThermalDrawFlag() == kTD_Cold)
      return kWM_Thermal;
    return kWM_ThermalHot;
  default:
    return kWM_Normal;
  }
}

bool CModelData::HasModel(EWhichModel which) const {
  if (HasAnimation()) {
    switch (which) {
    case kWM_Normal:
      return true;
    case kWM_XRay:
      return xc_animData->GetXRayModel() != nullptr;
    case kWM_Thermal:
    case kWM_ThermalHot:
      return xc_animData->GetInfraModel() != nullptr;
    default:
      return false;
    }
  }
  switch (which) {
  case kWM_Normal:
    return x1c_normalModel;
  case kWM_XRay:
    return x2c_xrayModel;
  case kWM_Thermal:
  case kWM_ThermalHot:
    return x3c_infraModel;
  default:
    return false;
  }
}

void CModelData::Render(const CStateManager& mgr, const CTransform4f& xf,
                        const CActorLights* lights, const CModelFlags& flags) const {
  Render(GetRenderingModel(mgr), xf, lights, flags);
}

bool CModelData::IsLoaded(int shaderIdx) const {
  if (HasAnimation()) {
    const CAnimData& animData = *xc_animData;
    if (!animData.GetModelData()->GetModel()->IsLoaded(shaderIdx))
      return false;
    CSkinnedModel* xrayModel = animData.GetXRayModel();
    CSkinnedModel* infraModel = animData.GetInfraModel();
    if (xrayModel != nullptr && !xrayModel->GetModel()->IsLoaded(shaderIdx))
      return false;
    if (infraModel != nullptr && !infraModel->GetModel()->IsLoaded(shaderIdx))
      return false;
  }
  if (x1c_normalModel && !(*x1c_normalModel)->IsLoaded(shaderIdx))
    return false;
  if (x2c_xrayModel && !(*x2c_xrayModel)->IsLoaded(shaderIdx))
    return false;
  if (x3c_infraModel && !(*x3c_infraModel)->IsLoaded(shaderIdx))
    return false;
  return true;
}

int CModelData::GetNumMaterialSets() const {
  if (HasAnimation())
    return xc_animData->GetModelData()->GetModel()->GetNumMaterialSets();
  if (x1c_normalModel)
    return (*x1c_normalModel)->GetNumMaterialSets();
  return 1;
}

#ifdef TARGET_PC
int CModelData::PortSlot(EWhichModel which) const {
  switch (which) {
  case kWM_Thermal:
  case kWM_ThermalHot:
    return x3c_infraModel ? 2 : 0;
  case kWM_XRay:
    return x2c_xrayModel ? 1 : 0;
  default:
    return 0;
  }
}

void CModelData::PortPlace(uint mrea, uint editorId) {
  xPortCorrected = 0;
  if (HasAnimation())
    return;
  const rstl::optional_object< TLockedToken< CModel > >* slots[3] = {&x1c_normalModel, &x2c_xrayModel,
                                                                     &x3c_infraModel};
  for (int i = 0; i < 3; ++i) {
    if (*slots[i] &&
        PortRetailPlacement::Find(mrea, editorId, static_cast< uint >((**slots[i]).GetTag().GetId()),
                                  xPortCorrection[i]))
      xPortCorrected |= uchar(1 << i);
  }
}

CTransform4f CModelData::PortScaledXf(EWhichModel which, const CTransform4f& xf) const {
  const CTransform4f scaledXf = xf * CTransform4f::Scale(x0_scale);
  const int slot = PortSlot(which);
  if (HasAnimation() || (xPortCorrected & (1 << slot)) == 0)
    return scaledXf;
  const float m[12] = {xf.Get00(), xf.Get01(), xf.Get02(), xf.Get03(), xf.Get10(), xf.Get11(),
                       xf.Get12(), xf.Get13(), xf.Get20(), xf.Get21(), xf.Get22(), xf.Get23()};
  const float scale[3] = {x0_scale.GetX(), x0_scale.GetY(), x0_scale.GetZ()};
  float o[12];
  PortRetailPlacement::Compose(m, scale, xPortCorrection[slot], o);
  return CTransform4f(o[0], o[1], o[2], o[3], o[4], o[5], o[6], o[7], o[8], o[9], o[10], o[11]);
}

// The models' boxes, each through its own correction, as GetBounds unions them.
CAABox CModelData::PortLocalBounds() const {
  const rstl::optional_object< TLockedToken< CModel > >* slots[3] = {&x1c_normalModel, &x2c_xrayModel,
                                                                     &x3c_infraModel};
  CAABox bounds = (*x1c_normalModel)->GetBoundingBox();
  bool first = true;
  for (int i = 0; i < 3; ++i) {
    if (!*slots[i])
      continue;
    CAABox box = (**slots[i])->GetBoundingBox();
    if ((xPortCorrected & (1 << i)) != 0) {
      const float* c = xPortCorrection[i];
      box = box.GetTransformedAABox(
          CTransform4f(c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11]));
    }
    if (first)
      bounds = box;
    else
      bounds.Include(box);
    first = false;
  }
  return bounds;
}
#endif
