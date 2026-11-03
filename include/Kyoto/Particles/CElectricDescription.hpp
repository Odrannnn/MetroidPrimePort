#ifndef _CELECTRICDESCRIPTION
#define _CELECTRICDESCRIPTION

#include "Kyoto/Particles/CGenDescription.hpp"
#include "Kyoto/Particles/CParticleDataFactory.hpp"
#include "Kyoto/Particles/CSwooshDescription.hpp"
#include "Kyoto/TToken.hpp"
#include "rstl/optional_object.hpp"

#ifdef TARGET_PC
#include "Kyoto/Particles/CPortParticleVars.hpp"
#include <memory>
struct CPortVfxData;
#endif

class CColorElement;
class CEmitterElement;
class CIntElement;
class CRealElement;

class CElectricDescription {
public:
  CElectricDescription();
  ~CElectricDescription();

  CIntElement* x0_LIFE;
  CIntElement* x4_SLIF;
  CRealElement* x8_GRAT;
  CIntElement* xc_SCNT;
  CIntElement* x10_SSEG;
  CColorElement* x14_COLR;
  CEmitterElement* x18_IEMT;
  CEmitterElement* x1c_FEMT;
  CRealElement* x20_AMPL;
  CRealElement* x24_AMPD;
  CRealElement* x28_LWD1;
  CRealElement* x2c_LWD2;
  CRealElement* x30_LWD3;
  CColorElement* x34_LCL1;
  CColorElement* x38_LCL2;
  CColorElement* x3c_LCL3;
  rstl::optional_object< TCachedToken< CSwooshDescription > > x40_SSWH;
  rstl::optional_object< TCachedToken< CGenDescription > > x50_GPSM;
  rstl::optional_object< TCachedToken< CGenDescription > > x60_EPSM;
  bool x70_ZERY;
#ifdef TARGET_PC
  // Port-only ELC2 (Remastered CParticleElectric2) fields, read when the stream starts with PEL2.
  // Absent properties keep the ELC2 defaults the runtime applies (8 nodes, SIDE 2, RADS 1, ...).
  bool xPortElc2 = false;
  // Port-only PIRN: any converted Remastered electric, ELSM-form ones included.
  bool xPortIrnd = false;
  CIntElement* xPortSIDE = nullptr;
  CIntElement* xPortLWQD = nullptr;
  CIntElement* xPortNFOM = nullptr;
  CIntElement* xPortTSPN = nullptr;
  CIntElement* xPortBDEP = nullptr;
  CIntElement* xPortBNDI = nullptr;
  CRealElement* xPortRADS = nullptr;
  CRealElement* xPortLRAD = nullptr;
  CRealElement* xPortRRAD = nullptr;
  CRealElement* xPortSLVF = nullptr;
  CRealElement* xPortBLNC = nullptr;
  CRealElement* xPortBCFF = nullptr;
  CRealElement* xPortBLEN = nullptr;
  CRealElement* xPortBANG = nullptr;
  CRealElement* xPortBIDF = nullptr;
  CRealElement* xPortBLDF = nullptr;
  CRealElement* xPortAMDF = nullptr;
  CColorElement* xPortNCLR = nullptr;
  bool xPortLLRD = true;
  bool xPortFPLB = false;
  bool xPortXfmdFollow = false; // XFMD 3: bolts stay emitter-local and follow its live transform
  // IEXP (CreateELC2 0xb716f4 -> desc+0x26a, ctor 0xb71bb8 defaults it to 1): the vertex writer
  // 0x2b9868 scales the AddVar by resolve_inverse_exposure only while this is set.
  bool xPortInverseExposure = true;
  // Port-only visor effect data (desc +0x218, enabled +0x250 bit 0), as CGenDescription's.
  bool xPortVisorData = false;
  float xPortVisorMinScale = 1.4f;
  float xPortVisorMaxScale = 2.1f;
  CColorElement* xPortVisorStartColor = nullptr;
  CColorElement* xPortVisorMidColor = nullptr;
  CColorElement* xPortVisorEndColor = nullptr;
  CRealElement* xPortVisorBlend = nullptr;
  int xPortVisorTicks = -1;
  // port-only Remastered material (VMAT etc.; see CGenDescription::xPortVfx)
  std::unique_ptr< CPortVfxData > xPortVfx;
#endif
};

#ifndef TARGET_PC
CHECK_SIZEOF(CElectricDescription, 0x74)
#endif

#endif // _CELECTRICDESCRIPTION
