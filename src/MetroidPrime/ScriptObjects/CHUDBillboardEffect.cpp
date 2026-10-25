#include "MetroidPrime/ScriptObjects/CHUDBillboardEffect.hpp"

#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Cameras/CGameCamera.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"

#include "MetaRender/CCubeRenderer.hpp"

#include "Kyoto/Particles/CElementGen.hpp"
#include "Kyoto/Particles/CParticleElectric.hpp"

#include "rstl/math.hpp"

#ifdef TARGET_PC
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Math/CMath.hpp"
#include "Kyoto/Particles/CGenDescription.hpp"
#include "MetroidPrime/TCastTo.hpp"

#include <cmath>
#endif

#include "port_debug.h"

int CHUDBillboardEffect::g_BillboardCount = 0;
int CHUDBillboardEffect::g_IndirectTexturedBillboardCount = 0;

float CHUDBillboardEffect::CalcGenRate() {
  float f1 = (g_BillboardCount + g_IndirectTexturedBillboardCount <= 4)
                 ? 0.f
                 : g_BillboardCount * 0.2f + g_IndirectTexturedBillboardCount * 0.1f;
  return 1.f - rstl::min_val(0.8f, f1);
}

const CVector3f& CHUDBillboardEffect::GetScaleForPOV(const CStateManager& mgr) {
  static CVector3f result(0.155f, 1.f, 0.155f);
  return result;
}

float CHUDBillboardEffect::GetNearClipDistance(const CStateManager& mgr) {
  return mgr.GetCameraManager()->GetCurrentCamera(mgr).GetNearClipDistance() + 0.01f;
}

CHUDBillboardEffect::CHUDBillboardEffect(
    const rstl::optional_object< TToken< CGenDescription > >& particle,
    const rstl::optional_object< TToken< CElectricDescription > >& electric, TUniqueId uid,
    bool active, const rstl::string& name, float dist, const CVector3f& scale0, const CColor& color,
    const CVector3f& scale1, const CVector3f& translation)
: CEffect(uid, CEntityInfo(kInvalidAreaId, CEntity::NullConnectionList), active, name,
          CTransform4f::Identity())

, xec_translation(translation.GetX(), translation.GetY() + dist, translation.GetZ())
, xf8_localScale(CVector3f::ByElementMultiply(scale1, scale0))
, x104_24_renderAsParticleGen(true)
, x104_25_enableRender(false)
, x104_26_isElementGen(false)
, x104_27_runIndefinitely(false)
, x108_timeoutTimer(0.f) {

  if (particle) {
    x104_26_isElementGen = true;
    xe8_generator = rs_new CElementGen(*particle);
    if (static_cast< CElementGen& >(*xe8_generator).IsIndirectTextured())
      ++g_IndirectTexturedBillboardCount;
  } else {
    xe8_generator = rs_new CParticleElectric(TToken< CElectricDescription >(*electric));
  }
  ++g_BillboardCount;
  xe8_generator->SetModulationColor(color);
  xe8_generator->SetLocalScale(xf8_localScale);
}

CHUDBillboardEffect::~CHUDBillboardEffect() {
  --g_BillboardCount;
  if (xe8_generator->Get4CharId() == 'PART')
    if (static_cast< CElementGen& >(*xe8_generator).IsIndirectTextured())
      --g_IndirectTexturedBillboardCount;
}

void CHUDBillboardEffect::AddToRenderer(const CFrustumPlanes& frustum,
                                        const CStateManager& mgr) const {
  if (x104_25_enableRender && x104_24_renderAsParticleGen) {
    gpRender->AddParticleGen(*xe8_generator);
  }
}

void CHUDBillboardEffect::PreRender(CStateManager& mgr, const CFrustumPlanes& frustum) {
  if (mgr.GetPlayer()->GetCameraState() == CPlayer::kCS_FirstPerson &&
      !PortDebug::HideVisorEffects()) {
    CTransform4f camXf = mgr.GetCameraManager()->GetCurrentCameraTransform(mgr);
#ifdef TARGET_PC
    if (xPortManagedType == 4) {
      // Remastered places the goo manager's effect in HUD space (CHUDVisorEffectData): x right
      // and y up on the plane D ahead of the eye, scaled per frame by GetHUDVisorEffectScale
      // (D * tan(fov / 2), times the aspect across), so a splat at (x, y) with size s lands at
      // (x, y) in normalized screen coordinates with half-extent s.
      const CGameCamera& cam = mgr.GetCameraManager()->GetCurrentCamera(mgr);
      const float d = xec_translation.GetY();
      const float halfHeight = d * std::tan(cam.GetRenderFov() * (M_PIF / 180.f) * 0.5f);
      const CTransform4f hudToCamera(1.f, 0.f, 0.f, 0.f, 0.f, 0.f, -1.f, 0.f, 0.f, 1.f, 0.f, 0.f);
      xe8_generator->SetGlobalTranslation(camXf * CVector3f(0.f, d, 0.f));
      xe8_generator->SetGlobalOrientation(camXf * hudToCamera);
      xe8_generator->SetLocalScale(
          CVector3f(cam.GetAspectRatio() * halfHeight, 1.f, halfHeight));
    } else if (PortIsRemasteredHUD()) {
      // CHUDBillboardEffectMP1 ctor 0xd7e75c: Remastered drops the retail dist/scale/translation.
      // Its only caller, the GOC Create 0xd80010, passes scale skOneVector and offset
      // skZeroVector, so the effect sits in the HUD camera's space at (0, 0, -D), D the visor
      // plane depth, with scale (1, 1, 1), times GetHUDVisorEffectScale 0xcb0ef8 =
      // (aspect * D * tan(fov / 2), D * tan(fov / 2), 1) for scale type 1. Type 1 maps a particle
      // at (x, y) to (x, y) in normalized screen coordinates, sprites stretched with it (16:9);
      // type 0 leaves HUD units, one of them 1 / (D * tan(fov / 2)) of the half-height. The port
      // scales that HUD scene by d / D onto its own near plane, keeping each particle's
      // direction from the eye (and the VisorRain depth ratio, vfx.cpp). Remastered ran at 16:9
      // only: other aspects keep the 16:9 sprite size and spread the spawns instead
      // (PortUpdateSpawnSpread).
      const CGameCamera& cam = mgr.GetCameraManager()->GetCurrentCamera(mgr);
      const float d = GetNearClipDistance(mgr);
      const float unit = d * std::tan(cam.GetRenderFov() * (M_PIF / 180.f) * 0.5f) *
                         rstl::min_val(1.f, cam.GetAspectRatio() / kPortHUDAspect);
      const CTransform4f hudToCamera(1.f, 0.f, 0.f, 0.f, 0.f, 0.f, -1.f, 0.f, 0.f, 1.f, 0.f, 0.f);
      xe8_generator->SetGlobalTranslation(camXf * CVector3f(0.f, d, 0.f));
      xe8_generator->SetGlobalOrientation(camXf * hudToCamera);
      if (xPortScaleType == 1) {
        xe8_generator->SetLocalScale(CVector3f(kPortHUDAspect * unit, d / kPortHUDDepth, unit));
      } else {
        const float hudUnit = unit / (kPortHUDDepth * kPortHUDTanHalfFov);
        xe8_generator->SetLocalScale(CVector3f(hudUnit, d / kPortHUDDepth, hudUnit));
      }
    } else
#endif
    {
      xe8_generator->SetGlobalTranslation(camXf * xec_translation);
      xe8_generator->SetGlobalOrientation(camXf);
    }
#ifdef TARGET_PC
    // camXf is the presented camera: blending it again would trail a frame behind.
    if (x104_26_isElementGen)
      static_cast< CElementGen& >(*xe8_generator).PortGlobalIsPresented();
#endif
    x104_25_enableRender = true;
  } else {
    x104_25_enableRender = false;
  }
  x104_24_renderAsParticleGen = !mgr.RenderLast(GetUniqueId());
}

void CHUDBillboardEffect::Render(const CStateManager& mgr) const {
  if (x104_25_enableRender && !x104_24_renderAsParticleGen) {
    xe8_generator->Render();
  }
}

#ifdef TARGET_PC
// Port: the effects are authored to fill a 4:3 view (GetScaleForPOV). Wider screens keep the
// vertical FOV and extend sideways, narrower ones extend vertically (Vert+), so spread where the
// particles and bolts spawn along the extended axis. Scaling the system instead would stretch
// every sprite and bolt with it. The visor goo manager places its splats itself. Remastered's
// HUD-space effects are authored for 16:9, with HUD y up.
void CHUDBillboardEffect::PortUpdateSpawnSpread(const CStateManager& mgr) {
  CVector3f spread(1.f, 1.f, 1.f);
  if (xPortManagedType == 4) {
    // Placed by PortAddVisorGooSplat.
  } else if (PortIsRemasteredHUD()) {
    const float aspect = mgr.GetCameraManager()->GetCurrentCamera(mgr).GetAspectRatio();
    if (aspect > kPortHUDAspect) {
      spread.SetX(aspect / kPortHUDAspect);
    } else if (aspect > 0.f) {
      spread.SetY(kPortHUDAspect / aspect);
    }
  } else {
    const float kMinAspect = 4.f / 3.f;
    const float aspect = mgr.GetCameraManager()->GetCurrentCamera(mgr).GetAspectRatio();
    if (aspect > kMinAspect) {
      spread.SetX(aspect / kMinAspect);
    } else if (aspect > 0.f) {
      spread.SetZ(kMinAspect / aspect);
    }
  }
  PortApplySpawnSpread(spread);
}

// Remastered marks its PARTs; an electric-only billboard has no such flag and stays retail.
bool CHUDBillboardEffect::PortIsRemasteredHUD() const {
  return x104_26_isElementGen &&
         static_cast< const CElementGen& >(*xe8_generator).PortIsRemastered();
}

void CHUDBillboardEffect::PortApplySpawnSpread(const CVector3f& spread) {
  if (spread == xPortSpawnSpread) {
    return;
  }
  xPortSpawnSpread = spread;
  xe8_generator->PortSetSpawnSpread(spread, xe8_generator->GetTranslation());
}
#endif

void CHUDBillboardEffect::Think(float dt, CStateManager& mgr) {
  if (GetActive()) {
    mgr.SetActorAreaId(*this, mgr.GetWorld()->GetCurrentAreaId());
#ifdef TARGET_PC
    PortUpdateSpawnSpread(mgr);
#endif
    float oldGenRate = xe8_generator->GetGeneratorRate();
    xe8_generator->SetGeneratorRate(oldGenRate * CalcGenRate());
    xe8_generator->Update(dt);
    xe8_generator->SetGeneratorRate(oldGenRate);
    if (!x104_27_runIndefinitely) {
      x108_timeoutTimer += dt;
      if (x108_timeoutTimer > 30.f) {
        mgr.DeleteObjectRequest(GetUniqueId());
        return;
      }
    }
    if (xe8_generator->IsSystemDeletable())
      mgr.DeleteObjectRequest(GetUniqueId());
  }
}

#ifdef TARGET_PC
void CHUDBillboardEffect::AcceptScriptMsg(EScriptObjectMessage msg, TUniqueId uid,
                                          CStateManager& mgr) {
  // CHUDBillboardEffectMP1: a managed effect going away resets its manager.
  if (msg == kSM_Deleted && xPortManagedType == 4 &&
      mgr.PortVisorGooManager().effect == GetUniqueId()) {
    PortSetVisorGoo(mgr, kInvalidUniqueId, 0.f);
  }
  // A billboard whose PART has visor effect data is added to the override list
  // (AddBillboardEffect); Remastered adds it from the ctor, the port on registration (unverified).
  if (msg == kSM_Registered && x104_26_isElementGen) {
    const CGenDescription* desc = static_cast< CElementGen& >(*xe8_generator).PortDesc();
    if (desc != nullptr && desc->xPortVisorData) {
      PortVisorOverrides::Add(mgr.PortVisorOverrideManager(), GetUniqueId());
      xPortVisorOverride = true;
    }
  } else if (msg == kSM_Deleted && xPortVisorOverride) {
    PortVisorOverrides::Remove(mgr.PortVisorOverrideManager(), GetUniqueId());
    xPortVisorOverride = false;
  }
  CEffect::AcceptScriptMsg(msg, uid, mgr);
}

// ExtractCurrentData: the generator's VE* values at its current frame (unverified: the frame the
// KEYE/PULS elements are evaluated at). A missing element keeps white / 1.
static void PortExtractVisorData(const CElementGen& gen, PortVisorOverrides::Data& d) {
  const CGenDescription* desc = gen.PortDesc();
  d.valid = desc != nullptr && desc->xPortVisorData;
  if (!d.valid) {
    return;
  }
  const int frame = gen.PortCurFrame();
  if (desc->xPortVisorStartColor != nullptr) {
    desc->xPortVisorStartColor->GetValueHdr(frame, d.c0);
  }
  if (desc->xPortVisorMidColor != nullptr) {
    desc->xPortVisorMidColor->GetValueHdr(frame, d.c1);
  }
  if (desc->xPortVisorEndColor != nullptr) {
    desc->xPortVisorEndColor->GetValueHdr(frame, d.c2);
  }
  if (desc->xPortVisorBlend != nullptr) {
    desc->xPortVisorBlend->GetValue(frame, d.f);
  }
  d.ticks = desc->xPortVisorTicks;
}

void CHUDBillboardEffect::PortUpdateVisorOverrides(CStateManager& mgr, float dt) {
  const bool unmorphed = mgr.GetPlayer()->GetMorphballTransitionState() == CPlayer::kMS_Unmorphed;
  PortVisorOverrides::Update(
      mgr.PortVisorOverrideManager(), dt, unmorphed,
      [&mgr](TUniqueId id, PortVisorOverrides::Data& d, bool& active) {
        const CHUDBillboardEffect* fx = TCastToPtr< CHUDBillboardEffect >(mgr.ObjectById(id));
        if (!fx || !fx->x104_26_isElementGen) {
          return false;
        }
        const CElementGen& gen = static_cast< const CElementGen& >(*fx->xe8_generator);
        PortExtractVisorData(gen, d);
        // The generator's vtable+0x40 check, read as "has live particles" (unverified).
        active = gen.GetParticleCountAll() > 0;
        return true;
      });
}

bool CHUDBillboardEffect::PortHasVisorGoo(CStateManager& mgr) {
  return TCastToPtr< CHUDBillboardEffect >(mgr.ObjectById(mgr.PortVisorGooManager().effect));
}

// SetManagedEffect(4, id): the old effect is unmanaged, the new one is updated once and its size
// variable looked up; an invalid id resets the ring.
void CHUDBillboardEffect::PortSetVisorGoo(CStateManager& mgr, TUniqueId uid, float dt) {
  CStateManager::PortVisorGooState& goo = mgr.PortVisorGooManager();
  if (CHUDBillboardEffect* old = TCastToPtr< CHUDBillboardEffect >(mgr.ObjectById(goo.effect))) {
    old->xPortManagedType = -1;
  }
  goo.effect = uid;
  CHUDBillboardEffect* fx = TCastToPtr< CHUDBillboardEffect >(mgr.ObjectById(uid));
  if (!fx) {
    goo.ring.Reset();
    goo.sizeHandle = kPortNoVar;
    return;
  }
  fx->xPortManagedType = 4;
  // Before the update below: the goo manager computes the splat placement itself.
  fx->PortApplySpawnSpread(CVector3f(1.f, 1.f, 1.f));
  fx->xe8_generator->Update(dt);
  goo.sizeHandle = kPortNoVar;
  if (fx->x104_26_isElementGen) {
    // 0c7177e8-ec26-4794-b8f3-64e9517fa245, as stored (the first three fields little-endian).
    PortGuid kSize;
    kSize.bytes = {0xe8, 0x77, 0x71, 0x0c, 0x26, 0xec, 0x94, 0x47,
                   0xb8, 0xf3, 0x64, 0xe9, 0x51, 0x7f, 0xa2, 0x45};
    goo.sizeHandle =
        static_cast< CElementGen& >(*fx->xe8_generator).PortGetRealHandle(kSize);
  }
}

void CHUDBillboardEffect::PortAddVisorGooSplat(CStateManager& mgr) {
  CStateManager::PortVisorGooState& goo = mgr.PortVisorGooManager();
  CHUDBillboardEffect* fx = TCastToPtr< CHUDBillboardEffect >(mgr.ObjectById(goo.effect));
  if (!fx || fx->xPortManagedType != 4 || !fx->x104_26_isElementGen || mgr.Random() == nullptr) {
    return;
  }
  CElementGen& gen = static_cast< CElementGen& >(*fx->xe8_generator);
  const CGenDescription* desc = gen.PortDesc();
  PortVisorGoo::Splat splat;
  if (!PortVisorGoo::Plan(goo.ring, *mgr.Random(), static_cast< int >(gen.GetParticleCountAll()),
                          desc->xPortVisorData, desc->xPortVisorMinScale,
                          desc->xPortVisorMaxScale,
                          static_cast< float >(CGraphics::GetViewportWidth()),
                          static_cast< float >(CGraphics::GetViewportHeight()), splat)) {
    return;
  }
  gen.PortBindReal(goo.sizeHandle, splat.size);
  gen.PortCreateParticlesAtPositionsIncludingChildren(1, &splat.pos);
  PortVisorGoo::Push(goo.ring, splat.budget, splat.pos);
}
#endif

ENTITY_ACCEPT_IMPL(CHUDBillboardEffect)
