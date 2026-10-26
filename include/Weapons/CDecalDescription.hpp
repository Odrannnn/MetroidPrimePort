#ifndef _CDECALDESCRIPTION
#define _CDECALDESCRIPTION

#include "Kyoto/Particles/CGenDescription.hpp"
#include "Kyoto/TToken.hpp"
#include "rstl/optional_object.hpp"

#ifdef TARGET_PC
#include <vector>
#endif

class CDecalDescription {
public:
  struct SQuadDescr {
    rstl::single_ptr< CIntElement > x0_LFT;
    rstl::single_ptr< CRealElement > x4_SZE;
    rstl::single_ptr< CRealElement > x8_ROT;
    rstl::single_ptr< CVectorElement > xc_OFF;
    rstl::single_ptr< CColorElement > x10_CLR;
    rstl::single_ptr< CUVElement > x14_TEX;
    bool x18_ADD;
    SQuadDescr();
  };

  CDecalDescription();

  // private:
  SQuadDescr x0_quad1;
  SQuadDescr x1c_quad2;
  rstl::optional_object< TLockedToken< CModel > > x38_DMDL;
  rstl::single_ptr< CIntElement > x48_DLFT;
  rstl::single_ptr< CVectorElement > x4c_DMOP;
  rstl::single_ptr< CVectorElement > x50_DMRT;
  rstl::single_ptr< CVectorElement > x54_DMSC;
  rstl::single_ptr< CColorElement > x58_DMCL;
  bool x5c_24_DMAB : 1;
  bool x5c_25_DMOO : 1;
#ifdef TARGET_PC
  // port-only: a Remastered decal (DPSM) the importer wrote over its paired DPSC (PRDC). The
  // fields are Remastered's CDecalDescription: MSTL lifetime, MSTR rotation (degrees), MSTW/MSTH
  // the box's width/height, DUL1/DUL2 each material slot's fade, and the slots' MATIs (PMTI).
  struct SPortMaterial {
    uint xSlot;
    uint xTexture;
    unsigned char xGuid[16];
    std::vector< unsigned char > xMati;
  };
  bool xPortRemastered = false;
  rstl::single_ptr< CIntElement > xPortMSTL;
  rstl::single_ptr< CRealElement > xPortMSTR;
  rstl::single_ptr< CRealElement > xPortMSTW;
  rstl::single_ptr< CRealElement > xPortMSTH;
  rstl::single_ptr< CRealElement > xPortDUL[2];
  std::vector< SPortMaterial > xPortMaterials;
#endif
};

CHECK_SIZEOF(CDecalDescription, 0x60);

#endif // _CDECALDESCRIPTION
