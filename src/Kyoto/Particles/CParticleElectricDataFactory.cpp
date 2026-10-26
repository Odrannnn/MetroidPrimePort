#include "Kyoto/Particles/CParticleElectricDataFactory.hpp"

#include "Kyoto/CRandom16.hpp"
#ifdef TARGET_PC
#include "Kyoto/Particles/CPortParticleParseScope.hpp"
#endif
#include "Kyoto/Particles/CElectricDescription.hpp"
#include "Kyoto/Particles/CParticleDataFactory.hpp"
#include "Kyoto/Streams/CInputStream.hpp"

const CFactoryFnReturn FParticleElectricDataFactory(const SObjectTag& tag, CInputStream& in,
                                              const CVParamTransfer& transfer) {
  rstl::rc_ptr< IVParamObj > obj = transfer.x0_obj;
  CSimplePool* pool = static_cast< TObjOwnerParam< CSimplePool* >* >(obj.GetPtr())->GetData();
  return CParticleElectricDataFactory::GetGeneratorDesc(in, pool);
}

CElectricDescription* CParticleElectricDataFactory::GetGeneratorDesc(CInputStream& in,
                                                                     CSimplePool* pool) {
  rstl::vector< CAssetId > resources;
  return CreateGeneratorDescription(in, pool);
}

CElectricDescription* CParticleElectricDataFactory::CreateGeneratorDescription(CInputStream& in,
                                                                               CSimplePool* pool) {
  const FourCC classId = CParticleDataFactory::GetClassID(in);
  if (classId != 'ELSM') {
    return nullptr;
  }
  CElectricDescription* desc = rs_new CElectricDescription();
  CreateELSM(desc, in, pool);
  LoadELSMTokens(desc);
#ifdef TARGET_PC
  if (desc->xPortVfx) {
    for (CPortVfxMat::Tex& t : desc->xPortVfx->mat.tex) {
      t.token.ForceCache();
    }
  }
#endif
  return desc;
}

void CParticleElectricDataFactory::LoadELSMTokens(CElectricDescription* desc) {
  if (desc->x40_SSWH) {
    desc->x40_SSWH->ForceCache();
  }
  if (desc->x50_GPSM) {
    desc->x50_GPSM->ForceCache();
  }
  if (desc->x60_EPSM) {
    desc->x60_EPSM->ForceCache();
  }
}

bool CParticleElectricDataFactory::CreateELSM(CElectricDescription* desc, CInputStream& in,
                                              CSimplePool* pool) {
  bool done = false;
#ifdef TARGET_PC
  CPortParticleParseScope parseScope;
#endif
  CRandom16 random(99);
  while (!done) {
    CGlobalRandom globalRandom(random);
    const FourCC classId = CParticleDataFactory::GetClassID(in);
    switch (classId) {
    case 'LIFE':
      desc->x0_LIFE = CParticleDataFactory::GetIntElement(in);
      break;
    case 'SLIF':
      desc->x4_SLIF = CParticleDataFactory::GetIntElement(in);
      break;
    case 'GRAT':
      desc->x8_GRAT = CParticleDataFactory::GetRealElement(in);
      break;
    case 'SCNT':
      desc->xc_SCNT = CParticleDataFactory::GetIntElement(in);
      break;
    case 'SSEG':
      desc->x10_SSEG = CParticleDataFactory::GetIntElement(in);
      break;
    case 'COLR':
      desc->x14_COLR = CParticleDataFactory::GetColorElement(in);
      break;
    case 'IEMT':
      desc->x18_IEMT = CParticleDataFactory::GetEmitterElement(in);
      break;
    case 'FEMT':
      desc->x1c_FEMT = CParticleDataFactory::GetEmitterElement(in);
      break;
    case 'AMPL':
      desc->x20_AMPL = CParticleDataFactory::GetRealElement(in);
      break;
    case 'AMPD':
      desc->x24_AMPD = CParticleDataFactory::GetRealElement(in);
      break;
    case 'SSWH': {
      const FourCC childId = CParticleDataFactory::GetClassID(in);
      if (childId != 'NONE') {
        const CAssetId id = in.ReadLong();
        if (id != 0) {
          desc->x40_SSWH = TCachedToken< CSwooshDescription >(pool->GetObj(SObjectTag('SWHC', id)));
        }
      }
      break;
    }
    case 'GPSM': {
      rstl::vector< CAssetId > resources;
      rstl::optional_object< TToken< CGenDescription > > child =
          CParticleDataFactory::GetChildGeneratorDesc(in, pool, resources);
      if (child) {
        desc->x50_GPSM = TCachedToken< CGenDescription >(*child);
      }
      break;
    }
    case 'EPSM': {
      rstl::vector< CAssetId > resources;
      rstl::optional_object< TToken< CGenDescription > > child =
          CParticleDataFactory::GetChildGeneratorDesc(in, pool, resources);
      if (child) {
        desc->x60_EPSM = TCachedToken< CGenDescription >(*child);
      }
      break;
    }
    case 'LWD1':
      desc->x28_LWD1 = CParticleDataFactory::GetRealElement(in);
      break;
    case 'LWD2':
      desc->x2c_LWD2 = CParticleDataFactory::GetRealElement(in);
      break;
    case 'LWD3':
      desc->x30_LWD3 = CParticleDataFactory::GetRealElement(in);
      break;
    case 'LCL1':
      desc->x34_LCL1 = CParticleDataFactory::GetColorElement(in);
      break;
    case 'LCL2':
      desc->x38_LCL2 = CParticleDataFactory::GetColorElement(in);
      break;
    case 'LCL3':
      desc->x3c_LCL3 = CParticleDataFactory::GetColorElement(in);
      break;
    case 'ZERY':
      desc->x70_ZERY = CParticleDataFactory::GetBool(in);
      break;
#ifdef TARGET_PC
    // Port-only marker of a converted Remastered ELC2 (CParticleElectric2).
    case 'PEL2':
      CParticleDataFactory::GetClassID(in);
      desc->xPortElc2 = in.ReadLong() != 0;
      break;
    case 'SIDE':
      desc->xPortSIDE = CParticleDataFactory::GetIntElement(in);
      break;
    case 'LWQD':
      desc->xPortLWQD = CParticleDataFactory::GetIntElement(in);
      break;
    case 'NFOM':
      desc->xPortNFOM = CParticleDataFactory::GetIntElement(in);
      break;
    case 'TSPN':
      desc->xPortTSPN = CParticleDataFactory::GetIntElement(in);
      break;
    case 'BDEP':
      desc->xPortBDEP = CParticleDataFactory::GetIntElement(in);
      break;
    case 'BNDI':
      desc->xPortBNDI = CParticleDataFactory::GetIntElement(in);
      break;
    case 'RADS':
      desc->xPortRADS = CParticleDataFactory::GetRealElement(in);
      break;
    case 'LRAD':
      desc->xPortLRAD = CParticleDataFactory::GetRealElement(in);
      break;
    case 'RRAD':
      desc->xPortRRAD = CParticleDataFactory::GetRealElement(in);
      break;
    case 'SLVF':
      desc->xPortSLVF = CParticleDataFactory::GetRealElement(in);
      break;
    case 'BLNC':
      desc->xPortBLNC = CParticleDataFactory::GetRealElement(in);
      break;
    case 'BCFF':
      desc->xPortBCFF = CParticleDataFactory::GetRealElement(in);
      break;
    case 'BLEN':
      desc->xPortBLEN = CParticleDataFactory::GetRealElement(in);
      break;
    case 'BANG':
      desc->xPortBANG = CParticleDataFactory::GetRealElement(in);
      break;
    case 'BIDF':
      desc->xPortBIDF = CParticleDataFactory::GetRealElement(in);
      break;
    case 'BLDF':
      desc->xPortBLDF = CParticleDataFactory::GetRealElement(in);
      break;
    case 'AMDF':
      desc->xPortAMDF = CParticleDataFactory::GetRealElement(in);
      break;
    case 'NCLR':
      desc->xPortNCLR = CParticleDataFactory::GetColorElement(in);
      break;
    // Port-only Remastered transform mode; only 3 (bolts follow the emitter) is written.
    case 'XFMD': {
      CParticleDataFactory::GetClassID(in);
      desc->xPortXfmdFollow = in.ReadLong() == 3;
      break;
    }
    case 'LLRD':
      desc->xPortLLRD = CParticleDataFactory::GetBool(in);
      break;
    case 'FPLB':
      desc->xPortFPLB = CParticleDataFactory::GetBool(in);
      break;
    // Port-only Remastered material, as a PART's.
    case 'VMAT':
    case 'VTMT':
    case 'VPMT':
    case 'VSMT':
    case 'ITEN':
      if (!CParticleDataFactory::PortReadVfxProperty(classId, desc->xPortVfx, in, pool)) {
        return false;
      }
      break;
#endif
    case '_END':
      done = true;
      break;
    default:
      return false;
    }
  }
  return true;
}
