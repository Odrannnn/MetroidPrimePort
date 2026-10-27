#ifndef _CFLAMETHROWER
#define _CFLAMETHROWER
#include "CFlameInfo.hpp"
#include "MetroidPrime/CFlameWarp.hpp"
#include "MetroidPrime/Weapons/CGameProjectile.hpp"

class CFlameThrower : public CGameProjectile {
  friend struct PortEntitySchema; // save-state field schema (platform/include/port_entity_schema.h)
public:
  enum EFlameState {
    kFS_Default,
    kFS_FireStart,
    kFS_FireActive,
    kFS_FireStopTimer,
    kFS_FireWaitForParticlesDone,
  };
  CFlameThrower(const TToken< CWeaponDescription >& wDesc, const rstl::string& name,
                const EWeaponType wType, const CFlameInfo& flameInfo, const CTransform4f& xf,
                const EMaterialTypes matType, const CDamageInfo& dInfo, const TUniqueId uid,
                const TAreaId aId, const TUniqueId owner, EProjectileAttrib attribs,
                const CAssetId playerSteamTxtr, const ushort playerHitSfx,
                const CAssetId playerIceTxtr);
  DECLARE_TYPES_MATCH_OR_ACCEPT;
  void AcceptScriptMsg(EScriptObjectMessage msg, TUniqueId uid, CStateManager& mgr) override;
  void Think(float dt, CStateManager& mgr) override;
  void AddToRenderer(const CFrustumPlanes& frustum, const CStateManager& mgr) const override;
  void Render(const CStateManager& mgr) const override;
  rstl::optional_object< CAABox > GetTouchBounds() const override;
  void Touch(CActor& actor, CStateManager& mgr) override;

  void Reset(CStateManager& mgr, bool resetWarp);
  bool GetParticlesActive() const { return x400_25_particlesActive; }

  void Fire(const CTransform4f&, CStateManager& mgr, bool);

  void SetTransform(const CTransform4f& xf, CStateManager& mgr, float);

#ifdef TARGET_PC
  // Remastered's CFlameThrowerMP1 renders a separate flame effect (id1, flame info +0x450) and
  // keeps the flame-info effect (id3, +0x498) unrendered, carrying the flame warp for collision.
  // A collision warp goes on whichever of id1 or its active children is the id2 (+0x470) system.
  void PortSetRemasteredFlame(CAssetId fxId, CAssetId collisionFxId);
  // The frozen VFX FreezePlayer 0x1015c1c passes in its SPlayerFrozenStateDataShip1 (+0x10).
  void PortSetFrozenFx(CAssetId id) { xPortFrozenFxId = id; }
#endif

private:
  void CreateFlameParticles(CStateManager& mgr);
  void SetFlameLightActive(bool active, CStateManager& mgr);
  float UpdateFlameState(float dt, CStateManager& mgr);
  CRayCastResult DoCollisionCheck(TUniqueId& idOut, const CAABox& bounds, CStateManager& mgr);
  void ApplyDamageToActor(CStateManager& mgr, TUniqueId id, float dt);

  CTransform4f x2e8_flameXf;
  CAABox x318_flameBounds;
  float x330_particleWaitDelayTimer;
  float x334_fireStopTimer;
  float x338_flame;
  TLockedToken< CGenDescription > x33c_flamethrowerDesc;
  rstl::single_ptr< CElementGen > x348_flameGen;
  CFlameWarp x34c_flameWarp;
  EFlameState x3f0_flameState;
  CAssetId x3f4_playerSteamTextureId;
  ushort x3f8_playerHitSfx;
  CAssetId x3fc_playerIceTextureId;
  bool x400_24_active : 1;
  bool x400_25_particlesActive : 1;
  bool x400_26_zTest : 1;
  bool x400_27_coneCollision : 1;

#ifdef TARGET_PC
  // CCollisionWarpMP1: keeps particles on the near side of solid world geometry.
  class PortCollisionWarp : public CWarp {
  public:
    PortCollisionWarp() : xMgr(nullptr), xActive(false) {}
    bool UpdateWarp() override { return xActive; }
    void ModifyParticles(int particleCount, int stripe, int*, CVector3f* particlePrevPos,
                         CVector3f* particlePos, CVector3f* particleVelocity, CColor*, float*,
                         float*) override;
    void Activate(bool active) override { xActive = active; }
    bool IsActivated() override { return xActive; }
    FourCC Get4CharID() override { return 'CWRP'; }
    void SetStateManager(CStateManager& mgr) { xMgr = &mgr; }

  private:
    CStateManager* xMgr;
    bool xActive;
  };

  void PortTryAddCollisionWarp();

  rstl::single_ptr< TLockedToken< CGenDescription > > xPortFlameFxDesc;
  rstl::single_ptr< CElementGen > xPortFlameFxGen;
  CAssetId xPortCollisionFxId = kInvalidAssetId;
  CAssetId xPortFrozenFxId = kInvalidAssetId;
  PortCollisionWarp xPortCollisionWarp;
  bool xPortCollisionWarpPending = false;
#endif

  static const CVector3f kLightOffset;
};
CHECK_SIZEOF(CFlameThrower, (VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02 ? 0x430
                             : VERSION >= VERSION_GM8P_00                             ? 0x418
                                                                                      : 0x408))

#endif // _CFLAMETHROWER
