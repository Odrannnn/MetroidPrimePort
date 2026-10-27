#ifndef _CELEMENTGEN
#define _CELEMENTGEN

#include "types.h"

#include "Kyoto/Particles/CPortParticleVars.hpp"
#include "Kyoto/CRandom16.hpp"
#include "Kyoto/Graphics/CColor.hpp"
#include "Kyoto/Graphics/CLight.hpp"
#include "Kyoto/Math/CAABox.hpp"
#include "Kyoto/Math/CMatrix3f.hpp"
#include "Kyoto/Math/CQuaternion.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "Kyoto/Particles/CParticleGen.hpp"
#include "Kyoto/TToken.hpp"

class CGenDescription;
class CModVectorElement;

#pragma cpp_extensions on
#ifdef TARGET_PC
#include "Kyoto/Particles/CGenDescription.hpp" // PortKssmChild points at a CGenDescription::PortKssm
#include "Kyoto/Particles/CPortVfxConstants.hpp"
#include "Kyoto/Particles/PortParticleMath.hpp"
#include <utility>
#include <vector>
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
    SPortKeep xPortKeep;      // KPIN values latched at birth (CREPortKeep)
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
    , xPortKeep{}
#endif
    {}
  };
  struct CAdvancedValues {
#ifdef TARGET_PC
    // 0..8 ADV1..9 (ADV9 is Remastered's), 9..12 the bits of IAV0..3 (Remastered's int variables).
    float values[13] = {};
#else
    float values[8];
#endif
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
#ifdef TARGET_PC
  // SHTM/SHTT (PSHM/PSHT). ShutdownEffect starts the countdown if the description has one, else
  // acts at once; PortDoShutdownBehavior(0xff) uses the description's mode.
  void ShutdownEffect() override;
  void PortDoShutdownBehavior(u32 mode) override;
  void PortSetSpawnSpread(const CVector3f& spread, const CVector3f& origin) override;
  // SDTR: the countdown's remaining over its total, 1 when none is running.
  float PortShutdownTimeRemaining() const;
#endif
  virtual void SetModulationColor(const CColor& col) override;
  virtual void SetGeneratorRate(float rate) override;
#ifdef TARGET_PC
  // With INTR/INTT the owner reads back what it set, as Remastered's getters return the external
  // vars (0x24d880/0x24d8ac); the system's own are PortSystemRotation/PortSystemTranslation.
  virtual const CTransform4f& GetOrientation() const override {
    return xPortIntFrame ? xPortExtOrientation : x1d8_orientation;
  }
  virtual const CVector3f& GetTranslation() const override {
    return xPortIntFrame ? xPortExtTranslation : xdc_translation;
  }
#else
  virtual const CTransform4f& GetOrientation() const override { return x1d8_orientation; }
  virtual const CVector3f& GetTranslation() const override { return xdc_translation; }
#endif
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
  void UpdateAdvanceAccessParameters(int, int, bool spawn = false);
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
#ifdef TARGET_PC
  const float* PortExternalVars() const { return xPortExternalSet ? x9c_externalVars : nullptr; }
#endif
  float GetExternalVar(int index) const;

  static void Initialize();
  static void ShutDown();

  void SetGlobalOrientAndTrans(const CTransform4f& xf);
  void SetLeaveLightsEnabledForModelRender(bool b) { x26d_26_modelsUseLights = b; }

  static void SetSubtractBlend(bool subtract) { sSubtractBlend = subtract; }
  static void SetMoveRedToAlphaBuffer(const bool move) { sMoveRedToAlphaBuffer = move; }

  static void SetGlobalSeed(const ushort seed) { sSeed = seed; }

#ifdef TARGET_PC
  struct CPortNestedLightSnapshot {
    const CParticleGen* generator; // identity key only; never dereferenced after Update()
    CVector3f origin;
    CLight light;
  };
  static bool PortHasNestedLightSnapshotFor(
      const std::vector< CPortNestedLightSnapshot >& snapshots, const CParticleGen* generator) {
    for (const CPortNestedLightSnapshot& snapshot : snapshots) {
      if (snapshot.generator == generator) {
        return true;
      }
    }
    return false;
  }
  // World position for DFCS (the system origin) / DFCP (a system-local particle position).
  CVector3f PortWorldFromLocal(const CVector3f& local) const {
    return PortWorldFromLocal(xe8_globalTranslation, x10c_globalScaleTransform, local);
  }
  // Exposed for the numeric spawn-translation fixture; this is the same affine
  // mapping used by PortSystemOrigin and intentionally excludes EMTR/PMOP.
  static CVector3f PortWorldFromLocal(const CVector3f& globalTranslation,
                                      const CTransform4f& globalScaleTransform,
                                      const CVector3f& local) {
    const CVector3f transformed(
        globalScaleTransform.Get00() * local.GetX() + globalScaleTransform.Get01() * local.GetY() +
            globalScaleTransform.Get02() * local.GetZ() + globalScaleTransform.Get03(),
        globalScaleTransform.Get10() * local.GetX() + globalScaleTransform.Get11() * local.GetY() +
            globalScaleTransform.Get12() * local.GetZ() + globalScaleTransform.Get13(),
        globalScaleTransform.Get20() * local.GetX() + globalScaleTransform.Get21() * local.GetY() +
            globalScaleTransform.Get22() * local.GetZ() + globalScaleTransform.Get23());
    return CVector3f(globalTranslation.GetX() + transformed.GetX(),
                     globalTranslation.GetY() + transformed.GetY(),
                     globalTranslation.GetZ() + transformed.GetZ());
  }
  static void PortSetNestedHdrCarrier(CLight& light, const float sourceRgba[4], float intensity,
                                      float innerRadius, float outerRadius,
                                      u8 falloff = CLight::kPHF_Smooth) {
    const float linearColor[3] = {sourceRgba[0] * intensity, sourceRgba[1] * intensity,
                                  sourceRgba[2] * intensity};
    const CVector3f noOffset = CVector3f::Zero();
    light.SetPosition(noOffset);
    // Draws without PBR (TEV) take the retail light: GetLight's custom light carries the raw LINT
    // (up to 256) as its angle constant over 1/d^2, which whites out everything near it. Give it
    // the same intensity, bounded at the source and at half strength halfway to the outer radius.
    const float half = outerRadius > 0.f ? 0.5f * outerRadius : 1.f;
    light.SetAttenuation(1.f, 0.f, 1.f / (half * half));
    light.SetAngleAttenuation(intensity, 0.f, 0.f);
    light.SetPortHdr(linearColor, noOffset, innerRadius, outerRadius,
                      static_cast< CLight::EPortHdrFalloff >(falloff & 3));
    light.SetPortHdrBoundsRadius(outerRadius);
  }
  CVector3f PortSystemOrigin() const { return PortWorldFromLocal(xdc_translation); }
  // Remastered's external translation/rotation/scale (EXTT/EXTR/EXTS, system vars 0xb/0xc/0xa):
  // what the owner hands CParticleGen::SetExternalDataSet 0x27b3d8. Remastered has one translation,
  // one rotation and no local scale; the port's retail split is folded into them. With INTT/INTR
  // (PINT/PINR) they are what the owner set, not the system's own (PortApplyIntFrame).
  CVector3f PortExternalTranslation() const {
    return xe8_globalTranslation + (xPortIntFrame ? xPortExtTranslation : xdc_translation);
  }
  CQuaternion PortExternalRotation() const {
    return xPortIntFrame ? CQuaternion::FromMatrix(xPortExtGlobalOrientation * xPortExtOrientation)
                         : PortSystemRotation();
  }
  // The system translation/rotation (Compile 0x289af0 +0x3c/+0x38): INTT/INTR's when authored,
  // else the external ones.
  CVector3f PortSystemTranslation() const { return xe8_globalTranslation + xdc_translation; }
  CQuaternion PortSystemRotation() const {
    return CQuaternion::FromMatrix(x22c_globalOrientation * x1d8_orientation);
  }
  CVector3f PortExternalScale() const {
    return CVector3f(xPortExternalScale.GetX() * x16c_localScale.GetX(),
                     xPortExternalScale.GetY() * x16c_localScale.GetY(),
                     xPortExternalScale.GetZ() * x16c_localScale.GetZ());
  }
  // The global frame T_g/Q_g/S_g (desc +0xa8/+0xac/+0xb0, SetupTransformMode 0x2887d8) for the
  // XFMD mode: 2 = zero/identity/external scale, 3 = the external T/Q/S, 4 = the mover's
  // position and rotation (the external T/Q for the identity SMVR the converter drops) at unit scale.
  void PortGlobalFrame(CVector3f& t, CQuaternion& q, CVector3f& s) const;
  // True for a converted Remastered PART (its PIRN marker).
  bool PortIsRemastered() const;
  // Remastered _CreateNewParticlesAtPositions / ...IncludingChildren (0x24c0dc / 0x24bfd0): n
  // particles at the given local positions, at rest, the PART children first.
  void PortCreateParticlesAtPositions(int n, const CVector3f* pos);
  const CGenDescription* PortDesc() const { return x28_loadedGenDesc; }
  // The PART id this system was built from (Remastered compares a generator's desc id).
  CAssetId PortDescId() const { return x1c_genDesc.GetTag().GetId(); }
  int PortCurFrame() const { return x74_curFrame; }
  void PortCreateParticlesAtPositionsIncludingChildren(int n, const CVector3f* pos);
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
  void PortBindExternal(const SPortExternalVar& v) { xPortVars.BindExternal(v); }
  const CPortVarMemory& PortVars() const { return xPortVars; }
  // The global translation just set is already the presented one (placed from
  // the presented camera on every drawn frame): draw it as is, unblended.
  void PortGlobalIsPresented();
  // A Remastered HUD billboard's scene: the local scale is in camera axes and model particles are
  // pushed out from the eye by eyeScale (CHUDBillboardEffect::PreRender). 0 = a world effect.
  void PortSetHudScene(float eyeScale);
  // The lights of the active nested child generators (PHDR); not this generator's own, which the
  // host carries as the retail root light.
  void PortCollectNestedLightSnapshots(std::vector< CPortNestedLightSnapshot >& out) const {
    PortCollectNestedLightSnapshots(out, false);
  }
  void PortCollectNestedLightSnapshots(std::vector< CPortNestedLightSnapshot >& out,
                                       bool includeSelf) const;
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
#ifdef TARGET_PC
  // port-only: SetExternalVar was called (Remastered's external data block exists); a PMXV reads
  // x9c_externalVars then, else its fallback
  bool xPortExternalSet = false;
#endif
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
#ifdef TARGET_PC
  bool xPortVelFinalizes = false;
  bool xPortVelocityGlobal[4] = {}; // packed alongside x280_VELSources, as Fixup 0x292ed4
  int xPortShutdownCountdown = -1; // ticks to the delayed shutdown, -1 = not counting
  int xPortShutdownTotal = 0;
#endif
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
  // A moving local emitter (projectile trails): the local translation at the
  // last two single-step updates, and the particles born in the last one moved
  // back to the presented emitter while drawn (saved pos/prevPos pairs).
  CVector3f xPortSpawnTranslation;
  CVector3f xPortPrevSpawnTranslation;
  uint xPortSpawnGeneration;
  uint xPortPrevSpawnGeneration;
  rstl::vector< CVector3f > xPortNewbornSaved;
  bool PortSpawnOffset(CVector3f& offset) const;
  void PortShiftNewborns(const CVector3f& shift);
  void PortRestoreNewborns();
  // Particles emitted along a line each tick (grapple beam): the next Render
  // moves the newborns by the blend of the two ends' shifts, by where each
  // lies between them (world space ends).
  CVector3f xPortLineEnd[2];
  CVector3f xPortLineShift[2];
  bool xPortLineShiftSet;
  void PortShiftNewbornsAlongLine();

public:
  void PortSetNewbornLineShift(const CVector3f& firstEnd, const CVector3f& lastEnd,
                               const CVector3f& firstShift, const CVector3f& lastShift);

private:
  // VMAT material: VSMT results for this frame (port_vfx_particles.cpp). Bit n of the
  // mask = slot n was written; slots 0..10 are the Src values, 11+2*tex+k the warp scales.
  float xPortVsmt[kPortVfxVsmtCount];
  uint xPortVsmtMask;
  float xPortNestedHdrColor[4];
  float xPortNestedHdrInnerRadius, xPortNestedHdrOuterRadius;
  bool xPortNestedHdrRadiiValid;
  // A converted Remastered system keeps its seed as 32 bits (x94 is 16): the SEED value, else the
  // seed it was built with (sPortChildSeed for a Remastered child, else sSeed).
  uint xPortSeed;
  // The seed a Remastered parent gives the child it constructs next; consumed by that ctor.
  static uint sPortChildSeed;
  static bool sPortChildSeedSet;
  uint PortChildSeed(const CGenDescription& child) const;
  // Remastered's CElementGen::SpawnChildSystems (0x249bd8) for a converted effect, and its
  // spawn tables (PKSM); xPortKssmPeriods are the tables' period slots (their CVars).
  std::vector< int > xPortKssmPeriods;
  void PortSpawnChildSystems();
  void PortSpawnTables();
  void PortSpawnTableEntry(size_t table, int frame);
  void PortAddChild(CParticleGen* child);
  // A child a spawn table placed with an SEVT(SMOV) component: its own frame, the pos slot and
  // the mover's velocity, the rot it ended on, and the KPIN/IRND latches. `placed` is the world
  // position the child is held at (child T = placed - its GT); with hasRot its orientation is
  // held at `rot` the same way. The parent's propagation leaves what the component holds alone.
  // The tree runs in Remastered's particle space (CVEPortBasis): remPos and vel are in it, pos,
  // rot and rotQuat are remPos and the tree's rotation taken to the port's.
  struct PortKssmChild {
    const CGenDescription::PortKssm* kssm = nullptr; // holds its component and nodes
    CParticleGen* child = nullptr;
    u32 component = 0;
    int frame = 0;
    CVector3f remPos = CVector3f::Zero();
    CVector3f pos = CVector3f::Zero();
    CVector3f vel = CVector3f::Zero();
    CTransform4f rot = CTransform4f::Identity();
    CQuaternion rotQuat = CQuaternion::NoRotation();
    bool hasRot = false;
    std::vector< float > latches;
  };
  std::vector< PortKssmChild > xPortKssmChildren;
  PortKssmChild* PortFindKssmChild(const CParticleGen* child);
  float PortKssmReal(PortKssmChild& state, u32 node);
  CVector3f PortKssmVector(PortKssmChild& state, u32 node);
  CQuaternion PortKssmRotation(PortKssmChild& state, u32 node);
  void PortKssmEvaluate(PortKssmChild& state);
  void PortKssmPlace(const PortKssmChild& state);
  // XFMD 4's SMVR (CGenDescription::xPortMover), a system-scoped mover: initialised by the first
  // Update, stepped at the start of each tick. Its pos/rot are T_g/Q_g.
  PortKssmChild xPortMover;
  bool xPortMoverInit = false;
public:
  // PLNV's index shufflers, one per element (CIndexShuffler's bitset lives in the generator's
  // variable memory); created zeroed on first use.
  std::vector< std::pair< const void*, PortParticleMath::IndexShuffler > > xPortShufflers;
  PortParticleMath::IndexShuffler& PortShuffler(const void* element) {
    for (auto& entry : xPortShufflers) {
      if (entry.first == element) {
        return entry.second;
      }
    }
    xPortShufflers.emplace_back(element, PortParticleMath::IndexShuffler());
    return xPortShufflers.back().second;
  }
  // PEMV (Remastered EMRV, var 0x16, CVEVelocityCalc): the change of the system translation (over
  // its scale, per component) since the previous tick, 0 on the first. Updated once per tick.
  CVector3f xPortPemvPrev = CVector3f::Zero();
  CVector3f xPortPemvDelta = CVector3f::Zero();
  bool xPortPemvStarted = false;
  void PortStepPemv();
  const CVector3f& PortEmitterVelocity() const { return xPortPemvDelta; }
  // PZTS (Remastered ZTES 0): the depth test passes everywhere (static render state compare 1 = ALWAYS)
  bool PortNoZTest() const { return x28_loadedGenDesc->xPortNoZTest; }

private:
  bool PortHasMover() const;
  void PortInitMover();
  void PortEvalSys(bool init);
  void PortStepMover();
  // The system scale: INTS (PINS) evaluated when authored, else the external scale.
  CVector3f PortSystemScale();
  void PortSetSystemScale(const CVector3f& scale);
  // The scale the owner (or parent) set, which EXTS reads; x100_globalScale is the system's.
  CVector3f xPortExternalScale{1.f, 1.f, 1.f};
  // The system orientation and translation: INTR (PINR) and INTT (PINT) evaluated, applied as
  // x22c = identity, x1d8 = INTR and xdc = INTT - xe8 (xe8 stays the owner's), and handed to the
  // children. force = the external values changed; otherwise only a changed value is applied.
  void PortApplyIntFrame(bool force);
  // A PINR or PINT outside XFMD 4 (whose mover overrides them): the setters keep the owner's
  // orientations and local translation here, which EXTR/EXTT and the getters read.
  bool xPortIntFrame = false;
  CVector3f xPortSpawnSpread{1.f, 1.f, 1.f};
  float xPortHudEyeScale = 0.f;
  CVector3f PortSpreadSpawn(const CVector3f& rel) const;
  CTransform4f xPortExtOrientation = CTransform4f::Identity();
  CTransform4f xPortExtGlobalOrientation = CTransform4f::Identity();
  CVector3f xPortExtTranslation{0.f, 0.f, 0.f};
  CVector3f xPortIntEuler{0.f, 0.f, 0.f};
  CVector3f xPortIntTranslation{0.f, 0.f, 0.f};
  void PortVfxUpdateSystem();
  // XFMD 3/4: the spawn-baked part of the emitter's transform, and moving the live particles from
  // one such frame to the next.
  bool PortFollowsEmitter() const;
  CTransform4f PortEmitterFrame() const;
  void PortFollowEmitter(const CTransform4f& before);
  void PortRenderParticlesVfx();
  void PortRenderPlanarVfx(const CParticle& particle, int frame);
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
