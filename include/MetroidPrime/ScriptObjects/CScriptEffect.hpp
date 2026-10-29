#ifndef _CSCRIPTEFFECT
#define _CSCRIPTEFFECT

#include "MetroidPrime/CActor.hpp"
#ifdef TARGET_PC
#include "MetroidPrime/CPortNestedLights.hpp"
#include "Kyoto/Particles/CPortParticleVars.hpp"
#endif

class CLightParameters;
class CElectricDescription;
class CParticleElectric;
class CGenDescription;
class CElementGen;
class CScriptEffect : public CActor {
  friend struct PortEntitySchema; // save-state field schema (platform/include/port_entity_schema.h)
public:
  ~CScriptEffect() override;
  DECLARE_TYPES_MATCH_OR_ACCEPT;
  void Think(float dt, CStateManager& mgr) override;
  void AcceptScriptMsg(EScriptObjectMessage msg, TUniqueId uid, CStateManager& mgr) override;
  void SetActive(bool active) override;
  void PreRender(CStateManager& mgr, const CFrustumPlanes& frustum) override;
  void AddToRenderer(const CFrustumPlanes& frustum, const CStateManager& mgr) const override;
  void Render(const CStateManager& mgr) const override;
  bool CanRenderUnsorted(const CStateManager& mgr) const override;
  void CalculateRenderBounds() override;
  CAABox GetSortingBounds(const CStateManager& mgr) const override;

  CScriptEffect(TUniqueId uid, const rstl::string& name, const CEntityInfo& info,
                const CTransform4f& xf, const CVector3f& scale, CAssetId partId, CAssetId elscId,
                bool hotInThermal, bool noTimerUnlessAreaOccluded, bool rebuildSystemsOnActivate,
                bool active, bool useRateInverseCamDist, float rateInverseCamDist,
                float rateInverseCamDistRate, float duration, float durationResetWhileVisible,
                bool useRateCamDistRange, float rateCamDistRangeMin, float rateCamDistRangeMax,
                float rateCamDistRangeFarRate, bool combatVisorVisible, bool thermalVisorVisible,
                bool xrayVisorVisible, const CLightParameters& lParms, bool dieWhenSystemsDone);

  static void ResetParticleCounts();
#ifdef TARGET_PC
  // A Remastered effect actor's values for its system's particle variables (its
  // InitializeExternalVariables); kept, as a rebuilt system starts from the defaults again.
  void PortSetExternalVars(const std::vector< SPortExternalVar >& vars);
  // Updates only this real variable, including the value used after a system rebuild.
  bool PortSetExternalReal(const PortGuid& guid, float value);
  CElementGen* PortParticleSystem() const { return x104_particleSystem.get(); }
  CAssetId PortPartId() const { return x10c_partId; }
  void PortAttachBarrier(TUniqueId controller, TUniqueId platform) {
    xPortBarrierController = controller;
    xPortBarrierPlatform = platform;
  }
  void PortFollowBarrier(const CActor& source);
#endif

private:
  bool AreBothSystemsDeleteable() const;
  static uint mNumParticlesUpdating;
  static uint mNumParticlesDrawing;

  TLockedToken< CElectricDescription > xe8_electricToken;
  rstl::single_ptr< CParticleElectric > xf4_electric;
  TLockedToken< CGenDescription > xf8_particleSystemToken;
  rstl::single_ptr< CElementGen > x104_particleSystem;
  TUniqueId x108_lightId;
  CAssetId x10c_partId;
  bool x110_24_enable : 1;
  bool x110_25_noTimerUnlessAreaOccluded : 1;
  bool x110_26_rebuildSystemsOnActivate : 1;
  bool x110_27_useRateInverseCamDist : 1;
  bool x110_28_combatVisorVisible : 1;
  bool x110_29_thermalVisorVisible : 1;
  bool x110_30_xrayVisorVisible : 1;
  bool x110_31_anyVisorVisible : 1;
  bool x111_24_useRateCamDistRange : 1;
  bool x111_25_dieWhenSystemsDone : 1;
  bool x111_26_canRender : 1;
  float x114_rateInverseCamDist;
  float x118_rateInverseCamDistSq;
  float x11c_rateInverseCamDistRate;
  float x120_rateCamDistRangeMin;
  float x124_rateCamDistRangeMax;
  float x128_rateCamDistRangeFarRate;
  mutable float x12c_remTime;
  float x130_duration;
  float x134_durationResetWhileVisible;
  rstl::single_ptr< CActorLights > x138_actorLights;
  TUniqueId x13c_triggerId;
  float x140_destroyDelayTimer;
#ifdef TARGET_PC
  CPortNestedLights xPortNestedLights; // lights of the nested PHDR children of x104_particleSystem
  std::vector< SPortExternalVar > xPortExternalVars;
  TUniqueId xPortBarrierController = kInvalidUniqueId;
  TUniqueId xPortBarrierPlatform = kInvalidUniqueId;
  // The followed platform's model bounds: the barrier mesh's own extent, which the
  // point-sized particle bounds miss (and which go stale once a culled effect stops updating).
  CAABox xPortBarrierBounds = CAABox::MakeMaxInvertedBox();
  bool xPortHasBarrierBounds = false;
#endif
};

CHECK_SIZEOF(CScriptEffect, (VERSION >= VERSION_GM8P_00 ? 0x158 : 0x148))

#endif // _CSCRIPTEFFECT
