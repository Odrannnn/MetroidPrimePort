#ifndef _CPARTICLEGEN
#define _CPARTICLEGEN

#include "Kyoto/SObjectTag.hpp"

#include "rstl/list.hpp"
#include "rstl/optional_object.hpp"
#include "rstl/pair.hpp"

class CAABox;
class CColor;
class CLight;
class CTransform4f;
class CVector3f;
class CWarp;

#ifdef TARGET_PC
struct PortFxInfo;
class CParticleGen;
namespace PortFx {
void Register(CParticleGen* gen);
void Unregister(CParticleGen* gen);
} // namespace PortFx
#endif

class CParticleGen {
public:
#ifdef TARGET_PC
  // Debug registry (port_fx_debug.cpp): every generator is in one intrusive list.
  CParticleGen() { PortFx::Register(this); }
  // A copy is a new generator with its own registry slot; assignment leaves the links alone.
  CParticleGen(const CParticleGen& o)
  : x4_modifiersList(o.x4_modifiersList), xPortInheritedShutdownMode(o.xPortInheritedShutdownMode) {
    PortFx::Register(this);
  }
  CParticleGen& operator=(const CParticleGen& o) {
    x4_modifiersList = o.x4_modifiersList;
    return *this;
  }
  virtual uint PortFxAsset() const { return 0; }
  virtual void PortFxDescribe(PortFxInfo&) const {}
  CParticleGen* xPortFxPrev = nullptr;
  CParticleGen* xPortFxNext = nullptr;
  uint xPortFxId = 0;
  // Remastered's live shutdown mode (gen+0x98): a parent with OSDM hands its own to each child it
  // constructs (ConstructChildParticleSystem 0x27b1c0); -1 = none handed, the generator's own SHTM.
  int xPortInheritedShutdownMode = -1;
  // The generator's own SHTM (CParticleGen ctor 0x279784 reads desc+0xcc).
  virtual u32 PortOwnShutdownMode() const { return 0; }
  u32 PortShutdownMode() const {
    return xPortInheritedShutdownMode >= 0 ? u32(xPortInheritedShutdownMode) : PortOwnShutdownMode();
  }
#endif
  virtual ~CParticleGen() = 0;
  virtual const bool Update(double) = 0;
  virtual void Render() = 0;
  virtual void SetOrientation(const CTransform4f& orientation) = 0;
  virtual void SetTranslation(const CVector3f& translation) = 0;
  virtual void SetGlobalOrientation(const CTransform4f& orientation) = 0;
  virtual void SetGlobalTranslation(const CVector3f& translation) = 0;
  virtual void SetGlobalScale(const CVector3f& scale) = 0;
  virtual void SetLocalScale(const CVector3f& scale) = 0;
  virtual void SetParticleEmission(bool emission) = 0;
  virtual void SetModulationColor(const CColor& col) = 0;
  virtual void SetGeneratorRate(float rate) {}
  virtual const CTransform4f& GetOrientation() const = 0;
  virtual const CVector3f& GetTranslation() const = 0;
  virtual const CTransform4f& GetGlobalOrientation() const = 0;
  virtual const CVector3f& GetGlobalTranslation() const = 0;
  virtual const CVector3f& GetGlobalScale() const = 0;
  virtual bool GetParticleEmission() const = 0;
  virtual const CColor& GetModulationColor() const = 0;
  virtual float GetGeneratorRate() const { return 1.f; }
  virtual bool IsSystemDeletable() const = 0;
  virtual rstl::optional_object< CAABox > GetBounds() const = 0;
  virtual int GetParticleCount() const = 0;
  virtual bool SystemHasLight() const = 0;
  virtual CLight GetLight() const = 0;
  virtual void DestroyParticles() = 0;
#ifdef TARGET_PC
  // The owner ends the effect (Remastered's CParticleGen::ShutdownEffect, vtable +0x98): stop
  // emitting. CElementGen adds the description's SHTM (kill live particles) and SHTT (delay).
  virtual void ShutdownEffect() { SetParticleEmission(false); }
  // Scales where new particles (or bolt end points) spawn in the system's local axes, without
  // scaling the particles themselves: HUD effects use it to cover a view wider than 4:3. `origin`
  // is the parent's translation: a child placed off it (SSPO/SEPO) has its offset spread too.
  virtual void PortSetSpawnSpread(const CVector3f& spread, const CVector3f& origin) {}
  // Remastered's sort bias (SBIS), added to the view depth the renderer sorts the generator by.
  virtual float PortSortBias() const { return 0.f; }
  // Remastered's DoShutdownBehavior (vtable +0x1e8), which a parent with ISDT calls on its children:
  // mode 0xff is the generator's own SHTM, acting at once (no SHTT countdown); 0 and 1 stop emitting.
  virtual void PortDoShutdownBehavior(u32 mode) {
    if ((mode == 0xff ? PortShutdownMode() : (mode & 0xff)) < 2) {
      SetParticleEmission(false);
    }
  }
#endif
  virtual void AddModifier(CWarp*);
  virtual uint Get4CharId() const = 0;

  static FourCC ResType() { return 'PART'; }

protected:
  rstl::list< CWarp* > x4_modifiersList;
};

inline CParticleGen::~CParticleGen() {
#ifdef TARGET_PC
  PortFx::Unregister(this);
#endif
}

#endif // _CPARTICLEGEN
