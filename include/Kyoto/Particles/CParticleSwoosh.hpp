#ifndef _CPARTICLESWOOSH
#define _CPARTICLESWOOSH

#include "Kyoto/CRandom16.hpp"
#include "Kyoto/Graphics/CColor.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Particles/CParticleGen.hpp"
#include "Kyoto/Particles/IElement.hpp"
#include "Kyoto/TToken.hpp"
#include "MetroidPrime/CParticleDatabase.hpp"

#include "dolphin/gx/GXEnum.h"

#ifdef TARGET_PC
#include "Kyoto/Particles/CPortParticleVars.hpp"
#include "port_tick_pair.h"
#include <vector>
#endif

class CParticleSwoosh : public CParticleGen {
  friend class CParticleElectric;

public:
  struct SSwooshData {
    SSwooshData(const CVector3f& translation = CVector3f::Zero(),
                const CVector3f& offset = CVector3f::Zero(), float irot = 0.f, float rotm = 0.f,
                int startFrame = 0, bool active = false,
                const CTransform4f& orient = CTransform4f::Identity(),
                const CVector3f& velocity = CVector3f::Zero(), float leftRadius = 0.f,
                float rightRadius = 0.f, const CColor& color = CColor(0.f, 0.f, 0.f, 0.f))
    : mActive(active)
    , mLeftRad(leftRadius)
    , mRightRad(rightRadius)
    , mTranslation(translation)
    , mOffset(offset)
    , mUseOffset(offset)
    , mInitialRot(irot)
    , mRotm(rotm)
    , mOrientation(orient)
    , mColor(color)
    , mStartFrame(startFrame)
    , mVelocity(velocity) {}

    bool mActive;
    float mLeftRad;
    float mRightRad;
    CVector3f mTranslation;    // Updated by system's velocity sources or user code
    CVector3f mOffset;         // Updated by POFS once per system update (also resets x24_useOffset)
    CVector3f mUseOffset;      // Combination of POFS and NPOS, once per particle instance
    float mInitialRot;         // Rotation bias once per system update
    float mRotm;               // Rotation bias once per particle instance
    CTransform4f mOrientation; // Updated by user code
    int mFrame;                // Frame index of evaluated data
    CColor mColor;             // Updated by COLR
    int mStartFrame;
    CVector3f mVelocity;
#ifdef TARGET_PC
    SPortKeep mPortKeep{}; // KPIN values latched at the point's birth (CREPortKeep)
    // PSCL (point+0x40): the ring's local scale, skOneVector until evaluated (0x27c8d0)
    CVector3f mPortScale{1.f, 1.f, 1.f};
#endif
  };
  CParticleSwoosh(TToken< CSwooshDescription > desc, int i);
  ~CParticleSwoosh();

  const bool Update(double dt) override;
  void Render() override;
  void Render2SidedNoSplineNoGaps();
  void Render2SidedNoSplineGaps();
  void Render2SidedSpline();
  void Render3SidedSolidNoSplineNoGaps();
  void Render3SidedSolidSpline();
  void RenderNSidedNoSpline();
  void RenderNSidedSpline();
  void SetOrientation(const CTransform4f& orientation) override;
  void UpdateTranslationAndOrientation();
  void SetTranslation(const CVector3f& translation) override;
  void SetGlobalOrientation(const CTransform4f& orientation) override;
  void SetGlobalTranslation(const CVector3f& translation) override;
  void SetGlobalScale(const CVector3f& scale) override;
  void SetLocalScale(const CVector3f& scale) override;
  void SetParticleEmission(bool emission) override;
  void SetModulationColor(const CColor& col) override;
  const CTransform4f& GetOrientation() const override;
  const CVector3f& GetTranslation() const override;
  const CTransform4f& GetGlobalOrientation() const override;
  const CVector3f& GetGlobalTranslation() const override;
  const CVector3f& GetGlobalScale() const override;
  bool GetParticleEmission() const override;
  const CColor& GetModulationColor() const override;
  bool IsSystemDeletable() const override;
  rstl::optional_object< CAABox > GetBounds() const override;
  int GetParticleCount() const override;
  bool SystemHasLight() const override;
  CLight GetLight() const override;
  void DestroyParticles() override;
  uint Get4CharId() const override;
#ifdef TARGET_PC
  void ShutdownEffect() override;
  void PortDoShutdownBehavior(u32 mode) override;
  u32 PortOwnShutdownMode() const override;
  void PortSetSpawnSpread(const CVector3f& spread, const CVector3f& origin) override;
  uint PortFxAsset() const override;
  bool PortIsRemastered() const;
  void PortFxDescribe(PortFxInfo& out) const override;
  // Remastered particle variables (PVRT), as CElementGen's.
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
#endif

  static int GetAliveParticleSystemCount();

  void SetWarmUp() { x1d0_26_forceOneUpdate = true; }
  void SetRenderGaps(bool gaps) { x1d0_27_renderGaps = gaps; }

  const int GetSwooshCount() const { return x15c_swooshes.size(); }
  int GetCurParticle() const { return x158_curParticle; }
  rstl::vector< SSwooshData >& Swooshes() { return x15c_swooshes; }
  const rstl::vector< SSwooshData >& GetSwooshes() const { return x15c_swooshes; }

  bool IsLargeEnough() const;
  void UpdateSwooshTranslation(const CVector3f& translation);

  int WrapIndex(int index);
  float GetLeftRadius(int index);
  float GetRightRadius(int index);
  CVector3f GetSplinePoint(const CVector3f& p0, const CVector3f& p1, const CVector3f& p2,
                           const CVector3f& p3, float t) const;
  void UpdateBounds(const CVector3f& pos);
  void UpdateMaxRadius(float radius);

private:
  TLockedToken< CSwooshDescription > x1c_desc;
  int x28_curFrame;
  int x2c_PSLT;
  double x30_curTime;
  CVector3f x38_translation;
  CTransform4f x44_orientation;
  CTransform4f x74_invOrientation;
  CVector3f xa4_globalTranslation;
  CTransform4f xb0_globalOrientation;
  CVector3f xe0_globalScale;
  CTransform4f xec_scaleXf;
  CTransform4f x11c_invScaleXf;
  CVector3f x14c_localScale;
  int x158_curParticle;
  rstl::vector< SSwooshData > x15c_swooshes;
  rstl::vector< CVector3f > x16c_p0;
  rstl::vector< CVector3f > x17c_p1;
  rstl::vector< CVector3f > x18c_p2;
  rstl::vector< CVector3f > x19c_p3;
  int x1ac_particleCount;
  int x1b0_SPLN;
  int x1b4_LENG;
  int x1b8_SIDE;
  GXPrimitive x1bc_prim;
  CRandom16 x1c0_rand;
  float x1c4_;
  float x1c8_;
  float x1cc_TSPN;
  bool x1d0_24_emitting : 1;
  bool x1d0_25_AALP : 1;
  bool x1d0_26_forceOneUpdate : 1;
  bool x1d0_27_renderGaps : 1;
  bool x1d0_28_LLRD : 1;
  bool x1d0_29_VLS1 : 1;
  bool x1d0_30_VLS2 : 1;
  bool x1d0_31_constantTex : 1;
  bool x1d1_24_constantUv : 1;
  float x1d4_uMin;
  float x1d8_vMin;
  float x1dc_uMax;
  float x1e0_vMax;
  CTexture* x1e4_tex;
  float x1e8_uvSpan;
  int x1ec_TSPN;
  CVector3f x1f0_aabbMin;
  CVector3f x1fc_aabbMax;
  float x208_maxRadius;
  CColor x20c_moduColor;

#ifdef TARGET_PC
public:
  // The seed a Remastered parent gives the converted swoosh it constructs next; consumed by
  // that ctor (CElementGen::PortSeedSwooshChild).
  static uint sPortChildSeed;
  static bool sPortChildSeedSet;

private:
  CPortVarMemory xPortVars; // PVRT variables, initialised from the description's defaults
  CVector3f xPortSpawnSpread{1.f, 1.f, 1.f}; // PortSetSpawnSpread
  CVector3f xPortSpawnOrigin{0.f, 0.f, 0.f};
  // Presentation smoothing (particle_interpolation): what the last two ticks
  // drew, recorded at draw time, since grapple and Wave Beam swooshes are
  // written directly by their owners rather than stepped by Update.
  struct SPortSegment {
    CVector3f pos;
    CVector3f useOffset;
    float rot;
    bool active;
  };
  struct SPortState {
    SPortState() : head(0), frame(0), global(CTransform4f::Identity()) {}
    std::vector< SPortSegment > segs;
    int head;
    int frame;
    CTransform4f global;
  };
  PortTickPair< SPortState > xPortState;
  std::vector< SPortSegment > xPortSaved;
  bool PortBeginPresent();
  void PortEndPresent();
  // A converted Remastered swoosh with a VMAT: ITEN and the VPMT rows per point (by slot),
  // evaluated with COLR, and its draw through the VFX material.
  struct SPortVfxPoint {
    float iten = 1.f;
    float color[4] = {1.f, 1.f, 1.f, 1.f}; // COLR unclamped (HDR); white without a COLR
    float vpmt[4][4] = {};
  };
  std::vector< SPortVfxPoint > xPortVfxPts;
  // Remastered's EMRV (CVEVelocityCalc 0x2a288c): the translation one step back, for PRAZ.
  CVector3f xPortRazyPrev = CVector3f(0.f, 0.f, 0.f);
  // Remastered's +0x17c: points created since construction or the last DestroyParticles (TXUP).
  int xPortCreated = 0;
  int xPortShutdownCountdown = -1; // SHTT: ticks to the delayed shutdown, -1 = not counting
  // CAddVarSystem::Init has run in the first Update (the CParticleGen +0xa0 0x1000 flag).
  bool xPortSysInit = false;
  // An ELC2's per-bolt swoosh (Remastered's +0x188 bit 5, HACK_InitForElectric 0x27ebfc): each
  // point keeps the age it was created with (the live count less one), points never die, and
  // with emission off a step neither advances the ring nor retires a point.
  bool xPortElectric = false;
  void PortEvalSys(bool init);

public:
  void PortInitForElectric();
  void PortReInitForElectric();

private:
  // The per-point variables' values (the buffer at Remastered's this+0x158), kPortPartValues a
  // point in ring order; empty without a PPVR.
  static constexpr int kPortPartValues = 13;
  std::vector< float > xPortPartValues;
  float* PortPartValues(int index);
  float PortTexUScroll() const;
  CTransform4f PortRazyOrientation(const CVector3f& motion) const;
  CTransform4f PortRotZOrientation() const;
  CTransform4f PortIntOrientation(const CTransform4f& external) const;
  // The external orientation (Remastered's var at desc+0x28), which EXTR reads apart from INTR
  CTransform4f xPortExtOrientation = CTransform4f::Identity();
  // XFMD 1: the external translation, which only MPT_GetTranslation 0x27fb54 reads there
  CVector3f xPortExtTranslation = CVector3f(0.f, 0.f, 0.f);
  bool PortVfxSwoosh() const;
  void PortRenderVfx();
  int PortSplineRef(int curIdx, int j);
  int PortSplineSteps(int curIdx, int prevIdx) const;
#endif

  static uint mSwooshAliveCount;
};

CHECK_SIZEOF(CParticleSwoosh, 0x210)
NESTED_CHECK_SIZEOF(CParticleSwoosh, SSwooshData, 0x80)

#endif // _CPARTICLESWOOSH
