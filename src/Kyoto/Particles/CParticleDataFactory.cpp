#include "Kyoto/Particles/CParticleDataFactory.hpp"

#include <algorithm>

#ifdef TARGET_PC
#include <cstdlib>
#include "aurora/vfx.hpp"
#include "port_log.h"
#include "port_vfx_read.h"
#endif
#include "Kyoto/CFactoryFnReturn.hpp"
#include "Kyoto/CRandom16.hpp"
#include "Kyoto/CSimplePool.hpp"
#include "Kyoto/CVParamTransfer.hpp"
#include "Kyoto/Graphics/CTexture.hpp"
#include "Kyoto/Particles/CColorElement.hpp"
#include "Kyoto/Particles/CEmitterElement.hpp"
#include "Kyoto/Particles/CGenDescription.hpp"
#include "Kyoto/Particles/CIntElement.hpp"
#include "Kyoto/Particles/CModVectorElement.hpp"
#ifdef TARGET_PC
#include "Kyoto/Particles/CPortCurlNoise.hpp"
#endif
#include "Kyoto/Particles/CParticleGen.hpp"
#include "Kyoto/Particles/CPortKeyframeInput.hpp"
#include "Kyoto/Particles/CPortParticleVars.hpp"
#include "Kyoto/Particles/CPortSystemTween.hpp"
#include "Kyoto/Particles/CRealElement.hpp"
#include "Kyoto/Particles/CSpawnSystemKeyframeData.hpp"
#include "Kyoto/Particles/CSwooshDescription.hpp"
#include "Kyoto/Particles/CUVElement.hpp"
#include "Kyoto/Particles/CVectorElement.hpp"
#include "Kyoto/Particles/IElement.hpp"
#include "Kyoto/SObjectTag.hpp"
#include "Kyoto/Streams/CInputStream.hpp"
#include "Kyoto/TToken.hpp"
#include "dolphin/types.h"
#include "rstl/algorithm.hpp"
#include "rstl/list.hpp"
#include "rstl/optional_object.hpp"

#include <cstring>
#define SBIG(v) v

CTexture* CreateTexture(int value);

rstl::list< CElementAllocationChunk > sElementAllocationChunks;
CElementAllocationChunk* IElement::CElementAllocator::sCurrentChunk = nullptr;
CElementAllocationChunk* IElement::CElementAllocator::sFreeChunk = nullptr;

#ifdef TARGET_PC
// Set by a CODE emitter (GetEmitterElement 0x264bc4 sets its bool&), read by the EMTR property.
static bool sPortCodeEmitter = false;
// Set by a PEMV vector (CGenDescription::xPortPemv), collected per description in GetGPSM.
static u8 sPortPemv = 0;
// A PITP mode word with this bit is an ITPE the effect left out (Remastered's default 1);
// MP_ITPE_DEFAULT=0 drops those, to compare.
static const u32 kPortSpreadDefault = 0x100;
static bool PortSpreadDefault() {
  static const bool on = [] {
    const char* env = getenv("MP_ITPE_DEFAULT");
    return env == nullptr || env[0] != '0';
  }();
  return on;
}

namespace {
// Port-only SLCT (the converter's Writer::Element): an int index, a count word, then count
// entries of the selecting element's own type.
template < class Select, class Element >
Select* PortReadSelect(CInputStream& in, Element* (*readEntry)(CInputStream&)) {
  CIntElement* index = CParticleDataFactory::GetIntElement(in);
  const uint count = in.ReadLong();
  std::vector< Element* > entries;
  bool ok = index != nullptr && count != 0 && count <= 256;
  for (uint i = 0; ok && i < count; ++i) {
    entries.push_back(readEntry(in));
    ok = entries.back() != nullptr;
  }
  if (!ok) {
    delete index;
    for (Element* entry : entries) {
      delete entry;
    }
    return nullptr;
  }
  return rs_new Select(index, std::move(entries));
}

// SPA*/DPV*/TPV* (the converter's parent reads): n or a PVAR guid, then the fallback.
template < class Read, class Element >
Element* PortReadParentVar(CInputStream& in, EPortParentRead kind, Element* (*get)(CInputStream&)) {
  SPortParentRead read;
  read.kind = kind;
  if (kind == EPortParentRead::System) {
    read.n = in.ReadLong();
  } else {
    in.ReadBytes(read.guid.bytes.data(), 16);
  }
  Element* fallback = get(in);
  if (fallback == nullptr) {
    return nullptr;
  }
  return rs_new Read(read, fallback);
}

// A PSYS body (after the CNST class id): u32 count, then per item u32 type, slot and
// init-only flag and the element. Null on a malformed one.
CPortSys* PortReadSys(CInputStream& in) {
  const u32 count = in.ReadLong();
  if (count > 256) {
    return nullptr;
  }
  std::unique_ptr< CPortSys > sys(new CPortSys());
  sys->items.resize(count);
  for (SPortSysItem& item : sys->items) {
    const u32 type = in.ReadLong();
    item.slot = in.ReadLong();
    item.initOnly = in.ReadLong() != 0;
    if (type > static_cast< u32 >(EPortVarType::Vector) || item.slot > 255) {
      return nullptr;
    }
    item.type = static_cast< EPortVarType >(type);
    bool ok = false;
    switch (item.type) {
    case EPortVarType::Real:
      ok = (item.real = CParticleDataFactory::GetRealElement(in)) != nullptr;
      break;
    case EPortVarType::Int:
      ok = (item.intE = CParticleDataFactory::GetIntElement(in)) != nullptr;
      break;
    case EPortVarType::Color:
      ok = (item.color = CParticleDataFactory::GetColorElement(in)) != nullptr;
      break;
    default:
      ok = (item.vec = CParticleDataFactory::GetVectorElement(in)) != nullptr;
      break;
    }
    if (!ok) {
      return nullptr;
    }
    sys->counts[type] = std::max(sys->counts[type], item.slot + 1);
    if (!item.initOnly && item.slot < 10) {
      sys->defined[type] |= 1u << item.slot;
    }
  }
  return sys.release();
}

// A PPVR body (after the CNST class id): u32 count, then per item u32 kind, slot and
// constant-after-start flag, and for kinds 1 and 2 the element. Null on a malformed one.
CPortPartVars* PortReadPartVars(CInputStream& in) {
  const u32 count = in.ReadLong();
  if (count > 13) {
    return nullptr;
  }
  std::unique_ptr< CPortPartVars > vars(new CPortPartVars());
  vars->items.resize(count);
  u32 seen = 0;
  bool bad = false;
  for (SPortPartVarItem& item : vars->items) {
    const u32 kind = in.ReadLong();
    const u32 slot = in.ReadLong();
    item.constAfterStart = in.ReadLong() != 0;
    if (kind > 2) {
      return nullptr;  // the item's body length is unknown: the stream cannot be resynced
    }
    // A bad slot or a duplicate (it would evaluate the variable twice) still reads its element,
    // so the rest of the property is consumed and the stream stays in sync; the table is dropped.
    if (slot >= (kind == 2 ? 4u : kind == 1 ? 9u : 8u) || (kind == 1 && slot != 8)) {
      bad = true;
    } else {
      const u32 bit = 1u << (kind == 2 ? 9 + slot : slot);
      bad = bad || (seen & bit) != 0;
      seen |= bit;
    }
    item.kind = static_cast< EPortPartVarKind >(kind);
    item.slot = slot;
    if (kind == 1 && (item.real = CParticleDataFactory::GetRealElement(in)) == nullptr) {
      return nullptr;
    }
    if (kind == 2 && (item.intE = CParticleDataFactory::GetIntElement(in)) == nullptr) {
      return nullptr;
    }
  }
  return bad ? nullptr : vars.release();
}

enum EPortKssmKind { kPKK_Real, kPKK_Vector, kPKK_Rotation };

// One node of a PKSM component tree (the converter's Writer::Component*), appended to kssm.nodes
// with its index in `out`; false on anything the converter does not write.
bool PortReadKssmNode(CInputStream& in, CGenDescription::PortKssm& kssm,
                      CGenDescription::PortKssmComponent& component, EPortKssmKind kind, int depth,
                      u32& out, bool noneIsAbsent = false) {
  if (depth > 64 || kssm.nodes.size() >= 100000) {
    return false;
  }
  CGenDescription::PortKssmNode node;
  node.op = in.ReadLong();
  if (noneIsAbsent && node.op == SBIG('NONE')) {
    out = ~0u;
    return true;
  }
  EPortKssmKind args[5];
  int argCount = 0;
  if (kind == kPKK_Real) {
    switch (node.op) {
    case SBIG('CNST'):
      node.value = in.ReadFloat();
      break;
    case SBIG('VARF'):
    case SBIG('PSYR'):
      node.slot = in.ReadLong();
      break;
    case SBIG('KEYE'): {
      const u32 percent = in.ReadLong();
      in.ReadLong();
      node.loop = in.ReadChar() != 0;
      in.ReadChar();
      in.ReadLong();
      node.loopStart = in.ReadLong();
      const u32 count = in.ReadLong();
      if (percent != 0 || count == 0 || count > kPortVfxMaxElems) {
        return false;
      }
      node.keys.resize(count);
      for (u32 i = 0; i < count; ++i) {
        node.keys[i] = in.ReadFloat();
      }
      break;
    }
    case SBIG('IRND'):
      node.slot = component.latches;
      component.latches += 1;
      args[0] = args[1] = kPKK_Real;
      argCount = 2;
      break;
    case SBIG('ADD_'):
    case SBIG('MULT'):
    case SBIG('RAND'):
      args[0] = args[1] = kPKK_Real;
      argCount = 2;
      break;
    default:
      return false;
    }
  } else if (kind == kPKK_Vector) {
    switch (node.op) {
    case SBIG('EXTT'):
    case SBIG('EXTS'):
      break;
    case SBIG('KPIN'):
      node.slot = component.latches;
      component.latches += 3;
      args[0] = kPKK_Vector;
      argCount = 1;
      break;
    case SBIG('ADD_'):
    case SBIG('MULT'):
      args[0] = args[1] = kPKK_Vector;
      argCount = 2;
      break;
    case SBIG('ROTV'):
      args[0] = kPKK_Vector;
      args[1] = kPKK_Rotation;
      argCount = 2;
      break;
    case SBIG('CNST'):
      args[0] = args[1] = args[2] = kPKK_Real;
      argCount = 3;
      break;
    case SBIG('CIRC'):
      args[0] = args[1] = kPKK_Vector;
      args[2] = args[3] = args[4] = kPKK_Real;
      argCount = 5;
      break;
    default:
      return false;
    }
  } else {
    switch (node.op) {
    case SBIG('EXTR'):
    case SBIG('IDNT'):
      break;
    case SBIG('REUL'):
      args[0] = args[1] = args[2] = kPKK_Real;
      argCount = 3;
      break;
    case SBIG('RADD'):
      args[0] = args[1] = kPKK_Rotation;
      argCount = 2;
      break;
    default:
      return false;
    }
  }
  node.argCount = u32(argCount);
  out = u32(kssm.nodes.size());
  kssm.nodes.push_back(node);
  for (int i = 0; i < argCount; ++i) {
    u32 arg;
    if (!PortReadKssmNode(in, kssm, component, args[i], depth + 1, arg)) {
      return false;
    }
    kssm.nodes[out].args[i] = arg;
  }
  return true;
}

// Whether a vector tree reads EXTT outside any KPIN (KPIN keeps only its frame-0 value).
bool PortKssmLiveExtt(const CGenDescription::PortKssm& kssm, u32 index) {
  const CGenDescription::PortKssmNode& node = kssm.nodes[index];
  if (node.op == SBIG('EXTT')) {
    return true;
  }
  if (node.op == SBIG('KPIN')) {
    return false;
  }
  for (u32 i = 0; i < node.argCount; ++i) {
    if (PortKssmLiveExtt(kssm, node.args[i])) {
      return true;
    }
  }
  return false;
}

// One KSSM SMOV component: pos vector, delta, rot (NONE = absent), modifier (GRAV or NONE).
bool PortReadKssmComponent(CInputStream& in, CGenDescription::PortKssm& kssm,
                           CGenDescription::PortKssmComponent& component) {
  if (!PortReadKssmNode(in, kssm, component, kPKK_Vector, 0, component.pos)) {
    return false;
  }
  component.liveExtt = PortKssmLiveExtt(kssm, component.pos);
  for (float& delta : component.delta) {
    delta = in.ReadFloat();
  }
  if (!PortReadKssmNode(in, kssm, component, kPKK_Rotation, 0, component.rot, true)) {
    return false;
  }
  const u32 modifier = in.ReadLong();
  if (modifier == SBIG('GRAV')) {
    return PortReadKssmNode(in, kssm, component, kPKK_Vector, 0, component.grav);
  }
  return modifier == SBIG('NONE');
}
} // namespace
#endif

void* IElement::CElementAllocator::Alloc(size_t size, const char*, const char*) {
  if (sCurrentChunk == nullptr || !sCurrentChunk->CanAllocate(size)) {
    sElementAllocationChunks.push_back(CElementAllocationChunk());
    sCurrentChunk = &sElementAllocationChunks.back();
  }

  return sCurrentChunk->Allocate(size);
}

void IElement::CElementAllocator::Free(void* ptr, size_t) {
  if (ptr == nullptr) {
    return;
  }

  if (sFreeChunk == nullptr || !sFreeChunk->Contains(ptr)) {
    sFreeChunk = nullptr;
    for (AUTO(it, sElementAllocationChunks.begin()); it != sElementAllocationChunks.end(); ++it) {
      if (it->Contains(ptr)) {
        sFreeChunk = &*it;
        break;
      }
    }
  }

  sFreeChunk->Free(ptr);
  if (sFreeChunk->GetAllocationCount() == 0) {
    for (AUTO(it, sElementAllocationChunks.begin()); it != sElementAllocationChunks.end(); ++it) {
      if (&*it == sFreeChunk) {
        sElementAllocationChunks.erase(it);
        if (sCurrentChunk == sFreeChunk) {
          sCurrentChunk = nullptr;
        }
        break;
      }
    }
    sFreeChunk = nullptr;
  }
}

const CFactoryFnReturn FParticleFactory(const SObjectTag& tag, CInputStream& in,
                                  const CVParamTransfer& xfer) {
  rstl::rc_ptr< IVParamObj > obj = xfer.x0_obj;
  CSimplePool* pool = static_cast< TObjOwnerParam< CSimplePool* >* >(obj.GetPtr())->GetData();
  CGenDescription* desc = CParticleDataFactory::GetGeneratorDesc(in, pool, tag.GetId());
  return desc;
}

CGenDescription* CParticleDataFactory::GetGeneratorDesc(CInputStream& in, CSimplePool* pool,
                                                        uint id) {
  rstl::vector< uint > assets;
  assets.reserve(8);
  return CParticleDataFactory::CreateGeneratorDescription(in, assets, id, pool);
}

CGenDescription* CParticleDataFactory::CreateGeneratorDescription(CInputStream& in,
                                                                  rstl::vector< uint >& assets,
                                                                  const uint id,
                                                                  CSimplePool* pool) {
  if (rstl::count(assets.begin(), assets.end(), id) != 0) {
    return nullptr;
  }
  assets.push_back(id);
  FourCC clsId = GetClassID(in);
  if (clsId != SBIG('GPSM')) {
    return nullptr;
  }
  CGenDescription* desc = rs_new CGenDescription;
#ifdef TARGET_PC
  std::vector< const CREPortKeep* >* const outerKeeps = CPortKeepGlobals::sCollect;
  CPortKeepGlobals::sCollect = &desc->xPortKeeps;
  const u8 outerPemv = sPortPemv;
  sPortPemv = 0;
#endif
  CreateGPSM(desc, in, assets, pool);
#ifdef TARGET_PC
  CPortKeepGlobals::sCollect = outerKeeps;
  desc->xPortPemv = sPortPemv;
  sPortPemv = outerPemv;
#endif
  LoadGPSMTokens(desc);
  return desc;
}

void CParticleDataFactory::LoadGPSMTokens(CGenDescription* desc) {
  if (desc->x48_PMDL) {
    desc->x48_PMDL->ForceCache();
  }
#ifdef TARGET_PC
  for (TCachedToken< CModel >& variant : desc->xPortPMDV) {
    variant.ForceCache();
  }
  if (desc->xPortVfx) {
    for (CPortVfxMat::Tex& t : desc->xPortVfx->mat.tex) {
      t.token.ForceCache();
    }
  }
#endif
  if (desc->x78_ICTS) {
    desc->x78_ICTS->ForceCache();
  }
  if (desc->x90_IDTS) {
    desc->x90_IDTS->ForceCache();
  }
  if (desc->xa4_IITS) {
    desc->xa4_IITS->ForceCache();
  }
  if (desc->xc0_SSWH) {
    desc->xc0_SSWH->ForceCache();
  }
#ifdef TARGET_PC
  for (auto& extra : desc->xPortExtraSwooshes) {
    extra.swoosh.ForceCache();
  }
  if (desc->xPortKssm) {
    for (auto& table : desc->xPortKssm->tables) {
      for (auto& entry : table.entries) {
        for (auto& spawn : entry.spawns) {
          if (spawn.type == SBIG('PART')) {
            spawn.part->ForceCache();
          } else if (spawn.type == SBIG('SWHC')) {
            spawn.swoosh->ForceCache();
          }
        }
      }
    }
  }
#endif
}

#ifdef TARGET_PC
// CParticleDataFactory::PortReadVfxProperty and its VMSH/VMAT readers live in
// platform/port_particle_vfx_read.cpp.
#endif

bool CParticleDataFactory::CreateGPSM(CGenDescription* desc, CInputStream& in,
                                      rstl::vector< CAssetId >& resources, CSimplePool* pool) {
  bool done = false;
  CRandom16 random(99);
  CGlobalRandom context(random);
  while (!done) {
    FourCC clsId = GetClassID(in);
    switch (clsId) {
    case SBIG('PSIV'):
      delete GetVectorElement(in);
      break;
    case SBIG('PSVM'):
      delete GetModVectorElement(in);
      break;
    case SBIG('PSOV'):
      delete GetVectorElement(in);
      break;
    case SBIG('PSTS'):
      desc->x8_PSTS = GetRealElement(in);
      break;
    case SBIG('PSLT'):
      desc->x0_PSLT = GetIntElement(in);
      break;
    case SBIG('PSWT'):
      desc->x4_PSWT = GetIntElement(in);
      break;
    case SBIG('LIT_'):
      desc->x30_29_LIT_ = GetBool(in);
      break;
    case SBIG('ORNT'):
      desc->x30_30_ORNT = GetBool(in);
      break;
    case SBIG('RSOP'):
      desc->x30_31_RSOP = GetBool(in);
      break;
    case SBIG('AAPH'):
      desc->x30_26_AAPH = GetBool(in);
      break;
    case SBIG('ZBUF'):
      desc->x30_27_ZBUF = GetBool(in);
      break;
    case SBIG('SORT'):
      desc->x30_28_SORT = GetBool(in);
      break;
    case SBIG('MBLR'):
      desc->x31_24_MBLR = GetBool(in);
      break;
    case SBIG('MBSP'):
      desc->x34_MBSP = GetIntElement(in);
      break;
    case SBIG('MAXP'):
      desc->x1c_MAXP = GetIntElement(in);
      break;
    case SBIG('GRTE'):
      desc->x20_GRTE = GetRealElement(in);
      break;
    case SBIG('ILOC'):
      delete GetVectorElement(in);
      break;
    case SBIG('IVEC'):
      delete GetVectorElement(in);
      break;
    case SBIG('EMTR'):
#ifdef TARGET_PC
      sPortCodeEmitter = false;
#endif
      desc->x2c_EMTR = GetEmitterElement(in);
#ifdef TARGET_PC
      desc->xPortCodeSpawn = sPortCodeEmitter;
#endif
      break;
    case SBIG('SIZE'):
      desc->x38_SIZE = GetRealElement(in);
      break;
    case SBIG('COLR'):
      desc->x24_COLR = GetColorElement(in);
      break;
    case SBIG('POFS'):
      desc->xc_POFS = GetVectorElement(in);
      break;
    case SBIG('VMD1'):
      desc->x31_28_VMD1 = GetBool(in);
      break;
    case SBIG('VMD2'):
      desc->x31_29_VMD2 = GetBool(in);
      break;
    case SBIG('VMD3'):
      desc->x31_30_VMD3 = GetBool(in);
      break;
    case SBIG('VMD4'):
      desc->x31_31_VMD4 = GetBool(in);
      break;
    case SBIG('VEL1'):
      desc->x68_VEL1 = GetModVectorElement(in);
      break;
    case SBIG('VEL2'):
      desc->x6c_VEL2 = GetModVectorElement(in);
      break;
    case SBIG('VEL3'):
      desc->x70_VEL3 = GetModVectorElement(in);
      break;
    case SBIG('VEL4'):
      desc->x74_VEL4 = GetModVectorElement(in);
      break;
    case SBIG('LTME'):
      desc->x28_LTME = GetIntElement(in);
      break;
    case SBIG('ROTA'):
      desc->x3c_ROTA = GetRealElement(in);
      break;
    case SBIG('LENG'):
      desc->x14_LENG = GetRealElement(in);
      break;
    case SBIG('WIDT'):
      desc->x18_WIDT = GetRealElement(in);
      break;
    case SBIG('TEXR'):
      desc->x40_TEXR = GetTextureElement(in, pool);
      break;
    case SBIG('TIND'):
      desc->x44_TIND = GetTextureElement(in, pool);
      break;
    case SBIG('CIND'):
      desc->x32_24_CIND = GetBool(in);
      break;
    case SBIG('PMDL'): {
      rstl::optional_object< TToken< CModel > > model(GetModel(in, pool));
      if (model) {
        desc->x48_PMDL = TCachedToken< CModel >(*model);
      } else {
        desc->x48_PMDL = rstl::optional_object_null();
      }
    } break;
#ifdef TARGET_PC
    case SBIG('PMDV'): {
      // 'CNST' <s32 n>, then n x 'CNST' <CMDL id>
      desc->xPortPMDV.clear();
      GetClassID(in);
      const int count = in.ReadLong();
      for (int i = 0; i < count; ++i) {
        GetClassID(in);
        const CAssetId id = in.ReadLong();
        if (id != 0) {
          desc->xPortPMDV.push_back(
              TCachedToken< CModel >(pool->GetObj(SObjectTag(SBIG('CMDL'), id))));
        }
      }
    } break;
    // Port-only Remastered transform mode (EPortXfmd); unknown modes draw as retail.
    case SBIG('XFMD'): {
      GetClassID(in);
      const u32 mode = in.ReadLong();
      desc->xPortXfmd = mode == kPortXfmdFollow || mode == kPortXfmdFollowUnscaled
                            ? static_cast< u8 >(mode)
                            : static_cast< u8 >(kPortXfmdRetail);
    } break;
    // Port-only Remastered INTS system scale (CGenDescription::xPortIntScale).
    case SBIG('PINS'):
      delete desc->xPortIntScale;
      desc->xPortIntScale = GetVectorElement(in);
      break;
    // Port-only Remastered INTR/INTT system orientation and translation
    // (CGenDescription::xPortIntRotation/xPortIntTranslation).
    case SBIG('PINR'):
      delete desc->xPortIntRotation;
      desc->xPortIntRotation = GetVectorElement(in);
      break;
    case SBIG('PINT'):
      delete desc->xPortIntTranslation;
      desc->xPortIntTranslation = GetVectorElement(in);
      break;
    // Port-only Remastered PMRB/PMCF model rotation frame (CGenDescription::xPortPmrqFrame).
    case SBIG('PMRF'): {
      GetClassID(in);
      const u32 frame = in.ReadLong();
      desc->xPortPmrqFrame = frame <= 2 ? static_cast< u8 >(frame) : 0;
    } break;
    // Port-only Remastered particle variables (PVRT); a malformed table leaves none.
    case SBIG('PVRT'):
      GetClassID(in);
      desc->xPortVars.reset(PortReadVarTable(in));
      break;
    // Port-only Remastered system variables (PSYS); a malformed one leaves none.
    case SBIG('PSYS'):
      GetClassID(in);
      desc->xPortSys.reset(PortReadSys(in));
      break;
    // Port-only PADO: the order ADV1..8 are evaluated in, a count and 0-based indices.
    case SBIG('PADO'): {
      GetClassID(in);
      const u32 count = in.ReadLong();
      desc->xPortAdvOrder.clear();
      if (count > 8) {
        break;
      }
      // Each index at most once: a duplicate would run one ADV twice and skip another.
      u32 seen = 0;
      for (u32 i = 0; i < count; ++i) {
        const u32 n = in.ReadLong();
        if (n < 8 && (seen & (1u << n)) == 0) {
          seen |= 1u << n;
          desc->xPortAdvOrder.push_back(static_cast< u8 >(n));
        }
      }
      if (desc->xPortAdvOrder.size() != count) {
        desc->xPortAdvOrder.clear();
      }
      break;
    }
    // Port-only PPVR: Remastered's variables ADV1..9 and IAV0..3 in SortVars order (PortReadPartVars).
    case SBIG('PPVR'):
      GetClassID(in);
      desc->xPortPartVars.reset(PortReadPartVars(in));
      if (!desc->xPortPartVars) {
        PortLog::Write("particles: malformed PPVR, falling back to the retail ADV path\n");
      }
      break;
    // Port-only marker of a converted Remastered PART (xPortIrnd).
    case SBIG('PIRN'):
      GetClassID(in);
      desc->xPortIrnd = in.ReadLong() != 0;
      break;
    // Port-only visor effect data of a converted Remastered PART: present, and the splat size
    // range (VESN/VESM as float bits).
    case SBIG('PVSD'):
      GetClassID(in);
      desc->xPortVisorData = in.ReadLong() != 0;
      break;
    case SBIG('PVSN'):
    case SBIG('PVSM'): {
      GetClassID(in);
      const uint bits = in.ReadLong();
      float value;
      std::memcpy(&value, &bits, sizeof(value));
      (clsId == SBIG('PVSN') ? desc->xPortVisorMinScale : desc->xPortVisorMaxScale) = value;
      break;
    }
    // Port-only visor effect override data: the three colours, the blend and the ticks.
    case SBIG('PVSC'):
      delete desc->xPortVisorStartColor;
      desc->xPortVisorStartColor = GetColorElement(in);
      break;
    case SBIG('PVMC'):
      delete desc->xPortVisorMidColor;
      desc->xPortVisorMidColor = GetColorElement(in);
      break;
    case SBIG('PVTC'):
      delete desc->xPortVisorEndColor;
      desc->xPortVisorEndColor = GetColorElement(in);
      break;
    case SBIG('PVBS'):
      delete desc->xPortVisorBlend;
      desc->xPortVisorBlend = GetRealElement(in);
      break;
    case SBIG('PVDT'):
      GetClassID(in);
      desc->xPortVisorTicks = static_cast< int >(in.ReadLong());
      break;
    // Port-only first word of a converted Remastered PART's GUID (the child seed hashes it).
    case SBIG('PGID'):
      GetClassID(in);
      desc->xPortGuidWord = in.ReadLong();
      break;
    // Port-only metadata for the one nested Remastered point light (any child generator).
    case SBIG('PHDR'):
      GetClassID(in);
      desc->xPortNestedHdrLight = in.ReadLong() != 0;
      break;
    case SBIG('PIR0'):
      delete desc->xPortNestedLightInnerRadius;
      desc->xPortNestedLightInnerRadius = GetRealElement(in);
      break;
    case SBIG('PHFO'):
      GetClassID(in);
      desc->xPortNestedLightFalloff = static_cast< u8 >(in.ReadLong() & 3);
      break;
    case SBIG('PLIX'):
      GetClassID(in);
      desc->xPortNestedLightInverseExposure = in.ReadLong() != 0;
      break;
    case SBIG('POR0'):
      delete desc->xPortNestedLightOuterRadius;
      desc->xPortNestedLightOuterRadius = GetRealElement(in);
      break;
    // Port-only: the swooshes a converted effect starts beyond SSWH's one (xPortExtraSwooshes).
    case SBIG('PSWX'): {
      GetClassID(in);
      const u32 count = in.ReadLong();
      if (count > kPortVfxMaxElems) {
        return false;
      }
      for (u32 i = 0; i < count; ++i) {
        const CAssetId id = in.Get< CAssetId >();
        const int frame = int(in.ReadLong());
        if (id != 0) {
          desc->xPortExtraSwooshes.push_back(
              {TCachedToken< CSwooshDescription >(TToken< CSwooshDescription >(pool->GetObj(SObjectTag(SBIG('SWHC'), id)))), frame});
        }
      }
      break;
    }
    // Port-only: Remastered's spawn tables (xPortKssm): CNST, the end frame and the tables; per
    // table its start, whether the period is constant, the period, and the entries; per entry
    // its frame and spawns; per spawn the type, the asset and the instance count.
    case SBIG('PKSM'): {
      GetClassID(in);
      std::unique_ptr< CGenDescription::PortKssm > kssm(new CGenDescription::PortKssm);
      kssm->end = in.ReadLong();
      const u32 tables = in.ReadLong();
      if (tables > kPortVfxMaxElems) {
        return false;
      }
      kssm->tables.resize(tables);
      for (CGenDescription::PortKssmTable& table : kssm->tables) {
        table.start = in.ReadLong();
        table.periodConst = in.ReadLong() != 0;
        table.periodElement = GetIntElement(in);
        table.period = 0;
        if (table.periodElement != nullptr) {
          table.periodElement->GetValue(0, table.period);
        }
        const u32 entries = in.ReadLong();
        if (entries > 1000) {
          return false;
        }
        table.entries.resize(entries);
        for (CGenDescription::PortKssmEntry& entry : table.entries) {
          entry.frame = in.ReadLong();
          const u32 spawns = in.ReadLong();
          if (spawns > 100) {
            return false;
          }
          entry.spawns.resize(spawns);
          for (CGenDescription::PortKssmSpawn& spawn : entry.spawns) {
            spawn.type = in.ReadLong();
            const CAssetId id = in.Get< CAssetId >();
            spawn.instances = GetIntElement(in);
            spawn.component = in.ReadLong();
            if (spawn.type == SBIG('PART')) {
              spawn.part.emplace(
                  TToken< CGenDescription >(pool->GetObj(SObjectTag(CParticleGen::ResType(), id))));
            } else if (spawn.type == SBIG('SWHC')) {
              spawn.swoosh.emplace(
                  TToken< CSwooshDescription >(pool->GetObj(SObjectTag(SBIG('SWHC'), id))));
            } else if (spawn.type == SBIG('ELSC')) {
              spawn.electric.emplace(
                  TToken< CElectricDescription >(pool->GetObj(SObjectTag(SBIG('ELSC'), id))));
            } else {
              return false;
            }
          }
        }
      }
      // Then the SEVT components the spawns place their children with: per component its pos
      // tree, the constant delta, the rot tree or NONE, and NONE or GRAV and its vector tree.
      const u32 components = in.ReadLong();
      if (components > 1000) {
        return false;
      }
      kssm->components.resize(components);
      for (CGenDescription::PortKssmComponent& component : kssm->components) {
        if (!PortReadKssmComponent(in, *kssm, component)) {
          return false;
        }
      }
      for (const CGenDescription::PortKssmTable& table : kssm->tables) {
        for (const CGenDescription::PortKssmEntry& entry : table.entries) {
          for (const CGenDescription::PortKssmSpawn& spawn : entry.spawns) {
            if (spawn.component != ~0u && spawn.component >= components) {
              return false;
            }
          }
        }
      }
      desc->xPortKssm = std::move(kssm);
      break;
    }
    // Port-only: XFMD 4's mover (xPortMover): CNST, the component count (0 or 1), then the
    // component as in PKSM.
    case SBIG('PSMV'): {
      GetClassID(in);
      std::unique_ptr< CGenDescription::PortKssm > mover(new CGenDescription::PortKssm);
      const u32 components = in.ReadLong();
      if (components > 1) {
        return false;
      }
      mover->components.resize(components);
      for (CGenDescription::PortKssmComponent& component : mover->components) {
        if (!PortReadKssmComponent(in, *mover, component)) {
          return false;
        }
      }
      desc->xPortMover = std::move(mover);
      break;
    }
    // Port-only: Remastered's sprite polygon (SSHP 3-6; xPortSpriteShape).
    case SBIG('PSHP'):
      GetClassID(in);
      desc->xPortSpriteShape = u8(in.ReadLong());
      break;
    // Port-only: Remastered's PBDM 1 or 3 (xPortBlend).
    case SBIG('PBDM'):
      GetClassID(in);
      desc->xPortBlend = u8(in.ReadLong());
      break;
    // Port-only: Remastered's ZTES 0 (xPortNoZTest).
    case SBIG('PZTS'):
      GetClassID(in);
      desc->xPortNoZTest = in.ReadLong() == 0;
      break;
    // Port-only: Remastered's IVSE 0 (xPortNoIvse).
    case SBIG('PIVS'):
      GetClassID(in);
      desc->xPortNoIvse = in.ReadLong() == 0;
      break;
    // Port-only: Remastered's ISVF (xPortInheritVel), after PEMV's mode word.
    case SBIG('PISV'): {
      GetClassID(in);
      sPortPemv = u8(in.ReadLong() + 1);
      delete desc->xPortInheritVel;
      desc->xPortInheritVel = GetRealElement(in);
      break;
    }
    // Port-only: Remastered's ITPE 1 (xPortSpread), with PEMV's mode word.
    case SBIG('PITP'): {
      GetClassID(in);
      const u32 word = in.ReadLong();
      if ((word & kPortSpreadDefault) == 0 || PortSpreadDefault()) {
        sPortPemv = u8((word & 0xff) + 1);
        desc->xPortSpread = true;
      }
      break;
    }
    // Port-only: Remastered's SHTM, SHTT, PMOV and PMPV (CGenDescription::xPortShutdownMode and
    // the elements after it).
    case SBIG('PSHM'):
      GetClassID(in);
      desc->xPortShutdownMode = u8(in.ReadLong());
      break;
    case SBIG('PISD'):
      GetClassID(in);
      desc->xPortChildShutdownByMode = in.ReadLong() != 0;
      break;
    case SBIG('PBLT'):
      GetClassID(in);
      desc->xPortBakedLighting = in.ReadLong() != 0;
      break;
    case SBIG('VMPC'):
      GetClassID(in);
      desc->xPortVelocityAtCreation = in.ReadLong() != 0;
      break;
    case SBIG('DVVN'):
      GetClassID(in);
      desc->xPortDefaultValuesVersion = u8(in.ReadLong());
      break;
    case SBIG('PSHT'):
      delete desc->xPortShutdownTime;
      desc->xPortShutdownTime = GetIntElement(in);
      break;
    case SBIG('PMOV'):
      delete desc->xPortPmov;
      desc->xPortPmov = GetVectorElement(in);
      break;
    case SBIG('PMPV'):
      delete desc->xPortPmpv;
      desc->xPortPmpv = GetVectorElement(in);
      break;
    // Port-only: Remastered's DFOG 1 (xPortUnfogged).
    case SBIG('PDFG'):
      GetClassID(in);
      desc->xPortUnfogged = in.ReadLong() != 0;
      break;
    // Port-only: Remastered's DBIS (xPortDepthBias), a constant real.
    case SBIG('PDBS'):
      GetClassID(in);
      desc->xPortDepthBias = in.ReadFloat();
      break;
    // Port-only Remastered particle material (build/mpr/vfx/DESIGN.md section 1).
    case SBIG('VMAT'):
    case SBIG('VMSH'):
    case SBIG('VPRM'):
    case SBIG('VTMT'):
    case SBIG('VPMT'):
    case SBIG('VSMT'):
    case SBIG('SSZE'):
    case SBIG('ITEN'):
    case SBIG('VORN'):
      if (!PortReadVfxProperty(clsId, desc->xPortVfx, in, pool)) {
        return false;
      }
      break;
#endif
    case SBIG('PMOP'):
      desc->x58_PMOP = GetVectorElement(in);
      break;
    case SBIG('PMRT'):
      desc->x5c_PMRT = GetVectorElement(in);
      break;
    case SBIG('PMSC'):
      desc->x60_PMSC = GetVectorElement(in);
      break;
    case SBIG('PMCL'):
      desc->x64_PMCL = GetColorElement(in);
      break;
    case SBIG('PMAB'):
      desc->x31_25_PMAB = GetBool(in);
      break;
    case SBIG('PMUS'):
      desc->x31_26_PMUS = GetBool(in);
      break;
    case SBIG('PMOO'):
      desc->x31_27_PMOO = GetBool(in);
      break;
    case SBIG('SEED'):
      desc->x10_SEED = GetIntElement(in);
      break;
    case SBIG('ICTS'): {
      rstl::optional_object< TToken< CGenDescription > > child(
          GetChildGeneratorDesc(in, pool, resources));
      if (child) {
        desc->x78_ICTS = TCachedToken< CGenDescription >(*child);
      } else {
        desc->x78_ICTS = rstl::optional_object_null();
      }
      break;
    }
    case SBIG('NCSY'):
      desc->x88_NCSY = GetIntElement(in);
      break;
    case SBIG('CSSD'):
      desc->x8c_CSSD = GetIntElement(in);
      break;
    case SBIG('IDTS'): {
      rstl::optional_object< TToken< CGenDescription > > child(
          GetChildGeneratorDesc(in, pool, resources));
      if (child) {
        desc->x90_IDTS = TCachedToken< CGenDescription >(*child);
      } else {
        desc->x90_IDTS = rstl::optional_object_null();
      }
      break;
    }
    case SBIG('NDSY'):
      desc->xa0_NDSY = GetIntElement(in);
      break;
    case SBIG('IITS'): {
      rstl::optional_object< TToken< CGenDescription > > child(
          GetChildGeneratorDesc(in, pool, resources));
      if (child) {
        desc->xa4_IITS = TCachedToken< CGenDescription >(*child);
      } else {
        desc->xa4_IITS = rstl::optional_object_null();
      }
      break;
    }
    case SBIG('PISY'):
      desc->xb4_PISY = GetIntElement(in);
      break;
    case SBIG('SISY'):
      desc->xb8_SISY = GetIntElement(in);
      break;
    case SBIG('SSWH'): {
      rstl::optional_object< TToken< CSwooshDescription > > child(GetSwooshGeneratorDesc(in, pool));
      if (child) {
        desc->xc0_SSWH = TCachedToken< CSwooshDescription >(*child);
      } else {
        desc->xc0_SSWH = rstl::optional_object_null();
      }
      break;
    }
    case SBIG('SSSD'):
      desc->xd0_SSSD = GetIntElement(in);
      break;
    case SBIG('SSPO'):
      desc->xd4_SSPO = GetVectorElement(in);
      break;
    case SBIG('SELC'): {
      rstl::optional_object< TToken< CElectricDescription > > electric =
          GetElectricGeneratorDesc(in, pool);
      if (electric) {
        desc->xd8_SELC = *electric;
      } else {
        desc->xd8_SELC = rstl::optional_object_null();
      }
      break;
    }
    case SBIG('SESD'):
      desc->xe4_SESD = GetIntElement(in);
      break;
    case SBIG('SEPO'):
      desc->xe8_SEPO = GetVectorElement(in);
      break;
    case SBIG('KSSM'):
      if (GetClassID(in) == SBIG('CNST')) {
        desc->xbc_KSSM = rs_new CSpawnSystemKeyframeData(in);
        desc->xbc_KSSM->LoadAllSpawnedSystemTokens(pool);
      }
      break;
    case SBIG('LINE'):
      desc->x30_24_LINE = GetBool(in);
      break;
    case SBIG('FXLL'):
      desc->x30_25_FXLL = GetBool(in);
      break;
    case SBIG('LTYP'):
      desc->xec_LTYP = GetIntElement(in);
      break;
    case SBIG('LCLR'):
      desc->xf0_LCLR = GetColorElement(in);
      break;
    case SBIG('LINT'):
      desc->xf4_LINT = GetRealElement(in);
      break;
    case SBIG('LOFF'):
      desc->xf8_LOFF = GetVectorElement(in);
      break;
    case SBIG('LDIR'):
      desc->xfc_LDIR = GetVectorElement(in);
      break;
    case SBIG('LFOT'):
      desc->x100_LFOT = GetIntElement(in);
      break;
    case SBIG('LFOR'):
      desc->x104_LFOR = GetRealElement(in);
      break;
    case SBIG('LSLA'):
      desc->x108_LSLA = GetRealElement(in);
      break;
    case SBIG('OPTS'):
      desc->x32_25_OPTS = GetBool(in);
      break;
    case SBIG('ADV1'):
      desc->x10c_ADV1 = GetRealElement(in);
      break;
    case SBIG('ADV2'):
      desc->x110_ADV2 = GetRealElement(in);
      break;
    case SBIG('ADV3'):
      desc->x114_ADV3 = GetRealElement(in);
      break;
    case SBIG('ADV4'):
      desc->x118_ADV4 = GetRealElement(in);
      break;
    case SBIG('ADV5'):
      desc->x11c_ADV5 = GetRealElement(in);
      break;
    case SBIG('ADV6'):
      desc->x120_ADV6 = GetRealElement(in);
      break;
    case SBIG('ADV7'):
      desc->x124_ADV7 = GetRealElement(in);
      break;
    case SBIG('ADV8'):
      desc->x128_ADV8 = GetRealElement(in);
      break;
    case SBIG('_END'):
      done = true;
      break;
    default:
      return false;
    }
  }

  return true;
}

FourCC CParticleDataFactory::GetClassID(CInputStream& in) { return in.ReadLong(); }

bool CParticleDataFactory::GetBool(CInputStream& in) {
  GetClassID(in);
  return in.ReadBool();
}

int CParticleDataFactory::GetInt(CInputStream& in) { return in.ReadInt32(); }

float CParticleDataFactory::GetReal(CInputStream& in) { return in.ReadFloat(); }

CIntElement* CParticleDataFactory::GetIntElement(CInputStream& in) {
  FourCC clsId = GetClassID(in);
  switch (clsId) {
#ifdef TARGET_PC
  // Port-only VARI: a Remastered particle variable of the system being evaluated (PVRT index).
  case SBIG('VARI'):
    return rs_new CIEPortVar(in.ReadLong());
  case SBIG('SPAI'):
    return PortReadParentVar< CIEPortParentVar >(in, EPortParentRead::System, &GetIntElement);
  case SBIG('DPVI'):
    return PortReadParentVar< CIEPortParentVar >(in, EPortParentRead::Direct, &GetIntElement);
  case SBIG('TPVI'):
    return PortReadParentVar< CIEPortParentVar >(in, EPortParentRead::Topmost, &GetIntElement);
#endif
  case SBIG('CNST'): {
    return rs_new CIEConstant(GetInt(in));
  }
  case SBIG('KEYE'):
  case SBIG('KEYP'): {
    return rs_new CIEKeyframeEmitter(in);
  }
  case SBIG('TSCL'): {
    return rs_new CIETimescale(GetRealElement(in));
  }
  case SBIG('DETH'): {
    CIntElement* a = GetIntElement(in);
    CIntElement* b = GetIntElement(in);
    return rs_new CIEDeath(a, b);
  }
  case SBIG('CHAN'): {
    CIntElement* a = GetIntElement(in);
    CIntElement* b = GetIntElement(in);
    CIntElement* c = GetIntElement(in);
    return rs_new CIETimeChain(a, b, c);
  }
  case SBIG('ADD_'): {
    CIntElement* a = GetIntElement(in);
    CIntElement* b = GetIntElement(in);
    return rs_new CIEAdd(a, b);
  }
  case SBIG('MULT'): {
    CIntElement* a = GetIntElement(in);
    CIntElement* b = GetIntElement(in);
    return rs_new CIEMultiply(a, b);
  }
  case SBIG('MODU'): {
    CIntElement* a = GetIntElement(in);
    CIntElement* b = GetIntElement(in);
    return rs_new CIEModulo(a, b);
  }
  case SBIG('RAND'): {
    CIntElement* a = GetIntElement(in);
    CIntElement* b = GetIntElement(in);
    return rs_new CIERandom(a, b);
  }
  case SBIG('IMPL'): {
    return rs_new CIEImpulse(GetIntElement(in));
  }
  case SBIG('ILPT'): {
    return rs_new CIELifetimePercent(GetIntElement(in));
  }
  case SBIG('SPAH'): {
    CIntElement* a = GetIntElement(in);
    CIntElement* b = GetIntElement(in);
    CIntElement* c = GetIntElement(in);
    return rs_new CIESampleAndHold(c, a, b);
  }
  case SBIG('IRND'): {
    CIntElement* a = GetIntElement(in);
    CIntElement* b = GetIntElement(in);
    return rs_new CIEInitialRandom(a, b);
  }
  case SBIG('CLMP'): {
    CIntElement* a = GetIntElement(in);
    CIntElement* b = GetIntElement(in);
    CIntElement* c = GetIntElement(in);
    return rs_new CIEClamp(a, b, c);
  }
#ifdef TARGET_PC
  case SBIG('FIAT'): {
    CIntElement* start = GetIntElement(in);
    CIntElement* count = GetIntElement(in);
    CIntElement* divisor = GetIntElement(in);
    const bool loop = GetBool(in);
    return rs_new CIEFrameIndexFromAnimationTime(start, count, divisor, loop);
  }
  case SBIG('PLPT'): {
    const int offset = in.ReadLong();
    return rs_new CIEPortLifetimePercent(offset, GetIntElement(in));
  }
  case SBIG('PAP0'):
    return rs_new CIEPortParticleVar(0);
  case SBIG('PAP1'):
    return rs_new CIEPortParticleVar(1);
  case SBIG('PAP2'):
    return rs_new CIEPortParticleVar(2);
  case SBIG('PAP3'):
    return rs_new CIEPortParticleVar(3);
  case SBIG('PRAN'): {
    CIntElement* a = GetIntElement(in);
    CIntElement* b = GetIntElement(in);
    return rs_new CIEPortRandom(a, b);
  }
  // ABS_ (CIEAbsoluteValue 0x2a86b0): one int child.
  case SBIG('ABS_'): {
    CIntElement* value = GetIntElement(in);
    if (value == nullptr) {
      return nullptr;
    }
    return rs_new CIEPortAbs(value);
  }
  // Port-only PSYI: a PSYS int slot of the system being evaluated.
  case SBIG('PSYI'):
    return rs_new CIEPortSysVar(in.ReadLong());
#endif
  case SBIG('PULS'): {
    CIntElement* a = GetIntElement(in);
    CIntElement* b = GetIntElement(in);
    CIntElement* c = GetIntElement(in);
    CIntElement* d = GetIntElement(in);
    return rs_new CIEPulse(a, b, c, d);
  }
  case SBIG('NONE'): {
    return nullptr;
  }
  case SBIG('RTOI'): {
    CRealElement* a = GetRealElement(in);
    CRealElement* b = GetRealElement(in);
    return rs_new CIERealToInt(a, b);
  }
  case SBIG('SUB_'): {
    CIntElement* a = GetIntElement(in);
    CIntElement* b = GetIntElement(in);
    return rs_new CIESubtract(a, b);
  }
  case SBIG('GTCP'): {
    return rs_new CIEGetCumulativeParticleCount();
  }
  case SBIG('GAPC'): {
    return rs_new CIEGetActiveParticleCount();
  }
  case SBIG('GEMT'): {
    return rs_new CIEGetEmitterTime();
  }
  }
  return nullptr;
}

CRealElement* CParticleDataFactory::GetRealElement(CInputStream& in) {
  FourCC clsId = GetClassID(in);
  switch (clsId) {
#ifdef TARGET_PC
  // Port-only VARF: a Remastered particle variable of the system being evaluated (PVRT index).
  case SBIG('VARF'):
    return rs_new CREPortVar(in.ReadLong());
  // Port-only PSDT: the system's shutdown countdown fraction left (SDTR).
  case SBIG('PSDT'):
    return rs_new CREPortShutdownRemaining();
  // Port-only PSYR: a PSYS real slot of the system being evaluated.
  case SBIG('PSYR'):
    return rs_new CREPortSysVar(in.ReadLong());
  // Port-only PKPR: a per-particle KPIN latch (slot), then its element.
  case SBIG('PKPR'): {
    const u32 slot = in.ReadLong();
    CRealElement* value = GetRealElement(in);
    if (value == nullptr || slot >= SPortKeep::kMax) {
      delete value;
      return nullptr;
    }
    return rs_new CREPortKeep(slot, value);
  }
  // Port-only PIRS: Remastered's IRSN, a PKPR-counter slot (or kNoSlot), then its real.
  case SBIG('PIRS'): {
    const u32 slot = in.ReadLong();
    CRealElement* value = GetRealElement(in);
    if (value == nullptr) {
      return nullptr;
    }
    return rs_new CREPortInitialRandomSign(slot, value);
  }
  // MPXV (MPT_CREExternalValue 0x29715c): the index, then the fallback real.
  case SBIG('MPXV'): {
    CIntElement* index = GetIntElement(in);
    CRealElement* fallback = GetRealElement(in);
    if (index == nullptr || fallback == nullptr) {
      delete index;
      delete fallback;
      return nullptr;
    }
    return rs_new CREPortExternalVar(index, fallback);
  }
  // ABS_ (CREAbsoluteValue 0x295094), RSGN (CRERandomSign 0x299318): one real child.
  case SBIG('ABS_'): {
    CRealElement* value = GetRealElement(in);
    if (value == nullptr) {
      return nullptr;
    }
    return rs_new CREPortAbs(value);
  }
  case SBIG('RSGN'): {
    CRealElement* value = GetRealElement(in);
    if (value == nullptr) {
      return nullptr;
    }
    return rs_new CREPortRandomSign(value);
  }
  // COSE (CRECosineWave 0x298788): frequency, amplitude, phase, as SINE.
  case SBIG('COSE'): {
    CRealElement* frequency = GetRealElement(in);
    CRealElement* amplitude = GetRealElement(in);
    CRealElement* phase = GetRealElement(in);
    if (frequency == nullptr || amplitude == nullptr || phase == nullptr) {
      delete frequency;
      delete amplitude;
      delete phase;
      return nullptr;
    }
    return rs_new CREPortCosineWave(frequency, amplitude, phase);
  }
  case SBIG('SPAF'):
    return PortReadParentVar< CREPortParentVar >(in, EPortParentRead::System, &GetRealElement);
  case SBIG('DPVF'):
    return PortReadParentVar< CREPortParentVar >(in, EPortParentRead::Direct, &GetRealElement);
  case SBIG('TPVF'):
    return PortReadParentVar< CREPortParentVar >(in, EPortParentRead::Topmost, &GetRealElement);
  case SBIG('SLCT'):
    return PortReadSelect< CREPortSelect >(in, &CParticleDataFactory::GetRealElement);
#endif
  case SBIG('CNST'): {
    return rs_new CREConstant(GetReal(in));
  }
  case SBIG('NONE'): {
    return nullptr;
  }
  case SBIG('KEYE'):
  case SBIG('KEYP'): {
    return rs_new CREKeyframeEmitter(in);
  }
  case SBIG('SCAL'): {
    return rs_new CRETimeScale(GetRealElement(in));
  }
  case SBIG('SINE'): {
    CRealElement* a = GetRealElement(in);
    CRealElement* b = GetRealElement(in);
    CRealElement* c = GetRealElement(in);
    return rs_new CRESineWave(c, a, b);
  }
  case SBIG('ADD_'): {
    CRealElement* a = GetRealElement(in);
    CRealElement* b = GetRealElement(in);
    return rs_new CREAdd(a, b);
  }
  case SBIG('MULT'): {
    CRealElement* a = GetRealElement(in);
    CRealElement* b = GetRealElement(in);
    return rs_new CREMultiply(a, b);
  }
  case SBIG('DOTP'): {
    CVectorElement* a = GetVectorElement(in);
    CVectorElement* b = GetVectorElement(in);
    return rs_new CREDotProduct(a, b);
  }
  case SBIG('RAND'): {
    CRealElement* min = GetRealElement(in);
    CRealElement* max = GetRealElement(in);
    return rs_new CRERandom(min, max);
  }
  case SBIG('IRND'): {
    CRealElement* min = GetRealElement(in);
    CRealElement* max = GetRealElement(in);
    return rs_new CREInitialRandom(min, max);
  }
  case SBIG('CHAN'): {
    CRealElement* a = GetRealElement(in);
    CRealElement* b = GetRealElement(in);
    CIntElement* c = GetIntElement(in);
    return rs_new CRETimeChain(a, b, c);
  }
  case SBIG('CLMP'): {
    CRealElement* a = GetRealElement(in);
    CRealElement* b = GetRealElement(in);
    CRealElement* c = GetRealElement(in);
    return rs_new CREClamp(a, b, c);
  }
  case SBIG('PULS'): {
    CIntElement* a = GetIntElement(in);
    CIntElement* b = GetIntElement(in);
    CRealElement* c = GetRealElement(in);
    CRealElement* d = GetRealElement(in);
    return rs_new CREPulse(a, b, c, d);
  }
  case SBIG('RLPT'): {
    CRealElement* a = GetRealElement(in);
    return rs_new CRELifetimePercent(a);
  }
  case SBIG('LFTW'): {
    CRealElement* a = GetRealElement(in);
    CRealElement* b = GetRealElement(in);
    return rs_new CRELifetimeTween(a, b);
  }
  case SBIG('PRLW'): {
    return rs_new CREParticleRotationOrLineWidth();
  }
  case SBIG('PSLL'): {
    return rs_new CREParticleSizeOrLineLength();
  }
#ifdef TARGET_PC
  case SBIG('PSSZ'): {
    return rs_new CREParticleSecondarySize();
  }
  case SBIG('DFCS'):
  case SBIG('DFCP'): {
    CRealElement* a = GetRealElement(in);
    CRealElement* b = GetRealElement(in);
    return rs_new CREDistanceFromCameraBlend(a, b, clsId == SBIG('DFCP'));
  }
  case SBIG('KEWS'): {
    return rs_new CREPortSwooshNodeTime(GetRealElement(in));
  }
  case SBIG('PKYF'): {
    // Remastered's KEYF: the loop byte, the key count, the loop start, start and rate, the keys,
    // then the input element.
    const bool loop = in.ReadBool();
    int count = static_cast< int >(in.ReadLong());
    const float loopStart = in.ReadFloat();
    const float start = in.ReadFloat();
    const float rate = in.ReadFloat();
    // A zero or implausible key count has no keys to read at the input (the original indexes
    // key 0 regardless): the element evaluates to 0.
    if (count <= 0 || count > 0x100000) {
      PortLog::Write("particles: PKYF with key count %d, evaluating to 0\n", count);
      count = 0;
    }
    std::vector< float > keys;
    for (int i = 0; i < count; ++i) {
      keys.push_back(in.ReadFloat());
    }
    return rs_new CREPortKeyframeInput(loop, count, loopStart, start, rate, std::move(keys),
                                       GetRealElement(in));
  }
  case SBIG('PSLW'): {
    CRealElement* a = GetRealElement(in);
    CRealElement* b = GetRealElement(in);
    return rs_new CREPortSystemTween(a, b);
  }
  case SBIG('DIVD'): {
    CRealElement* a = GetRealElement(in);
    CRealElement* b = GetRealElement(in);
    return rs_new CREPortDivide(a, b);
  }
  case SBIG('T2S_'): {
    CIntElement* a = GetIntElement(in);
    if (a == nullptr) {
      return nullptr;
    }
    return rs_new CREPortTicksToSeconds(a);
  }
#endif
  case SBIG('PAP1'): {
    return rs_new CREParticleAccessParameter1();
  }
  case SBIG('PAP2'): {
    return rs_new CREParticleAccessParameter2();
  }
  case SBIG('PAP3'): {
    return rs_new CREParticleAccessParameter3();
  }
  case SBIG('PAP4'): {
    return rs_new CREParticleAccessParameter4();
  }
  case SBIG('PAP5'): {
    return rs_new CREParticleAccessParameter5();
  }
  case SBIG('PAP6'): {
    return rs_new CREParticleAccessParameter6();
  }
  case SBIG('PAP7'): {
    return rs_new CREParticleAccessParameter7();
  }
  case SBIG('PAP8'): {
    return rs_new CREParticleAccessParameter8();
  }
#ifdef TARGET_PC
  case SBIG('PAP9'):
    return rs_new CREParticleAccessParameter9();
#endif
  case SBIG('VXTR'): {
    CVectorElement* a = GetVectorElement(in);
    return rs_new CREVectorXToReal(a);
  }
  case SBIG('VYTR'): {
    CVectorElement* a = GetVectorElement(in);
    return rs_new CREVectorYToReal(a);
  }
  case SBIG('VZTR'): {
    CVectorElement* a = GetVectorElement(in);
    return rs_new CREVectorZToReal(a);
  }
  case SBIG('VMAG'): {
    CVectorElement* a = GetVectorElement(in);
    return rs_new CREVectorMagnitude(a);
  }
  case SBIG('ISWT'): {
    CRealElement* a = GetRealElement(in);
    CRealElement* b = GetRealElement(in);
    return rs_new CREInitialSwitch(a, b);
  }
  case SBIG('CLTN'): {
    CRealElement* a = GetRealElement(in);
    CRealElement* b = GetRealElement(in);
    CRealElement* c = GetRealElement(in);
    CRealElement* d = GetRealElement(in);
    return rs_new CRECompareLessThan(a, b, c, d);
  }
  case SBIG('CEQL'): {
    CRealElement* a = GetRealElement(in);
    CRealElement* b = GetRealElement(in);
    CRealElement* c = GetRealElement(in);
    CRealElement* d = GetRealElement(in);
    return rs_new CRECompareEqual(a, b, c, d);
  }
  case SBIG('CRNG'): {
    CRealElement* a = GetRealElement(in);
    CRealElement* b = GetRealElement(in);
    CRealElement* c = GetRealElement(in);
    CRealElement* d = GetRealElement(in);
    CRealElement* e = GetRealElement(in);
    return rs_new CREConstantRange(a, b, c, d, e);
  }
  case SBIG('CEXT'): {
    CIntElement* a = GetIntElement(in);
    return rs_new CREExternalVar(a);
  }
  case SBIG('ITRL'): {
    CIntElement* a = GetIntElement(in);
    CRealElement* b = GetRealElement(in);
    return rs_new CREIntTimesReal(a, b);
  }
  case SBIG('SUB_'): {
    CRealElement* a = GetRealElement(in);
    CRealElement* b = GetRealElement(in);
    return rs_new CRESubtract(a, b);
  }
  case SBIG('GTCR'): {
    CColorElement* alpha = GetColorElement(in);
    return rs_new CREGetComponentRed(alpha);
  }
  case SBIG('GTCG'): {
    CColorElement* alpha = GetColorElement(in);
    return rs_new CREGetComponentGreen(alpha);
  }
  case SBIG('GTCB'): {
    CColorElement* alpha = GetColorElement(in);
    return rs_new CREGetComponentBlue(alpha);
  }
  case SBIG('GTCA'): {
    CColorElement* alpha = GetColorElement(in);
    return rs_new CREGetComponentAlpha(alpha);
  }
  }
  return nullptr;
}

CVectorElement* CParticleDataFactory::GetVectorElement(CInputStream& in) {
  CVectorElement* ret;
  FourCC clsId = GetClassID(in);
  switch (clsId) {
#ifdef TARGET_PC
  case SBIG('VARV'):
    ret = rs_new CVEPortVar(in.ReadLong());
    break;
  // ABS_ (CVEAbsoluteValue 0x29e254): one vector child.
  case SBIG('ABS_'): {
    CVectorElement* value = GetVectorElement(in);
    ret = value != nullptr ? rs_new CVEPortAbs(value) : nullptr;
    break;
  }
  case SBIG('SPAV'):
    return PortReadParentVar< CVEPortParentVar >(in, EPortParentRead::System, &GetVectorElement);
  case SBIG('DPVV'):
    return PortReadParentVar< CVEPortParentVar >(in, EPortParentRead::Direct, &GetVectorElement);
  case SBIG('TPVV'):
    return PortReadParentVar< CVEPortParentVar >(in, EPortParentRead::Topmost, &GetVectorElement);
  case SBIG('SLCT'):
    return PortReadSelect< CVEPortSelect >(in, &CParticleDataFactory::GetVectorElement);
  case SBIG('RNDV'):
    ret = rs_new CVEPortRandomVector(GetRealElement(in));
    break;
  case SBIG('PCRN'): {
    // CVECurlNoise_Perlin3d: P, F, A, then the optional field (NONE | CNST + asset id).
    CVectorElement* pos = GetVectorElement(in);
    CRealElement* freq = GetRealElement(in);
    CVectorElement* amp = GetVectorElement(in);
    CVEPortCurlNoise::FieldHandle* handle = nullptr;
    if (GetClassID(in) == SBIG('CNST')) {
      handle = CVEPortCurlNoise::MakeHandle(in.ReadLong());
    }
    ret = rs_new CVEPortCurlNoise(pos, freq, amp, handle);
    break;
  }
  case SBIG('PCRO'): {
    // CVECurlNoise_PerlinOctave3d: P, the frequency, the octave count, the gain, A.
    CVectorElement* pos = GetVectorElement(in);
    CRealElement* freq = GetRealElement(in);
    CIntElement* octaves = GetIntElement(in);
    CRealElement* gain = GetRealElement(in);
    CVectorElement* amp = GetVectorElement(in);
    ret = rs_new CVEPortCurlNoiseOctave(pos, freq, octaves, gain, amp);
    break;
  }
  case SBIG('PRTV'): {
    // Port-only ROTV: the vector, the rotation kind, and REUL's three angles.
    CVectorElement* vec = GetVectorElement(in);
    const FourCC kind = GetClassID(in);
    CRealElement* x = nullptr;
    CRealElement* y = nullptr;
    CRealElement* z = nullptr;
    if (kind == SBIG('REUL')) {
      x = GetRealElement(in);
      y = GetRealElement(in);
      z = GetRealElement(in);
    }
    ret = rs_new CVEPortRotateVector(vec, kind, x, y, z);
    break;
  }
  case SBIG('PNCV'):
    ret = rs_new CVEPortNormalizedVelocity();
    break;
  case SBIG('ANCR'): {
    CRealElement* rotateX = GetRealElement(in);
    CRealElement* rotateY = GetRealElement(in);
    CRealElement* rotateZ = GetRealElement(in);
    CRealElement* xRange = GetRealElement(in);
    CRealElement* yRange = GetRealElement(in);
    CRealElement* magnitude = GetRealElement(in);
    ret = rs_new CVEPortAngleCone(rotateX, rotateY, rotateZ, xRange, yRange, magnitude);
    break;
  }
  case SBIG('PANM'): {
    CRealElement* rotateX = GetRealElement(in);
    CRealElement* rotateY = GetRealElement(in);
    CRealElement* rotateZ = GetRealElement(in);
    CRealElement* minAngle = GetRealElement(in);
    CRealElement* maxAngle = GetRealElement(in);
    CRealElement* magnitude = GetRealElement(in);
    if (rotateX == nullptr || rotateY == nullptr || rotateZ == nullptr || minAngle == nullptr || maxAngle == nullptr || magnitude == nullptr) {
      delete rotateX;
      delete rotateY;
      delete rotateZ;
      delete minAngle;
      delete maxAngle;
      delete magnitude;
      return nullptr;
    }
    ret = rs_new CVEPortAngleConeMinMax(rotateX, rotateY, rotateZ, minAngle, maxAngle, magnitude);
    break;
  }
  case SBIG('PMAC'): {
    CRealElement* xCentre = GetRealElement(in);
    CRealElement* yCentre = GetRealElement(in);
    CRealElement* xRange = GetRealElement(in);
    CRealElement* yRange = GetRealElement(in);
    CRealElement* magnitude = GetRealElement(in);
    if (xCentre == nullptr || yCentre == nullptr || xRange == nullptr || yRange == nullptr || magnitude == nullptr) {
      delete xCentre;
      delete yCentre;
      delete xRange;
      delete yRange;
      delete magnitude;
      return nullptr;
    }
    ret = rs_new CVEPortMpac(xCentre, yCentre, xRange, yRange, magnitude);
    break;
  }
  case SBIG('PCRC'): {
    CVectorElement* centre = GetVectorElement(in);
    CVectorElement* axis = GetVectorElement(in);
    CRealElement* start = GetRealElement(in);
    CIntElement* count = GetIntElement(in);
    CRealElement* variance = GetRealElement(in);
    CRealElement* radius = GetRealElement(in);
    const u8 shuffle = u8(in.ReadChar());
    if (centre == nullptr || axis == nullptr || start == nullptr || count == nullptr || variance == nullptr ||
        radius == nullptr) {
      delete centre;
      delete axis;
      delete start;
      delete count;
      delete variance;
      delete radius;
      return nullptr;
    }
    ret = rs_new CVEPortCircle(centre, axis, start, count, variance, radius, shuffle);
    break;
  }
  case SBIG('PLWV'): {
    CVectorElement* start = GetVectorElement(in);
    CVectorElement* end = GetVectorElement(in);
    CIntElement* count = GetIntElement(in);
    CRealElement* variance = GetRealElement(in);
    const u8 shuffle = u8(in.ReadChar());
    if (start == nullptr || end == nullptr || count == nullptr || variance == nullptr) {
      delete start;
      delete end;
      delete count;
      delete variance;
      return nullptr;
    }
    ret = rs_new CVEPortLine(start, end, count, variance, shuffle);
    break;
  }
  case SBIG('PMCN'): {
    CVectorElement* direction = GetVectorElement(in);
    CRealElement* radius = GetRealElement(in);
    if (direction == nullptr || radius == nullptr) {
      delete direction;
      delete radius;
      return nullptr;
    }
    ret = rs_new CVEPortCone(direction, radius);
    break;
  }
  case SBIG('NORM'):
    ret = rs_new CVEPortNormalize(GetVectorElement(in));
    break;
  case SBIG('BSIS'):
  case SBIG('BSSC'):
    ret = rs_new CVEPortBasis(GetVectorElement(in), clsId == SBIG('BSSC'));
    break;
  case SBIG('PRZY'): {
    CVectorElement* first = GetVectorElement(in);
    CVectorElement* second = GetVectorElement(in);
    CVectorElement* fallback = GetVectorElement(in);
    ret = rs_new CVEPortRotationFromAxis(first, second, fallback);
    break;
  }
  case SBIG('PEMV'): {
    // The mode word: 0 the external translation, 1 the local one, 2 the local one plus the
    // description's constant POFS (XFMD 5).
    const u32 mode = in.ReadLong();
    sPortPemv = u8(mode + 1);
    ret = rs_new CVEPortEmitterVelocity();
    break;
  }
  case SBIG('CMPS'):
  case SBIG('CPSS'):
  case SBIG('EXTT'):
  case SBIG('EXTS'):
  case SBIG('EXTR'):
  case SBIG('CMRT'):
    ret = rs_new CVEPortSystemVector(clsId);
    break;
#endif
  case SBIG('NONE'):
    ret = nullptr;
    break;
  case SBIG('CNST'): {
    CElementAllocationChunk* allocationContext = IElement::CElementAllocator::GetCurrentChunk();
    uint initialSize = IElement::CElementAllocator::GetCurrentAllocatedSize();
    CRealElement* x = GetRealElement(in);
    CRealElement* y = GetRealElement(in);
    CRealElement* z = GetRealElement(in);
    if (x && y && z) {
      if (x->IsConstant() && y->IsConstant() && z->IsConstant()) {
        float xf, yf, zf;
        x->GetValue(0, xf);
        y->GetValue(0, yf);
        z->GetValue(0, zf);

        delete x;
        delete y;
        delete z;

        if (allocationContext != nullptr &&
            allocationContext == IElement::CElementAllocator::GetCurrentChunk()) {
          allocationContext->Rewind(allocationContext->GetAllocatedSize() - initialSize);
        }
        ret = rs_new CVEFastConstant(xf, yf, zf);
        break;
      }
    }
    ret = rs_new CVEConstant(x, y, z);
    break;
  }
  case SBIG('KEYE'):
  case SBIG('KEYP'):
    ret = rs_new CVEKeyframeEmitter(in);
    break;
  case SBIG('ANGC'): {
    CRealElement* angleXBias = GetRealElement(in);
    CRealElement* angleYBias = GetRealElement(in);
    CRealElement* angleXRange = GetRealElement(in);
    CRealElement* angleYRange = GetRealElement(in);
    CRealElement* magnitude = GetRealElement(in);
    ret = rs_new CVEAngleCone(angleXBias, angleYBias, angleXRange, angleYRange, magnitude);
    break;
  }
  case SBIG('CONE'): {
    CVectorElement* direction = GetVectorElement(in);
    CRealElement* baseRadius = GetRealElement(in);
    ret = rs_new CVECone(direction, baseRadius);
    break;
  }
  case SBIG('CIRC'): {
    CVectorElement* circleOffset = GetVectorElement(in);
    CVectorElement* circleNormal = GetVectorElement(in);
    CRealElement* angleConstant = GetRealElement(in);
    CRealElement* angleLinear = GetRealElement(in);
    CRealElement* radius = GetRealElement(in);
    ret = rs_new CVECircle(circleOffset, circleNormal, angleConstant, angleLinear, radius);
    break;
  }
  case SBIG('CCLU'): {
    CVectorElement* circleOffset = GetVectorElement(in);
    CVectorElement* circleNormal = GetVectorElement(in);
    CIntElement* cycleFrames = GetIntElement(in);
    CRealElement* randomFactor = GetRealElement(in);

    ret = rs_new CVECircleCluster(circleOffset, circleNormal, cycleFrames, randomFactor);
    break;
  }
  case SBIG('ADD_'): {
    CVectorElement* a = GetVectorElement(in);
    CVectorElement* b = GetVectorElement(in);
    ret = rs_new CVEAdd(a, b);
    break;
  }
  case SBIG('MULT'): {
    CVectorElement* a = GetVectorElement(in);
    CVectorElement* b = GetVectorElement(in);
    ret = rs_new CVEMultiply(a, b);
    break;
  }
  case SBIG('CHAN'): {
    CVectorElement* a = GetVectorElement(in);
    CVectorElement* b = GetVectorElement(in);
    CIntElement* switchFrame = GetIntElement(in);

    ret = rs_new CVETimeChain(a, b, switchFrame);
    break;
  }
  case SBIG('PULS'): {
    CIntElement* durationA = GetIntElement(in);
    CIntElement* durationB = GetIntElement(in);
    CVectorElement* a = GetVectorElement(in);
    CVectorElement* b = GetVectorElement(in);
    ret = rs_new CVEPulse(durationA, durationB, a, b);
    break;
  }
  case SBIG('RTOV'): {
    CRealElement* value = GetRealElement(in);
    ret = rs_new CVERealToVector(value);
    break;
  }
  case SBIG('PLOC'): {
    ret = rs_new CVEParticleLocation();
    break;
  }
  case SBIG('PLCO'): {
    ret = rs_new CVEParticlePreviousLocation();
    break;
  }
  case SBIG('PVEL'): {
    ret = rs_new CVEParticleVelocity();
    break;
  }
  case SBIG('PSOF'): {
    ret = rs_new CVEParticleSystemOrientationFront();
    break;
  }
  case SBIG('PSOU'): {
    ret = rs_new CVEParticleSystemOrientationUp();
    break;
  }
  case SBIG('PSOR'): {
    ret = rs_new CVEParticleSystemOrientationRight();
    break;
  }
  case SBIG('PSTR'): {
    ret = rs_new CVEParticleSystemTranslation();
    break;
  }
  case SBIG('SUB_'): {
    CVectorElement* a = GetVectorElement(in);
    CVectorElement* b = GetVectorElement(in);
    ret = rs_new CVESubtract(a, b);
    break;
  }
  case SBIG('CTVC'): {
    CColorElement* value = GetColorElement(in);
    ret = rs_new CVEColorToVector(value);
    break;
  }
#ifdef TARGET_PC
  case SBIG('PQAD'): {
    CVectorElement* first = GetVectorElement(in);
    CVectorElement* second = GetVectorElement(in);
    ret = rs_new CVEOrderedEulerProduct(first, second);
    break;
  }
#endif
  default:
    ret = nullptr;
    break;
  }
  return ret;
}

CEmitterElement* CParticleDataFactory::GetEmitterElement(CInputStream& in) {
  CEmitterElement* ret;
  FourCC clsId = GetClassID(in);
  switch (clsId) {
#ifdef TARGET_PC
  // CODE: a null emitter, and the generator spawns only from code (desc[0x2fa] bit 4).
  case SBIG('CODE'):
    sPortCodeEmitter = true;
    ret = nullptr;
    break;
#endif
  case SBIG('NONE'):
    ret = nullptr;
    break;
  case SBIG('SETR'): {
    FourCC prop = GetClassID(in);
    CVectorElement* pos = nullptr;
    CVectorElement* vel = nullptr;
    bool valid = false;
    if (prop == SBIG('ILOC')) {
      pos = GetVectorElement(in);
      prop = GetClassID(in);
      if (prop == SBIG('IVEC')) {
        vel = GetVectorElement(in);
        valid = true;
      }
    }
    ret = valid ? rs_new CEESimpleEmitter(pos, vel) : nullptr;
    break;
  }
  case SBIG('SEMR'): {
    CVectorElement* pos = GetVectorElement(in);
    CVectorElement* vel = GetVectorElement(in);
    ret = rs_new CEESimpleEmitter(pos, vel);
    break;
  }
  case SBIG('SPHE'): {
    CVectorElement* origin = GetVectorElement(in);
    CRealElement* radius = GetRealElement(in);
    CRealElement* velocity = GetRealElement(in);
    ret = rs_new CVESphere(origin, radius, velocity);
    break;
  }
  case SBIG('ASPH'): {
    CVectorElement* origin = GetVectorElement(in);
    CRealElement* angleXBias = GetRealElement(in);
    CRealElement* angleYBias = GetRealElement(in);
    CRealElement* angleXRange = GetRealElement(in);
    CRealElement* angleYRange = GetRealElement(in);
    CRealElement* radius = GetRealElement(in);
    CRealElement* velocity = GetRealElement(in);
    ret = rs_new CVEAngleSphere(origin, radius, velocity, angleXBias, angleYBias, angleXRange,
                                angleYRange);
    break;
  }
#ifdef TARGET_PC
  case SBIG('ASPR'): {
    CVectorElement* origin = GetVectorElement(in);
    CRealElement* rotateX = GetRealElement(in);
    CRealElement* rotateY = GetRealElement(in);
    CRealElement* rotateZ = GetRealElement(in);
    CRealElement* range1 = GetRealElement(in);
    CRealElement* range2 = GetRealElement(in);
    CRealElement* radius = GetRealElement(in);
    CRealElement* speed = GetRealElement(in);
    ret = rs_new CEEPortAngleSphere(origin, rotateX, rotateY, rotateZ, range1, range2, radius,
                                    speed);
    break;
  }
  case SBIG('PASV'): {
    CVectorElement* origin = GetVectorElement(in);
    CRealElement* rotateX = GetRealElement(in);
    CRealElement* rotateY = GetRealElement(in);
    CRealElement* rotateZ = GetRealElement(in);
    CRealElement* range1 = GetRealElement(in);
    CRealElement* range2 = GetRealElement(in);
    CIntElement* polarCount = GetIntElement(in);
    CRealElement* polarVariance = GetRealElement(in);
    CRealElement* azimuthRange = GetRealElement(in);
    CIntElement* azimuthCount = GetIntElement(in);
    CRealElement* azimuthVariance = GetRealElement(in);
    CRealElement* radius = GetRealElement(in);
    CRealElement* speed = GetRealElement(in);
    const u8 shuffle = u8(in.ReadChar());
    ret = rs_new CEEPortAngleSphereVariance(origin, rotateX, rotateY, rotateZ, range1, range2, polarCount,
                                            polarVariance, azimuthRange, azimuthCount, azimuthVariance, radius, speed,
                                            shuffle);
    break;
  }
  case SBIG('PLNE'): {
    CVectorElement* position = GetVectorElement(in);
    CVectorElement* euler = GetVectorElement(in);
    CVectorElement* velocity = GetVectorElement(in);
    CRealElement* wx = GetRealElement(in);
    CRealElement* wz = GetRealElement(in);
    CRealElement* spread = GetRealElement(in);
    ret = rs_new CEEPortPlane(position, euler, velocity, wx, wz, spread);
    break;
  }
  case SBIG('PLNV'): {
    CVectorElement* position = GetVectorElement(in);
    CVectorElement* euler = GetVectorElement(in);
    CRealElement* velocityY = GetRealElement(in);
    CRealElement* wx = GetRealElement(in);
    CRealElement* wz = GetRealElement(in);
    CRealElement* spread = GetRealElement(in);
    CIntElement* countX = GetIntElement(in);
    CRealElement* varianceX = GetRealElement(in);
    CIntElement* countZ = GetIntElement(in);
    CRealElement* varianceZ = GetRealElement(in);
    const u8 shuffle = u8(in.ReadChar());
    ret = rs_new CEEPortPlaneVariance(position, euler, velocityY, wx, wz, spread, countX,
                                      varianceX, countZ, varianceZ, shuffle);
    break;
  }
  case SBIG('PSPV'): {
    CVectorElement* position = GetVectorElement(in);
    CRealElement* radius = GetRealElement(in);
    CRealElement* speed = GetRealElement(in);
    CIntElement* count1 = GetIntElement(in);
    CRealElement* variance1 = GetRealElement(in);
    CIntElement* count2 = GetIntElement(in);
    CRealElement* variance2 = GetRealElement(in);
    const u8 shuffle = u8(in.ReadChar());
    ret = rs_new CEEPortSphereVariance(position, radius, speed, count1, variance1, count2, variance2, shuffle);
    break;
  }
  case SBIG('PELP'): {
    CVectorElement* centre = GetVectorElement(in);
    CVectorElement* size = GetVectorElement(in);
    CRealElement* speed = GetRealElement(in);
    const bool surfaceOnly = in.ReadChar() != 0;
    ret = rs_new CEEPortEllipsoid(centre, size, speed, surfaceOnly);
    break;
  }
#endif
  default:
    ret = nullptr;
    break;
  }
  return ret;
}

CModVectorElement* CParticleDataFactory::GetModVectorElement(CInputStream& in) {
  CModVectorElement* ret;
  FourCC clsId = GetClassID(in);
  switch (clsId) {
  case SBIG('NONE'): {
    ret = nullptr;
    break;
  }
  case SBIG('CNST'): {
    CRealElement* a = GetRealElement(in);
    CRealElement* b = GetRealElement(in);
    CRealElement* c = GetRealElement(in);
    if (a && b && c && a->IsConstant() && b->IsConstant() && c->IsConstant()) {
      float af, bf, cf;
#if NONMATCHING
      a->GetValue(0, af);
      b->GetValue(0, bf);
      c->GetValue(0, cf);
#else
      // BUG: Fetching from the same element for each
      a->GetValue(0, af);
      a->GetValue(0, bf);
      a->GetValue(0, cf);
#endif
      ret = rs_new CMVEFastConstant(af, bf, cf);
      delete a;
      delete b;
      delete c;
    } else {
      ret = rs_new CMVEConstant(a, b, c);
    }
    break;
  }
#ifdef TARGET_PC
  case SBIG('PCRM'): {
    // CMVECurlNoise: raw u32 mode, V, t.
    const uint mode = in.ReadLong();
    CVectorElement* v = GetVectorElement(in);
    CRealElement* t = GetRealElement(in);
    ret = rs_new CMVEPortCurlNoise(mode, v, t);
    break;
  }
#endif
  case SBIG('GRAV'): {
    ret = rs_new CMVEGravity(GetVectorElement(in));
    break;
  }
  case SBIG('WIND'): {
    CVectorElement* a = GetVectorElement(in);
    CRealElement* b = GetRealElement(in);
    ret = rs_new CMVEWind(a, b);
    break;
  }
  case SBIG('EXPL'): {
    CRealElement* a = GetRealElement(in);
    CRealElement* b = GetRealElement(in);
    ret = rs_new CMVEExplode(a, b);
    break;
  }
  case SBIG('CHAN'): {
    CModVectorElement* a = GetModVectorElement(in);
    CModVectorElement* b = GetModVectorElement(in);
    CIntElement* c = GetIntElement(in);
    ret = rs_new CMVETimeChain(a, b, c);
    break;
  }
  case SBIG('PULS'): {
    CIntElement* a = GetIntElement(in);
    CIntElement* b = GetIntElement(in);
    CModVectorElement* c = GetModVectorElement(in);
    CModVectorElement* d = GetModVectorElement(in);
    ret = rs_new CMVEPulse(a, b, c, d);
    break;
  }
  case SBIG('IMPL'): {
    CVectorElement* a = GetVectorElement(in);
    CRealElement* b = GetRealElement(in);
    CRealElement* c = GetRealElement(in);
    CRealElement* d = GetRealElement(in);
    bool e = GetBool(in);
    ret = rs_new CMVEImplosion(a, b, c, d, e);
    break;
  }
  case SBIG('LMPL'): {
    CVectorElement* a = GetVectorElement(in);
    CRealElement* b = GetRealElement(in);
    CRealElement* c = GetRealElement(in);
    CRealElement* d = GetRealElement(in);
    bool e = GetBool(in);
    ret = rs_new CMVELinearImplosion(a, b, c, d, e);
    break;
  }
  case SBIG('EMPL'): {
    CVectorElement* a = GetVectorElement(in);
    CRealElement* b = GetRealElement(in);
    CRealElement* c = GetRealElement(in);
    CRealElement* d = GetRealElement(in);
    bool e = GetBool(in);
    ret = rs_new CMVEExponentialImplosion(a, b, c, d, e);
    break;
  }
  case SBIG('SWRL'): {
    CVectorElement* a = GetVectorElement(in);
    CVectorElement* b = GetVectorElement(in);
    CRealElement* c = GetRealElement(in);
    CRealElement* d = GetRealElement(in);
    ret = rs_new CMVESwirl(a, b, c, d);
    break;
  }
  case SBIG('BNCE'): {
    CVectorElement* a = GetVectorElement(in);
    CVectorElement* b = GetVectorElement(in);
    CRealElement* c = GetRealElement(in);
    CRealElement* d = GetRealElement(in);
    bool e = GetBool(in);
    ret = rs_new CMVEBounce(a, b, c, d, e);
    break;
  }
  case SBIG('SPOS'): {
    ret = rs_new CMVESetPosition(GetVectorElement(in));
    break;
  }
  default:
    return nullptr;
    break;
  }
  return ret;
}

CColorElement* CParticleDataFactory::GetColorElement(CInputStream& in) {
  CColorElement* ret;
  FourCC clsId = GetClassID(in);
  switch (clsId) {
#ifdef TARGET_PC
  case SBIG('VARC'):
    ret = rs_new CCEPortVar(in.ReadLong());
    break;
  case SBIG('SPAC'):
    return PortReadParentVar< CCEPortParentVar >(in, EPortParentRead::System, &GetColorElement);
  case SBIG('DPVC'):
    return PortReadParentVar< CCEPortParentVar >(in, EPortParentRead::Direct, &GetColorElement);
  case SBIG('TPVC'):
    return PortReadParentVar< CCEPortParentVar >(in, EPortParentRead::Topmost, &GetColorElement);
  case SBIG('MULT'): {
    CColorElement* a = GetColorElement(in);
    CColorElement* b = GetColorElement(in);
    ret = rs_new CCEPortMultiply(a, b);
    break;
  }
  case SBIG('MDAO'): {
    CColorElement* c = GetColorElement(in);
    CRealElement* a = GetRealElement(in);
    ret = rs_new CCEPortModifyAlpha(c, a);
    break;
  }
  case SBIG('SLCT'):
    return PortReadSelect< CCEPortSelect >(in, &CParticleDataFactory::GetColorElement);
#endif
  case SBIG('CNST'): {
    CElementAllocationChunk* allocationContext = IElement::CElementAllocator::GetCurrentChunk();
    uint initialSize = IElement::CElementAllocator::GetCurrentAllocatedSize();
    CRealElement* r = GetRealElement(in);
    CRealElement* g = GetRealElement(in);
    CRealElement* b = GetRealElement(in);
    CRealElement* a = GetRealElement(in);
    if (r && g && b && a) {
      if (r->IsConstant() && g->IsConstant() && b->IsConstant() && a->IsConstant()) {
        float rf, gf, bf, af;
        r->GetValue(0, rf);
        g->GetValue(0, gf);
        b->GetValue(0, bf);
        a->GetValue(0, af);

        delete r;
        delete g;
        delete b;
        delete a;

        if (allocationContext != nullptr &&
            allocationContext == IElement::CElementAllocator::GetCurrentChunk()) {
          allocationContext->Rewind(allocationContext->GetAllocatedSize() - initialSize);
        }
        ret = rs_new CCEFastConstant(rf, gf, bf, af);
        break;
      }
    }
    ret = rs_new CCEConstant(r, g, b, a);
    break;
  }
  case SBIG('KEYE'):
  case SBIG('KEYP'):
    ret = rs_new CCEKeyframeEmitter(in);
    break;
#ifdef TARGET_PC
  case SBIG('PGRD'):
    ret = rs_new CCEGradient(in);
    break;
#endif
  case SBIG('FADE'): {
    CColorElement* a = GetColorElement(in);
    CColorElement* b = GetColorElement(in);
    CRealElement* end = GetRealElement(in);
    ret = rs_new CCEFade(a, b, end);
    break;
  }
  case SBIG('CFDE'): {
    CColorElement* a = GetColorElement(in);
    CColorElement* b = GetColorElement(in);
    CRealElement* start = GetRealElement(in);
    CRealElement* end = GetRealElement(in);
    ret = rs_new CCEFadeEnd(a, b, start, end);
    break;
  }
  case SBIG('CHAN'): {
    CColorElement* a = GetColorElement(in);
    CColorElement* b = GetColorElement(in);
    CIntElement* frame = GetIntElement(in);
    ret = rs_new CCETimeChain(a, b, frame);
    break;
  }
  case SBIG('PULS'): {
    CIntElement* aDuration = GetIntElement(in);
    CIntElement* bDuration = GetIntElement(in);
    CColorElement* a = GetColorElement(in);
    CColorElement* b = GetColorElement(in);
    ret = rs_new CCEPulse(aDuration, bDuration, a, b);
    break;
  }
  case SBIG('PCOL'):
    ret = rs_new CCEParticleColor();
    break;
  case SBIG('NONE'):
    ret = nullptr;
    break;
  default:
    ret = nullptr;
    break;
  }
  return ret;
}

CUVElement* CParticleDataFactory::GetTextureElement(CInputStream& in, CSimplePool* resPool) {
  CUVElement* ret;
  FourCC clsId = GetClassID(in);
  switch (clsId) {
  case SBIG('NONE'):
    ret = nullptr;
    break;
  case SBIG('CNST'): {
    CAssetId id = 0;
    FourCC subId = GetClassID(in);
    if (subId != SBIG('NONE')) {
      id = in.ReadLong();
    }
    if (id == 0) {
      TToken< CTexture > tex = CreateTexture(-1);
      ret = rs_new CUVEConstant(tex);
    } else {
      TToken< CTexture > tex = resPool->GetObj(SObjectTag(SBIG('TXTR'), id));
      ret = rs_new CUVEConstant(tex);
    }
    break;
  }
  case SBIG('ATEX'): {
    CAssetId id = 0;
    FourCC subId = GetClassID(in);
    if (subId != SBIG('NONE')) {
      id = in.ReadLong();
    }
    CIntElement* tileW = GetIntElement(in);
    CIntElement* tileH = GetIntElement(in);
    CIntElement* strideW = GetIntElement(in);
    CIntElement* strideH = GetIntElement(in);
    CIntElement* cycleFrames = GetIntElement(in);
    bool loop = GetBool(in);
    if (id == 0) {
      TToken< CTexture > tex = CreateTexture(-1);
      ret = rs_new CUVEAnimTexture(tex, tileW, tileH, strideW, strideH, cycleFrames, loop);
    } else {
      TToken< CTexture > tex = resPool->GetObj(SObjectTag(SBIG('TXTR'), id));
      ret = rs_new CUVEAnimTexture(tex, tileW, tileH, strideW, strideH, cycleFrames, loop);
    }
    break;
  }
#ifdef TARGET_PC
  case SBIG('PATL'): {
    // 'CNST' <TXTR id>, then cols, rows, count, mode, flipX as int elements
    CAssetId id = 0;
    FourCC subId = GetClassID(in);
    if (subId != SBIG('NONE')) {
      id = in.ReadLong();
    }
    CIntElement* cols = GetIntElement(in);
    CIntElement* rows = GetIntElement(in);
    CIntElement* count = GetIntElement(in);
    CIntElement* mode = GetIntElement(in);
    CIntElement* flipX = GetIntElement(in);
    TToken< CTexture > tex = id == 0 ? TToken< CTexture >(CreateTexture(-1))
                                     : TToken< CTexture >(resPool->GetObj(SObjectTag(SBIG('TXTR'), id)));
    ret = rs_new CUVEAtlasTexture(tex, cols, rows, count, mode, flipX);
    break;
  }
#endif
  default:
    return nullptr;
  }
  return ret;
}

rstl::optional_object< TToken< CGenDescription > >
CParticleDataFactory::GetChildGeneratorDesc(CInputStream& in, CSimplePool* pool,
                                            const rstl::vector< CAssetId >& resources) {
  FourCC clsId = GetClassID(in);
  CAssetId id;
  if (clsId != SBIG('NONE')) {
    id = in.Get< CAssetId >();
  } else {
    return rstl::optional_object_null();
  }
  if (id == 0) {
    return rstl::optional_object_null();
  }
  return GetChildGeneratorDesc(id, pool, resources);
}

rstl::optional_object< TToken< CGenDescription > >
CParticleDataFactory::GetChildGeneratorDesc(CAssetId id, CSimplePool* pool,
                                            const rstl::vector< CAssetId >& resources) {
  if (rstl::count(resources.begin(), resources.end(), id) == 0) {
    return TToken< CGenDescription >(pool->GetObj(SObjectTag(CParticleGen::ResType(), id)));
  }
  return rstl::optional_object_null();
}

rstl::optional_object< TToken< CSwooshDescription > >
CParticleDataFactory::GetSwooshGeneratorDesc(CInputStream& in, CSimplePool* pool) {
  FourCC clsId = GetClassID(in);
  CAssetId id;
  if (clsId != SBIG('NONE')) {
    id = in.Get< CAssetId >();
  } else {
    return rstl::optional_object_null();
  }
  if (id == 0) {
    return rstl::optional_object_null();
  }
  return TToken< CSwooshDescription >(pool->GetObj(SObjectTag(SBIG('SWHC'), id)));
}

rstl::optional_object< TToken< CElectricDescription > >
CParticleDataFactory::GetElectricGeneratorDesc(CInputStream& in, CSimplePool* pool) {
  FourCC clsId = GetClassID(in);
  CAssetId id;
  if (clsId != SBIG('NONE')) {
    id = in.Get< CAssetId >();
  } else {
    return rstl::optional_object_null();
  }
  if (id == 0) {
    return rstl::optional_object_null();
  }
  return TToken< CElectricDescription >(pool->GetObj(SObjectTag(SBIG('ELSC'), id)));
}

rstl::optional_object< TToken< CModel > > CParticleDataFactory::GetModel(CInputStream& in,
                                                                         CSimplePool* pool) {
  FourCC clsId = GetClassID(in);
  CAssetId id;
  if (clsId != SBIG('NONE')) {
    id = in.Get< CAssetId >();
  } else {
    return rstl::optional_object_null();
  }
  if (id == 0) {
    return rstl::optional_object_null();
  }
  return TToken< CModel >(pool->GetObj(SObjectTag(SBIG('CMDL'), id)));
}

CTexture* CreateTexture(int value) {
  CTexture* texture = rs_new CTexture(kTF_RGBA8, 4, 4, 1);
  int* data = static_cast< int* >(texture->Lock());
  for (int i = 1; i <= 16; ++i) {
    data[i - 1] = value;
  }
  texture->UnLock();
  return texture;
}
