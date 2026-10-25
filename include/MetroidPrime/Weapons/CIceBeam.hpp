#ifndef _CICEBEAM
#define _CICEBEAM

#include "types.h"

#include "MetroidPrime/Weapons/CGunWeapon.hpp"

class CIceBeam : public CGunWeapon {
public:
  CIceBeam(CAssetId characterId, EWeaponType type, TUniqueId playerId,
           EMaterialTypes playerMaterial, const CVector3f& scale);
  ~CIceBeam() override;

  // CGunWeapon
  void PreRenderGunFx(const CStateManager& mgr, const CTransform4f& xf) override;
  void PostRenderGunFx(const CStateManager& mgr, const CTransform4f& xf) override;
  void UpdateGunFx(bool shotSmoke, float dt, const CStateManager& mgr,
                   const CTransform4f& xf) override;
  void Update(float dt, CStateManager& mgr) override;
  void Fire(bool underwater, float dt, CPlayerState::EChargeStage chargeState,
            const CTransform4f& xf, CStateManager& mgr, TUniqueId homingTarget, float chargeFactor1,
            float chargeFactor2) override;

  void Load(CStateManager& mgr, bool subtypeBasePose) override;
  void Unload(CStateManager& mgr) override;
  bool IsLoaded() const override;

  void EnableFx(bool enable) override;
  void EnableSecondaryFx(ESecondaryFxType type) override;

private:
  TCachedToken< CGenDescription > x21c_iceSmoke;
  TCachedToken< CGenDescription > x228_ice2nd1;
  TCachedToken< CGenDescription > x234_ice2nd2;
  rstl::single_ptr< CElementGen > x240_smokeGen;
  rstl::single_ptr< CElementGen > x244_chargeFx;
  bool x248_24_loaded : 1;
  bool x248_25_inEndFx : 1;
#ifdef TARGET_PC
  // Remastered's shell glow pulse (CIceBeamMP1 +0x768/+0x76c): 0..0.5..0 at 1/s, driven by
  // UpdateGunFx, the shell's IINT 3 + 14 v (PreRenderGunFx). Not reset by ReInitVariables.
  float mPortShellPulse;
  bool mPortShellRising;
#endif

  void ReInitVariables();
};
CHECK_SIZEOF(CIceBeam, 0x24c)

#endif // _CICEBEAM
