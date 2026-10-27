#ifndef _CSCRIPTSTEAM
#define _CSCRIPTSTEAM

#include "MetroidPrime/ScriptObjects/CScriptTrigger.hpp"

#ifdef TARGET_PC
namespace PortRoomEffects {
struct Steam;
}
#endif

class CScriptSteam : public CScriptTrigger {
  friend struct PortEntitySchema; // save-state field schema (platform/include/port_entity_schema.h)
public:
  CScriptSteam(TUniqueId, const rstl::string& name, const CEntityInfo& info, const CVector3f& pos,
               const CAABox&, const CDamageInfo& dInfo, const CVector3f& orientedForce,
               unsigned int flags, bool active, CAssetId, float, float, float, float, bool);
  ~CScriptSteam();

  void AcceptScriptMsg(EScriptObjectMessage, TUniqueId, CStateManager&);
  void Think(float, CStateManager&);
  DECLARE_TYPES_MATCH_OR_ACCEPT;
#ifdef TARGET_PC
  struct PortShip1;
  // Port: take Remastered's visor steam, from the SteamMP1 a mod's .roomfx paired with this
  // object (PortRoomEffects::FindSteam).
  void PortSetShip1(const PortRoomEffects::Steam& steam);
#endif

private:
#ifdef TARGET_PC
  PortShip1* xPortShip1 = nullptr;
  void PortThinkShip1(CStateManager& mgr);
#endif
  bool x150_;
  CAssetId x154_texture;
  float x158_strength;
  float x15c_alphaInDur;
  float x160_alphaOutDur;
  float x164_maxDist;
  float x168_ooMaxDist;

  float GetMaxDist() const { return x164_maxDist; }
  float GetStrength() const { return x158_strength; }
  float GetOOMaxDist() const { return x168_ooMaxDist; }
  CAssetId GetTextureId() const { return x154_texture; }
  float GetFadeOutRate() const { return x160_alphaOutDur; }
  float GetFadeInRate() const { return x15c_alphaInDur; }
};

#endif // _CSCRIPTSTEAM
