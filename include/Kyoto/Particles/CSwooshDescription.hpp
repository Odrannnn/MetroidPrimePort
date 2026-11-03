#ifndef _CSWOOSHDESCRIPTION
#define _CSWOOSHDESCRIPTION

class CColorElement;
class CIntElement;
class CModVectorElement;
class CRealElement;
class CUVElement;
class CVectorElement;
#ifdef TARGET_PC
#include "Kyoto/Particles/CPortParticleVars.hpp"
#include <memory>
struct CPortVfxData;
#endif
class CSwooshDescription {
public:
  CSwooshDescription();
  ~CSwooshDescription();

  CIntElement* x0_PSLT;
  CRealElement* x4_TIME;
  CRealElement* x8_LRAD;
  CRealElement* xc_RRAD;
  CIntElement* x10_LENG;
  CColorElement* x14_COLR;
  CIntElement* x18_SIDE;
  CRealElement* x1c_IROT;
  CRealElement* x20_ROTM;
  CVectorElement* x24_POFS;
  CVectorElement* x28_IVEL;
  CVectorElement* x2c_NPOS;
  CModVectorElement* x30_VELM;
  CModVectorElement* x34_VLM2;
  CIntElement* x38_SPLN;
  CUVElement* x3c_TEXR;
  CIntElement* x40_TSPN;
  bool x44_24_LLRD : 1;
  bool x44_25_CROS : 1;
  bool x44_26_VLS1 : 1;
  bool x44_27_VLS2 : 1;
  bool x44_28_SROT : 1;
  bool x44_29_WIRE : 1;
  bool x44_30_TEXW : 1;
  bool x44_31_AALP : 1;
  bool x45_24_ZBUF : 1;
  bool x45_25_ORNT : 1;
  bool x45_26_CRND : 1;
#ifdef TARGET_PC
  // port-only Remastered material (VMAT etc.; see CGenDescription::xPortVfx)
  std::unique_ptr< CPortVfxData > xPortVfx;
  // port-only PSRO: POFS is turned by the swoosh's orientation (Remastered's ROTV(v, EXTR))
  bool xPortRotateOffset = false;
  // port-only PRAZ/PRAF: Remastered's PROT KPIN(RAZY(EMRV, ROTV(axis, RLCL), fallback)), the
  // fallback the orientation (PRAF 1) or identity
  std::unique_ptr< CVectorElement > xPortRazyAxis;
  bool xPortRazyFallbackLocal = false;
  // port-only PROZ: Remastered's PROT of [KPIN]([RADD(EXTR, ]REUL(0, 0, z))), z in degrees,
  // kept from each point's birth or else re-evaluated for every live point each frame
  std::unique_ptr< CRealElement > xPortRotZ;
  bool xPortRotKeep = false;
  bool xPortRotOriented = false;
  // RADD(INTR, …): the system rotation (PSIR's when present), not the external one
  bool xPortRotIntr = false;
  // port-only XFMD 3: Remastered draws the points in the emitter's frame (SetupTransformMode
  // 0x2887d8: global T/Q/S alias the system vars, local orientation identity, point position zero)
  bool xPortXfmdFollow = false;
  // port-only XFMD 4 with an EMVR of SMOV(EXTT, 0, EXTR) and no SMVR: points are born at the
  // external translation and rotation (vars b8/bc), drawn at identity and unit scale (a8/ac/b0)
  bool xPortXfmdMover = false;
  // port-only XFMD 1 (a zero SYSR under XFMD 2): the system's own transform, zero, identity and
  // one, births and draws the points, so no setter moves them; only EXTR still reads the rotation
  bool xPortXfmdSystem = false;
  // port-only PIRN: a converted Remastered swoosh, whose COLR runs over the live points
  bool xPortIrnd = false;
  // port-only PSED: Remastered's SEED (desc+0x204); nonzero replaces the system's seed
  uint xPortSeed = 0;
  // port-only PGID: the first word of the swoosh's GUID (CElementGen::PortChildSeed)
  u32 xPortGuidWord = 0;
  // port-only PTXU: Remastered's TXUP (desc+0x22c bit 0x80); u scrolls back one TSPN step per point created
  bool xPortTexUScroll = false;
  // port-only PLWQ: Remastered's LWQD (desc+0x228, 1..10); strips per side face across the trail
  int xPortLwqd = 1;
  // port-only PSRD/PSRA: Remastered's SNRD (desc+0x21c, 0..10) and SNRA (+0x220, 0..60). Either
  // above 0 sizes each segment's spline steps by its length and turn instead of SPLN
  // (SetupSplineNodeGenerationMode 0x29bfac; CParticleSwoosh::PortSplineSteps)
  float xPortSnrd = 0.f;
  float xPortSnra = 0.f;
  // port-only PSIR: Remastered's INTR, a constant Euler vector in the port's space (PINR's), times
  // the external orientation when xPortIntExternal; the local orientation under XFMD 2, the global
  // one under 3 (CParticleSwoosh::PortIntOrientation)
  std::unique_ptr< CVectorElement > xPortIntRotation;
  bool xPortIntExternal = false;
  // port-only PSSC: Remastered's PSCL (desc+0x138) in its own numbers, each point's ring scale
  // along its local X and Y (CParticleSwoosh::SSwooshData::mPortScale)
  std::unique_ptr< CVectorElement > xPortScale;
  // port-only PDFG: Remastered's DFOG 1, drawn without volumetric fog (CGenDescription::xPortUnfogged)
  bool xPortUnfogged = false;
  // port-only PZTS: Remastered's ZTES 0, the depth test passes everywhere (compare ALWAYS)
  bool xPortNoZTest = false;
  // port-only PDBS: Remastered's DBIS (CGenDescription::xPortDepthBias)
  float xPortDepthBias = 0.f;
  // port-only PSHM: Remastered's SHTM (CParticleGen::DoShutdownBehavior 0x27b88c); 1 = a shutdown
  // also destroys the live segments (CParticleSwoosh::DestroyParticles 0x27f744)
  u8 xPortShutdownMode = 0;
  // port-only PSHT: Remastered's SHTT (CGenDescription::xPortShutdownTime), the delayed shutdown
  std::unique_ptr< CIntElement > xPortShutdownTime;
  // port-only DVVN/IEXP: Remastered's exposure policy (desc+0x22c bit 0x2000, CreateSWSH 0x2dceac
  // and 0x2dd1b4); DVVN defaults it to DVVN == 1, a later IEXP sets it. UpdateRenderData_Swoosh
  // 0x2b6820 then scales ITEN by the inverse exposure.
  bool xPortExposurePolicy = false;
  bool xPortInverseExposure = false;
  // port-only PVRT: Remastered particle variables (names, types, defaults)
  std::unique_ptr< CPortVarTable > xPortVars;
  // port-only PSYS: the system variables (CParticleSwoosh::PortEvalSys)
  std::unique_ptr< CPortSys > xPortSys;
  // port-only PPVR: the per-point variables (Remastered's desc+0x18 CAddVarSystem), each point's
  // values in CParticleSwoosh::xPortPartValues
  std::unique_ptr< CPortPartVars > xPortPartVars;
  // port-only: the PKPR elements (per-point KPIN latches), latched at a point's birth in this
  // order; owned by the element trees
  std::vector< const CREPortKeep* > xPortKeeps;
#endif
};

#endif // _CSWOOSHDESCRIPTION
