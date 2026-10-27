#ifndef _CPLASMAPROJECTILE
#define _CPLASMAPROJECTILE

#include "MetroidPrime/Weapons/CBeamInfo.hpp"
#include "MetroidPrime/Weapons/CBeamProjectile.hpp"
#include "MetroidPrime/Weapons/CWeaponAssetInfo.hpp"

#ifdef TARGET_PC
#include "Kyoto/Particles/CParticleElectric.hpp"
#include "port_tick_pair.h"
#endif

class CElectricDescription;

class CPlasmaProjectile : public CBeamProjectile {
  friend struct PortEntitySchema; // save-state field schema (platform/include/port_entity_schema.h)
public:
  enum EExpansionState { kES_Inactive, kES_Attack, kES_Sustain, kES_Release, kES_Done };

  CPlasmaProjectile(const TToken< CWeaponDescription >& wDesc, const rstl::string& name,
                    EWeaponType wType, const CBeamInfo& bInfo, const CTransform4f& xf,
                    EMaterialTypes matType, const CDamageInfo& dInfo, TUniqueId uid, TAreaId aid,
                    TUniqueId owner, const CWeaponAssetInfo& res, bool growingBeam,
                    EProjectileAttrib attribs);

  DECLARE_TYPES_MATCH_OR_ACCEPT;
  void AcceptScriptMsg(EScriptObjectMessage msg, TUniqueId sender, CStateManager& mgr) override;
  void ResetBeam(CStateManager& mgr, bool fullReset) override;
  void UpdateFx(const CTransform4f& xf, float dt, CStateManager& mgr) override;
  void Fire(const CTransform4f& xf, CStateManager& mgr, bool b) override;
  void Touch(CActor& other, CStateManager& mgr) override;
  bool CanRenderUnsorted(const CStateManager& mgr) const override;
  void AddToRenderer(const CFrustumPlanes& frustum, const CStateManager& mgr) const override;
  void Render(const CStateManager& mgr) const override;
  CColor GetInnerColor() const { return x490_innerColor; }
  CColor GetOuterColor() const { return x494_outerColor; }
  bool IsFiring() const { return x548_26_firing; }
  void SetPlayerSustainedDamage(float duration, const CDamageInfo& damage);

private:
  static const int kMaxPlasmaLights;
  static const float kInvMaxPlasmaLights;

  float UpdateBeamState(float dt, CStateManager& mgr);
  void MakeBillboardEffect(const rstl::optional_object< TToken< CGenDescription > >& particle,
                           const rstl::optional_object< TToken< CElectricDescription > >& electric,
                           const rstl::string& name, CStateManager& mgr);
  void UpdatePlayerEffects(float dt, CStateManager& mgr);
  void RenderBeam(int subdivs, float width, const CColor& color, int flags) const;
  void RenderMotionBlur() const;
  void UpdateEnergyPulse(float dt);
  void SetLightsActive(bool active, CStateManager& mgr);
  void CreatePlasmaLights(uint sourceId, const CLight& light, CStateManager& mgr);
  void DeletePlasmaLights(CStateManager& mgr);
  void UpdateLights(float expansion, float dt, CStateManager& mgr);

  rstl::vector< TUniqueId > x468_lights;
  int x478_beamAttributes;
  float x47c_lifeTime;
  float x480_pulseSpeed;
  float x484_shutdownTime;
  float x488_expansionSpeed;
  float x48c_maxLength;
  CColor x490_innerColor;
  CColor x494_outerColor;
  CDamageInfo x498_phazonDamage;
  EExpansionState x4b4_expansionState;
  float x4b8_beamWidth;
  float x4bc_lifeTimer;
  float x4c0_expansionT;
  float x4c4_expansion;
  float x4c8_beamAngle;
  float x4cc_energyPulseStartY;
  float x4d0_shutdownTimer;
  float x4d4_contactPulseTimer;
  float x4d8_energyPulseTimer;
  float x4dc_playerEffectPulseTimer;
  float x4e0_playerDamageDuration;
  float x4e4_playerDamageTimer;
  TCachedToken< CTexture > x4e8_texture;
  TCachedToken< CTexture > x4f4_glowTexture;
  TLockedToken< CGenDescription > x500_contactFxDesc;
  TLockedToken< CGenDescription > x50c_pulseFxDesc;
  rstl::single_ptr< CElementGen > x518_contactGen;
  rstl::single_ptr< CElementGen > x51c_pulseGen;
  rstl::single_ptr< CElementGen > x520_weaponGen;
  CAssetId x524_freezeSteamTxtr;
  CAssetId x528_freezeIceTxtr;
  rstl::optional_object< TToken< CElectricDescription > > x52c_visorElectric;
  rstl::optional_object< TToken< CGenDescription > > x538_visorParticle;
  u16 x544_freezeSfx;
  u16 x546_electricSfx;
  bool x548_24_ : 1;
  bool x548_25_enableEnergyPulse : 1;
  bool x548_26_firing : 1;
  bool x548_27_texturesLoaded : 1;
  bool x548_28_drawOwnerFirst : 1;
  bool x548_29_activePlayerPhazon : 1;
#ifdef TARGET_PC
  // Presentation smoothing (particle_interpolation): the beam the last two
  // ticks drew, and the length RenderBeam uses for the blended draw.
  struct SPortBeam {
    SPortBeam() : xf(CTransform4f::Identity()), length(0.f), width(0.f), angle(0.f) {}
    CTransform4f xf;
    float length;
    float width;
    float angle;
  };
  mutable PortTickPair< SPortBeam > xPortBeam;
  mutable float xPortDrawLength;

public:
  // Remastered CPlasmaProjectileMP1's beam electric (this+2000, built in its ctor from the
  // beam info's ELC2 id at 0x7a4): an ELC2 drawn from the beam's start to its end.
  void PortSetBeamElectric(CAssetId id);
  // The frozen VFX of CPlasmaProjectileMP1's SPlayerFrozenStateDataShip1 (+0x764, id +0x10),
  // built in its ctor from the weapon asset info's +0x64.
  void PortSetFrozenFx(CAssetId id) { xPortFrozenFxId = id; }

private:
  CAssetId xPortFrozenFxId = kInvalidAssetId;
  void PortResetBeamElectric();
  rstl::single_ptr< CParticleElectric > xPortBeamElectric;
#endif
};
CHECK_SIZEOF(CPlasmaProjectile, (VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02 ? 0x578
                                 : VERSION >= VERSION_GM8P_00                             ? 0x560
                                                                                          : 0x550))

#endif // _CPLASMAPROJECTILE
