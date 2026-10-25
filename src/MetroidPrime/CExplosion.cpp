#include "MetroidPrime/CExplosion.hpp"

#include "MetroidPrime/CGameLight.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"

#include "Kyoto/Graphics/CCubeModel.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Math/CFrustumPlanes.hpp"
#include "Kyoto/Particles/CElementGen.hpp"
#include "Kyoto/Particles/CGenDescription.hpp"
#include "Kyoto/Particles/CParticleElectric.hpp"
#include "MetaRender/CCubeRenderer.hpp"

CExplosion::CExplosion(const TLockedToken< CGenDescription >& particle, TUniqueId uid, bool active,
                       const CEntityInfo& info, const rstl::string& name, const CTransform4f& xf,
                       const uint flags, const CVector3f& scale, const CColor& color)
: CEffect(uid, info, active, name, xf)
, xe8_particleGen(rs_new CElementGen(TToken< CGenDescription >(particle), CElementGen::kMOT_Normal,
                                     flags & 0x2 ? CElementGen::kOSF_Two : CElementGen::kOSF_One))
, xec_explosionLight(kInvalidUniqueId)
, xf0_sourceId(CToken(particle).GetTag().GetId())
, xf4_24_renderThermalHot(flags & 0x4)
, xf4_25_hasRenderBounds(true)
, xf4_26_renderXray(flags & 0x8)
, xf8_time(0.0f) {
#ifdef TARGET_PC
  xPortFixedStep = (flags & 0x10) != 0;
#endif
  SetThermalFlags(flags & 0x1 ? kTF_Cold : kTF_Hot);
  xe8_particleGen->SetGlobalTranslation(xf.GetTranslation());
  xe8_particleGen->SetOrientation(xf.GetRotation());
  xe8_particleGen->SetGlobalScale(scale);
  xe8_particleGen->SetModulationColor(color);
}

CExplosion::CExplosion(const TLockedToken< CElectricDescription >& electric, TUniqueId uid,
                       bool active, const CEntityInfo& info, const rstl::string& name,
                       const CTransform4f& xf, uint flags, const CVector3f& scale,
                       const CColor& color)
: CEffect(uid, info, active, name, xf)
, xe8_particleGen(rs_new CParticleElectric(electric))
, xec_explosionLight(kInvalidUniqueId)
, xf0_sourceId(CToken(electric).GetTag().GetId())
, xf4_24_renderThermalHot(flags & 0x4)
, xf4_25_hasRenderBounds(true)
, xf4_26_renderXray(flags & 0x8)
#if NONMATCHING || defined(TARGET_PC)
, xf8_time(0.0f)
#endif
{
#ifdef TARGET_PC
  xPortFixedStep = (flags & 0x10) != 0;
#endif
  SetThermalFlags(flags & 0x1 ? kTF_Cold : kTF_Hot);
  xe8_particleGen->SetGlobalTranslation(xf.GetTranslation());
  xe8_particleGen->SetOrientation(xf.GetRotation());
  xe8_particleGen->SetGlobalScale(scale);
  xe8_particleGen->SetModulationColor(color);
}

CExplosion::~CExplosion() {}

void CExplosion::AddToRenderer(const CFrustumPlanes& frustum, const CStateManager& mgr) const {
  if (GetPreRenderClipped()) {
    return;
  }

  if ((xf4_24_renderThermalHot && mgr.GetThermalDrawFlag() == kTD_Hot) ||
      (xf4_26_renderXray && mgr.GetPlayerState()->GetActiveVisor(mgr) == CPlayerState::kPV_XRay)) {
    EnsureRendered(mgr);
  } else {
    gpRender->AddParticleGen(*xe8_particleGen);
  }
}

void CExplosion::PreRender(CStateManager& mgr, const CFrustumPlanes& frustum) {
  CActor::PreRender(mgr, frustum);
  SetPreRenderClipped(!xf4_25_hasRenderBounds || !frustum.BoxInFrustumPlanes(GetRenderBoundsCached()));
}

void CExplosion::Think(float dt, CStateManager& mgr) {
  if (GetTransformDirtySpare()) {
    xe8_particleGen->SetGlobalTranslation(GetTranslation());
    xe8_particleGen->SetOrientation(GetTransform().GetRotation());
    SetTransformDirtySpare(false);
  }
#ifdef TARGET_PC
  // CExplosionMP1::Think (0xd0e300); the 15 s lifetime below still counts real time.
  xe8_particleGen->Update(xPortFixedStep ? 1.0 / 60.0 : dt);
#else
  xe8_particleGen->Update(dt);
#endif

  if (xec_explosionLight != kInvalidUniqueId) {
    CGameLight* light = TCastToPtr< CGameLight >(mgr.ObjectById(xec_explosionLight));
    if (light && GetActive())
      light->SetLight(xe8_particleGen->GetLight());
  }
#ifdef TARGET_PC
  PortUpdateNestedLights(mgr);
#endif

  xf8_time += dt;

  if (xf8_time > 15.f) {
#ifdef TARGET_PC
    xPortNestedLights.Delete(mgr, GetUniqueId());
#endif
    mgr.DeleteObjectRequest(GetUniqueId());
  } else if (xe8_particleGen->IsSystemDeletable()) {
#ifdef TARGET_PC
    xPortNestedLights.Delete(mgr, GetUniqueId());
#endif
    mgr.DeleteObjectRequest(GetUniqueId());
  }
}

#ifdef TARGET_PC
void CExplosion::PortUpdateNestedLights(CStateManager& mgr) {
  xPortNestedLights.Update(mgr, xe8_particleGen.get(), GetActive(), GetUniqueId(),
                           GetCurrentAreaId(), rstl::string_l("ExplodePHDR_") + GetDebugName(),
                           PortNestedLightSourceId(GetSourceId()), 1);
}
#endif

ENTITY_ACCEPT_IMPL(CExplosion)

void CExplosion::AcceptScriptMsg(EScriptObjectMessage msg, TUniqueId sender, CStateManager& mgr) {
#ifdef TARGET_PC
  xPortNestedLights.Forward(msg, sender, GetUniqueId(), mgr);
#endif
  switch (msg) {
  case kSM_Registered:
    if (xe8_particleGen->SystemHasLight()) {
      xec_explosionLight = mgr.AllocateUniqueId();
      uint sourceId = xf0_sourceId;
      mgr.AddObject(rs_new CGameLight(xec_explosionLight, GetCurrentAreaId(), GetActive(),
                                      rstl::string_l("ExplodePLight_") + GetDebugName(),
                                      GetTransform(), GetUniqueId(), xe8_particleGen->GetLight(),
                                      sourceId, 1, 0.f));
    }
    break;
  case kSM_Deleted:
    if (xec_explosionLight != kInvalidUniqueId) {
      mgr.DeleteObjectRequest(xec_explosionLight);
      xec_explosionLight = kInvalidUniqueId;
    }
    break;

  default:
    break;
  }
  CActor::AcceptScriptMsg(msg, sender, mgr);

  if (xec_explosionLight != kInvalidUniqueId)
    mgr.SendScriptMsgAlways(xec_explosionLight, sender, msg);
}

void CExplosion::CalculateRenderBounds() {
  rstl::optional_object< CAABox > bounds = xe8_particleGen->GetBounds();
  if (bounds) {
    SetRenderBounds(*bounds);
    xf4_25_hasRenderBounds = true;
  } else {
    xf4_25_hasRenderBounds = false;
    CVector3f pos = GetTransform().GetTranslation();
    SetRenderBounds(CAABox(pos, pos));
  }
}

void CExplosion::Render(const CStateManager& mgr) const {
  if (mgr.GetThermalDrawFlag() == kTD_Hot && xf4_24_renderThermalHot) {
    CElementGen::SetSubtractBlend(true);
    CCubeModel::SetRenderModelBlack(true);
    xe8_particleGen->Render();
    CCubeModel::SetRenderModelBlack(false);
    CElementGen::SetSubtractBlend(false);
    return;
  }

  CElementGen::SetSubtractBlend(!xf4_24_renderThermalHot);
  CGraphics::SetFog(kRFM_PerspLin, 0.f, 75.f, CColor::Black());
  xe8_particleGen->Render();
  mgr.SetupFogForArea(GetCurrentAreaId());
  CElementGen::SetSubtractBlend(false);
}
