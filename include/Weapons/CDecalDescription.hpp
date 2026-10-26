#ifndef _CDECALDESCRIPTION
#define _CDECALDESCRIPTION

#include "Kyoto/Particles/CGenDescription.hpp"
#include "Kyoto/TToken.hpp"
#include "rstl/optional_object.hpp"

#ifdef TARGET_PC
#include "Kyoto/Graphics/CTexture.hpp"

#include <memory>
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
  // the box's width/height, DUL1/DUL2 each material slot's fade, and per slot its material (PMTI)
  // with the SMTR parameter and TMTR texture-transform elements UpdateMaterialData 0xb7d07c runs.
  struct SPortParam {
    uint xRow;  // 0 DIFC, 1 CCH0, 2 CCH1
    uint xComp; // 4: the whole row (a colour), else the component a real writes
    std::unique_ptr< CColorElement > xColor;
    std::unique_ptr< CRealElement > xReal;
  };
  struct SPortMaterial {
    bool xPresent = false;
    uint xTexture = 0;
    unsigned char xGuid[16] = {};
    uint xShader = 0;
    int xBlend = 2;
    uint xWrapS = 1;
    uint xWrapT = 1;
    uint xLinear = 1;
    // The MATI's DIFC, CCH0 and CCH1, the slot's parameters before the SMTR writes.
    float xDefaults[3][4] = {};
    // Set only for a present slot: a default CToken holds no reference and cannot be released.
    rstl::optional_object< TCachedToken< CTexture > > xTextureToken;
    std::vector< SPortParam > xSmtr;
    // The TRSS's offset u/v, scale u/v, angle (degrees) and layer; none without a TMTR.
    std::unique_ptr< CRealElement > xTmtr[6];
  };
  bool xPortRemastered = false;
  rstl::single_ptr< CIntElement > xPortMSTL;
  rstl::single_ptr< CRealElement > xPortMSTR;
  rstl::single_ptr< CRealElement > xPortMSTW;
  rstl::single_ptr< CRealElement > xPortMSTH;
  rstl::single_ptr< CRealElement > xPortDUL[2];
  SPortMaterial xPortMaterial[2];
#endif
};

CHECK_SIZEOF(CDecalDescription, 0x60);

#endif // _CDECALDESCRIPTION
