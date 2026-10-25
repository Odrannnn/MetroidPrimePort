#ifndef _CGUNWEAPON
#define _CGUNWEAPON

#include "types.h"

#include "MetroidPrime/CDamageInfo.hpp"
#include "MetroidPrime/CModelData.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "MetroidPrime/Weapons/GunController/CGunMotion.hpp"
#include "MetroidPrime/Weapons/WeaponCommon.hpp"
#include "MetroidPrime/Weapons/WeaponTypes.hpp"

#include "Kyoto/Math/CAABox.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "Kyoto/TToken.hpp"
#ifdef TARGET_PC
#include "Kyoto/Particles/CPortEffectSet.hpp"
#endif

#include "Collision/CMaterialList.hpp"

#include "rstl/optional_object.hpp"
#include "rstl/reserved_vector.hpp"
#include "rstl/single_ptr.hpp"

class CGenDescription;
class CGunController;
class CAnimCharacterSet;
class CWeaponDescription;
class CElementGen;
class CRainSplashGenerator;
class CModelFlags;
class CActorLights;
struct SWeaponInfo;

#ifdef TARGET_PC
namespace NPortChargeFxDetail {
// Retail effects take the locator's translation only. A converted effect's vectors are already in
// the port's space (the importer turns Remastered's X left, Y up, Z forward particle space into it,
// CVEPortBasis), so it takes the full sampled locator: the Power charge disc's EMTR, Remastered
// (0,0,.15) and port (0,.15,0), lands .30 forward at scale 2, as on native's LBEAM.
template < typename Generator >
void SetLocalLocator(Generator& generator, const CTransform4f& locator) {
  if (!generator.PortIsRemastered()) {
    generator.SetGlobalTranslation(locator.GetTranslation());
  } else {
    // The sampled locator itself, no copy.
    generator.SetGlobalOrientAndTrans(locator);
  }
}
} // namespace NPortChargeFxDetail
#endif

enum EFrozenFxType {
  kFFT_None,
  kFFT_Frozen,
  kFFT_Thawed,
};

class CVelocityInfo {
public:
  ~CVelocityInfo();

  CVector3f& Velocity(int i) { return x0_vel[i]; }
  const CVector3f& GetVelocity(int i) const { return x0_vel[i]; }
  bool GetTargetHoming(int i) const { return x1c_targetHoming[i]; }

  void Clear();

  void AddVelocity(const CVector3f& vel) { x0_vel.push_back(vel); }
#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
  void AddTargetHoming(const bool homing) { x1c_targetHoming.push_back(homing); }
#else
  void AddTargetHoming(const bool& homing) { x1c_targetHoming.push_back(homing); }
#endif
  void AddTrat(const float& trat) { x24_trat.push_back(trat); }

private:
  rstl::reserved_vector< CVector3f, 2 > x0_vel;
  rstl::reserved_vector< bool, 2 > x1c_targetHoming;
  rstl::reserved_vector< float, 2 > x24_trat;
};

class CGunWeapon {
  friend class CPlayerGun;

public:
  CGunWeapon(CAssetId ancsId, EWeaponType type, TUniqueId playerId, EMaterialTypes playerMaterial,
             const CVector3f& scale);
  virtual ~CGunWeapon();

  enum ESecondaryFxType {
    kSFT_None,
    kSFT_Charge,
    kSFT_ToCombo,
    kSFT_CancelCharge,
  };

  // Virtual Methods
  virtual void Reset(CStateManager& mgr);
  virtual void PlayAnim(const NWeaponTypes::EGunAnimType type, bool loop);
  virtual void PreRenderGunFx(const CStateManager& mgr, const CTransform4f& xf) {}
  virtual void PostRenderGunFx(const CStateManager& mgr, const CTransform4f& xf);
  virtual void UpdateGunFx(const bool shotSmoke, const float dt, const CStateManager& mgr,
                           const CTransform4f& xf);
  virtual void Fire(const bool underwater, const float dt,
                    const CPlayerState::EChargeStage chargeState, const CTransform4f& xf,
                    CStateManager& mgr, const TUniqueId homingTarget, const float chargeFactor1,
                    const float chargeFactor2);
#ifdef TARGET_PC
  // Port (stack_beams): only the projectile(s) a Fire would spawn, no animation, shake,
  // charge state or sound. Skips when this beam's shot data is not loaded.
  virtual void PortFireStackedProjectile(const bool underwater, const float dt,
                                         const CPlayerState::EChargeStage chargeState,
                                         const CTransform4f& xf, CStateManager& mgr,
                                         const TUniqueId homingTarget, const float chargeFactor1,
                                         const float chargeFactor2);
  // Locks this beam's projectile descriptions independently of Load/Unload (idempotent).
  void PortLockStackTokens();
  void PortUnlockStackTokens();
  bool PortStackTokensLocked() const { return mPortStackWeapons.size() != 0; }
  // True when the Muzzle PART is Remastered: normal shots then start mPortMuzzleFlash
  // (CGunWeaponMP1::Fire 0xca2d3c / StartMuzzleFlash 0xca31c4) and muzzle generator 0
  // is neither updated nor drawn.
  bool PortRemasteredMuzzle() const;
  void PortStartMuzzleFlash();
  // CGunWeaponMP1::UpdateMuzzleFlash 0xca1e74.
  void PortUpdateMuzzleFlash(float dt, const CVector3f& scale, const CTransform4f& xf);
#endif
  virtual void EnableFx(const bool enable) {}
  virtual void EnableSecondaryFx(const ESecondaryFxType type) {
    x1cc_enabledSecondaryEffect = type;
  }
  virtual void Draw(const bool drawSuitArm, const CStateManager& mgr, const CTransform4f& xf,
                    const CModelFlags& flags, const CActorLights* lights) const;
  virtual void DrawMuzzleFx(const CStateManager& mgr) const;
  virtual void Update(const float dt, CStateManager& mgr);
  virtual void Load(CStateManager& mgr, const bool subtypeBasePose);
  virtual void Unload(CStateManager& mgr);
  virtual bool IsLoaded() const;

  const CVelocityInfo& GetVelocityInfo() const { return x1d0_velInfo; }
  rstl::optional_object< CModelData >& SolidModelData() { return x10_solidModelData; }
  const CModelData& GetSolidModelData() const { return x10_solidModelData.data(); }

  EWeaponType GetType() const { return x1c0_weaponType; }
  TUniqueId GetPlayerId() const { return x1c4_playerId; }
  EMaterialTypes GetPlayerMaterial() const { return x1c8_playerMaterial; }

  CAABox GetBounds() const;
  CAABox GetBounds(const CTransform4f& xf) const;
  const SWeaponInfo& GetWeaponInfo() const;
  void ActivateCharge(bool enable, bool resetEffect);
  bool IsCharged() const { return x218_25_enableCharge; }
  void EnableCharge(bool enable) { x218_25_enableCharge = enable; }
  bool PlayPasAnim(SamusGun::EAnimationState state, CStateManager& mgr, float angle);
  bool IsChargeAnimOver() const;
  void UpdateMuzzleFx(const float dt, const CVector3f& scale, const CVector3f& pos,
                      const bool emitting);
#ifdef TARGET_PC
  void UpdateMuzzleFx(const float dt, const CVector3f& scale, const CTransform4f& locator,
                      const bool emitting);
#endif
  CElementGen* GetChargeMuzzleFx() const;
  void DrawHologram(const CStateManager& mgr, const CTransform4f& xf,
                    const CModelFlags& flags) const;
#ifdef TARGET_PC
  void PortDrawHologramTransition(const CStateManager& mgr, const CTransform4f& xf,
                                  const CVector3f& normal, const CVector3f& point,
                                  const CModelFlags& flags, const CActorLights* lights) const;
  CTransform4f PortDreadCannonXf(const CTransform4f& xf) const;
  CVector3f PortDreadMuzzleShift() const;
#endif
  void ReturnToDefault(CStateManager& mgr);
  bool ComboFireOver() const;
  void EnterFidget(CStateManager& mgr, SamusGun::EFidgetType type, int parm2);
  void Touch(const CStateManager& mgr);
  void TouchHolo(const CStateManager& mgr);
  void AsyncLoadSuitArm(CStateManager& mgr);
  void AsyncLoadFidget(CStateManager& mgr, SamusGun::EFidgetType type, int animSet);
  void UnLoadFidget();
  bool IsFidgetLoaded();
  void EnableFrozenEffect(EFrozenFxType type);

  CDamageInfo GetDamageInfo(CStateManager& mgr, CPlayerState::EChargeStage chargeState,
                            float chargeFactor);
  static const char* GetMuzzleLocator() { return skMuzzleLocator; }

protected:
  // x0 is vtable
  CVector3f x4_scale;
  rstl::optional_object< CModelData > x10_solidModelData;
  rstl::optional_object< CModelData > x60_holoModelData;
  rstl::optional_object< CModelData > xb0_suitArmModelData;
  rstl::single_ptr< CGunController > x100_gunController;
  TToken< CAnimCharacterSet > x104_gunCharacter;
  rstl::vector< CToken > x10c_anims;
  rstl::vector< CToken > x11c_unk;
  rstl::vector< CToken > x12c_deps;
  TToken< CAnimCharacterSet > x13c_armCharacter;
  rstl::reserved_vector< TCachedToken< CWeaponDescription >, 2 > x144_weapons;
  TCachedToken< CGenDescription > x160_xferEffect;
  rstl::reserved_vector< TCachedToken< CGenDescription >, 2 > x16c_muzzleEffects;
  rstl::reserved_vector< TCachedToken< CGenDescription >, 2 > x188_frozenEffects;
  rstl::reserved_vector< rstl::auto_ptr< CElementGen >, 2 > x1a4_muzzleGenerators;
  rstl::single_ptr< CElementGen > x1b8_frozenGenerator;
  CRainSplashGenerator* x1bc_rainSplashGenerator;
  EWeaponType x1c0_weaponType;
  TUniqueId x1c4_playerId;
  EMaterialTypes x1c8_playerMaterial;
  ESecondaryFxType x1cc_enabledSecondaryEffect;
  CVelocityInfo x1d0_velInfo;
  CPlayerState::EBeamId x200_beamId;
  EFrozenFxType x204_frozenEffect;
  uint x208_muzzleEffectIdx;
  uint x20c_shaderIdx;
  // 0x1: load request, 0x2: muzzle fx, 0x4: projectile data, 0x8: anims, 0x10: everything else
  int x210_loadFlags;
  CAssetId x214_ancsId;
  bool x218_24 : 1;
  bool x218_25_enableCharge : 1;
  bool x218_26_loaded : 1;
  // Initialize in selected beam's pose, rather than power beam's pose
  bool x218_27_subtypeBasePose : 1;
  bool x218_28_suitArmLocked : 1;
  bool x218_29_drawHologram : 1;

  static const char* const skMuzzleLocator;
  static const char* const skElbowLocator;
  static const int skShootAnim[2];
  static const char* const skMuzzleNames[10];
  static const char* const skFrozenNames[10];
  static const char* const skBeamXferNames[5];
  static const char* const skAnimDependencyNames[5];
  static const char* const skDependencyNames[5];
  static const char* const skSuitArmNames[8];
  static const int skAnimTypeList[11];

  void AllocResPools(CPlayerState::EBeamId beam);
  void FreeResPools();
  static void FillTokenVector(const rstl::vector< SObjectTag >& tags,
                              rstl::vector< CToken >& objects, bool includeTxtr);
  void BuildDependencyList(CPlayerState::EBeamId beam);
  void LoadSuitArm(CStateManager& mgr);
  void LoadGunModels(CStateManager& mgr);
  void LoadAnimations();
  bool IsAnimsLoaded() const;
  void LoadMuzzleFx(float dt);
  void LoadProjectileData(CStateManager& mgr);
  void LoadFxIdle(float dt, CStateManager& mgr);
  void LockTokens(CStateManager& mgr);
  void UnlockTokens();

  static void PointGenerator(void*, const CVector3f*, const CVector3f*, int);

  // The projectile part of Fire, for the shot's `weapon` description.
  void SpawnProjectile(const TToken< CWeaponDescription >& weapon, bool underwater, float dt,
                       CPlayerState::EChargeStage chargeState, const CTransform4f& xf,
                       CStateManager& mgr, TUniqueId homingTarget, float chargeFactor1,
                       float chargeFactor2);
#ifdef TARGET_PC
  // The stacked copy of x144_weapons[chargeState], null unless locked and loaded.
  const TToken< CWeaponDescription >* PortStackedWeapon(int chargeState);

  // Locked copies of x144_weapons; empty while stacking is off.
  rstl::reserved_vector< TCachedToken< CWeaponDescription >, 2 > mPortStackWeapons;

  // Remastered's normal-shot muzzle module (CGunWeaponMP1 +0x588, from the Muzzle PART),
  // used in place of muzzle generator 0 when that PART is Remastered.
  CPortEffectSet mPortMuzzleFlash;

  // xb0_suitArmModelData holds the Dread cannon (port_dread_samus_mod.h): it is drawn in place
  // of the gun model, which it carries along with the arm.
  bool mPortDreadCannon = false;
#endif
};
CHECK_SIZEOF(CGunWeapon, 0x21c)

#endif // _CGUNWEAPON
