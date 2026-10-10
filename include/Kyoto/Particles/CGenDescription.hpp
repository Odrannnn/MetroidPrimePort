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
#include <vector>
#endif

class CElectricDescription;
class CModel;
class CSwooshDescription;
class CTexture;

#ifdef TARGET_PC
// Port-only Remastered particle material (VMAT and friends, build/mpr/vfx/DESIGN.md
// section 1). Present only when the PART carries a VMAT; everything else draws as retail.
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
  // port-only PIRN: a converted Remastered effect, whose nested IRND elements are stable per particle
  bool xPortIrnd = false;
  // port-only PVRT: Remastered particle variables (names, types, defaults)
  std::unique_ptr< CPortVarTable > xPortVars;
  // port-only PFCM: each model particle is turned to face the camera before PMRT
  bool xPortFaceCamera = false;
  // port-only PSWX: swooshes started beyond SSWH's one, each with its start frame
  struct PortExtraSwoosh {
    TCachedToken< CSwooshDescription > swoosh;
    int frame;
  };
  std::vector< PortExtraSwoosh > xPortExtraSwooshes;
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
