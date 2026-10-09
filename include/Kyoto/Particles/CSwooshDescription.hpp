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
  // port-only PIRN: a converted Remastered swoosh, whose COLR runs over the live points
  bool xPortIrnd = false;
  // port-only PVRT: Remastered particle variables (names, types, defaults)
  std::unique_ptr< CPortVarTable > xPortVars;
#endif
};

#endif // _CSWOOSHDESCRIPTION
