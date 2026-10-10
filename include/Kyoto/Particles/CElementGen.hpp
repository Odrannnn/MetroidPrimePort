#ifndef _CELEMENTGEN
#define _CELEMENTGEN

#include "types.h"

#include "Kyoto/Particles/CPortParticleVars.hpp"
#include "Kyoto/CRandom16.hpp"
#include "Kyoto/Graphics/CColor.hpp"
#include "Kyoto/Graphics/CLight.hpp"
#include "Kyoto/Math/CAABox.hpp"
#include "Kyoto/Math/CMatrix3f.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "Kyoto/Particles/CParticleGen.hpp"
#include "Kyoto/TToken.hpp"

class CGenDescription;
class CModVectorElement;

#pragma cpp_extensions on
#ifdef TARGET_PC
struct CPortVfxMeshBatch;
#endif

class CElementGen : public CParticleGen {
public:
  struct CParticleListItem {
    ushort x0_partIdx;
    CVector3f x4_viewPoint;

    explicit CParticleListItem(short partIdx, const CVector3f& viewPoint)
    : x0_partIdx(partIdx), x4_viewPoint(viewPoint) {}
  };

  struct CTexturedParticleListItem {
    ushort x0_texMapIdx;
    ushort x2_partIdx;
    CVector3f x4_viewPoint;

    explicit CTexturedParticleListItem(short texMapIdx, short partIdx, const CVector3f& viewPoint)
    : x0_texMapIdx(texMapIdx), x2_partIdx(partIdx), x4_viewPoint(viewPoint) {}
  };

  enum EModelOrientationType {
    kMOT_Normal,
    kMOT_One,
  };
  enum EOptionalSystemFlags {
    kOSF_None,
    kOSF_One,
    kOSF_Two,
  };
  enum LightType {
    kLT_None = 0,
    kLT_Custom = 1,
    kLT_Directional = 2,
    kLT_Spot = 3,
  };
  struct CParticle {
    int x0_endFrame;
    CVector3f x4_pos;
    CVector3f x10_prevPos;
    CVector3f x1c_vel;
    int x28_startFrame;
    float x2c_lineLengthOrSize;
    float x30_lineWidthOrRota;
    CColor x34_color;
#ifdef TARGET_PC
    uint xPortSeed; // port-only: fixed per particle, mixes at spawn
    // port-only VMAT material (CPortVfxData), evaluated where SIZE/COLR are
    float xPortSsze;       // SSZE: secondary size (= SIZE when the PART has none)
    float xPortIten;       // ITEN: colour intensity
    float xPortColor[4];   // COLR unclamped (HDR), for the VFX paths; x34_color is the clamped one
    float xPortVpmt[4][4]; // VPMT rows
    CVector3f xPortLaunchDir; // unit launch velocity (zero if it launched at rest), for VORN 1
#endif

    CParticle()
    : x4_pos(CVector3f::Zero())
    , x10_prevPos(x4_pos)
    , x1c_vel(x10_prevPos)
    , x34_color(static_cast< u8 >(0xFF), 0x00, 0xFF, 0xFF)
#ifdef TARGET_PC
    , xPortSeed(0)
    , xPortSsze(0.f)
    , xPortIten(1.f)
    , xPortColor{1.f, 1.f, 1.f, 1.f}
    , xPortVpmt{}
    , xPortLaunchDir(CVector3f::Zero())
#endif
    {}
  };
  struct CAdvancedValues {
    float values[8];
  };

  CElementGen();
  CElementGen(TToken< CGenDescription >, EModelOrientationType = kMOT_Normal,
              EOptionalSystemFlags = kOSF_One);
  ~CElementGen() override;

  virtual const bool Update(double) override;
  virtual void Render() override;
  virtual void SetOrientation(const CTransform4f& orientation) override;
  virtual void SetTranslation(const CVector3f& translation) override;
  virtual void SetGlobalOrientation(const CTransform4f& orientation) override;
  virtual void SetGlobalTranslation(const CVector3f& translation) override;
  virtual void SetGlobalScale(const CVector3f& scale) override;
  virtual void SetLocalScale(const CVector3f& scale) override;
  virtual void SetParticleEmission(const bool emission) override;
  virtual void SetModulationColor(const CColor& col) override;
  virtual void SetGeneratorRate(float rate) override;
  virtual const CTransform4f& GetOrientation() const override { return x1d8_orientation; }
  virtual const CVector3f& GetTranslation() const override { return xdc_translation; }
  virtual const CTransform4f& GetGlobalOrientation() const override;
  virtual const CVector3f& GetGlobalTranslation() const override;
  virtual const CVector3f& GetGlobalScale() const override { return x100_globalScale; }
  virtual float GetGeneratorRate() const override;
  virtual bool GetParticleEmission() const override;
  virtual const CColor& GetModulationColor() const override;
  virtual bool IsSystemDeletable() const override;
  virtual rstl::optional_object< CAABox > GetBounds() const override;
  virtual int GetParticleCount() const override { return x25c_activeParticleCount; }
  virtual bool SystemHasLight() const override;
  virtual CLight GetLight() const override;
  virtual void DestroyParticles() override;
  virtual uint Get4CharId() const override;
#ifdef TARGET_PC
  uint PortFxAsset() const override;
  void PortFxDescribe(PortFxInfo& out) const override;
#endif
  int GetMaxParticles() const { return x90_MAXP; }
  void SetZTest(bool enabled) { x26c_28_zTest = enabled; }
  rstl::vector< CParticle >& Particles() { return x30_particles; }
  const rstl::vector< CParticle >& GetParticles() const { return x30_particles; }
  int GetEmitterTime() const;
  int GetSystemCount();
  void EndLifetime();
  void ForceParticleCreation(int amount);

  bool InternalUpdate(double dt);
  void UpdateLightParameters();
  void UpdateAdvanceAccessParameters(int, int);
  bool UpdateVelocitySource(int, int, CParticle&);
  void UpdateExistingParticles();
  void CreateNewParticles(int);
  void UpdatePSTranslationAndOrientation();
  CElementGen* ConstructChildParticleSystem(TToken< CGenDescription >) const;
  void UpdateChildParticleSystems(double);
  void RenderModels();
  void RenderLines();
  void RenderParticlesIndirectTexture();
  static void RenderParticlesFlameThrower(CElementGen* const* gens, int count);
  void RenderParticles();
  void RenderBasicParticlesRotNoTS(const CTransform4f&);
  void RenderBasicParticlesNoRotNoTS(const CTransform4f&);
  void RenderBasicParticlesRotTS(const CTransform4f&);
  void RenderBasicParticlesNoRotTS(const CTransform4f&);
  int GetParticleCountAll() const;
  int GetParticleCountAllInternal() const;
  void AccumulateBounds(const CVector3f&, float);
  void BuildParticleSystemBounds();

  int GetNumActiveChildParticles() const;
  CParticleGen* GetActiveChildParticle(int index) const;
  int GetCumulativeParticleCount() const { return x260_cumulativeParticles; }
  bool IsIndirectTextured()
      const; // { return x28_loadedGenDesc->x54_x40_TEXR && x28_loadedGenDesc->x58_x44_TIND; }
  void SetExternalVar(int index, float val);
  float GetExternalVar(int index) const;

  static void Initialize();
  static void ShutDown();

  void SetGlobalOrientAndTrans(const CTransform4f& xf);
  void SetLeaveLightsEnabledForModelRender(bool b) { x26d_26_modelsUseLights = b; }

  static void SetSubtractBlend(bool subtract) { sSubtractBlend = subtract; }
  static void SetMoveRedToAlphaBuffer(const bool move) { sMoveRedToAlphaBuffer = move; }

  static void SetGlobalSeed(const ushort seed) { sSeed = seed; }

#ifdef TARGET_PC
  // World position for DFCS (the system origin) / DFCP (a system-local particle position).
  CVector3f PortWorldFromLocal(const CVector3f& local) const {
    return xe8_globalTranslation + x10c_globalScaleTransform * local;
  }
  CVector3f PortSystemOrigin() const { return PortWorldFromLocal(xdc_translation); }
  // True for a converted Remastered PART (its PIRN marker).
  bool PortIsRemastered() const;
  // Remastered particle variables (PVRT): a handle is looked up once and then binds are cheap.
  // Binds are generator-wide, persist until rebound, and are no-ops when the variable is absent.
  u16 PortGetRealHandle(const PortGuid& g) const { return xPortVars.Find(g, EPortVarType::Real); }
  u16 PortGetIntHandle(const PortGuid& g) const { return xPortVars.Find(g, EPortVarType::Int); }
  u16 PortGetColorHandle(const PortGuid& g) const { return xPortVars.Find(g, EPortVarType::Color); }
  u16 PortGetVectorHandle(const PortGuid& g) const {
    return xPortVars.Find(g, EPortVarType::Vector);
  }
  void PortBindReal(u16 h, float v) { xPortVars.BindReal(h, v); }
  void PortBindInt(u16 h, int v) { xPortVars.BindInt(h, v); }
  void PortBindColor(u16 h, const CColor& v) { xPortVars.BindColor(h, v); }
  void PortBindVector(u16 h, const CVector3f& v) { xPortVars.BindVector(h, v); }
  void PortBindReal(const PortGuid& g, float v) { xPortVars.BindReal(PortGetRealHandle(g), v); }
  void PortBindInt(const PortGuid& g, int v) { xPortVars.BindInt(PortGetIntHandle(g), v); }
  void PortBindColor(const PortGuid& g, const CColor& v) {
    xPortVars.BindColor(PortGetColorHandle(g), v);
  }
  void PortBindVector(const PortGuid& g, const CVector3f& v) {
    xPortVars.BindVector(PortGetVectorHandle(g), v);
  }
  const CPortVarMemory& PortVars() const { return xPortVars; }
#endif

private:
  TLockedToken< CGenDescription > x1c_genDesc;
  CGenDescription* x28_loadedGenDesc;
  EModelOrientationType x2c_orientType;
  rstl::vector< CParticle > x30_particles;
  rstl::vector< CVector3f > x40;
  rstl::vector< CMatrix3f > x50_parentMatrices;
  rstl::vector< CAdvancedValues > x60_advValues;
  int x70_internalStartFrame;
  int x74_curFrame;
  double x78_curSeconds;
  float x80_timeDeltaScale;
  int x84_prevFrame;
  bool x88_particleEmission;
  float x8c_generatorRemainder;
  int x90_MAXP;
  short x94_randomSeed;
  float x98_generatorRate;
  float x9c_externalVars[16];
  CVector3f xdc_translation;
  CVector3f xe8_globalTranslation;
  CVector3f xf4_POFS;
  CVector3f x100_globalScale;
  CTransform4f x10c_globalScaleTransform;
  CTransform4f x13c_globalScaleTransformInverse;
  CVector3f x16c_localScale;
  CTransform4f x178_localScaleTransform;
  CTransform4f x1a8_localScaleTransformInverse;
  CTransform4f x1d8_orientation;
  CMatrix3f x208_orientationInverse;
  CTransform4f x22c_globalOrientation;
  uint x25c_activeParticleCount;
  uint x260_cumulativeParticles;
  uint x264_recursiveParticleCount;
  int x268_PSLT;
  bool x26c_24_translationDirty : 1;
  bool x26c_25_LIT_ : 1;
  bool x26c_26_AAPH : 1;
  bool x26c_27_ZBUF : 1;
  bool x26c_28_zTest : 1;
  bool x26c_29_ORNT : 1;
  bool x26c_30_MBLR : 1;
  bool x26c_31_LINE : 1;
  bool x26d_24_FXLL : 1;
  bool x26d_25_warmedUp : 1;
  bool x26d_26_modelsUseLights : 1;
  bool x26d_27_enableOPTS : 1;
  bool x26d_28_enableADV : 1;
  int x270_MBSP;
  uchar x274_backupLightActive;
  // uchar x275_pad[3];
  union {
    struct {
      bool x278_hasVMD[4];
    };
    uint x278_vmdStates;
  };
  CRandom16 x27c_randState;
  CModVectorElement* x280_VELSources[4];
  rstl::vector< CParticleGen* > x290_activePartChildren;
  int x2a0_CSSD;
  int x2a4_SISY;
  int x2a8_PISY;
  int x2ac_SSSD;
  CVector3f x2b0_SSPO;
  int x2bc_SESD;
  CVector3f x2c0_SEPO;
  float x2cc;
  float x2d0;
  CVector3f x2d4_aabbMin;
  CVector3f x2e0_aabbMax;
  float x2ec_maxSize;
  CAABox x2f0_systemBounds;
  LightType x308_lightType;
  CColor x30c_LCLR;
  float x310_LINT;
  CVector3f x314_LOFF;
  CVector3f x320_LDIR;
  EFalloffType x32c_falloffType;
  float x330_LFOR;
  float x334_LSLA;
  CColor x338_moduColor;
#ifdef TARGET_PC
  // Presentation smoothing (particle_interpolation): the tick generation of
  // the last single-step update, and the global translation before this
  // tick's first move.
  CPortVarMemory xPortVars; // PVRT variables, initialised from the description's defaults
  uint xPortStepGeneration;
  uint xPortGlobalGeneration;
  CVector3f xPortPrevGlobalTranslation;
  bool xPortPresenting;
  bool PortBeginPresent(CVector3f& savedGlobal);
  // VMAT material: VSMT results for this frame (port_vfx_particles.cpp). Bit n of the
  // mask = slot n was written; slots 0..10 are the Src values, 11+2*tex+k the warp scales.
  float xPortVsmt[19];
  uint xPortVsmtMask;
  void PortVfxUpdateSystem();
  // XFMD 3/4: the spawn-baked part of the emitter's transform, and moving the live particles from
  // one such frame to the next.
  bool PortFollowsEmitter() const;
  CTransform4f PortEmitterFrame() const;
  void PortFollowEmitter(const CTransform4f& before);
  void PortRenderParticlesVfx();
  void PortRenderMeshesVfx(CPortVfxMeshBatch& batch);
#endif

  static double kTickTime;
  static ushort sSeed;
  static int mParticleAliveCount;
  static int mParticleSystemAliveCount;
  static bool sSubtractBlend;
  static bool sMoveRedToAlphaBuffer;
};
CHECK_SIZEOF(CElementGen, 0x340)

#pragma cpp_extensions reset

#endif // _CELEMENTGEN
