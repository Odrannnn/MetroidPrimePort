#ifndef _CHUDBILLBOARDEFFECT
#define _CHUDBILLBOARDEFFECT

#include "MetroidPrime/CEffect.hpp"

class CGenDescription ;
class CElectricDescription;
class CParticleGen;

class CHUDBillboardEffect : public CEffect {
  friend struct PortEntitySchema; // save-state field schema (platform/include/port_entity_schema.h)
public:
  CHUDBillboardEffect(const rstl::optional_object< TToken< CGenDescription > >& particle,
                      const rstl::optional_object< TToken< CElectricDescription > >& electric,
                      TUniqueId uid, bool active, const rstl::string& name, float dist,
                      const CVector3f& scale0, const CColor& color, const CVector3f& scale1,
                      const CVector3f& translation);
  ~CHUDBillboardEffect() override;

  DECLARE_TYPES_MATCH_OR_ACCEPT;
  void Think(float dt, CStateManager& mgr) override;
  void PreRender(CStateManager& mgr, const CFrustumPlanes& frustum) override;
  void AddToRenderer(const CFrustumPlanes& frustum, const CStateManager& mgr) const override;
  void Render(const CStateManager& mgr) const override;
  bool IsElementGen() const { return x104_26_isElementGen; }
  void SetRunIndefinitely(bool b) { x104_27_runIndefinitely = b; }
  CParticleGen* GetParticleGen() const { return xe8_generator.get(); }

  static float GetNearClipDistance(const CStateManager& mgr);
  static const CVector3f& GetScaleForPOV(const CStateManager& mgr);
#ifdef TARGET_PC
  void AcceptScriptMsg(EScriptObjectMessage msg, TUniqueId uid, CStateManager& mgr) override;
  // Remastered's visor goo splat manager (CVisorEffectManagerMP1, managed effect type 4).
  // HasActiveManagedEffect(4) / SetManagedEffect(4, id) / AddVisorGooSplat 0xd1078c.
  static bool PortHasVisorGoo(CStateManager& mgr);
  static void PortSetVisorGoo(CStateManager& mgr, TUniqueId uid, float dt);
  static void PortAddVisorGooSplat(CStateManager& mgr);
  // CVisorEffectManagerMP1::Update over the billboards whose PART has visor effect data.
  static void PortUpdateVisorOverrides(CStateManager& mgr, float dt);
  // CHUDBillboardEffectMP1::EScaleType, the CreateHUDBillboardEffect argument (0xd7e67c): 1 at
  // every caller but SetMorphBallState's unmorph runoff (0), whose effect stays in HUD units.
  void PortSetScaleType(int type) { xPortScaleType = type; }
#endif

private:
  rstl::single_ptr< CParticleGen > xe8_generator;
  CVector3f xec_translation;
  CVector3f xf8_localScale;
  bool x104_24_renderAsParticleGen : 1l; // = true;
  bool x104_25_enableRender : 1l; // = false;
  bool x104_26_isElementGen : 1l; // = false;
  bool x104_27_runIndefinitely : 1l; // = false;
  float x108_timeoutTimer; // = 0.f;
#ifdef TARGET_PC
  // Remastered's managed effect type: 4 = the visor goo manager's billboard, -1 none.
  int xPortManagedType = -1;
  // Added to the visor effect override list (AddBillboardEffect).
  bool xPortVisorOverride = false;
  // The spread last given to the generator, which passes it down to its children.
  CVector3f xPortSpawnSpread{1.f, 1.f, 1.f};
  int xPortScaleType = 1;
  // The HUD camera's visor plane depth (FRME_BaseHud basewidget_visorFX), aspect and
  // tan(fov / 2) (camera_default, fov 65).
  static constexpr float kPortHUDDepth = 2.2311f;
  static constexpr float kPortHUDAspect = 16.f / 9.f;
  static constexpr float kPortHUDTanHalfFov = 0.637070f;
  bool PortIsRemasteredHUD() const;
  void PortUpdateSpawnSpread(const CStateManager& mgr);
  void PortApplySpawnSpread(const CVector3f& spread);
#endif

  static int g_IndirectTexturedBillboardCount;
  static int g_BillboardCount;

  static float CalcGenRate();
};

#endif // _CHUDBILLBOARDEFFECT
