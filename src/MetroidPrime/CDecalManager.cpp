#include "MetroidPrime/CDecalManager.hpp"

#include "Kyoto/Graphics/CCubeMaterial.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Math/CAABox.hpp"
#include "MetaRender/CCubeRenderer.hpp"
#include "MetroidPrime/CStateManager.hpp"

#ifdef TARGET_PC
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Cameras/CGameCamera.hpp"
#include "port_log.h"
#include "port_projected_decal.h"
#endif

rstl::reserved_vector< CDecalManager::SDecal, 64 > CDecalManager::mDecalPool;
rstl::reserved_vector< int, 64 > CDecalManager::mActiveIndexList;
int CDecalManager::mFreeIndex;
bool CDecalManager::mbPoolInitialized = false;
float CDecalManager::mDeltaTimeSinceLastDecalCreation;
int CDecalManager::mLastDecalCreatedIndex;
CAssetId CDecalManager::mLastDecalCreatedAssetId;

void CDecalManager::Initialize() {
  if (mbPoolInitialized) {
    return;
  }

  mDecalPool.clear();

  for (int i = 0; i < mDecalPool.capacity(); ++i) {
    mDecalPool.push_back(SDecal(rstl::optional_object_null(), TAreaId(0), i - 1, false));
  }

  mFreeIndex = mDecalPool.capacity() - 1;
  mbPoolInitialized = true;
  mDeltaTimeSinceLastDecalCreation = 0.f;
  mLastDecalCreatedIndex = -1;
  mLastDecalCreatedAssetId = kInvalidAssetId;
}

void CDecalManager::ShutDown() {
#ifdef TARGET_PC
  PortProjectedDecal::ReleaseAll();
#endif
  mActiveIndexList.clear();
  mDecalPool.clear();
  mbPoolInitialized = false;
}

void CDecalManager::Reinitialize() {
  if (!mbPoolInitialized) {
    Initialize();
  }
#ifdef TARGET_PC
  PortProjectedDecal::ReleaseAll();
#endif

  for (int i = 0; i < mDecalPool.capacity(); ++i) {
    mDecalPool[i] = SDecal(rstl::optional_object_null(), TAreaId(0), i - 1, false);
  }

  mActiveIndexList.clear();
  mFreeIndex = mDecalPool.capacity() - 1;
}

#ifdef TARGET_PC
// CDecalManagerMP1::AddDecal 0x100fd64, for a description carrying Remastered data: a 3 s / 0.31
// dedup window, and the decal seeded from the state manager's random.
static void PortAddRemasteredDecal(const TToken< CDecalDescription >& desc, const CTransform4f& xf,
                                   CDecalManager::EThermalHeat heat, CStateManager& mgr) {
  const CAssetId assetId = desc.GetTag().GetId();
  if (CDecalManager::mLastDecalCreatedIndex != -1 &&
      CDecalManager::mDeltaTimeSinceLastDecalCreation < 3.f &&
      CDecalManager::mLastDecalCreatedAssetId == assetId) {
    const CDecal& last = *CDecalManager::mDecalPool[CDecalManager::mLastDecalCreatedIndex].mDecal;
    const CVector3f delta = last.GetPortTransform().GetTranslation() - xf.GetTranslation();
    if (delta.MagSquared() < 0.31f) {
      PortLog::Write("decal: %08X dedup (%.2fs, dist2 %.3f)\n", uint(assetId),
                     CDecalManager::mDeltaTimeSinceLastDecalCreation, delta.MagSquared());
      return;
    }
  }

  if (CDecalManager::mFreeIndex == -1) {
    CDecalManager::RemoveFromActiveList(CDecalManager::mActiveIndexList.begin(),
                                        CDecalManager::mActiveIndexList[0]);
  }

  const int index = CDecalManager::mFreeIndex;
  CDecalManager::SDecal& decal = CDecalManager::mDecalPool[index];
  CDecalManager::mFreeIndex = decal.mNextFreeIndex;
  decal.mDecal.clear();
  decal.mDecal = CDecal(desc, uint(mgr.PortRandom16().Next()), xf);
  decal.mAreaId = mgr.GetNextAreaId();
  decal.mNotIce = heat == CDecalManager::kTH_Hot;
  CDecalManager::mDeltaTimeSinceLastDecalCreation = 0.f;
  CDecalManager::mLastDecalCreatedIndex = index;
  CDecalManager::mLastDecalCreatedAssetId = assetId;
  CDecalManager::mActiveIndexList.push_back(index);
  const CDecal& made = *decal.mDecal;
  PortLog::Write("decal: %08X remastered spawn slot %d lifetime %d box %.3f x %.3f x %.3f angle %.4f\n",
                 uint(assetId), index, made.GetPortLifetime(), made.GetPortWidth(),
                 made.GetPortHeight(), made.GetPortBoxDepth(), made.GetPortAngle());
  PortProjectedDecal::Spawn(index, made, uint(assetId));
}
#endif

void CDecalManager::AddDecal(const TToken< CDecalDescription >& desc, const CTransform4f& xf,
                             EThermalHeat heat, CStateManager& mgr) {
#ifdef TARGET_PC
  if (const_cast< TToken< CDecalDescription >& >(desc)->xPortRemastered) {
    PortAddRemasteredDecal(desc, xf, heat, mgr);
    return;
  }
#endif
  const CAssetId assetId = desc.GetTag().GetId();
  if (mLastDecalCreatedIndex != -1 && mDeltaTimeSinceLastDecalCreation < 0.75f &&
      mLastDecalCreatedAssetId == assetId) {
    SDecal& existing = mDecalPool[mLastDecalCreatedIndex];
    if ((existing.mDecal->GetTranslation() - xf.GetTranslation()).MagSquared() < 0.01f) {
      return;
    }
  }

  if (mFreeIndex == -1) {
    RemoveFromActiveList(mActiveIndexList.begin(), mActiveIndexList[0]);
  }

  const int index = mFreeIndex;
  SDecal& decal = mDecalPool[index];
  mFreeIndex = decal.mNextFreeIndex;
  decal.mDecal.clear();
  decal.mDecal = CDecal(desc, xf);
  decal.mAreaId = mgr.GetNextAreaId();
  decal.mNotIce = heat == kTH_Hot;
  mDeltaTimeSinceLastDecalCreation = 0.f;
  mLastDecalCreatedIndex = index;
  mLastDecalCreatedAssetId = assetId;
  mActiveIndexList.push_back(index);
}

rstl::reserved_vector< int, 64 >::iterator
CDecalManager::RemoveFromActiveList(rstl::reserved_vector< int, 64 >::iterator it, int idx) {
#ifdef TARGET_PC
  PortProjectedDecal::Release(idx);
#endif
  AUTO(next, mActiveIndexList.erase(it));
  mDecalPool[idx].mNextFreeIndex = mFreeIndex;
  mFreeIndex = idx;
  if (mLastDecalCreatedIndex == mFreeIndex) {
    mLastDecalCreatedIndex = -1;
  }
  return next;
}

void CDecalManager::Update(float dt, CStateManager& mgr) {
  mDeltaTimeSinceLastDecalCreation += dt;
#ifdef TARGET_PC
  // CDecalManagerMP1::Update 0x1010348 for Remastered decals: removed when done (no area check),
  // then stepped with a distance and load factor. Retail decals keep retail's rules.
  int remastered = 0;
  for (AUTO(it, mActiveIndexList.begin()); it != mActiveIndexList.end();) {
    SDecal& decal = mDecalPool[*it];
    const bool port = decal.mDecal->IsPortRemastered();
    if (port ? decal.mDecal->IsPortRemasteredDone()
             : (decal.mAreaId != mgr.GetNextAreaId() || decal.mDecal->IsDone())) {
      it = RemoveFromActiveList(it, *it);
    } else {
      if (port) {
        ++remastered;
      } else {
        decal.mDecal->Update(dt);
      }
      ++it;
    }
  }
  if (remastered == 0) {
    return;
  }
  // The load metric: the decal context's average GPU time (ms) of the decal generation pass over
  // its last 3 frames (CDecalRenderGen::UpdateTimers 0xb38300 -> RenderDecalGeometry 0xb386a8 ->
  // proxy +0xa8). The port has no such pass yet, so it reads 0, as with no decal context.
  const float load = 0.f;
  float step = 1.f;
  float stepDecrement = 0.f;
  if (load > 0.18f) {
    stepDecrement = (load - 0.18f + 1.f) * 0.5f;
    step = stepDecrement * static_cast< float >(remastered - 1) + 1.f;
  }
  const CVector3f eye = mgr.GetCameraManager()->GetCurrentCamera(mgr).GetTranslation();
  for (AUTO(it, mActiveIndexList.begin()); it != mActiveIndexList.end(); ++it) {
    CDecal& decal = *mDecalPool[*it].mDecal;
    if (!decal.IsPortRemastered()) {
      continue;
    }
    const float distance =
        (decal.GetPortTransform().GetTranslation() - eye).MagSquared() < 12.25f ? 2.f : 1.f;
    const float scale = step * distance - 2.f < 0.f ? step * distance : 2.f;
    decal.Update(scale * dt);
    step -= stepDecrement;
  }
#else
  for (AUTO(it, mActiveIndexList.begin()); it != mActiveIndexList.end();) {
    SDecal& decal = mDecalPool[*it];
    if (decal.mAreaId != mgr.GetNextAreaId() || decal.mDecal->IsDone()) {
      it = RemoveFromActiveList(it, *it);
    } else {
      decal.mDecal->Update(dt);
      ++it;
    }
  }
#endif
}

void CDecalManager::AddToRenderer(const CFrustumPlanes&, const CStateManager& mgr) {
#ifdef TARGET_PC
  // GenerateDecalGeometry's place in RenderPrimaryPass: the opaque world (the receivers) is in.
  PortProjectedDecal::Build();
  // In the HDR scene the Remastered decals draw as their clipped meshes, here, ahead of the
  // sorted drawables: RenderDecalGeometry 0xb386a8 from the main (and thermal cold) scene at
  // 0xf9d854 and from the thermal hot pass at 0xf9dd28. Without HDR they keep the retail quads.
  const bool portHdr = CCubeMaterial::sPortSceneHdr;
  if (portHdr) {
    PortRenderProjected(mgr);
  }
  // Retail adds no decal in the thermal cold pass (DrawWorld only calls this there in the HDR scene).
  if (mgr.GetThermalDrawFlag() == kTD_Cold) {
    return;
  }
#endif
  AUTO(it, mActiveIndexList.begin());
  AUTO(end, mActiveIndexList.end());
  for (; it != end; ++it) {
    const rstl::optional_object< CDecal >& decal = mDecalPool[*it].mDecal;
#ifdef TARGET_PC
    if (portHdr && decal->IsPortRemastered()) {
      continue;
    }
#endif
    if (mDecalPool[*it].mNotIce || mgr.GetThermalDrawFlag() != kTD_Hot) {
      gpRender->AddDrawable(&*decal, decal->GetTranslation(),
                            CAABox(decal->GetTranslation(), decal->GetTranslation()), 2,
                            IRenderer::kDS_SortedCallback);
    }
  }
}

#ifdef TARGET_PC
void CDecalManager::PortRenderProjected(const CStateManager& mgr) {
  // The thermal mask (info+0x138): the main scene draws every decal, the thermal cold scene the
  // ice ones, the thermal hot pass the others.
  const EThermalDrawFlag flag = mgr.GetThermalDrawFlag();
  // RenderDecalGeometry pushes the camera's near plane out by 0.01 (camera+0x64) for both passes,
  // the field of view kept: the same frustum at the new near distance.
  const CGraphics::CProjectionState saved = CGraphics::GetProjectionState();
  const float nearZ = saved.GetNear() + 0.01f;
  const float k = saved.IsPerspective() ? nearZ / saved.GetNear() : 1.f;
  CGraphics::SetProjectionState(CGraphics::CProjectionState(
      saved.IsPerspective(), saved.GetLeft() * k, saved.GetRight() * k, saved.GetTop() * k,
      saved.GetBottom() * k, nearZ, saved.GetFar()));
  // Pass 0 the non-additive materials, pass 1 the additive (MATI blend 1/6), each decal by decal.
  for (int pass = 0; pass < 2; ++pass) {
    for (AUTO(it, mActiveIndexList.begin()); it != mActiveIndexList.end(); ++it) {
      const SDecal& entry = mDecalPool[*it];
      const CDecal& decal = *entry.mDecal;
      if (!decal.IsPortRemastered()) {
        continue;
      }
      if ((flag == kTD_Cold && entry.mNotIce) || (flag == kTD_Hot && !entry.mNotIce)) {
        continue;
      }
      const PortProjectedDecal::Mesh* mesh = PortProjectedDecal::MeshFor(*it);
      if (mesh != nullptr) {
        decal.PortRenderProjected(*mesh, pass);
      }
    }
  }
  CGraphics::SetProjectionState(saved);
}
#endif
