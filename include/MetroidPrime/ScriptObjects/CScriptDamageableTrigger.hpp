#ifndef _CSCRIPTDAMAGEABLETRIGGER
#define _CSCRIPTDAMAGEABLETRIGGER

#include "MetroidPrime/CActor.hpp"
#include "MetroidPrime/CDamageVulnerability.hpp"
#include "MetroidPrime/CHealthInfo.hpp"

#include "MetroidPrime/CFluidPlaneDoor.hpp"

#include "Kyoto/Math/CAABox.hpp"
#include "Kyoto/Math/CFrustumPlanes.hpp"

class CVisorParameters;

class CScriptDamageableTrigger : public CActor {
  friend struct PortEntitySchema; // save-state field schema (platform/include/port_entity_schema.h)
public:
  enum ECanOrbit {
    kCO_NoOrbit,
    kCO_Orbit,
  };

  ~CScriptDamageableTrigger() override;
  CScriptDamageableTrigger(TUniqueId uid, const rstl::string& name, const CEntityInfo& info,
                           const CVector3f& position, const CVector3f& extent,
                           const CHealthInfo& hInfo, const CDamageVulnerability& dVuln,
                           uint faceFlag, CAssetId patternTex1, CAssetId patternTex2,
                           CAssetId colorTex, ECanOrbit canOrbit, bool active,
                           const CVisorParameters& vParams);

  DECLARE_TYPES_MATCH_OR_ACCEPT;
  void AcceptScriptMsg(EScriptObjectMessage, TUniqueId, CStateManager&) override;
  EWeaponCollisionResponseTypes GetCollisionResponseType(const CVector3f&, const CVector3f&,
                                                         const CWeaponMode&, int) const override;
  void Render(const CStateManager& mgr) const override;
  void AddToRenderer(const CFrustumPlanes& frustum, const CStateManager& mgr) const override;
#ifdef TARGET_PC
  void PortAddThermalColdNode(const CFrustumPlanes&, const CStateManager&) const override;
#endif
  void PreRender(CStateManager& mgr, const CFrustumPlanes& frustum) override;
  const CDamageVulnerability* GetDamageVulnerability() const override;
  CHealthInfo* HealthInfo(CStateManager&) override;
  void Think(float, CStateManager&) override;
  rstl::optional_object< CAABox > GetTouchBounds() const override;
  // The alpha its MaxReached -> Activate actors are drawn at (for the port's room geometry).
  float PortLinkedAlpha() const { return GetPuddleAlphaScale(); }
#ifdef TARGET_PC
  // Beam doors that stay unlocked (PortBeamDoors): the force field of a door
  // that is already loaded takes another door type's weaknesses and textures.
  void PortRestyle(const CDamageVulnerability& vuln, CAssetId patternTex1, CAssetId patternTex2,
                   CAssetId colorTex);
#endif

private:
  CFrustumPlanes xe8_frustum;
  CAABox x14c_bounds;
  CHealthInfo x164_origHInfo;
  CHealthInfo x16c_hInfo;
  CDamageVulnerability x174_dVuln;
  uint x1dc_faceFlag;
  float x1e0_alpha;
  CTransform4f x1e4_faceDir;
  CTransform4f x214_faceDirInv;
  CVector3f x244_faceTranslate;
  float x250_alphaTimer;
  CFluidPlaneDoor x254_fluidPlane;
  bool x300_24_notOccluded : 1;
  bool x300_25_alphaOut : 1;
  bool x300_26_outOfFrustum : 1;
  bool x300_27_invulnerable : 1;
  bool x300_28_canOrbit : 1;

  void SetLinkedObjectAlpha(float a, CStateManager& mgr);
  float GetPuddleAlphaScale() const;
  const CFluidPlane& GetFluidPlane() const { return x254_fluidPlane; }
  const CFrustumPlanes& GetFrustumPlanes() const { return xe8_frustum; }
};
CHECK_SIZEOF(CScriptDamageableTrigger, (VERSION >= VERSION_GM8P_00 ? 0x318 : 0x308))

#endif // _CSCRIPTDAMAGEABLETRIGGER
