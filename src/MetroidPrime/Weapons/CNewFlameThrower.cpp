#include "MetroidPrime/Weapons/CNewFlameThrower.hpp"
#include "MetroidPrime/Weapons/CWeaponAssetInfo.hpp"
#include "port_debug.h"
#ifdef TARGET_PC
#include "port_synthetic_effect.h"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Cameras/CGameCamera.hpp"
#endif

#include "Collision/CCollidableAABox.hpp"
#include "Collision/CCollidableSphere.hpp"
#include "Collision/CCollisionInfo.hpp"
#include "Collision/CCollisionInfoList.hpp"
#include "Collision/CInternalCollisionStructure.hpp"
#include "Collision/CollisionUtil.hpp"
#include "Kyoto/Audio/CSfxManager.hpp"
#include "Kyoto/Particles/CElementGen.hpp"
#include "Kyoto/Particles/CGenDescription.hpp"
#include "Kyoto/Particles/CParticleGlobals.hpp"
#include "Kyoto/Particles/CParticleSwoosh.hpp"
#include "MetroidPrime/CCollisionActor.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CGameCollision.hpp"
#include "MetroidPrime/CGameLight.hpp"
#include "MetroidPrime/CMain.hpp"
#include "MetroidPrime/CPhysicsActor.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Enemies/CPuddleToadGamma.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/ScriptObjects/CScriptPlatform.hpp"
#include "MetroidPrime/ScriptObjects/CScriptTrigger.hpp"
#include "MetroidPrime/ScriptObjects/CSnakeWeedSwarm.hpp"
#include "MetroidPrime/TCastTo.hpp"
#include "WorldFormat/CMetroidAreaCollider.hpp"
#include "rstl/algorithm.hpp"

static const CMaterialFilter skExcludeProjectilePassthrough =
    CMaterialFilter::MakeExclude(CMaterialList(kMT_ProjectilePassthrough));

CNewFlameThrower::CNewFlameThrower(const TToken< CWeaponDescription >& desc,
                                   const rstl::string& name, EWeaponType wType,
                                   const CWeaponAssetInfo& resInfo,
                                   const CTransform4f& xf, EMaterialTypes matType,
                                   const CDamageInfo& dInfo, TUniqueId uid, TAreaId aid,
                                   TUniqueId owner, uint attribs)
: CGameProjectile(false, desc, name, wType, xf, matType, dInfo, uid, aid, owner, kInvalidUniqueId,
                  attribs, false, CVector3f(1.f, 1.f, 1.f), rstl::optional_object_null(),
                  CSfxManager::kInternalInvalidSfxId, false)
, x2e8_rand(99)
, x2ec_particlesDoneTimer(0.f)
, x2f0_flamesDoneTimer(0.f)
, x2f4_lastParticleCollisionLoc()
, x304_mainFire(gpSimplePool->GetObj(SObjectTag('PART', resInfo.data[0])))
, x310_mainSmoke(gpSimplePool->GetObj(SObjectTag('PART', resInfo.data[1])))
, x31c_secondarySmoke(gpSimplePool->GetObj(SObjectTag('PART', resInfo.data[4])))
, x328_secondaryFire(gpSimplePool->GetObj(SObjectTag('PART', resInfo.data[5])))
, x334_secondarySparks(gpSimplePool->GetObj(SObjectTag('PART', resInfo.data[6])))
, x340_swooshCenter(gpSimplePool->GetObj(SObjectTag('SWHC', resInfo.data[2])))
, x34c_swooshFire(gpSimplePool->GetObj(SObjectTag('SWHC', resInfo.data[3])))
, x358_mainFireGen(nullptr)
, x35c_mainSmokeGen(nullptr)
, x360_secondarySmokeGen(nullptr)
, x364_secondaryFireGen(nullptr)
, x368_secondarySparksGen(nullptr)
, x36c_swooshCenterGen(nullptr)
, x370_swooshFireGen(nullptr)
, x374_flameState(kFS_Default)
, x378_currentLitArea(kInvalidAreaId)
, x37c_24_renderAuxEffects(false)
, x37c_25_firing(false)
, x37c_26_runningSlowish(false)
, x37c_27_newPointAdded(true)
, x37c_28_activeLighting(false)
, x380_flameContactPoints(3, rstl::vector< SSortedListEntry >())
, x3b4_numSmokeParticlesSpawned(0)
, x3b8_lightIds() {
#ifdef TARGET_PC
  // The flame particles and swooshes blend themselves; a rigid actor shift would move them twice.
  PortSetOwnPresentation();
#endif
}

rstl::optional_object< CAABox > CNewFlameThrower::GetTouchBounds() const {
  return rstl::optional_object_null();
}

ENTITY_ACCEPT_IMPL(CNewFlameThrower)

void CNewFlameThrower::Touch(CActor& actor, CStateManager& mgr) {}

void CNewFlameThrower::Think(float dt, CStateManager& mgr) {
  CWeapon::Think(dt, mgr);
  const TAreaId area = mgr.GetWorld()->GetCurrentAreaId();
  mgr.SetActorAreaId(*this, area);
  for (int i = 0; i < x3b8_lightIds.size(); ++i) {
    CEntity* light = mgr.ObjectById(x3b8_lightIds[i]);
    if (light != nullptr) {
      mgr.SetActorAreaId(static_cast< CActor& >(*light), area);
    } else {
      x3b8_lightIds[i] = kInvalidUniqueId;
    }
  }
}

float CNewFlameThrower::UpdateFlameState(float dt, CStateManager& mgr) {
  bool activeLighting = false;
  switch (x374_flameState) {
  case kFS_FireStart:
    x374_flameState = kFS_FireActive;
    break;
  case kFS_FireActive:
    activeLighting = true;
    break;
  case kFS_FireStopTimer:
    activeLighting = true;
    x2f0_flamesDoneTimer = 4.f * dt + x2f0_flamesDoneTimer;
    if (x2f0_flamesDoneTimer > 1.f) {
      x2f0_flamesDoneTimer = 1.f;
      x374_flameState = kFS_FireWaitForParticlesDone;
      x37c_24_renderAuxEffects = false;
    }
    break;
  case kFS_FireWaitForParticlesDone:
    x2ec_particlesDoneTimer += dt;
    if (x2ec_particlesDoneTimer > 0.1f && AreEffectsFinished()) {
      x374_flameState = kFS_Default;
      Reset(mgr, true);
    }
    break;
  }
  if (activeLighting) {
    SetWorldDarkening(mgr, mgr.GetPlayer()->GetCurrentAreaId(), 4.f, 0.7f);
  } else {
    SetWorldDarkening(mgr, mgr.GetPlayer()->GetCurrentAreaId(), 1.f, 1.f);
  }
  return 0.f;
}

void CNewFlameThrower::UpdateFx(const CTransform4f& xf, float dt, CStateManager& mgr) {
  if (GetActive()) {
    x37c_26_runningSlowish = gpMain->GetAverageDrawTime() + gpMain->GetAverageTickTime() > 0.65f;
    UpdateFlameState(dt, mgr);
    const CVector3f pos = xf.GetTranslation();
    const CTransform4f rotation = xf.GetRotation();
    x358_mainFireGen->SetTranslation(pos);
    x358_mainFireGen->SetOrientation(rotation);
    x36c_swooshCenterGen->SetTranslation(pos);
    x36c_swooshCenterGen->SetOrientation(rotation);
    x370_swooshFireGen->SetTranslation(pos);
    x370_swooshFireGen->SetOrientation(rotation);
    x358_mainFireGen->SetGeneratorRate(x37c_26_runningSlowish ? 0.5f : 1.f);
    x358_mainFireGen->Update(dt);
    x35c_mainSmokeGen->Update(dt);
    x360_secondarySmokeGen->Update(dt);
    x364_secondaryFireGen->Update(dt);
    x368_secondarySparksGen->Update(dt);
    x36c_swooshCenterGen->Update(dt);
    x370_swooshFireGen->Update(dt);
    rstl::reserved_vector< CSphere, 32 > collisions;
    DoCollisionCheck(mgr, collisions, dt);
    if (collisions.size() > 0) {
      rstl::vector< CParticleSwoosh::SSwooshData >& swooshes = x36c_swooshCenterGen->Swooshes();
      for (int i = 0; i < swooshes.size(); ++i) {
        CParticleSwoosh::SSwooshData& swoosh = swooshes[i];
        if (swoosh.mActive) {
          for (int j = 0; j < collisions.size(); ++j) {
            const CSphere& sphere = collisions[j];
            const float radiusSquared = sphere.GetRadius() * sphere.GetRadius();
            const float distance = (sphere.GetCenter() - swoosh.mTranslation).MagSquared();
            if (distance < radiusSquared) {
              swoosh.mActive = false;
            }
          }
        }
      }
      for (int i = 0; i < x358_mainFireGen->GetParticleCount(); ++i) {
        const int index = i;
        CElementGen::CParticle& particle = x358_mainFireGen->Particles()[index];
        for (int j = 0; j < collisions.size(); ++j) {
          const CSphere& sphere = collisions[j];
          const float radiusSquared = sphere.GetRadius() * sphere.GetRadius();
          const float distance = (sphere.GetCenter() - particle.x4_pos).MagSquared();
          if (distance < radiusSquared) {
            particle.x0_endFrame = -1;
          }
        }
      }
      for (int i = 0; i < x35c_mainSmokeGen->GetParticleCount(); ++i) {
        const int index = i;
        CElementGen::CParticle& particle = x35c_mainSmokeGen->Particles()[index];
        for (int j = 0; j < collisions.size(); ++j) {
          const CSphere& sphere = collisions[j];
          const float radiusSquared = sphere.GetRadius() * sphere.GetRadius();
          const float distance = (sphere.GetCenter() - particle.x4_pos).MagSquared();
          if (distance < radiusSquared) {
            particle.x0_endFrame = -1;
          }
        }
      }
    }
    if (x374_flameState == kFS_FireActive) {
      const rstl::vector< CParticleSwoosh::SSwooshData >& swooshes =
          x36c_swooshCenterGen->GetSwooshes();
      if (swooshes.capacity() - swooshes.size() < 4) {
        const int step = (swooshes.size() / 2 * 3) / 2;
        const int index = (step + x36c_swooshCenterGen->GetCurParticle()) % swooshes.size();
        const CParticleSwoosh::SSwooshData& swoosh = swooshes[index];
        if (swoosh.mActive) {
          const CVector3f& position = swoosh.mTranslation;
          const CTransform4f& orientation = swoosh.mOrientation;
          CVector3f delta = position - swooshes[(index + 1) % swooshes.size()].mTranslation;
          delta -= CVector3f::Dot(delta, orientation.GetForward()) * orientation.GetForward();
          const float magnitude = delta.Magnitude();
          const float maxRate = x37c_26_runningSlowish ? 2.f : 4.f;
          const float rate = rstl::min_val(rstl::max_val(1.f, magnitude * 30.f), maxRate);
          const int particles = int(rate);
          x3b4_numSmokeParticlesSpawned =
              rstl::max_val(x3b4_numSmokeParticlesSpawned - 1, particles);
          x35c_mainSmokeGen->SetTranslation(position);
          x35c_mainSmokeGen->SetOrientation(orientation);
          x35c_mainSmokeGen->ForceParticleCreation(x3b4_numSmokeParticlesSpawned);
        }
      }
    }
    UpdateLights(mgr);
#ifdef TARGET_PC
    PortUpdateEndCaps(mgr, dt);
    PortUpdateLights(dt);
#endif
  }
}

void CNewFlameThrower::AddToRenderer(const CFrustumPlanes& planes, const CStateManager& mgr) const {
  EnsureRendered(mgr, GetTranslation(), GetSortingBounds(mgr));
}

void CNewFlameThrower::Render(const CStateManager& mgr) const {
  if (GetActive()) {
    x36c_swooshCenterGen->Render();
    x370_swooshFireGen->Render();
    CElementGen* gens[5];
    gens[0] = x358_mainFireGen.get();
    gens[1] = x35c_mainSmokeGen.get();
    gens[2] = x360_secondarySmokeGen.get();
    gens[3] = x364_secondaryFireGen.get();
    gens[4] = x368_secondarySparksGen.get();
#ifdef TARGET_PC
    // Remastered's flamethrower never loads MainSmoke, SecondaryFire or
    // SecondarySparks (its flame body is the swooshes), so with the converted
    // effects (marked by PIRN) only MainFire and SecondarySmoke are drawn.
    if (x304_mainFire->xPortIrnd) {
      gens[1] = x360_secondarySmokeGen.get();
      CElementGen::RenderParticlesFlameThrower(gens, 2);
      for (int i = 0; i < 4; ++i) {
        if (!xPortSourceCaps[i].null()) {
          xPortSourceCaps[i]->Render();
        }
        if (!xPortTargetCaps[i].null()) {
          xPortTargetCaps[i]->Render();
        }
      }
      for (int i = 0; i < 5; ++i) {
        if (!xPortLightGens[i].null()) {
          xPortLightGens[i]->Render();
        }
      }
      return;
    }
#endif
    CElementGen::RenderParticlesFlameThrower(gens, 5);
  }
}

void CNewFlameThrower::Fire(const CTransform4f& xf, CStateManager& mgr, bool) {
  SetActive(true);
  x37c_25_firing = true;
  x37c_24_renderAuxEffects = true;
  x374_flameState = kFS_FireStart;
  CreateFlameParticles(mgr);
}

void CNewFlameThrower::Reset(CStateManager& mgr, bool deactivate) {
  if (deactivate) {
    SetLightsActive(mgr, false);
    SetActive(false);
    x374_flameState = kFS_Default;
    x2ec_particlesDoneTimer = 0.f;
    x2f0_flamesDoneTimer = 0.f;
  } else {
    x374_flameState = kFS_FireStopTimer;
  }
  x37c_25_firing = false;
  x358_mainFireGen->SetParticleEmission(false);
  x35c_mainSmokeGen->SetParticleEmission(false);
  x36c_swooshCenterGen->SetParticleEmission(false);
  x370_swooshFireGen->SetParticleEmission(false);
#ifdef TARGET_PC
  PortStopGens();
#endif
}

void CNewFlameThrower::CreateFlameParticles(CStateManager& mgr) {
  DeleteProjectileLight(mgr);
  x358_mainFireGen = rs_new CElementGen(x304_mainFire);
  x35c_mainSmokeGen = rs_new CElementGen(x310_mainSmoke);
  x360_secondarySmokeGen = rs_new CElementGen(x31c_secondarySmoke);
  x364_secondaryFireGen = rs_new CElementGen(x328_secondaryFire);
  x368_secondarySparksGen = rs_new CElementGen(x334_secondarySparks);
  x36c_swooshCenterGen = rs_new CParticleSwoosh(x340_swooshCenter, 0);
  x36c_swooshCenterGen->SetRenderGaps(true);
  x370_swooshFireGen = rs_new CParticleSwoosh(x34c_swooshFire, 0);
  x370_swooshFireGen->SetRenderGaps(true);
#ifdef TARGET_PC
  PortCreateGens();
#endif
  if (!x358_mainFireGen.null() && x358_mainFireGen->SystemHasLight() && x3b8_lightIds.empty()) {
    CreateLights(mgr);
  }
}

void CNewFlameThrower::AcceptScriptMsg(EScriptObjectMessage msg, TUniqueId uid,
                                       CStateManager& mgr) {
  switch (msg) {
  case kSM_Registered:
    SetThermalFlags(kTF_Hot);
    Think(1.f / 60.f, mgr);
    mgr.AddWeaponId(GetOwnerId(), GetType());
    break;
  case kSM_Deleted:
    mgr.RemoveWeaponId(GetOwnerId(), GetType());
    DestroyLights(mgr);
    SetWorldDarkening(mgr, mgr.GetPlayer()->GetCurrentAreaId(), 4.f, 1.f);
    break;
  }
  CGameProjectile::AcceptScriptMsg(msg, uid, mgr);
}

bool CNewFlameThrower::DoCollisionCheck(CStateManager& mgr,
                                        rstl::reserved_vector< CSphere, 32 >& collisions,
                                        float dt) {
  bool collided = false;
  x2f4_lastParticleCollisionLoc = rstl::optional_object_null();
  rstl::vector< CParticleSwoosh::SSwooshData >& swooshes = x370_swooshFireGen->Swooshes();
  TEntityList nearList;
  const int batchSize = rstl::max_val(swooshes.size() / 4, 6);
  const int count = x370_swooshFireGen->GetSwooshCount();
  int current = count;
  current += x370_swooshFireGen->GetCurParticle();
  --current;
  const int previous = current % count;
  for (int start = 0; start < swooshes.size(); start += batchSize) {
    rstl::optional_object< CAABox > bounds;
    const int end = rstl::min_val(start + batchSize, swooshes.size());
    float radius = 0.f;
    for (int i = start; i < end; ++i) {
      const CParticleSwoosh::SSwooshData& swoosh = swooshes[i];
      if (swoosh.mActive) {
        if (!bounds) {
          bounds = CAABox::MakeMaxInvertedBox();
        }
        bounds->AccumulateBounds(swoosh.mTranslation);
        radius = rstl::max_val(radius, swoosh.mVelocity.MagSquared());
      }
    }
    const float expandedRadius = CMath::SqrtF(radius) + 0.1f;
    radius = expandedRadius;
    if (bounds) {
      bounds->AccumulateBounds(bounds->GetMinPoint() -
                               CVector3f(expandedRadius, expandedRadius, expandedRadius));
      bounds->AccumulateBounds(bounds->GetMaxPoint() +
                               CVector3f(expandedRadius, expandedRadius, expandedRadius));
      nearList.clear();
      mgr.BuildNearList(nearList, *bounds,
                        CMaterialFilter::MakeIncludeExclude(
                            CMaterialList(kMT_Solid), CMaterialList(kMT_ProjectilePassthrough)),
                        mgr.GetPlayer());
      CAreaCollisionCache cache(*bounds);
      CGameCollision::BuildAreaCollisionCache(mgr, cache);
      for (int i = start; i < end; ++i) {
        CParticleSwoosh::SSwooshData& swoosh = swooshes[i];
        if (previous != i && swoosh.mActive) {
          const float sphereRadius = radius;
          const CSphere sphere(swoosh.mTranslation, sphereRadius);
          CCollidableSphere primitive(sphere, CMaterialList(kMT_Solid));
          TUniqueId firstCollision = kInvalidUniqueId;
          CCollisionInfoList contacts;
          bool hit = CGameCollision::DetectStaticCollision_Cached(
              mgr, cache, primitive, CTransform4f::Identity(), skExcludeProjectilePassthrough,
              contacts);
          hit |= DynamicObjectCollision(mgr, nearList, primitive, firstCollision, contacts);
          if (hit && contacts.GetCount() != 0) {
#ifdef TARGET_PC
            while (xPortImpactType.size() < swooshes.size()) {
              xPortImpactType.push_back(0);
            }
            xPortImpactType[i] = 2;
#endif
            swoosh.mActive = false;
            collided = true;
            CCollisionInfoList filteredContacts;
            CollisionUtil::AccumulateCollisionInfo(contacts, filteredContacts);
            CVector3f lastPoint = CVector3f::Zero();
            for (int j = 0; j < filteredContacts.GetCount(); ++j) {
              if (j > 3) {
                break;
              }
              const CCollisionInfo& info = filteredContacts[j];
              const CSphere contactSphere(info.GetPoint(), x37c_26_runningSlowish ? 1.f : 0.75f);
              const int nearbyPoints = GetApproxNumSortedListPointsInSphere(contactSphere);
              if (nearbyPoints < (x37c_26_runningSlowish ? 2 : 3)) {
                AddToSortedLists(info, 10);
                const CTransform4f rotation =
                    CTransform4f::LookAt(CVector3f::Zero(), info.GetNormalLeft(), CVector3f::Up());
                x360_secondarySmokeGen->SetOrientation(rotation);
                x364_secondaryFireGen->SetOrientation(rotation);
                x368_secondarySparksGen->SetOrientation(rotation);
#ifdef TARGET_PC
                if (x304_mainFire->xPortIrnd) {
                  // Remastered re-places its one SecondarySmoke module at the adjusted
                  // impact position (CalculateAdjustedImpactPosition 0x0101f638) and flags
                  // points that hit a surface edge-on (angle to the normal 75..105 deg).
                  const CVector3f velocity = swoosh.mVelocity;
                  if (velocity.IsNonZero()) {
                    const float dot = rstl::min_val(
                        1.f, rstl::max_val(-1.f, CVector3f::Dot(velocity.AsNormalized(),
                                                                info.GetNormalLeft())));
                    const float degrees = CMath::ArcCosineR(dot) * (180.f / 3.14159265f);
                    if (degrees > 75.f && degrees < 105.f) {
                      xPortImpactType[i] = 1;
                    }
                  }
                  x360_secondarySmokeGen->SetTranslation(
                      PortAdjustedImpactPosition(mgr, info.GetPoint(), swoosh));
                } else {
                  x360_secondarySmokeGen->SetTranslation(info.GetPoint());
                }
#else
                x360_secondarySmokeGen->SetTranslation(info.GetPoint());
#endif
                x364_secondaryFireGen->SetTranslation(info.GetPoint());
                x368_secondarySparksGen->SetTranslation(info.GetPoint());
                x360_secondarySmokeGen->ForceParticleCreation(1);
                x364_secondaryFireGen->ForceParticleCreation(x37c_26_runningSlowish ? 2 : 3);
                x368_secondarySparksGen->ForceParticleCreation(x37c_26_runningSlowish ? 3 : 5);
                if (x37c_26_runningSlowish) {
                  break;
                }
                lastPoint = info.GetPoint();
              }
            }
            if (!x37c_26_runningSlowish && x2f4_lastParticleCollisionLoc) {
              if (filteredContacts.GetCount() < 3 ||
                  (*x2f4_lastParticleCollisionLoc - lastPoint).MagSquared() > 3.f) {
                const CVector3f midpoint =
                    CVector3f::Lerp(lastPoint, *x2f4_lastParticleCollisionLoc, 0.5f);
                x364_secondaryFireGen->SetTranslation(midpoint);
                x364_secondaryFireGen->ForceParticleCreation(2);
              }
            }
            x2f4_lastParticleCollisionLoc = lastPoint;
            if (firstCollision != kInvalidUniqueId) {
              if (CActor* actor = TCastToPtr< CActor >(mgr.ObjectById(firstCollision))) {
                if (CanDamage(*actor, mgr)) {
                  mgr.ApplyDamage(GetUniqueId(), actor->GetUniqueId(), GetOwnerId(),
                                  GetCurrentDamageInfo().MakeScaledForTime(dt), GetFilter(),
                                  swoosh.mVelocity.AsNormalized());
                }
              }
            }
            mgr.ApplyDamageToWorld(GetOwnerId(), *this, swoosh.mTranslation,
                                   GetCurrentDamageInfo().MakeScaledForTime(dt), GetFilter());
            collisions.push_back(sphere);
            if (collisions.size() == 32) {
              return true;
            }
          }
        }
      }
      nearList.clear();
      mgr.BuildNearList(nearList, *bounds,
                        CMaterialFilter::MakeInclude(CMaterialList(kMT_NonSolidDamageable)),
                        mgr.GetPlayer());
      for (AUTO(it, nearList.begin()); it != nearList.end(); ++it) {
        if (CSnakeWeedSwarm* swarm = TCastToPtr< CSnakeWeedSwarm >(mgr.ObjectById(*it))) {
          for (int i = start; i < end; ++i) {
            const CParticleSwoosh::SSwooshData& swoosh = swooshes[i];
            if (previous != i && swoosh.mActive) {
              const float sphereRadius = radius;
              const CSphere sphere(swoosh.mTranslation, sphereRadius);
              swarm->ScareSnakeWeeds(mgr, sphere.GetCenter(),
                                     rstl::max_val(sphere.GetRadius(), 1.f) *
                                         swarm->GetWeaponDamageRadius());
            }
          }
        }
      }
      for (int i = start; i < end; ++i) {
        const CParticleSwoosh::SSwooshData& swoosh = swooshes[i];
        if (previous != i && swoosh.mActive) {
          TUniqueId firstCollision = kInvalidUniqueId;
          CCollisionInfoList contacts;
          const float sphereRadius = radius;
          const CSphere sphere(swoosh.mTranslation, sphereRadius);
          CCollidableSphere primitive(sphere, CMaterialList(kMT_Solid));
          DynamicObjectCollision(mgr, nearList, primitive, firstCollision, contacts);
          if (firstCollision != kInvalidUniqueId) {
            if (CActor* actor = TCastToPtr< CActor >(mgr.ObjectById(firstCollision))) {
              if (CanDamage(*actor, mgr)) {
                mgr.ApplyDamage(GetUniqueId(), actor->GetUniqueId(), GetOwnerId(),
                                GetCurrentDamageInfo().MakeScaledForTime(dt), GetFilter(),
                                swoosh.mVelocity.AsNormalized());
              }
            }
          }
        }
      }
    }
  }
  RemoveDeadEntriesFromSortedLists();
  return collided;
}

bool CNewFlameThrower::DynamicObjectCollision(CStateManager& mgr, const TEntityList& nearList,
                                              const CCollisionPrimitive& primitive,
                                              TUniqueId& firstCollision,
                                              CCollisionInfoList& collisions) {
  for (AUTO(it, nearList.begin()); it != nearList.end(); ++it) {
    if (CActor* actor = TCastToPtr< CActor >(mgr.ObjectById(*it))) {
      if (CPhysicsActor* physicsActor = TCastToPtr< CPhysicsActor >(actor)) {
        if (CCollisionPrimitive::Collide(
                CInternalCollisionStructure::CPrimDesc(primitive, skExcludeProjectilePassthrough,
                                                       CTransform4f::Identity()),
                CInternalCollisionStructure::CPrimDesc(*physicsActor->GetCollisionPrimitive(),
                                                       physicsActor->GetMaterialFilter(),
                                                       physicsActor->GetPrimitiveTransform()),
                collisions)) {
          firstCollision = *it;
          return true;
        }
      } else {
        const rstl::optional_object< CAABox > bounds = actor->GetTouchBounds();
        if (bounds) {
          if (CCollisionPrimitive::Collide(
                  CInternalCollisionStructure::CPrimDesc(primitive, skExcludeProjectilePassthrough,
                                                         CTransform4f::Identity()),
                  CInternalCollisionStructure::CPrimDesc(
                      CCollidableAABox(*bounds, CMaterialList(kMT_Solid)),
                      CMaterialFilter::skPassEverything, CTransform4f::Identity()),
                  collisions)) {
            firstCollision = *it;
            return true;
          }
        }
      }
    }
  }
  return false;
}

const bool CNewFlameThrower::AreEffectsFinished() const {
  bool finished = false;
  if (!x358_mainFireGen.null()) {
    finished = x358_mainFireGen->GetParticleCount() == 0;
  }
  if (finished && !x35c_mainSmokeGen.null()) {
    finished = x35c_mainSmokeGen->GetParticleCount() == 0;
  }
  if (finished && !x360_secondarySmokeGen.null()) {
    finished = x360_secondarySmokeGen->GetParticleCount() == 0;
  }
  if (finished && !x364_secondaryFireGen.null()) {
    finished = x364_secondaryFireGen->GetParticleCount() == 0;
  }
  if (finished && !x368_secondarySparksGen.null()) {
    finished = x368_secondarySparksGen->GetParticleCount() == 0;
  }
#ifdef TARGET_PC
  for (int i = 0; i < 4 && finished; ++i) {
    if (!xPortSourceCaps[i].null()) {
      finished = xPortSourceCaps[i]->GetParticleCount() == 0;
    }
    if (finished && !xPortTargetCaps[i].null()) {
      finished = xPortTargetCaps[i]->GetParticleCount() == 0;
    }
  }
  for (int i = 0; i < 5 && finished; ++i) {
    if (!xPortLightGens[i].null()) {
      finished = xPortLightGens[i]->GetParticleCount() == 0;
    }
  }
#endif
  return finished;
}

void CNewFlameThrower::CreateLights(CStateManager& mgr) {
  DestroyLights(mgr);
  for (int i = 0; i < 4; ++i) {
    const TUniqueId uid = mgr.AllocateUniqueId();
    if (uid != kInvalidUniqueId) {
      const CAssetId lightId =
          static_cast< uint >(reinterpret_cast< uintptr_t >(this)) + (i & 1);
      CEntity* light = rs_new CGameLight(
          uid, GetAreaId(), false, rstl::string_l("FlamethrowerLight"), CTransform4f::Identity(),
          GetUniqueId(), x358_mainFireGen->GetLight(), lightId, 0, 0.f);
      mgr.AddObject(light);
      x3b8_lightIds.push_back(uid);
    }
  }
}

void CNewFlameThrower::UpdateLights(CStateManager& mgr) {
  CGlobalRandom globalRandom(x2e8_rand);
  CParticleSwoosh* const swooshGen = x370_swooshFireGen.get();
  const int count = swooshGen->GetSwooshCount();
  int current = count;
  current += swooshGen->GetCurParticle();
  --current;
  const int previous = current % count;
  const int stride = rstl::max_val(2, count / 4);
  for (int i = 0, offset = 0; i < x3b8_lightIds.size(); ++i, offset += stride) {
    if (CGameLight* light = TCastToPtr< CGameLight >(mgr.ObjectById(x3b8_lightIds[i]))) {
      bool active = true;
      if (offset >= swooshGen->GetSwooshCount()) {
        active = false;
      }
      const CParticleSwoosh::SSwooshData& swoosh =
          swooshGen->GetSwooshes()[(offset + previous) % swooshGen->GetSwooshCount()];
      if (!swoosh.mActive) {
        active = false;
      }
      light->SetActive(static_cast< bool >(active));
      if (active) {
        CLight lightData = x358_mainFireGen->GetLight();
        if (CColorElement* color = x304_mainFire->xf0_LCLR) {
          int time = x2e8_rand.Range(0, 16);
          CParticleGlobals::SetEmitterTime(time);
          CColor value = CColor::Yellow();
          color->GetValue(time, value);
          lightData.SetColor(value);
        }
        if (CRealElement* intensity = x304_mainFire->xf4_LINT) {
          int time = x2e8_rand.Range(0, 16);
          CParticleGlobals::SetEmitterTime(time);
          float value = 1.f;
          intensity->GetValue(time, value);
          lightData.SetAngleAttenuation(value, 0.f, 0.f);
        }
        light->SetLight(lightData);
        light->SetTranslation(swoosh.mTranslation);
      }
    }
  }
}

void CNewFlameThrower::DestroyLights(CStateManager& mgr) {
  for (int i = 0; i < x3b8_lightIds.size(); ++i) {
    mgr.DeleteObjectRequest(x3b8_lightIds[i]);
  }
  x3b8_lightIds.clear();
}

void CNewFlameThrower::SetLightsActive(CStateManager& mgr, bool active) {
  for (int i = 0; i < x3b8_lightIds.size(); ++i) {
    if (CGameLight* light = TCastToPtr< CGameLight >(mgr.ObjectById(x3b8_lightIds[i]))) {
      light->SetActive(active);
    }
  }
}

void CNewFlameThrower::AddToSortedLists(const CCollisionInfo& info, int time) {
  for (int i = 0; i < 3; ++i) {
    rstl::vector< SSortedListEntry >& entries = x380_flameContactPoints[i];
    if (entries.size() == entries.capacity()) {
      entries.reserve(entries.capacity() > 0 ? entries.capacity() * 2 : 10);
    }
    entries.push_back(SSortedListEntry(info.GetPoint()[i], time));
  }
  x37c_27_newPointAdded = true;
}

int CNewFlameThrower::GetApproxNumSortedListPointsInSphere(const CSphere& sphere) {
  if (x37c_27_newPointAdded) {
    for (int i = 0; i < 3; ++i) {
      rstl::vector< SSortedListEntry >& entries = x380_flameContactPoints[i];
      rstl::sort(entries.begin(), entries.end());
    }
  }
  int count = 0x7fffffff;
  for (int i = 0; i < 3; ++i) {
    const rstl::vector< SSortedListEntry >& entries = x380_flameContactPoints[i];
    count = rstl::min_val(
        count,
        int(rstl::upper_bound(entries.begin(), entries.end(),
                              SSortedListEntry(sphere.GetCenter()[i] + sphere.GetRadius(), 0)) -
            rstl::lower_bound(entries.begin(), entries.end(),
                              SSortedListEntry(sphere.GetCenter()[i] - sphere.GetRadius(), 0))));
    if (count == 0) {
      return count;
    }
  }
  return count;
}

void CNewFlameThrower::RemoveDeadEntriesFromSortedLists() {
  for (int i = 0; i < 3; ++i) {
    rstl::vector< SSortedListEntry >& entries = x380_flameContactPoints[i];
    int last = entries.size() - 1;
    for (int j = 0; j < entries.size(); ++j) {
      entries[j].x4_remainingTime -= PortDebug::TickFrames();
      if (entries[j].x4_remainingTime <= 0.f) {
        entries[j] = entries[last];
        entries.pop_back();
        --last;
      }
    }
  }
}

void CNewFlameThrower::SetWorldDarkening(CStateManager& mgr, TAreaId area, float speed,
                                         float target) {
  if (x37c_28_activeLighting && x378_currentLitArea != area &&
      x378_currentLitArea != kInvalidAreaId) {
    CGameArea* litArea = mgr.World()->Area(x378_currentLitArea);
    if (litArea->IsPostConstructed()) {
      litArea->SetWeaponWorldLighting(1.f, 1.f);
    }
  }
  x378_currentLitArea = area;
  x37c_28_activeLighting = target != 1.f;
  if (x378_currentLitArea != kInvalidAreaId) {
    CGameArea* litArea = mgr.World()->Area(x378_currentLitArea);
    if (litArea->IsPostConstructed()) {
      litArea->SetWeaponWorldLighting(speed, target);
    }
  }
}

bool CNewFlameThrower::CanDamage(CActor& actor, CStateManager& mgr) {
  if (actor.GetDamageVulnerability()->GetVulnerability(
          x12c_curDamageInfo.GetWeaponMode(), CDamageVulnerability::kRD_No) == kVN_PassThrough) {
    return false;
  }
  if (TCastToPtr< CScriptTrigger >(actor)) {
    return CanCollideWithTrigger(actor, mgr).GetActorId() != kInvalidUniqueId;
  }
  if (TCastToPtr< CScriptPlatform >(actor) || TCastToPtr< CCollisionActor >(actor) ||
      PATTERNED_CAST_TO(CPuddleToadGamma, &actor)) {
    return true;
  }
  return CanCollideWithGameObject(actor, mgr).GetActorId() != kInvalidUniqueId;
}

CNewFlameThrower::~CNewFlameThrower() {}

#ifdef TARGET_PC
// Remastered CalculateAdjustedImpactPosition (0x0101f638): ray from the swoosh point along its
// velocity (length = speed) to the surface; result = 0.25 * hit (else contact) + 0.75 * point.
CVector3f CNewFlameThrower::PortAdjustedImpactPosition(
    const CStateManager& mgr, const CVector3f& contact,
    const CParticleSwoosh::SSwooshData& swoosh) const {
  CVector3f hit = contact;
  if (swoosh.mVelocity.IsNonZero()) {
    const float length = swoosh.mVelocity.Magnitude();
    const CRayCastResult result = CGameCollision::RayStaticIntersection(
        mgr, swoosh.mTranslation, swoosh.mVelocity.AsNormalized(), length,
        skExcludeProjectilePassthrough);
    if (result.IsValid()) {
      hit = result.GetPoint();
    }
  }
  return hit * 0.25f + swoosh.mTranslation * 0.75f;
}

static CElementGen* PortMakeFlameGen(const char* name) {
  const SObjectTag tag('PART', PortRemastered::SyntheticEffectId(name));
  if (!gpResourceFactory->CanBuild(tag)) {
    return nullptr;
  }
  return rs_new CElementGen(gpSimplePool->GetObj(tag));
}

void CNewFlameThrower::PortCreateGens() {
  for (int i = 0; i < 4; ++i) {
    xPortSourceCaps[i] = PortMakeFlameGen("nftsourceendcap");
    xPortTargetCaps[i] = PortMakeFlameGen("nfttargetendcap");
    xPortTargetScale[i] = 1.f;
    if (!xPortSourceCaps[i].null()) {
      xPortSourceCaps[i]->SetParticleEmission(false);
    }
    if (!xPortTargetCaps[i].null()) {
      xPortTargetCaps[i]->SetParticleEmission(false);
    }
  }
  for (int i = 0; i < 5; ++i) {
    xPortLightGens[i] = PortMakeFlameGen("nftlight");
    if (!xPortLightGens[i].null()) {
      xPortLightGens[i]->SetParticleEmission(false);
    }
  }
  xPortImpactType.clear();
}

void CNewFlameThrower::PortStopGens() {
  for (int i = 0; i < 4; ++i) {
    if (!xPortSourceCaps[i].null()) {
      xPortSourceCaps[i]->SetParticleEmission(false);
    }
    if (!xPortTargetCaps[i].null()) {
      xPortTargetCaps[i]->SetParticleEmission(false);
    }
  }
  for (int i = 0; i < 5; ++i) {
    if (!xPortLightGens[i].null()) {
      xPortLightGens[i]->SetParticleEmission(false);
    }
  }
}

// Remastered UpdateEndCaps (0x0101d2fc): walk the fire swoosh ring back from the emitter point,
// collecting up to four runs of active points. A source cap sits on each run's newest point
// (skipped for the emitter point while firing starts/runs), a target cap on its oldest point when
// the next-older point is the emitter point or was flagged as an edge-on hit. Target caps fade
// their scale by the point's distance from the screen centre.
void CNewFlameThrower::PortUpdateEndCaps(CStateManager& mgr, float dt) {
  if (xPortSourceCaps[0].null() && xPortTargetCaps[0].null()) {
    return;
  }
  const rstl::vector< CParticleSwoosh::SSwooshData >& swooshes = x370_swooshFireGen->GetSwooshes();
  const int n = swooshes.size();
  if (n == 0) {
    PortStopGens();
    return;
  }
  while (xPortImpactType.size() < n) {
    xPortImpactType.push_back(0);
  }
  const int emitter = (x370_swooshFireGen->GetCurParticle() + n - 1) % n;
  const auto prev = [n](int i) { return (i - 1 + n) % n; };
  int runs[4][2];
  int runCount = 0;
  int index = emitter;
  while (runCount != 4) {
    int start = index;
    if (!swooshes[index].mActive) {
      do {
        start = prev(start);
        if (start == emitter || start == index) {
          goto done;
        }
      } while (!swooshes[start].mActive);
    }
    int end = start;
    int next = prev(end);
    if (next != emitter && next != start) {
      while (swooshes[next].mActive) {
        end = next;
        next = prev(end);
        if (next == emitter || next == start) {
          break;
        }
      }
    }
    if (start != end) {
      runs[runCount][0] = start;
      runs[runCount][1] = end;
      ++runCount;
    }
    index = prev(end);
    if (index == emitter) {
      break;
    }
  }
done:
  const CGameCamera& camera = mgr.GetCameraManager()->GetCurrentCamera(mgr);
  int sources = 0;
  int targets = 0;
  for (int r = 0; r < runCount; ++r) {
    const CParticleSwoosh::SSwooshData& head = swooshes[runs[r][0]];
    if (runs[r][0] != emitter || (x374_flameState != kFS_FireStart && x374_flameState != kFS_FireActive)) {
      if (sources < 4 && !xPortSourceCaps[sources].null()) {
        CElementGen& gen = *xPortSourceCaps[sources];
        gen.SetOrientation(head.mOrientation);
        gen.SetTranslation(head.mTranslation);
        gen.SetParticleEmission(true);
      }
      ++sources;
    }
    const int older = prev(runs[r][1]);
    if (older == emitter || xPortImpactType[older] == 1) {
      const CParticleSwoosh::SSwooshData& tail = swooshes[runs[r][1]];
      if (targets < 4 && !xPortTargetCaps[targets].null()) {
        CElementGen& gen = *xPortTargetCaps[targets];
        gen.SetOrientation(tail.mOrientation);
        gen.SetTranslation(tail.mTranslation);
        gen.SetParticleEmission(true);
        // CDisplayManager::ConvertToNormalizedSpace: NDC xy, (-1, -1) when off screen.
        float ndcX = -1.f;
        float ndcY = -1.f;
        const CVector3f view = camera.GetTransform().TransposeMultiply(tail.mTranslation);
        if (view.GetY() > 0.f) {
          const CVector3f ndc = camera.ConvertToScreenSpace(tail.mTranslation);
          if (ndc.GetX() >= -1.f && ndc.GetX() < 1.f && ndc.GetY() >= -1.f && ndc.GetY() <= 1.f) {
            ndcX = ndc.GetX();
            ndcY = ndc.GetY();
          }
        }
        float m = CMath::SqrtF(ndcX * ndcX + ndcY * ndcY) * 100.f;
        m = rstl::min_val(rstl::max_val(m, 0.f), 100.f);
        float target = 1.f;
        if (m > 4.5f) {
          target = 0.01f;
        } else if (m > 2.5f) {
          target = rstl::min_val(rstl::max_val((4.5f - m) * 0.5f, 0.01f), 1.f);
        }
        float scale = target;
        if (target > xPortTargetScale[targets]) {
          scale = rstl::min_val(xPortTargetScale[targets] + dt * 4.f, target);
        }
        xPortTargetScale[targets] = scale;
        gen.SetGlobalScale(CVector3f(scale, scale, scale));
      }
      ++targets;
    }
  }
  for (int i = sources; i < 4; ++i) {
    if (!xPortSourceCaps[i].null()) {
      xPortSourceCaps[i]->SetParticleEmission(false);
    }
  }
  for (int i = targets; i < 4; ++i) {
    if (!xPortTargetCaps[i].null()) {
      xPortTargetCaps[i]->SetParticleEmission(false);
    }
  }
  for (int i = 0; i < 4; ++i) {
    if (!xPortSourceCaps[i].null()) {
      xPortSourceCaps[i]->Update(dt);
    }
    if (!xPortTargetCaps[i].null()) {
      xPortTargetCaps[i]->Update(dt);
    }
  }
}

// Remastered UpdateLights (0x0101d854): each NFTLight gen sits on the fire swoosh point
// i * max(2, count / lights) back from the emitter point, emitting while that point is active.
void CNewFlameThrower::PortUpdateLights(float dt) {
  if (xPortLightGens[0].null()) {
    return;
  }
  const int n = x370_swooshFireGen->GetSwooshCount();
  if (n == 0) {
    return;
  }
  const int stride = rstl::max_val(2, n / 5);
  const int base = (x370_swooshFireGen->GetCurParticle() + n - 1) % n;
  for (int i = 0; i < 5; ++i) {
    CElementGen& gen = *xPortLightGens[i];
    const int offset = i * stride;
    const CParticleSwoosh::SSwooshData& swoosh = x370_swooshFireGen->GetSwooshes()[(base + offset) % n];
    const bool active = offset < n && swoosh.mActive;
    gen.SetParticleEmission(active);
    if (active) {
      gen.SetTranslation(swoosh.mTranslation);
      gen.Update(dt);
    }
  }
}
#endif
