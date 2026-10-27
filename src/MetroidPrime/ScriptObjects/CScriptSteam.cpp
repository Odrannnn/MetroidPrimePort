#include "MetroidPrime/ScriptObjects/CScriptSteam.hpp"

#include "MetroidPrime/CEnvFxManager.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"

#include "Kyoto/Math/CloseEnough.hpp"

#include "rstl/math.hpp"

#ifdef TARGET_PC
#include "MetroidPrime/TCastTo.hpp"
#include "port_maya_spline.h"
#include "port_room_effects.h"
#endif

CScriptSteam::CScriptSteam(TUniqueId uid, const rstl::string& name, const CEntityInfo& info,
                           const CVector3f& pos, const CAABox& aabb, const CDamageInfo& dInfo,
                           const CVector3f& orientedForce, unsigned int flags, bool active,
                           CAssetId texture, float f1, float f2, float f3, const float f4, bool b1)
: CScriptTrigger(uid, name, info, pos, aabb, dInfo, orientedForce, flags, active, false, false)
, x150_(b1)
, x154_texture(texture)
, x158_strength(f1)
, x15c_alphaInDur(f2 / f1)
, x160_alphaOutDur(f3 / f1)
, x164_maxDist(0.f)
, x168_ooMaxDist(0.f) {
  float r3 = rstl::min_val(aabb.GetMaxPoint().GetX(),
                           rstl::min_val(aabb.GetMaxPoint().GetY(), aabb.GetMaxPoint().GetZ()));
  x164_maxDist = close_enough(f4, 0.f) ? r3 : rstl::min_val(f4, r3);
  x168_ooMaxDist = 1.f / x164_maxDist;
}

#ifdef TARGET_PC
// CScriptSteamMP1's visor steam fields (SLdrSteamMP1, port_room_effects.h Steam).
struct CScriptSteam::PortShip1 {
  PortRoomEffects::Steam rec;
  PortMayaSpline s1, s2;
};

void CScriptSteam::PortSetShip1(const PortRoomEffects::Steam& steam) {
  delete xPortShip1;
  xPortShip1 = new PortShip1;
  xPortShip1->rec = steam;
  if (!steam.s1.empty()) {
    xPortShip1->s1.Load(steam.s1.data(), steam.s1.size());
  }
  if (!steam.s2.empty()) {
    xPortShip1->s2.Load(steam.s2.data(), steam.s2.size());
  }
}

CScriptSteam::~CScriptSteam() { delete xPortShip1; }
#else
CScriptSteam::~CScriptSteam() {}
#endif

void CScriptSteam::AcceptScriptMsg(EScriptObjectMessage msg, TUniqueId uid, CStateManager& mgr) {
#ifdef TARGET_PC
  // CScriptSteamMP1::AcceptScriptMsg only forwards to the trigger.
  if (xPortShip1 != nullptr) {
    CScriptTrigger::AcceptScriptMsg(msg, uid, mgr);
    return;
  }
#endif
  switch (msg) {
  case kSM_Deactivate:
    mgr.Player()->SetVisorSteam(0.f, x15c_alphaInDur, x160_alphaOutDur, kInvalidAssetId, !x150_);
    break;
  }

  CScriptTrigger::AcceptScriptMsg(msg, uid, mgr);
}

void CScriptSteam::Think(float dt, CStateManager& mgr) {
  if (!GetActive()) {
    return;
  }

  CScriptTrigger::Think(dt, mgr);

#ifdef TARGET_PC
  if (xPortShip1 != nullptr) {
    PortThinkShip1(mgr);
    return;
  }
#endif
  if (x148_28_playerTriggerProc && mgr.GetCameraManager()->GetFluidCounter() == 0) {
    CVector3f eyePos = mgr.GetPlayer()->GetEyePosition();
    const float mag = (GetTranslation() - eyePos).Magnitude();
    float distance;
    if (mag >= GetMaxDist()) {
      distance = 0.f;
    } else {
      distance = CMath::FastCosR((mag * 1.5707964f /* 90 deg */) * GetOOMaxDist()) * GetStrength();
    }
    mgr.Player()->SetVisorSteam(distance, GetFadeInRate(), GetFadeOutRate(), GetTextureId(),
                                !x150_);
    if (x150_) {
      mgr.EnvFxManager()->SetSplashRate(2.f * distance);
    }
  } else {
    mgr.Player()->SetVisorSteam(0.f, GetFadeInRate(), GetFadeOutRate(), kInvalidAssetId, !x150_);
  }
}

#ifdef TARGET_PC
// CScriptSteamMP1::Think 0xf36498 past the trigger's Think. Not ported: the skip while visor
// managed effect 3 is active (the port has no visor effect manager).
void CScriptSteam::PortThinkShip1(CStateManager& mgr) {
  if (!x148_28_playerTriggerProc || mgr.GetCameraManager()->GetFluidCounter() != 0) {
    return;
  }
  const PortRoomEffects::Steam& rec = xPortShip1->rec;
  const CVector3f eyePos = mgr.GetPlayer()->GetEyePosition();

  // GetAveragedSteamSourcePos 0xf36124: a linked source's live position (its static one when
  // the port has no actor for it), or the 1/d^2-weighted average of several.
  struct Local {
    static CVector3f SourcePos(const CStateManager& mgr, const PortRoomEffects::SteamSource& s) {
      if (s.id != 0) {
        const TUniqueId uid = mgr.GetIdForScript(TEditorId(s.id));
        if (const CActor* act = TCastToConstPtr< CActor >(mgr.GetObjectById(uid))) {
          return act->GetTranslation();
        }
      }
      return CVector3f(s.pos[0], s.pos[1], s.pos[2]);
    }
  };
  const size_t n = rec.sources.size();
  CVector3f pos = GetTranslation();
  if (n == 1) {
    pos = Local::SourcePos(mgr, rec.sources[0]);
  } else if (n > 1) {
    CVector3f sum(0.f, 0.f, 0.f);
    float wsum = 0.f;
    for (size_t k = 0; k < n; ++k) {
      const CVector3f p = Local::SourcePos(mgr, rec.sources[k]);
      const float d2 = (p - eyePos).MagSquared();
      const float w = 0.001f < d2 ? 1.f / d2 : 0.f;
      sum += p * w;
      wsum += w;
    }
    if (0.001f < wsum) {
      pos = sum * (1.f / wsum);
    }
  }

  CPlayer::SPortVisorSteamShip1Params p;
  p.flag = rec.colorFlag;
  for (int k = 0; k < 4; ++k) {
    p.colour[k] = rec.color[k];
  }
  p.dir = CVector3f(0.f, 0.f, 0.f);
  if (n != 0) {
    // GetSteamDirectionCameraSpace 0xf363a4.
    const CTransform4f camXf = mgr.GetCameraManager()->GetCurrentCameraTransform(mgr);
    CVector3f v = camXf.TransposeRotate(pos - camXf.GetTranslation());
    v = CVector3f(v.GetX(), v.GetY(), 0.f);
    if (CMath::AbsF(v.GetX() * v.GetX() + v.GetY() * v.GetY()) >= 1e-5f) {
      p.dir = -v.AsNormalized();
    }
  }
  const float dist = (pos - eyePos).Magnitude();
  p.b = rec.b;
  p.i = rec.i;
  p.s1 = xPortShip1->s1.Size() != 0 ? xPortShip1->s1.Eval(dist) : 0.f;
  p.a = rstl::max_val(rec.a, 0.001f);
  p.s2 = xPortShip1->s2.Size() != 0 ? xPortShip1->s2.Eval(dist) : 0.f;
  p.g = rec.g;
  p.h = rec.h;
  mgr.Player()->PortSetVisorSteamShip1(rec.part, p, mgr);
}
#endif

ENTITY_ACCEPT_IMPL(CScriptSteam)
