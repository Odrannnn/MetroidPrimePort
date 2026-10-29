#include "MetroidPrime/ScriptObjects/CScriptDoor.hpp"
#include "port_env.h"

#include "Collision/CMaterialList.hpp"
#include "Kyoto/Math/CFrustumPlanes.hpp"
#include "MetroidPrime/CAnimData.hpp"
#include "MetroidPrime/CAnimPlaybackParms.hpp"
#include "MetroidPrime/CEntity.hpp"
#include "MetroidPrime/CEntityInfo.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CMapWorld.hpp"
#include "MetroidPrime/CMapWorldInfo.hpp"
#include "MetroidPrime/CObjectList.hpp"
#include "MetroidPrime/CPhysicsActor.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Cameras/CBallCamera.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "MetroidPrime/ScriptObjects/CScriptDock.hpp"
#include "MetroidPrime/TCastTo.hpp"
#include "MetroidPrime/TGameTypes.hpp"

#ifdef TARGET_PC
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace {
// MP_LOG_DOORS: how long each door waited to open, and on which condition.
struct SPortDoorWait {
  std::chrono::steady_clock::time_point start;
  long long reportedMs;
  std::map< const char*, int > ticks;
};
std::map< ushort, SPortDoorWait > sPortDoorWaits;
const char* sPortDoorReason = "";
// The area (occluding) or door (otherDoor) the last wait was on, or -1.
int sPortDoorDetail = -1;

void PortPrintDoorWait(TUniqueId door, const char* what, const SPortDoorWait& wait, long long ms) {
  std::fprintf(stderr, "MP door %04X %s after %lld ms:", door.Value(), what, ms);
  for (std::map< const char*, int >::const_iterator r = wait.ticks.begin(); r != wait.ticks.end();
       ++r) {
    std::fprintf(stderr, " %s=%d", r->first, r->second);
  }
  std::fprintf(stderr, "\n");
}

bool PortLogDoors() {
  static const bool enabled = port::EnvFlag("MP_LOG_DOORS");
  return enabled;
}

// Anything else that happens to a door: opened without a wait, an Open it
// refused, or a wait that a Close cut short (the door's timer can close it
// while it is still waiting, and the shield comes back).
void PortLogDoorEvent(TUniqueId door, const char* what) {
  if (!PortLogDoors()) {
    return;
  }
  std::map< ushort, SPortDoorWait >::iterator it = sPortDoorWaits.find(door.Value());
  if (it == sPortDoorWaits.end()) {
    std::fprintf(stderr, "MP door %04X %s\n", door.Value(), what);
    return;
  }
  const long long ms = std::chrono::duration_cast< std::chrono::milliseconds >(
                           std::chrono::steady_clock::now() - it->second.start)
                           .count();
  PortPrintDoorWait(door, what, it->second, ms);
  sPortDoorWaits.erase(it);
}

void PortLogDoorWait(TUniqueId door, CScriptDoor::EDoorOpenCondition cond, const CWorld* world) {
  if (!PortLogDoors()) {
    return;
  }
  const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
  std::map< ushort, SPortDoorWait >::iterator it = sPortDoorWaits.find(door.Value());
  if (cond == CScriptDoor::kDOC_Loading) {
    if (it == sPortDoorWaits.end()) {
      it = sPortDoorWaits.insert(std::make_pair(door.Value(), SPortDoorWait())).first;
      it->second.start = now;
      it->second.reportedMs = 0;
    }
    ++it->second.ticks[sPortDoorReason];
    // A door that never opens would otherwise never be reported.
    const long long ms =
        std::chrono::duration_cast< std::chrono::milliseconds >(now - it->second.start).count();
    if (ms - it->second.reportedMs >= 3000) {
      it->second.reportedMs = ms;
      PortPrintDoorWait(door, "still waiting", it->second, ms);
      if (sPortDoorDetail >= 0) {
        std::fprintf(stderr, "MP door %04X now on %s %04X\n", door.Value(), sPortDoorReason,
                     sPortDoorDetail);
      }
      if (world != nullptr && std::strcmp(sPortDoorReason, "sky") == 0) {
        char sky[512];
        world->PortDescribeSky(sky, sizeof(sky));
        std::fprintf(stderr, "MP door %04X sky: %s\n", door.Value(), sky);
      }
    }
    return;
  }
  if (it == sPortDoorWaits.end()) {
    return;
  }
  const long long ms =
      std::chrono::duration_cast< std::chrono::milliseconds >(now - it->second.start).count();
  PortPrintDoorWait(door, cond == CScriptDoor::kDOC_Ready ? "opened" : "gave up", it->second, ms);
  sPortDoorWaits.erase(it);
}
} // namespace
#define DOOR_WAIT_ON(reason, id) (sPortDoorReason = (reason), sPortDoorDetail = (id), kDOC_Loading)
#else
#define DOOR_WAIT_ON(reason, id) kDOC_Loading
#endif
#define DOOR_WAIT(reason) DOOR_WAIT_ON(reason, -1)

CScriptDoor::CScriptDoor(TUniqueId uid, const rstl::string& name, const CEntityInfo& info,
                         const CTransform4f& xf, const CModelData& modelData,
                         const CActorParameters& actorParameters, const CVector3f& orbitPosition,
                         const CAABox& bounds, const bool active, const bool open,
                         const bool projectilesCollide, const float animationLength,
                         const bool ballDoor)
: CPhysicsActor(uid, active, name, info, xf, modelData,
                open ? CMaterialList(kMT_Solid, kMT_Immovable, kMT_Orbit)
                     : CMaterialList(kMT_Immovable, kMT_Occluder, kMT_Solid, kMT_Orbit),
                bounds, SMoverData(1.f), actorParameters, 0.3f, 0.1f)
, mAnimLength(animationLength)
, mAnimTime(0.f)
, mDoorState(kDAT_Open)
, x264_(GetBoundingBox())
, mPartner1(kInvalidUniqueId)
, mPartner2(kInvalidUniqueId)
, mPrevDoor(kInvalidUniqueId)
, mDockId(kInvalidUniqueId)
, mModelBounds(modelData.GetBounds(xf.GetRotation()))
, mOrbitPos(orbitPosition)
, mClosing(false)
, mWasOpen(open)
, mIsOpen(open)
, mConditionsMet(false)
, mProjectilesCollide(projectilesCollide)
, mBallDoor(ballDoor)
, mDoClose(false)
#ifdef TARGET_PC
, mPortRestoredOpen(false)
, mPortDockNotified(false)
, mPortPartnerSide(0)
, mPortRestoreTicks(0)
#endif
{
  SetThermalFlags(kTF_Cold);

  if (open) {
    SetDoorAnimation(kDAT_Open);
  }

  SetMass(0.f);
}

rstl::optional_object< CAABox > CScriptDoor::GetTouchBounds() const {
  if (GetActive() && GetMaterialList().HasMaterial(kMT_Solid)) {
    return CPhysicsActor::GetBoundingBox();
  }

  return rstl::optional_object_null();
}

rstl::optional_object< CAABox > CScriptDoor::GetProjectileBounds() const {
  if (mProjectilesCollide) {
    return CAABox(mModelBounds.GetMinPoint() + GetTranslation(),
                  mModelBounds.GetMaxPoint() + GetTranslation());
  }

  return rstl::optional_object_null();
}

CVector3f CScriptDoor::GetOrbitPosition(const CStateManager& mgr) const {
  return GetTranslation() + mOrbitPos;
}

CScriptDoor::EDoorOpenCondition CScriptDoor::GetDoorOpenCondition(CStateManager& mgr) {
  CScriptDock* const dock = TCastToPtr< CScriptDock >(mgr.ObjectById(mDockId));
  if (!dock) {
    return kDOC_Ready;
  }

  if (mAnimTime < 0.05f || mDoClose) {
    return DOOR_WAIT("anim");
  }

  const CWorld* world = mgr.GetWorld();
  if (!world->DoesAreaExist(dock->GetAreaId())) {
    return kDOC_NotReady;
  }
  if (!world->IsAreaValid(dock->GetAreaId())) {
    return DOOR_WAIT("thisArea");
  }
  if (!world->AreSkyNeedsMet()) {
    return DOOR_WAIT("sky");
  }

  const IGameArea::Dock& gameDock =
      mgr.GetWorld()->GetAreaAlways(dock->GetAreaId()).GetDock(dock->GetDockId());
  const TAreaId connectedArea = gameDock.GetConnectedAreaId(dock->GetDockReference(mgr));
  if (!mgr.GetWorld()->DoesAreaExist(connectedArea)) {
    return kDOC_NotReady;
  }

  CGameArea* area = mgr.World()->Area(connectedArea);
  if (!area->IsLoaded()) {
#ifdef TARGET_PC
    // Issue #88: a room stays loaded behind a door that closed because another
    // door opened. Streaming a third room on top can fill the fixed-size object
    // list (PCA + Phazon Processing Center + Elite Quarters in backwards Lower
    // Mines), and AllocateUniqueId then hangs in the error handler. Release
    // those rooms first, as the doorway trigger's SetToZero would; TravelToArea
    // deallocates them before it streams the new one. Always in PCA; elsewhere
    // only once the list is half full, so uncrowded doors keep the original
    // behaviour. Central Dynamo (621 objects) behind Quarantine Access A (231)
    // plus Metroid Quarantine A hung the same way on a tablet.
    const CAssetId kProcessingCenterAccess = 0xED6DE73B;
    const int kCrowdedObjects = 512;
    if (mgr.GetNextAreaId() == dock->GetAreaId() &&
        (mgr.GetObjectListById(kOL_All).size() >= kCrowdedObjects ||
         mgr.GetWorld()->GetAreaAlways(dock->GetAreaId()).IGetAreaAssetId() ==
             kProcessingCenterAccess)) {
      bool otherOpen = false;
      CObjectList& doors = mgr.ObjectListById(kOL_PlatformAndDoor);
      for (int i = doors.GetFirstObjectIndex(); i != -1; i = doors.GetNextObjectIndex(i)) {
        const CScriptDoor* const door = TCastToConstPtr< CScriptDoor >(doors[i]);
        if (!door || door == this || door->GetCurrentAreaId() != dock->GetAreaId()) {
          continue;
        }
        CScriptDock* const other = TCastToPtr< CScriptDock >(mgr.ObjectById(door->mDockId));
        if (!other || other == dock || other->GetAreaId() != dock->GetAreaId()) {
          continue;
        }
        const IGameArea::Dock& otherDock =
            mgr.GetWorld()->GetAreaAlways(other->GetAreaId()).GetDock(other->GetDockId());
        const int ref = other->GetDockReference(mgr);
        if (!otherDock.GetShouldLoadOther(ref) || otherDock.GetConnectedAreaId(ref) == connectedArea) {
          continue;
        }
        if (door->mIsOpen || door->mWasOpen) {
          otherOpen = true;
        } else {
          PortLogDoorEvent(GetUniqueId(), "released the room behind a closed door");
          mgr.DeliverScriptMsg(other, GetUniqueId(), kSM_SetToZero);
        }
      }
      if (otherOpen) {
        return DOOR_WAIT("otherDoorFirst");
      }
    }
#endif
    mgr.DeliverScriptMsg(dock, GetUniqueId(), kSM_SetToMax);
    return DOOR_WAIT("areaLoad");
  }
  if (area->GetPostConstructed()->x113c_playerActorsLoading != 0) {
    return DOOR_WAIT("actors");
  }

  const CObjectList& objects = mgr.ObjectListById(kOL_PlatformAndDoor);
  for (int i = objects.GetFirstObjectIndex(); i != -1; i = objects.GetNextObjectIndex(i)) {
    if (const CScriptDoor* const door = TCastToConstPtr< CScriptDoor >(objects[i])) {
      if (door->GetUniqueId() != GetUniqueId() &&
          (door->GetCurrentAreaId() == GetCurrentAreaId() ||
           door->GetCurrentAreaId() == connectedArea) &&
          door->mWasOpen && door->mDockId != kInvalidUniqueId) {
        return DOOR_WAIT_ON("otherDoor", door->GetUniqueId().Value());
      }
    }
  }

  for (CGameArea::CConstChainIterator it = mgr.GetWorld()->GetChainHead(CWorld::kC_Alive);
       it != CWorld::skGlobalEnd; ++it) {
    if (it->GetAreaId() != area->GetAreaId() && !it->IsFinishedOccluding()) {
      return DOOR_WAIT_ON("occluding", it->GetAreaId().Value());
    }
  }

  if (!area->TryTakingOutOfARAM()) {
    return DOOR_WAIT("aram");
  }

  return mgr.GetWorld()->GetMapWorld()->IsMapAreasStreaming() ? DOOR_WAIT("map") : kDOC_Ready;
}

void CScriptDoor::OpenDoor(TUniqueId uid, CStateManager& mgr) {
  mgr.MapWorldInfo()->SetDoorVisited(mgr.GetEditorIdForUniqueId(GetUniqueId()), true);
  mIsOpen = true;
  mWasOpen = true;
  mConditionsMet = false;
  mPartner1 = kInvalidUniqueId;
  mPartner2 = kInvalidUniqueId;

  if (const CScriptDoor* const door = TCastToConstPtr< CScriptDoor >(mgr.GetObjectById(uid))) {
    mPartner1 = door->GetUniqueId();
  }

  SetDoorAnimation(kDAT_Open);

  if (mPartner1 != kInvalidUniqueId) {
    SendScriptMsgs(kSS_MaxReached, mgr, kSM_None);
  } else {
    SendScriptMsgs(kSS_Open, mgr, kSM_None);
  }

  if (const CScriptDock* const dock1 = TCastToConstPtr< CScriptDock >(mgr.GetObjectById(mDockId))) {
    CObjectList& list = mgr.ObjectListById(kOL_PlatformAndDoor);
    for (int idx = list.GetFirstObjectIndex(); idx != -1; idx = list.GetNextObjectIndex(idx)) {
      if (CScriptDoor* const door = TCastToPtr< CScriptDoor >(list[idx])) {
        if (door->GetUniqueId() == uid) {
          continue;
        }

        if (const CScriptDock* const dock2 =
                TCastToConstPtr< CScriptDock >(mgr.GetObjectById(door->GetConnectedDockID()))) {
          if (dock2->GetAreaId() == dock1->GetCurrentConnectedAreaId(mgr) &&
              dock2->GetCurrentConnectedAreaId(mgr) == dock1->GetAreaId()) {
            mPartner2 = door->GetUniqueId();
            mgr.DeliverScriptMsg(door, GetUniqueId(), kSM_Open);
            break;
          }
        }
      }
    }
  }

  if (mPartner1 == kInvalidUniqueId && mPartner2 == kInvalidUniqueId) {

    for (rstl::vector< SConnection >::const_iterator it = GetConnectionList().begin();
         it != GetConnectionList().end(); ++it) {
      if (it->x4_msg != kSM_Open) {
        continue;
      }

      if (const CScriptDoor* const door =
              TCastToConstPtr< CScriptDoor >(mgr.GetObjectById(mgr.GetIdForScript(it->x8_objId)))) {
        mPartner2 = door->GetUniqueId();
        break;
      }
    }
  }
}

#ifdef TARGET_PC
void CScriptDoor::PortRestoreDoorState(const PortDoorFlags& state) {
  mIsOpen = state.open;
  // A close that was in flight is put back the way a live one leaves it: the door
  // reads closed and Think still owes the message its own branch sends - the
  // animation-end branch for a normal close, the mClosing branch for a refused
  // opening. A refused close does not set mWasOpen (it never opened, and the
  // animation-end branch would then send a second, fuller Closed). mAnimTime is
  // left alone: Think advances it only while the door is not open and the branch
  // that finishes the close resets it, so the animation replays from its start.
  mWasOpen = state.open || state.normalClosePending;
  mClosing = state.refusedClosePending;
  mDoClose = state.doClose;
  mConditionsMet = false;
  // A live open leaves the partner links, the dock's Increment and the connected
  // room's visibility behind; a save state carries none of them, so Think puts
  // them back (PortFinishRestoredOpen). A stale link would point at a door that
  // is no longer there.
  mPartner1 = kInvalidUniqueId;
  mPartner2 = kInvalidUniqueId;
  mPortRestoredOpen = state.open;
  mPortPartnerSide = state.open ? state.partner & 0x3 : 0;
  mPortDockNotified = false;
  mPortRestoreTicks = 0;
  SetDoorAnimation(state.open ? kDAT_Open : kDAT_Close);
}

// What OpenDoor leaves that a save state does not carry, for a door restored open.
// Without the dock's Increment the connected room is loaded but never made visible
// (a void behind the open door); without the partner link, closing this door never
// closes the door on the other side, which stays open and holds this one shut
// ("otherDoor" in GetDoorOpenCondition). The partner is found as OpenDoor finds
// it, by the door that led the open at the save (issue #201: the follower's close
// timer never started, so a pair linked the other way round never closes). A save
// older than the saved side has only the guess the door of the player's room makes.
// The search is bounded: the other room's door only exists once its area has loaded.
void CScriptDoor::PortFinishRestoredOpen(CStateManager& mgr) {
  if (!mIsOpen) {
    mPortRestoredOpen = false;
    return;
  }
  CScriptDock* const dock = TCastToPtr< CScriptDock >(mgr.ObjectById(mDockId));
  if (!dock) {
    mPortRestoredOpen = false;
    return;
  }
  if (!mPortDockNotified) {
    mPortDockNotified = true;
    mgr.DeliverScriptMsg(dock, GetUniqueId(), kSM_Increment);
  }
  // Unknown (0) searches from the player's room; leads (2) wherever it is; none (1)
  // and follows (3) never: a follower is linked by its leader.
  const bool searches = mPortPartnerSide == 2 ||
                        (mPortPartnerSide == 0 && GetCurrentAreaId() == mgr.GetNextAreaId());
  if (mPartner1 != kInvalidUniqueId || mPartner2 != kInvalidUniqueId || !searches ||
      ++mPortRestoreTicks > 600) {
    mPortRestoredOpen = false;
    return;
  }
  CObjectList& list = mgr.ObjectListById(kOL_PlatformAndDoor);
  for (int idx = list.GetFirstObjectIndex(); idx != -1; idx = list.GetNextObjectIndex(idx)) {
    CScriptDoor* const door = TCastToPtr< CScriptDoor >(list[idx]);
    if (!door || door == this || !door->mIsOpen || door->mPartner1 != kInvalidUniqueId ||
        door->mPartner2 != kInvalidUniqueId || door->mPortPartnerSide == 1 ||
        door->mPortPartnerSide == 2) {
      continue;
    }
    if (const CScriptDock* const dock2 =
            TCastToConstPtr< CScriptDock >(mgr.GetObjectById(door->GetConnectedDockID()))) {
      if (dock2->GetAreaId() == dock->GetCurrentConnectedAreaId(mgr) &&
          dock2->GetCurrentConnectedAreaId(mgr) == dock->GetAreaId()) {
        mPartner2 = door->GetUniqueId();
        door->mPartner1 = GetUniqueId();
        mPortRestoredOpen = false;
        return;
      }
    }
  }
}
#endif

void CScriptDoor::SetDoorAnimation(EDoorAnimType state) {
  mDoorState = state;
  if (HasAnimation()) {
    AnimationData()->SetAnimation(CAnimPlaybackParms(static_cast< int >(state), -1, 1.f, true),
                                  false);
  }
}

void CScriptDoor::AcceptScriptMsg(EScriptObjectMessage msg, TUniqueId uid, CStateManager& mgr) {
  switch (msg) {
  case kSM_Close: {
    if (!GetActive()) {
      return;
    }

    if (mPartner1 == kInvalidUniqueId || mPartner1 == uid) {
      if (mIsOpen) {
        if (mPartner2 != kInvalidUniqueId) {
          // lol, this is its actual name
          static int i = 0;
          if (CEntity* ent = mgr.ObjectById(mPartner2)) {
            ++i;
            mgr.DeliverScriptMsg(ent, GetUniqueId(), kSM_Close);
            --i;
          }
        }
        mIsOpen = false;
        SetDoorAnimation(kDAT_Close);
        mgr.GetCameraManager()->BallCamera()->DoorClosing(GetUniqueId());
      } else if (mConditionsMet) {
#ifdef TARGET_PC
        PortLogDoorEvent(GetUniqueId(), "wait cancelled by Close");
#endif
        mConditionsMet = false;
        SendScriptMsgs(kSS_Closed, mgr, kSM_None);
      }
    }
    break;
  }
  case kSM_Action: {
    if (mPartner1 != kInvalidUniqueId) {
      if (CScriptDoor* door = TCastToPtr< CScriptDoor >(mgr.ObjectById(mPartner1))) {
        if (door->IsOpen()) {
          mDoClose = true;
          mgr.DeliverScriptMsg(door, GetUniqueId(), kSM_Close);
          door->SetDoClose(true);
        }
      }
    } else if (mIsOpen) {
      mDoClose = true;
      if (CScriptDoor* door = TCastToPtr< CScriptDoor >(mgr.ObjectById(mPartner2))) {
        mgr.DeliverScriptMsg(door, GetUniqueId(), kSM_Close);
        door->SetDoClose(true);
      }
      mIsOpen = false;
      SetDoorAnimation(kDAT_Close);
      mgr.GetCameraManager()->BallCamera()->DoorClosing(GetUniqueId());
    }
    break;
  }
  case kSM_Open: {
    if (!GetActive()) {
#ifdef TARGET_PC
      PortLogDoorEvent(GetUniqueId(), "ignored Open (inactive)");
#endif
      return;
    }

    if (!mIsOpen) {

      const EDoorOpenCondition cond = TCastToConstPtr< CScriptDoor >(mgr.GetObjectById(uid))
                                          ? kDOC_Ready
                                          : GetDoorOpenCondition(mgr);

      switch (cond) {
      case kDOC_Loading:
        mConditionsMet = true;
        mPrevDoor = uid;
        break;
      case kDOC_Ready:
#ifdef TARGET_PC
        PortLogDoorEvent(GetUniqueId(), "opened at once");
#endif
        OpenDoor(uid, mgr);
        break;
      case kDOC_NotReady:
      default:
#ifdef TARGET_PC
        PortLogDoorEvent(GetUniqueId(), "refused Open (area missing)");
#endif
        mWasOpen = false;
        mClosing = true;
        break;
      }
    }
    break;
  }
  case kSM_InitializedInArea: {
    rstl::vector< SConnection >::const_iterator it = GetConnectionList().begin();
    for (; it != GetConnectionList().end(); ++it) {
      if (it->x4_msg != kSM_Increment) {
        continue;
      }

      if (const CScriptDock* dock =
              TCastToConstPtr< CScriptDock >(mgr.GetObjectById(mgr.GetIdForScript(it->x8_objId)))) {
        mDockId = dock->GetUniqueId();
        break;
      }
    }
    break;
  }
  case kSM_SetToZero:
    mProjectilesCollide = true;
    mgr.MapWorldInfo()->SetDoorVisited(mgr.GetEditorIdForUniqueId(GetUniqueId()), true);
    break;
  case kSM_SetToMax:
    mProjectilesCollide = false;
    break;
  default:
    CPhysicsActor::AcceptScriptMsg(msg, uid, mgr);
    break;
  }
}

void CScriptDoor::Think(float dt, CStateManager& mgr) {
  if (!GetActive()) {
    return;
  }

  if (!mIsOpen && mAnimTime < 0.05f) {
    mAnimTime += dt;
  }

#ifdef TARGET_PC
  if (mPortRestoredOpen) {
    PortFinishRestoredOpen(mgr);
  }
#endif

  if (mConditionsMet) {
    const EDoorOpenCondition cond = GetDoorOpenCondition(mgr);
#ifdef TARGET_PC
    PortLogDoorWait(GetUniqueId(), cond, mgr.GetWorld());
#endif
    if (cond == kDOC_Ready) {
      mConditionsMet = false;
      OpenDoor(mPrevDoor, mgr);
    }
  }

  if (mClosing) {
    mWasOpen = false;
    mgr.GetCameraManager()->BallCamera()->DoorClosed(GetUniqueId());
    mProjectilesCollide = false;
    mClosing = false;
    SendScriptMsgs(kSS_Closed, mgr, kSM_Decrement);
    mAnimTime = 0.f;
    mDoClose = false;
  }

  if (mIsOpen && !GetModelData()->IsAnimating()) {
    RemoveMaterial(kMT_Solid, kMT_Occluder, kMT_Orbit, kMT_Scannable, mgr);
  } else {
    if (mWasOpen && !GetModelData()->IsAnimating()) {
      mWasOpen = false;
      mgr.GetCameraManager()->BallCamera()->DoorClosed(GetUniqueId());
      mProjectilesCollide = false;
      mConditionsMet = false;
      SendScriptMsgs(kSS_Closed, mgr, kSM_None);
      mAnimTime = 0.f;
      mDoClose = false;
    }

    if (GetScannableObjectInfo()) {
      AddMaterial(kMT_Solid, kMT_Metal, kMT_Occluder, kMT_Orbit, kMT_Scannable, mgr);
    } else {
      AddMaterial(kMT_Solid, kMT_Metal, kMT_Occluder, kMT_Orbit, mgr);
    }
  }

  if (GetModelData()->IsAnimating()) {
    float len = GetModelData()->GetAnimationDuration(static_cast< int >(mDoorState));
    len /= mAnimLength;
    UpdateAnimation(len * dt, mgr, true);
  }
  SetTargetable(mgr.GetPlayerState()->GetCurrentVisor() == CPlayerState::kPV_Scan);
}

bool CScriptDoor::IsConnectedToArea(const CStateManager& mgr, TAreaId areaId) const {
  const CScriptDock* dockEnt = TCastToConstPtr< CScriptDock >(mgr.GetObjectById(mDockId));
  if (dockEnt) {
    if (dockEnt->GetAreaId() == areaId) {
      return true;
    }

    const CWorld* world = mgr.GetWorld();
    const CGameArea& area = world->GetAreaAlways(dockEnt->GetAreaId());
    const CGameArea::Dock& dock = area.GetDock(dockEnt->GetDockId());
    if (dock.GetConnectedAreaId(dockEnt->GetDockReference(mgr)) == areaId) {
      return true;
    }
  }
  return false;
}

void CScriptDoor::ForceClosed(CStateManager& mgr) {
  if (mIsOpen) {
    mIsOpen = false;
    mWasOpen = false;

    mgr.GetCameraManager()->BallCamera()->DoorClosing(GetUniqueId());
    mgr.GetCameraManager()->BallCamera()->DoorClosed(GetUniqueId());

    SetDoorAnimation(kDAT_Close);
    SendScriptMsgs(kSS_Closed, mgr, kSM_None);
    mConditionsMet = false;
    mAnimTime = 0.f;
    mDoClose = false;
  } else if (mConditionsMet) {
#ifdef TARGET_PC
    PortLogDoorEvent(GetUniqueId(), "wait cancelled by ForceClosed");
#endif
    mConditionsMet = false;
    mDoClose = false;
    SendScriptMsgs(kSS_Closed, mgr, kSM_None);
  }
}

void CScriptDoor::AddToRenderer(const CFrustumPlanes& /*frustum*/, const CStateManager& mgr) const {
  if (GetPreRenderClipped()) {
    return;
  }

  CPhysicsActor::Render(mgr);
}

void CScriptDoor::Render(const CStateManager& mgr) const {}

ENTITY_ACCEPT_IMPL(CScriptDoor)
