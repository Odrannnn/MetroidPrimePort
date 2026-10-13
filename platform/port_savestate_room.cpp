#include "port_savestate.h"

#include "Kyoto/Math/CTransform4f.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CHealthInfo.hpp"
#include "MetroidPrime/CObjectList.hpp"
#include "MetroidPrime/CPhysicsActor.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Enemies/CPatterned.hpp"
#include "MetroidPrime/ScriptObjects/CScriptCounter.hpp"
#include "MetroidPrime/ScriptObjects/CScriptDoor.hpp"
#include "MetroidPrime/ScriptObjects/CScriptPlatform.hpp"
#include "MetroidPrime/ScriptObjects/CScriptTimer.hpp"
#include "MetroidPrime/TCastTo.hpp"

#include <algorithm>
#include <cstring>
#include <set>

// Block tags of a RoomObject. A reader skips tags it does not know.
enum RoomTag : uint32_t {
  kTagTransform = 1,  // 12 floats, the CActor transform
  kTagMotion = 2,     // velocity, momentum (CPhysicsActor)
  kTagHealth = 3,     // hp, knockback resistance
  // 4 was the AI state: forcing one skips what the enemy class sets up around it
  // (a parasite put into Patrol from Generate deletes itself), so enemies resume
  // through their own Start.
  kTagTimer = 5,      // time, timing
  kTagCounter = 6,    // current
  kTagDoor = 7,       // open
  kTagPlatform = 8,   // current and target waypoint (editor ids), speed, move delay
};

// Friend of the script classes whose state is private.
struct PortRoomState {
  static void PutTimer(const CScriptTimer& t, PortSaveState::BlockWriter& w) {
    w.F32(t.x34_time);
    w.U8(t.x42_isTiming ? 1 : 0);
  }
  static void GetTimer(CScriptTimer& t, PortSaveState::BlockReader& r) {
    const float time = r.F32();
    const bool timing = r.U8() != 0;
    if (r.ok) {
      t.x34_time = time;
      t.x42_isTiming = timing;
      // Resumed timing starts at the saved time, not after a skipped tick.
#ifdef TARGET_PC
      t.mSkipNextTick = false;
#endif
    }
  }
  static void PutCounter(const CScriptCounter& c, PortSaveState::BlockWriter& w) {
    w.U32(static_cast< uint32_t >(c.x38_current));
  }
  static void GetCounter(CScriptCounter& c, PortSaveState::BlockReader& r) {
    const int current = static_cast< int >(r.U32());
    if (r.ok)
      c.x38_current = current;
  }
  static uint32_t WaypointId(const CStateManager& mgr, TUniqueId uid) {
    return uid == kInvalidUniqueId ? 0xFFFFFFFFu : mgr.GetEditorIdForUniqueId(uid).Value();
  }
  static void PutPlatform(const CStateManager& mgr, const CScriptPlatform& p,
                          PortSaveState::BlockWriter& w) {
    w.U32(WaypointId(mgr, p.x258_currentWaypoint));
    w.U32(WaypointId(mgr, p.x25a_targetWaypoint));
    w.F32(p.x25c_currentSpeed);
    w.F32(p.x260_moveDelay);
  }
  static void GetPlatform(CStateManager& mgr, CScriptPlatform& p, PortSaveState::BlockReader& r) {
    const uint32_t current = r.U32();
    const uint32_t target = r.U32();
    const float speed = r.F32();
    const float delay = r.F32();
    if (!r.ok)
      return;
    p.x258_currentWaypoint =
        current == 0xFFFFFFFFu ? kInvalidUniqueId : mgr.GetIdForScript(TEditorId(current));
    p.x25a_targetWaypoint =
        target == 0xFFFFFFFFu ? kInvalidUniqueId : mgr.GetIdForScript(TEditorId(target));
    p.x25c_currentSpeed = speed;
    p.x260_moveDelay = delay;
  }
};

namespace PortSaveState {
namespace {

constexpr uint32_t kNoEditorId = 0xFFFFFFFFu;

void PutTransform(BlockWriter& w, const CTransform4f& xf) {
  w.F32(xf.Get00()); w.F32(xf.Get01()); w.F32(xf.Get02()); w.F32(xf.Get03());
  w.F32(xf.Get10()); w.F32(xf.Get11()); w.F32(xf.Get12()); w.F32(xf.Get13());
  w.F32(xf.Get20()); w.F32(xf.Get21()); w.F32(xf.Get22()); w.F32(xf.Get23());
}

bool GetTransform(BlockReader& r, CTransform4f& out) {
  float m[12];
  for (float& f : m)
    f = r.F32();
  if (!r.ok)
    return false;
  for (float f : m)
    if (!(f == f) || f > 1e7f || f < -1e7f)
      return false;
  out = CTransform4f(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
  return true;
}

void PutVec(BlockWriter& w, const CVector3f& v) {
  w.F32(v.GetX());
  w.F32(v.GetY());
  w.F32(v.GetZ());
}

bool GetVec(BlockReader& r, CVector3f& out) {
  const float x = r.F32();
  const float y = r.F32();
  const float z = r.F32();
  if (!r.ok || !(x == x) || !(y == y) || !(z == z))
    return false;
  out = CVector3f(x, y, z);
  return true;
}

// Every live object of the world, one pass over the all-objects list.
template < typename F >
void ForEachObject(CStateManager& mgr, F&& fn) {
  CObjectList& list = mgr.ObjectListById(kOL_All);
  for (int idx = list.GetFirstObjectIndex(); idx != -1; idx = list.GetNextObjectIndex(idx)) {
    if (CEntity* ent = list[idx])
      fn(*ent);
  }
}

bool AreaReady(const CWorld& world, int area) {
  if (area < 0 || area >= world.GetNumAreas())
    return false;
  const CGameArea& a = world.GetAreaAlways(TAreaId(area));
  return a.IsLoaded() && a.IsValidated();
}

// The object's editor id when it is a scripted object of a captured-or-
// applied area (dynamic objects have none), else kNoEditorId.
uint32_t ScriptId(const CEntity& ent) {
  if (ent.IsInGraveyard() || ent.GetEditorId() == kInvalidEditorId)
    return kNoEditorId;
  return ent.GetEditorId().Value();
}

bool IsGone(const CEntity& ent) {
  const CPatterned* patterned = TCastToConstPtr< CPatterned >(const_cast< CEntity* >(&ent));
  return patterned != nullptr && !patterned->IsAlive();
}

void CaptureObject(CStateManager& mgr, CEntity& ent, RoomObject& obj) {
  obj.editorId = ent.GetEditorId().Value();
  obj.active = ent.GetActive();

  CActor* actor = TCastToPtr< CActor >(&ent);
  if (actor != nullptr) {
    if (const CHealthInfo* health = actor->GetHealthInfo(mgr)) {
      BlockWriter w;
      w.F32(health->GetHP());
      w.F32(health->GetKnockBackResistance());
      obj.blocks.push_back({kTagHealth, w.bytes});
    }
  }
  CPhysicsActor* physics = TCastToPtr< CPhysicsActor >(&ent);
  const bool isDoor = TCastToPtr< CScriptDoor >(&ent) != nullptr;
  if (physics != nullptr && !isDoor) {
    // Inert props keep the card load's pose: a snapshot of every script actor would
    // cost kilobytes (quick resume writes one every 30 s) for nothing. Enemies,
    // platforms and anything moving are saved.
    const bool moving = physics->GetVelocityWR().IsNonZero() ||
                        physics->GetMomentumWR().IsNonZero();
    if (moving || TCastToPtr< CPatterned >(&ent) != nullptr ||
        TCastToPtr< CScriptPlatform >(&ent) != nullptr) {
      BlockWriter xf;
      PutTransform(xf, physics->GetTransform());
      obj.blocks.push_back({kTagTransform, xf.bytes});
      BlockWriter motion;
      PutVec(motion, physics->GetVelocityWR());
      PutVec(motion, physics->GetMomentumWR());
      obj.blocks.push_back({kTagMotion, motion.bytes});
    }
  }
  if (CScriptTimer* timer = TCastToPtr< CScriptTimer >(&ent)) {
    BlockWriter w;
    PortRoomState::PutTimer(*timer, w);
    obj.blocks.push_back({kTagTimer, w.bytes});
  }
  if (CScriptCounter* counter = dynamic_cast< CScriptCounter* >(&ent)) {
    BlockWriter w;
    PortRoomState::PutCounter(*counter, w);
    obj.blocks.push_back({kTagCounter, w.bytes});
  }
  if (CScriptDoor* door = TCastToPtr< CScriptDoor >(&ent)) {
    BlockWriter w;
    w.U8(door->IsOpen() ? 1 : 0);
    obj.blocks.push_back({kTagDoor, w.bytes});
  }
  if (CScriptPlatform* platform = TCastToPtr< CScriptPlatform >(&ent)) {
    BlockWriter w;
    PortRoomState::PutPlatform(mgr, *platform, w);
    obj.blocks.push_back({kTagPlatform, w.bytes});
  }
}

void ApplyObject(CStateManager& mgr, CEntity& ent, const RoomObject& obj) {
  if (ent.GetActive() != obj.active)
    ent.SetActive(obj.active);

  CActor* actor = TCastToPtr< CActor >(&ent);
  if (actor != nullptr) {
    if (const RoomBlock* b = FindBlock(obj, kTagHealth)) {
      BlockReader r(b->data);
      const float hp = r.F32();
      const float resist = r.F32();
      if (r.ok && hp == hp) {
        if (CHealthInfo* health = actor->HealthInfo(mgr)) {
          health->SetHP(hp);
          health->SetKnockbackResistance(resist);
        }
      }
    }
  }
  if (CPhysicsActor* physics = TCastToPtr< CPhysicsActor >(&ent)) {
    if (const RoomBlock* b = FindBlock(obj, kTagTransform)) {
      BlockReader r(b->data);
      CTransform4f xf = CTransform4f::Identity();
      if (GetTransform(r, xf))
        physics->SetTransform(xf);
    }
    if (const RoomBlock* b = FindBlock(obj, kTagMotion)) {
      BlockReader r(b->data);
      CVector3f velocity;
      CVector3f momentum;
      if (GetVec(r, velocity) && GetVec(r, momentum)) {
        physics->SetVelocityWR(velocity);
        physics->SetMomentumWR(momentum);
      }
    }
  }
  if (CScriptTimer* timer = TCastToPtr< CScriptTimer >(&ent)) {
    if (const RoomBlock* b = FindBlock(obj, kTagTimer)) {
      BlockReader r(b->data);
      PortRoomState::GetTimer(*timer, r);
    }
  }
  if (CScriptCounter* counter = dynamic_cast< CScriptCounter* >(&ent)) {
    if (const RoomBlock* b = FindBlock(obj, kTagCounter)) {
      BlockReader r(b->data);
      PortRoomState::GetCounter(*counter, r);
    }
  }
  if (CScriptDoor* door = TCastToPtr< CScriptDoor >(&ent)) {
    if (const RoomBlock* b = FindBlock(obj, kTagDoor)) {
      BlockReader r(b->data);
      const bool open = r.U8() != 0;
      // No script message: Open and Close fire the door's connections again.
      if (r.ok && open != door->IsOpen())
        door->PortRestoreOpen(open);
    }
  }
  if (CScriptPlatform* platform = TCastToPtr< CScriptPlatform >(&ent)) {
    if (const RoomBlock* b = FindBlock(obj, kTagPlatform)) {
      BlockReader r(b->data);
      PortRoomState::GetPlatform(mgr, *platform, r);
    }
  }
}

} // namespace

void CaptureRoomState(CStateManager& mgr, RoomState& out) {
  out = {};
  const CWorld* world = mgr.GetWorld();
  if (world == nullptr)
    return;
  std::set< int > areas;
  for (int i = 0; i < world->GetNumAreas(); ++i) {
    if (AreaReady(*world, i)) {
      areas.insert(i);
      out.areas.push_back(i);
    }
  }
  ForEachObject(mgr, [&](CEntity& ent) {
    const uint32_t id = ScriptId(ent);
    if (id == kNoEditorId || areas.count(ent.GetEditorId().AreaNum()) == 0 || IsGone(ent))
      return;
    out.present.push_back(id);
    RoomObject obj;
    CaptureObject(mgr, ent, obj);
    out.objects.push_back(std::move(obj));
  });
}

int ApplyRoomState(CStateManager& mgr, const RoomState& room, std::vector< int32_t >& done,
                   RoomApplyStats& stats) {
  const CWorld* world = mgr.GetWorld();
  if (world == nullptr)
    return 0;
  std::set< int > now;
  int waiting = 0;
  for (int32_t area : room.areas) {
    if (std::find(done.begin(), done.end(), area) != done.end())
      continue;
    if (AreaReady(*world, area))
      now.insert(area);
    else
      ++waiting;
  }
  if (now.empty())
    return waiting;

  const std::set< uint32_t > present(room.present.begin(), room.present.end());
  std::vector< TUniqueId > doomed;
  ForEachObject(mgr, [&](CEntity& ent) {
    const uint32_t id = ScriptId(ent);
    if (id == kNoEditorId || now.count(ent.GetEditorId().AreaNum()) == 0)
      return;
    if (present.count(id) == 0 || IsGone(ent))
      doomed.push_back(ent.GetUniqueId());
  });
  for (TUniqueId uid : doomed) {
    mgr.DeleteObjectRequest(uid);
    ++stats.removed;
  }
  for (const RoomObject& obj : room.objects) {
    if (now.count(TEditorId(obj.editorId).AreaNum()) == 0)
      continue;
    CEntity* ent = mgr.ObjectById(mgr.GetIdForScript(TEditorId(obj.editorId)));
    if (ent == nullptr || ent->IsInGraveyard()) {
      ++stats.unmatched;
      continue;
    }
    ApplyObject(mgr, *ent, obj);
    ++stats.applied;
  }
  done.insert(done.end(), now.begin(), now.end());
  return waiting;
}

} // namespace PortSaveState
