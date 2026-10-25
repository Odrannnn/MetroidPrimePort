#ifndef _CENERGYPROJECTILE
#define _CENERGYPROJECTILE

#include "types.h"

#include "MetroidPrime/Cameras/CCameraShakeData.hpp"
#include "MetroidPrime/Weapons/CGameProjectile.hpp"
#ifdef TARGET_PC
#include "MetroidPrime/CPortNestedLights.hpp"
#endif

#include "Kyoto/Audio/CSfxHandle.hpp"
#include "Kyoto/Math/CVector3f.hpp"

class CEnergyProjectile : public CGameProjectile {
  friend struct PortEntitySchema; // save-state field schema (platform/include/port_entity_schema.h)
public:
  CEnergyProjectile(const bool active, const TToken< CWeaponDescription >& desc,
                    const EWeaponType type, const CTransform4f& xf, const EMaterialTypes excludeMat,
                    const CDamageInfo& damage, const TUniqueId uid, const TAreaId aid,
                    const TUniqueId owner, TUniqueId homingTarget, const uint attribs,
                    const bool underwater, const CVector3f& scale,
                    const rstl::optional_object< TLockedToken< CGenDescription > >& visorParticle,
                    const ushort visorSfx, const bool sendCollideMsg);

  // CEntity
  DECLARE_TYPES_MATCH_OR_ACCEPT;
  ~CEnergyProjectile() override;
  void Think(float dt, CStateManager& mgr) override;
  void AcceptScriptMsg(EScriptObjectMessage msg, TUniqueId uid, CStateManager& mgr) override;

  // CActor
  void AddToRenderer(const CFrustumPlanes&, const CStateManager&) const override;
  void Render(const CStateManager&) const override;
  void Touch(CActor&, CStateManager&) override;

  // CGameProjectile
  void ResolveCollisionWithActor(const CRayCastResult& res, CActor& act,
                                 CStateManager& mgr) override;

  // CEnergyProjectile
  virtual const bool Explode(const CVector3f& pos, const CVector3f& normal,
                             const EWeaponCollisionResponseTypes type, CStateManager& mgr,
                             const CDamageVulnerability& dVuln, const TUniqueId hitActor);

  void Set3d0_26(bool v) { x3d0_26_ = v; }

  CProjectileWeapon& ProjectileWeapon() { return x170_projectile; }

  void StopProjectile(CStateManager& mgr);

  void SetCameraShake(const CCameraShakeData& data);
  void PlayImpactSound(const CVector3f& pos, EWeaponCollisionResponseTypes type);
  void ChangeProjectileOwner(TUniqueId owner, CStateManager& mgr);
  void ResolveCollisionWithWorld(const CRayCastResult& res, CStateManager& mgr);
#ifdef TARGET_PC
  // CEnergyProjectileMP1::SetRemainingRicochetCount (0x1013554); read only by Remastered Explode.
  void PortSetRemainingRicochetCount(int count) { xPortRicochets = count; }
#endif

private:
  CSfxHandle x2e8_sfx;
  CVector3f x2ec_dir;
  float x2f8_mag;
  CCameraShakeData x2fc_camShake;
  bool x3d0_24_dead : 1;
  bool x3d0_25_ : 1;
  bool x3d0_26_ : 1;
  bool x3d0_27_camShakeDirty : 1;
  float x3d4_curTime;
#ifdef TARGET_PC
  CPortNestedLights xPortNestedLights; // lights of the nested PHDR children of the APSM effect
  int xPortRicochets = 8; // CEnergyProjectileMP1 +0x528, ctor 0x1011bdc
#endif

  static const CMaterialList kCheckMaterial;
};
CHECK_SIZEOF(CEnergyProjectile, (VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02 ? 0x400
                                 : VERSION >= VERSION_GM8P_00                             ? 0x3e8
                                                                                          : 0x3d8));

#endif // _CENERGYPROJECTILE
