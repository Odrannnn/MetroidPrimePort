#ifndef _CGENDESCRIPTION
#define _CGENDESCRIPTION

#include "Kyoto/CToken.hpp"
#include "types.h"

#include "Kyoto/Particles/CPortParticleVars.hpp"
#include "Kyoto/Particles/CSpawnSystemKeyframeData.hpp"
#include "Kyoto/Particles/IElement.hpp"
#include "Kyoto/TToken.hpp"

#include "rstl/optional_object.hpp"
#include "rstl/single_ptr.hpp"

#ifdef TARGET_PC
#include <memory>
#include <optional>
#include <vector>
#endif

class CElectricDescription;
class CModel;
class CSwooshDescription;
class CTexture;

#ifdef TARGET_PC
// Port-only Remastered particle material (VMAT and friends, docs/REMASTERED_VFX.md).
// Present only when the PART carries a VMAT; everything else draws as retail.
#include "Kyoto/Particles/CPortVfxConstants.hpp"

struct CPortVfxElem {
  enum EKind : u32 { kReal = 0, kVector = 1, kInt = 2, kColor = 3 };
  u32 kind = kReal;
  u32 a = 0; // VPMT: row; VSMT: target
  u32 b = 0; // VPMT: component
  CRealElement* real = nullptr;
  CVectorElement* vec = nullptr;
  CIntElement* integer = nullptr;
  CColorElement* color = nullptr;
};

struct CPortVfxMat {
  struct Tex {
    TCachedToken< CTexture > token;
    u32 uvSet = 0, wrapS = 0, wrapT = 0, linear = 0;
    u32 cols = 1, rows = 1, layers = 1, warped = 0;
    float warpScale[2] = {0.f, 0.f};
  };
  struct Src {
    s32 row = -1, comp = 0;
    float value = 0.f;
  };
  u32 version = 0, features = 0, blend = 0;
  std::vector< Tex > tex; // up to 4
  s32 colorSlot = -1, opacitySlot = -1, rampSlot = -1, ramp2Slot = -1, thresholdSlot = -1,
      indirectSlot = -1, paletteSlot = -1;
  s32 rampRow[2] = {-1, -1};
  s32 addRow = -1;
  // erosion, thrX, thrY, thrW, fresnelX, fresnelY, fadeX, fadeY, indexScale, indexOffset, indexRow
  Src src[11];
  float modulate = 1.f, depthSoften = 0.f;
  float opacityGain = 1.f, opacityBias = 0.f; // v3 tail; v2 keeps the identity
  u32 spriteCenter = 0; // 0..8, a row of the sprite pivot table
};

// XFMD (build/mpr/vfx/RESOLVED-xfmd.md): how the emitter's transform reaches the particles.
// 2 = retail (orientation and local translation baked at spawn); 3 = the particles stay in the
// emitter's space and follow its later moves; 4 = as 3 with unit scale (an identity SMVR mover).
enum EPortXfmd : u8 { kPortXfmdRetail = 2, kPortXfmdFollow = 3, kPortXfmdFollowUnscaled = 4 };

struct CPortVfxData {
  CPortVfxMat mat;
  std::vector< CRealElement* > vtmt; // n x 6 (A B C D E F)
  u32 vtmtCount = 0;
  std::vector< CPortVfxElem > vpmt;
  std::vector< CPortVfxElem > vsmt;
  CRealElement* ssze = nullptr;
  CRealElement* iten = nullptr;
  u32 vorn = 0; // 0 sprite, 1 oriented to the launch direction, 2 to the velocity
  // VMSH: the PMDL's LOD0 as one mesh in the converted CMDL's model space, 8 floats per vertex
  // (pos, normal, uv), indices u16 (idx16) when meshVerts <= 65535, else u32 (idx32).
  u32 meshVerts = 0, meshTris = 0;
  std::vector< float > meshV;
  std::vector< u16 > meshIdx16;
  std::vector< u32 > meshIdx32;
  // VMSH v2: 16 floats per vertex (pos, normal, uv, tangent, colour; normal and tangent raw, the
  // tangent's w the handedness with the conversion's mirror folded in), meshFlags bit 0 a real
  // tangent, bit 1 a real colour, and the conversion port = meshOrient x Remastered + meshOffset.
  u32 meshStride = 8, meshFlags = 0;
  float meshOrient[9] = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f};
  float meshOffset[3] = {0.f, 0.f, 0.f};
  // VPRM: the shader's NMAP slot (v2: and TCH0 env slot) and its CCHn uniforms (the MATI's), which
  // VSMT targets kPortVfxParamTarget.. override.
  s32 normalSlot = -1;
  s32 envSlot = -1;
  u32 paramCount = 0;
  float params[7][4] = {};
  CPortVfxData() = default;
  CPortVfxData(const CPortVfxData&) = delete;
  CPortVfxData& operator=(const CPortVfxData&) = delete;
  ~CPortVfxData(); // port_vfx_particles.cpp (owns the elements)
};
#endif

class CGenDescription {
public:
  CGenDescription();
  ~CGenDescription();

  CIntElement* x0_PSLT;
  CIntElement* x4_PSWT;
  CRealElement* x8_PSTS;
  CVectorElement* xc_POFS;
  CIntElement* x10_SEED;
  CRealElement* x14_LENG;
  CRealElement* x18_WIDT;
  CIntElement* x1c_MAXP;
  CRealElement* x20_GRTE;
  CColorElement* x24_COLR;
  CIntElement* x28_LTME;
  CEmitterElement* x2c_EMTR;
  bool x30_24_LINE : 1;
  bool x30_25_FXLL : 1;
  bool x30_26_AAPH : 1;
  bool x30_27_ZBUF : 1;
  bool x30_28_SORT : 1;
  bool x30_29_LIT_ : 1;
  bool x30_30_ORNT : 1;
  bool x30_31_RSOP : 1;
  bool x31_24_MBLR : 1;
  bool x31_25_PMAB : 1;
  bool x31_26_PMUS : 1;
  bool x31_27_PMOO : 1;
  bool x31_28_VMD1 : 1;
  bool x31_29_VMD2 : 1;
  bool x31_30_VMD3 : 1;
  bool x31_31_VMD4 : 1;
  bool x32_24_CIND : 1;
  bool x32_25_OPTS : 1;
  CIntElement* x34_MBSP;
  CRealElement* x38_SIZE;
  CRealElement* x3c_ROTA;
  CUVElement* x40_TEXR;
  CUVElement* x44_TIND;
  rstl::optional_object< TCachedToken< CModel > > x48_PMDL;
#ifdef TARGET_PC
  // port-only PMDV: model variants, a particle uses xPortSeed % size(); empty = just PMDL
  std::vector< TCachedToken< CModel > > xPortPMDV;
  // port-only VMAT material; null = retail draw
  std::unique_ptr< CPortVfxData > xPortVfx;
  // port-only XFMD (EPortXfmd); kept outside xPortVfx, which a bad VMAT block drops
  u8 xPortXfmd = kPortXfmdRetail;
  // port-only PMRF: Remastered PMRB/PMCF. 0 = retail (orientation * PMRT, PMOP turned by the
  // orientation), 1 = PMRT alone, 2 = orientation * PMRT; for 1 and 2 PMOP turns with the model.
  u8 xPortPmrqFrame = 0;
  // port-only PEMV (Remastered EMRV, var 0x16): the description reads the per-tick system
  // velocity. 0 none, 1 the external (global + local) translation, 2 the local translation.
  u8 xPortPemv = 0;
  // port-only PIRN: a converted Remastered effect, whose nested IRND elements are stable per particle
  bool xPortIrnd = false;
  // port-only PVSD/PVSN/PVSM: Remastered's visor effect data (any VE* property) and its splat
  // size range VESN/VESM, read by the VisorGoo splat manager (PortVisorGoo).
  bool xPortVisorData = false;
  float xPortVisorMinScale = 1.4f;
  float xPortVisorMaxScale = 2.1f;
  // port-only PVSC/PVMC/PVTC/PVBS/PVDT: VESC/VEMC/VETC, VEBS and VEDT (ticks, -1 = never
  // expires), read by the visor effect override manager (PortVisorOverrides). Owned here.
  CColorElement* xPortVisorStartColor = nullptr;
  CColorElement* xPortVisorMidColor = nullptr;
  CColorElement* xPortVisorEndColor = nullptr;
  CRealElement* xPortVisorBlend = nullptr;
  int xPortVisorTicks = -1;
  // port-only PGID: the first word of a converted Remastered PART's GUID (CElementGen::PortChildSeed)
  u32 xPortGuidWord = 0;
  // Nested Remastered point-light metadata (PHDR/PIR0/POR0/PHFO). The
  // radius elements are owned here and evaluated by the live generator.
  bool xPortNestedHdrLight = false;
  CRealElement* xPortNestedLightInnerRadius = nullptr;
  CRealElement* xPortNestedLightOuterRadius = nullptr;
  // port-only PHFO: the authored LFOT (CLight::EPortHdrFalloff 0..3); absent = smooth
  u8 xPortNestedLightFalloff = 3;
  // port-only PLIX: Remastered's LIXP, LINT at the static inverse exposure (PortRoomEnv::ParticleLightGain)
  bool xPortNestedLightInverseExposure = false;
  // port-only PINS: Remastered's INTS, the system scale in place of the external one (EXTS still
  // reads the external scale; CElementGen::PortSystemScale). Owned here; null = the external scale.
  CVectorElement* xPortIntScale = nullptr;
  // port-only PINR/PINT: Remastered's INTR/INTT, the system orientation (an Euler vector in the
  // port's space, as PMRT's) and translation in place of the external ones (EXTR/EXTT still read
  // the external ones; CElementGen::PortApplyIntFrame). Owned here; null = the external one.
  CVectorElement* xPortIntRotation = nullptr;
  CVectorElement* xPortIntTranslation = nullptr;
  // port-only PVRT: Remastered particle variables (names, types, defaults)
  std::unique_ptr< CPortVarTable > xPortVars;
  // port-only: the PKPR elements (per-particle KPIN latches), latched at birth in this order;
  // owned by the element trees
  std::vector< const CREPortKeep* > xPortKeeps;
  // port-only: an EMTR CODE (desc[0x2fa] bit 4): the rate-driven spawn (GRTE) is skipped, particles
  // come only from code (CElementGen::TickSystem_Node 0x24a16c)
  bool xPortCodeSpawn = false;
  // port-only PADO: the order ADV1..8 are evaluated in (0-based), when it is not 1..8
  std::vector< u8 > xPortAdvOrder;
  // port-only PPVR: ADV1..9 and IAV0..3 in Remastered's sorted order with constant-after-start
  // flags; supersedes PADO (CElementGen::UpdateAdvanceAccessParameters).
  std::unique_ptr< CPortPartVars > xPortPartVars;
  // Port-only PSYS: the system variables (CElementGen::PortEvalSys).
  std::unique_ptr< CPortSys > xPortSys;
  // port-only PSHP: Remastered SSHP sprite mesh (0 quad; 3 triangle, 4 pentagon, 5 hexagon, 6 octagon)
  u8 xPortSpriteShape = 0;
  // port-only PBDM: Remastered's blend row where the alpha blend stands in for it
  // (CElementGen::PortBlendSrc/Dst). 0 = retail alpha, 1 premultiplied, 3 opaque; 2 is AAPH.
  u8 xPortBlend = 0;
  // port-only PDFG: Remastered's DFOG 1, the static render state's "no fog" flag (0x8000): the
  // particles are not volumetric-fogged whatever their blend (PortFx::RenderScope)
  bool xPortUnfogged = false;
  // port-only PZTS: Remastered's ZTES 0 (static render state ctor 0x28c2fc: depth compare 1, which
  // the GPU table 0x1d0c3a8 maps to ALWAYS, instead of 4 = LEQUAL). The depth test passes everywhere;
  // writes (ZBUF) are as authored.
  bool xPortNoZTest = false;
  // port-only PIVS: Remastered's IVSE 0 (desc+0x2fa bit 2 clear, CreateNewParticles 0x24a9fc): the
  // emitter's velocity is not turned by the system orientation (retail always turns it).
  bool xPortNoIvse = false;
  // VMPC: run the complete velocity program once at birth (CreateNewParticles 0x24abd0).
  bool xPortVelocityAtCreation = false;
  // VGD1..4 select world-space modifiers; VMD true switches the same slot back to emitter space.
  bool xPortVelocityGlobal[4] = {};
  // DVVN: defaults version for the missing-EMTR velocity; zero preserves retail behavior.
  u8 xPortDefaultValuesVersion = 0;
  // DVVN and IEXP update this in stream order. The marker supersedes legacy VMAT exposure pins.
  bool xPortExposurePolicy = false;
  bool xPortInverseExposure = false;
  // port-only PISV: Remastered's ISVF, the factor of the system's per-tick translation change
  // (PEMV's delta, no time step) added to a new particle's velocity. Owned here; null = none.
  CRealElement* xPortInheritVel = nullptr;
  // port-only PITP: Remastered's ITPE 1: with several particles born in a tick and a moving system,
  // their positions are spread back along the change of translation (CreateNewParticles 0x24a630).
  bool xPortSpread = false;
  // port-only PDBS: Remastered's DBIS (desc+0xd4), the depth bias in world units, negative toward the
  // camera (CPortParticleDepthBias). 0 = none.
  float xPortDepthBias = 0.f;
  // port-only PSHM: Remastered's SHTM, what a shutdown does (CParticleGen::DoShutdownBehavior
  // 0x27b88c): 0 stops emission, 1 also destroys the live particles. Other values do neither.
  u8 xPortShutdownMode = 0;
  // port-only PSHT: Remastered's SHTT, the shutdown countdown in ticks (ShutdownEffect 0x27a66c,
  // UpdateCountdownTimerForShutdown 0x27b864). Owned; null = a shutdown acts at once.
  CIntElement* xPortShutdownTime = nullptr;
  // port-only PISD 0: Remastered's ISDT (desc+0x6c bit 1), on by default. On, a shutdown reaches the
  // children as DoShutdownBehavior(mode) (their own SHTM at once, no SHTT); off, as ShutdownEffect.
  bool xPortChildShutdownByMode = true;
  // port-only PBLT 1: Remastered's BLIT (static-state bit 0x4000). A lit sprite takes the room's baked
  // probe only with it (CParticleSceneNode::AppendBakedLightingIfRequested 0x2b12a8).
  bool xPortBakedLighting = false;
  // port-only PMOV/PMPV: Remastered's per-particle model orientation direction and pivot offset
  // (SolveParticleTransform 0x246f14), in the port's space. Owned; null = none.
  CVectorElement* xPortPmov = nullptr;
  CVectorElement* xPortPmpv = nullptr;
  // port-only PSWX: swooshes started beyond SSWH's one, each with its start frame
  struct PortExtraSwoosh {
    TCachedToken< CSwooshDescription > swoosh;
    int frame;
  };
  std::vector< PortExtraSwoosh > xPortExtraSwooshes;
  // port-only PKSM: Remastered's spawn tables as its CSpawnSystemKeyframeData keeps them
  // (ctor 0x293350; run by CElementGen::PortSpawnTables). Replaces KSSM when present.
  struct PortKssmSpawn {
    u32 type = 0; // 'PART', 'SWHC' or 'ELSC'
    // Only the one of `type` is set: a default CToken has no reference, which its
    // assignment and destructor would dereference.
    std::optional< TCachedToken< CGenDescription > > part;
    std::optional< TCachedToken< CSwooshDescription > > swoosh;
    std::optional< TCachedToken< CElectricDescription > > electric;
    CIntElement* instances = nullptr; // GetNumInstances (0x2940fc), evaluated at the current frame
    u32 component = ~0u; // its SEVT component in PortKssm::components; ~0u places it as the parent
  };
  // A node of a component's expression trees (pos, rot, the gravity): op is its fourcc, args index
  // PortKssm::nodes. CNST reals keep `value`; VARF keeps the variable in `slot`; KPIN and IRND keep
  // their per-instance latch (3 floats or 1) at `slot` of PortKssmChild::latches.
  struct PortKssmNode {
    u32 op = 0;
    u32 args[5] = {};
    u32 argCount = 0;
    float value = 0.f;
    u32 slot = 0;
    bool loop = false; // KEYE
    u32 loopStart = 0;
    std::vector< float > keys;
  };
  // An SEVT(SMOV) component: the pos node, the constant delta, the rot node (~0u for NONE) and the
  // GRAV vector node (~0u for none); `latches` floats per instance. liveExtt: pos reads EXTT
  // outside any KPIN, so the child follows the emitter.
  struct PortKssmComponent {
    u32 pos = 0;
    float delta[3] = {};
    u32 rot = ~0u;
    u32 grav = ~0u;
    u32 latches = 0;
    bool liveExtt = false;
  };
  struct PortKssmEntry {
    u32 frame = 0;
    std::vector< PortKssmSpawn > spawns;
  };
  struct PortKssmTable {
    u32 start = 0;
    bool periodConst = true; // the period attribute's type is 0 (a constant)
    s32 period = 0; // its value when constant
    CIntElement* periodElement = nullptr;
    std::vector< PortKssmEntry > entries;
  };
  struct PortKssm {
    u32 end = 0;
    std::vector< PortKssmTable > tables;
    std::vector< PortKssmNode > nodes;
    std::vector< PortKssmComponent > components;
    PortKssm() = default;
    PortKssm(const PortKssm&) = delete;
    PortKssm& operator=(const PortKssm&) = delete;
    ~PortKssm();
  };
  std::unique_ptr< PortKssm > xPortKssm;
  // port-only PSMV: XFMD 4's SMVR mover (CSimpleMoverElement, build/mpr/vfx/RESOLVED-xfmd.md)
  // as one component in the PKSM node format, or no component for an XFMD 4 with no SMVR (the
  // emitter's transform unused). Absent for the identity SMOV(EXTT, 0, EXTR), which is the
  // emitter's own frame.
  std::unique_ptr< PortKssm > xPortMover;
#endif
  CVectorElement* x58_PMOP;
  CVectorElement* x5c_PMRT;
  CVectorElement* x60_PMSC;
  CColorElement* x64_PMCL;
  CModVectorElement* x68_VEL1;
  CModVectorElement* x6c_VEL2;
  CModVectorElement* x70_VEL3;
  CModVectorElement* x74_VEL4;
  rstl::optional_object< TCachedToken< CGenDescription > > x78_ICTS;
  CIntElement* x88_NCSY;
  CIntElement* x8c_CSSD;
  rstl::optional_object< TCachedToken< CGenDescription > > x90_IDTS;
  CIntElement* xa0_NDSY;
  rstl::optional_object< TCachedToken< CGenDescription > > xa4_IITS;
  CIntElement* xb4_PISY;
  CIntElement* xb8_SISY;
  rstl::single_ptr< CSpawnSystemKeyframeData > xbc_KSSM;
  rstl::optional_object< TCachedToken< CSwooshDescription > > xc0_SSWH;
  CIntElement* xd0_SSSD;
  CVectorElement* xd4_SSPO;
  rstl::optional_object< CToken > xd8_SELC;
  CIntElement* xe4_SESD;
  CVectorElement* xe8_SEPO;
  CIntElement* xec_LTYP;
  CColorElement* xf0_LCLR;
  CRealElement* xf4_LINT;
  CVectorElement* xf8_LOFF;
  CVectorElement* xfc_LDIR;
  CIntElement* x100_LFOT;
  CRealElement* x104_LFOR;
  CRealElement* x108_LSLA;
  CRealElement* x10c_ADV1;
  CRealElement* x110_ADV2;
  CRealElement* x114_ADV3;
  CRealElement* x118_ADV4;
  CRealElement* x11c_ADV5;
  CRealElement* x120_ADV6;
  CRealElement* x124_ADV7;
  CRealElement* x128_ADV8;
};
CHECK_SIZEOF(CGenDescription, 0x12c)

#endif // _CGENDESCRIPTION
