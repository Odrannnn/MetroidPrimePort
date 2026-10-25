#include "port_savestate.h"
#include "port_log.h"
#include "port_savestate_boss.h"
#include "port_room_effects.h"

#include "Kyoto/Audio/CStreamAudioManager.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CHealthInfo.hpp"
#include "MetroidPrime/CObjectList.hpp"
#include "MetroidPrime/CPhysicsActor.hpp"
#include "MetroidPrime/CScriptLayerManager.hpp"
#include "MetroidPrime/ScriptObjects/CScriptSpecialFunction.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Cameras/CCinematicCamera.hpp"
#include "MetroidPrime/Enemies/COmegaPirate.hpp"
#include "MetroidPrime/Enemies/CPatterned.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/Player/CWorldState.hpp"
#include "MetroidPrime/ScriptObjects/CScriptActorRotate.hpp"
#include "MetroidPrime/ScriptObjects/CScriptCounter.hpp"
#include "MetroidPrime/ScriptObjects/CScriptDebris.hpp"
#include "MetroidPrime/ScriptObjects/CScriptDock.hpp"
#include "MetroidPrime/ScriptObjects/CScriptDoor.hpp"
#include "MetroidPrime/ScriptObjects/CScriptEMPulse.hpp"
#include "MetroidPrime/ScriptObjects/CScriptHUDMemo.hpp"
#include "MetroidPrime/ScriptObjects/CScriptMazeNode.hpp"
#include "MetroidPrime/ScriptObjects/CScriptMemoryRelay.hpp"
#include "MetroidPrime/ScriptObjects/CScriptMidi.hpp"
#include "MetroidPrime/ScriptObjects/CScriptPickup.hpp"
#include "MetroidPrime/ScriptObjects/CScriptPlatform.hpp"
#include "MetroidPrime/ScriptObjects/CScriptSpiderBallWaypoint.hpp"
#include "MetroidPrime/ScriptObjects/CScriptStreamedMusic.hpp"
#include "MetroidPrime/ScriptObjects/CScriptTimer.hpp"
#include "MetroidPrime/TCastTo.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

// Block tags of a RoomObject. A reader skips tags it does not know.
enum RoomTag : uint32_t {
  kTagTransform = 1,  // 12 floats, the CActor transform
  kTagMotion = 2,     // velocity, momentum (CPhysicsActor)
  kTagHealth = 3,     // hp, knockback resistance
  // 4 was the AI state, forced without the enemy's own fields (a parasite put into
  // Patrol from Generate deleted itself). kTagFields now carries it with them.
  kTagTimer = 5,      // time, timing
  kTagCounter = 6,    // current
  kTagDoor = 7,       // open
  kTagPlatform = 8,   // current and target waypoint (editor ids), speed, move delay
  kTagGenerated = 9,  // empty: a script generator's copy of the enemy or pickup with this editor id
  kTagMessages = 10,  // the script messages the enemy was sent (EncodeScriptMsgs)
  kTagBoss = 11,      // a boss's fight state beyond its health (port_savestate_boss.h)
  kTagRotate = 12,    // a turn in progress: flags, time, then per actor editor id + start rotation
  kTagFields = 13,    // an enemy's own fields through the entity schema (port_savestate_fields.cpp)
  kTagDock = 14,      // the dock's reference, then per reference its load-other flag
  kTagCinematic = 15, // a running cinematic camera: elapsed time, passed view point, passed target
  kTagMusic = 16,     // music playing at the save: kind (kMusicMidi/kMusicStream), stream file
};

enum : uint8_t {
  kMusicMidi = 0,
  kMusicStream = 1,
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
  static bool TimerValue(CEntity& ent, float& time, bool& timing) {
    const CScriptTimer* t = TCastToConstPtr< CScriptTimer >(&ent);
    if (t == nullptr)
      return false;
    time = t->x34_time;
    timing = t->x42_isTiming;
    return true;
  }
  static bool CounterValue(CEntity& ent, int& current) {
    const CScriptCounter* c = dynamic_cast< const CScriptCounter* >(&ent);
    if (c == nullptr)
      return false;
    current = c->x38_current;
    return true;
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
  // Applies `move` (the platform's step from its loaded pose to its saved one) to
  // every slave, nested platforms' too, and keeps each slave's offset the way
  // CScriptActorRotate leaves it after a turn (orientation, translation from the
  // platform), so the next turn starts from the restored pose. Returns whether a
  // spider ball waypoint moved.
  static bool CarrySlaves(CStateManager& mgr, CScriptPlatform& p, const CTransform4f& move,
                          CScriptPlatform::TMovedList& moved) {
    bool waypoints = false;
    rstl::vector< SRiders >* lists[] = {&p.x328_slavesStatic, &p.x338_slavesDynamic};
    for (rstl::vector< SRiders >* list : lists) {
      for (rstl::vector< SRiders >::iterator it = list->begin(); it != list->end(); ++it) {
        CActor* act = TCastToPtr< CActor >(mgr.ObjectById(it->x0_uid));
        if (act == nullptr || CScriptPlatform::IsInMovedList(act->GetUniqueId(), moved))
          continue;
        moved.push_back(act->GetUniqueId().Value());
        act->SetTransform(move * act->GetTransform());
        CTransform4f offset = act->GetTransform();
        offset.SetTranslation(act->GetTranslation() - p.GetTranslation());
        it->x8_transform = offset;
        if (TCastToPtr< CScriptSpiderBallWaypoint >(act) != nullptr)
          waypoints = true;
        if (CScriptPlatform* child = TCastToPtr< CScriptPlatform >(act))
          waypoints |= CarrySlaves(mgr, *child, move, moved);
      }
    }
    return waypoints;
  }
  // Puts a platform at its saved pose and carries its slaves with it, turned as well
  // as moved (slaves keep only their pose from the room load, so a bare SetTransform
  // leaves the flame jets of a piston where the room loaded them, and Ore
  // Processing's spider tracks unturned on a turned column, issue #172). A slave
  // with a saved pose of its own gets it after this. Returns whether a spider ball
  // waypoint moved.
  static bool RestorePlatformPose(CStateManager& mgr, CScriptPlatform& p, const CTransform4f& xf) {
    const CTransform4f move = xf * p.GetTransform().GetInverse();
    p.SetTransform(xf);
    CScriptPlatform::TMovedList moved;
    moved.push_back(p.GetUniqueId().Value());
    const bool waypoints = CarrySlaves(mgr, p, move, moved);
    // A delta PreThink worked out from the pose before this one would move the
    // slaves again on the next Think.
    p.x270_dragDelta = CVector3f::Zero();
    return waypoints;
  }
  // A turn in progress (Ore Processing's column, issue #172): without it the rebuilt
  // ActorRotate is idle, the column stays where the save caught it, and the next turn
  // starts from there, so the tracks never line up again. Only a turn in progress is
  // written; a finished one is in the actors' poses.
  static bool PutRotate(const CStateManager& mgr, const CScriptActorRotate& a,
                        PortSaveState::BlockWriter& w) {
    if (!a.x58_24_updateRotation)
      return false;
    w.U8(a.x58_25_updateSpiderBallWaypoints ? 1 : 0);
    w.F32(a.x44_currentTime);
    w.U32(static_cast< uint32_t >(a.x48_actors.size()));
    for (const auto& actor : a.x48_actors) {
      w.U32(WaypointId(mgr, actor.first));
      const CTransform4f& xf = actor.second;
      const float m[9] = {xf.Get00(), xf.Get01(), xf.Get02(), xf.Get10(), xf.Get11(),
                          xf.Get12(), xf.Get20(), xf.Get21(), xf.Get22()};
      for (float f : m)
        w.F32(f);
    }
    return true;
  }
  static void GetRotate(CStateManager& mgr, CScriptActorRotate& a, PortSaveState::BlockReader& r) {
    const bool waypoints = r.U8() != 0;
    const float time = r.F32();
    const uint32_t count = r.U32();
    if (!r.ok || !(time >= 0.f) || count > 1024)
      return;
    rstl::vector< rstl::pair< TUniqueId, CTransform4f > > actors;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t id = r.U32();
      float m[9];
      for (float& f : m)
        f = r.F32();
      if (!r.ok)
        return;
      const TUniqueId uid = id == 0xFFFFFFFFu ? kInvalidUniqueId : mgr.GetIdForScript(TEditorId(id));
      if (uid != kInvalidUniqueId)
        actors.push_back(rstl::pair< TUniqueId, CTransform4f >(
            uid, CTransform4f(m[0], m[1], m[2], 0.f, m[3], m[4], m[5], 0.f, m[6], m[7], m[8], 0.f)));
    }
    if (actors.empty())
      return;
    a.x48_actors = actors;
    a.x44_currentTime = time < a.x40_maxTime ? time : a.x40_maxTime;
    a.x58_24_updateRotation = true;
    a.x58_25_updateSpiderBallWaypoints = waypoints;
    // Think puts a turned platform's riders at turn * offset, the offset from before
    // the turn; RestorePlatforms left the offsets of the pose the save caught.
    const float t = a.x40_maxTime > 0.f ? a.x44_currentTime / a.x40_maxTime : 1.f;
    const CTransform4f turn = CTransform4f::RotateX(CRelAngle::FromDegrees(t * a.x34_rotation.GetX())) *
                              CTransform4f::RotateY(CRelAngle::FromDegrees(t * a.x34_rotation.GetY())) *
                              CTransform4f::RotateZ(CRelAngle::FromDegrees(t * a.x34_rotation.GetZ()));
    const CTransform4f undo = turn.GetInverse();
    CScriptPlatform::TMovedList seen;
    for (const auto& actor : a.x48_actors)
      if (CScriptPlatform* p = TCastToPtr< CScriptPlatform >(mgr.ObjectById(actor.first)))
        UnturnRiders(mgr, *p, undo, seen);
  }
  static void UnturnRiders(CStateManager& mgr, CScriptPlatform& p, const CTransform4f& undo,
                           CScriptPlatform::TMovedList& seen) {
    if (CScriptPlatform::IsInMovedList(p.GetUniqueId(), seen))
      return;
    seen.push_back(p.GetUniqueId().Value());
    rstl::vector< SRiders >* lists[] = {&p.x328_slavesStatic, &p.x338_slavesDynamic};
    for (rstl::vector< SRiders >* list : lists) {
      for (rstl::vector< SRiders >::iterator it = list->begin(); it != list->end(); ++it) {
        it->x8_transform = undo * it->x8_transform;
        if (CScriptPlatform* child = TCastToPtr< CScriptPlatform >(mgr.ObjectById(it->x0_uid)))
          UnturnRiders(mgr, *child, undo, seen);
      }
    }
  }
  // What CScriptActorRotate does after turning a waypoint: every waypoint's path and
  // touch bounds are rebuilt from the poses now in place.
  static void RebuildSpiderBallWaypoints(CStateManager& mgr) {
    std::vector< CScriptSpiderBallWaypoint* > waypoints;
    CObjectList& list = mgr.ObjectListById(kOL_All);
    for (int i = list.GetFirstObjectIndex(); i != -1; i = list.GetNextObjectIndex(i))
      if (CScriptSpiderBallWaypoint* wp = TCastToPtr< CScriptSpiderBallWaypoint >(list[i]))
        waypoints.push_back(wp);
    for (CScriptSpiderBallWaypoint* wp : waypoints)
      wp->ClearWaypoints();
    for (CScriptSpiderBallWaypoint* wp : waypoints) {
      wp->BuildWaypointListAndBounds(mgr);
      wp->SetTransformDirty(true);
    }
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

// The room script data a loaded area runs now: the patched copy when a
// cutscene-skip patch swapped one in, or null/0 when there is nothing to read.
void LoadedScriptData(const CWorld& world, int area, const uint8_t*& data, size_t& size) {
  const CGameArea& a = world.GetAreaAlways(TAreaId(area));
  const int n = a.GetScriptingSize();
  const char* bytes = a.GetScriptingData();
  if (n <= 0 || bytes == nullptr) {
    data = nullptr;
    size = 0;
    return;
  }
  data = reinterpret_cast< const uint8_t* >(bytes);
  size = static_cast< size_t >(n);
}

// The object's editor id when it is a scripted object of a captured-or-
// applied area (dynamic objects have none), else kNoEditorId. A mod's Remastered effect
// actors (port_room_effects.h) are in no layer buffer, so they are left to the area load too.
uint32_t ScriptId(const CEntity& ent) {
  if (ent.IsInGraveyard() || ent.GetEditorId() == kInvalidEditorId ||
      PortRoomEffects::IsAddOnId(ent.GetEditorId().value))
    return kNoEditorId;
  return ent.GetEditorId().Value();
}

// The area an object lives in, which owns its record. Usually its editor id's area, but
// not always (Subchamber Two holds a relay, 000600B5, with Subchamber One's area bits):
// keyed by the id, the restore of the area it is in would neither save nor delete it.
int OwnerArea(const CEntity& ent) {
  const int area = ent.GetAreaId().Value();
  return area >= 0 ? area : ent.GetEditorId().AreaNum();
}

// What the CStateManager hooks noted per object (raw TUniqueId value), tagged with
// the manager it belongs to so a world being torn down never touches the next one's.
struct Noted {
  const CStateManager* mgr = nullptr;
  bool generated = false;
  std::vector< ScriptMsgEntry > msgs;
};
std::map< uint16_t, Noted > sNoted;

const Noted* FindNoted(const CStateManager& mgr, TUniqueId uid) {
  const auto it = sNoted.find(uid.value);
  return it != sNoted.end() && it->second.mgr == &mgr ? &it->second : nullptr;
}

Noted& NoteFor(const CStateManager& mgr, TUniqueId uid) {
  Noted& n = sNoted[uid.value];
  if (n.mgr != &mgr) {
    n = Noted();
    n.mgr = &mgr;
  }
  return n;
}

TUniqueId RawId(uint16_t value) {
  TUniqueId uid;
  uid.value = value;
  return uid;
}

// A dead enemy that is still active plays out its death and deletes itself. One killed
// while inactive never does: it stays, and may be a script generator's template (Hive
// Totem's wasps, each one a generator's), so it is saved like any other object.
bool IsGone(const CEntity& ent) {
  const CPatterned* patterned = TCastToConstPtr< CPatterned >(const_cast< CEntity* >(&ent));
  return patterned != nullptr && !patterned->IsAlive() && patterned->GetActive();
}

// A script generator's copy carries its template's editor id. Enemy copies are saved
// as copies (fights count their deaths), and so are pickups (the Essence's drops) and
// debris (SavesGeneratedCopy); other copies (maze walls, sounds) are not saved at all,
// but no longer overwrite their template's record.
bool IsGeneratedCopy(const CStateManager& mgr, CEntity& ent) {
  const Noted* n = FindNoted(mgr, ent.GetUniqueId());
  return n != nullptr && n->generated;
}

void CaptureObject(CStateManager& mgr, CEntity& ent, RoomObject& obj) {
  obj.editorId = ent.GetEditorId().Value();
  obj.active = ent.GetActive();
  if (const Noted* n = FindNoted(mgr, ent.GetUniqueId())) {
    if (n->generated)
      obj.blocks.push_back({kTagGenerated, std::string()});
    if (!n->msgs.empty())
      obj.blocks.push_back({kTagMessages, EncodeScriptMsgs(n->msgs)});
  }

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
    // A generated pickup or debris stands where its generator dropped it, not at its template.
    const bool dropped = IsGeneratedCopy(mgr, ent);
    if (moving || dropped || TCastToPtr< CPatterned >(&ent) != nullptr ||
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
  if (CScriptDock* dock = TCastToPtr< CScriptDock >(&ent)) {
    const CWorld* world = mgr.GetWorld();
    const int area = dock->GetAreaId().Value();
    if (area >= 0 && area < world->GetNumAreas() &&
        dock->GetDockId() < world->GetAreaAlways(TAreaId(area)).GetDockCount()) {
      const IGameArea::Dock& d = world->GetAreaAlways(TAreaId(area)).GetDock(dock->GetDockId());
      const int refs = static_cast< int >(d.GetDockRefs().size());
      BlockWriter w;
      w.U8(static_cast< uint8_t >(d.GetReferenceCount()));
      w.U8(static_cast< uint8_t >(refs));
      for (int i = 0; i < refs; ++i)
        w.U8(d.GetShouldLoadOther(i) ? 1 : 0);
      obj.blocks.push_back({kTagDock, w.bytes});
    }
  }
  if (CCinematicCamera* cine = TCastToPtr< CCinematicCamera >(&ent)) {
    if (cine->GetActive()) {
      BlockWriter w;
      w.F32(cine->PortElapsed());
      w.U32(static_cast< uint32_t >(cine->PortPassedViewPoint()));
      w.U32(static_cast< uint32_t >(cine->PortPassedTarget()));
      obj.blocks.push_back({kTagCinematic, w.bytes});
    }
  }
  if (CScriptDoor* door = TCastToPtr< CScriptDoor >(&ent)) {
    DoorSaveState state;
    state.open = door->IsOpen();
    // A door that reports open has no close in flight, so its pending bits stay
    // clear ("open with a close in flight" is not a payload this build writes).
    if (!state.open) {
      state.pendingClose = door->PortClosingPending();
      state.refusedClosePending = door->PortRefusedClosePending();
    } else {
      state.partner = static_cast< DoorSaveState::Partner >(door->PortPartnerSide());
    }
    state.doClose = door->PortDoClosePending();
    obj.blocks.push_back({kTagDoor, EncodeDoorSaveState(state)});
  }
  if (CScriptPlatform* platform = TCastToPtr< CScriptPlatform >(&ent)) {
    BlockWriter w;
    PortRoomState::PutPlatform(mgr, *platform, w);
    obj.blocks.push_back({kTagPlatform, w.bytes});
  }
  if (CScriptActorRotate* rotate = dynamic_cast< CScriptActorRotate* >(&ent)) {
    BlockWriter w;
    if (PortRoomState::PutRotate(mgr, *rotate, w))
      obj.blocks.push_back({kTagRotate, w.bytes});
  }
  // The music actually playing, which the load restarts: the rebuilt objects never get
  // the Play their trigger sent (CScriptMidi's destructor stopped its song).
  if (CScriptMidi* midi = dynamic_cast< CScriptMidi* >(&ent)) {
    if (midi->PortPlaying()) {
      BlockWriter w;
      w.U8(kMusicMidi);
      obj.blocks.push_back({kTagMusic, w.bytes});
    }
  }
  if (CScriptStreamedMusic* music = dynamic_cast< CScriptStreamedMusic* >(&ent)) {
    if (music->PortPlayingLoop()) {
      BlockWriter w;
      w.U8(kMusicStream);
      w.Str(std::string(music->PortFileName().data()));
      obj.blocks.push_back({kTagMusic, w.bytes});
    }
  }
  std::string boss;
  if (CaptureBossState(mgr, ent, boss))
    obj.blocks.push_back({kTagBoss, boss});
  std::string fields;
  if (CaptureEntityFields(mgr, ent, fields))
    obj.blocks.push_back({kTagFields, fields});
}

// Delivers `msg` with the entity's links cleared: the handler's own sends (kSS_Active
// and the like) are dropped, since their targets are restored from the save themselves.
void DeliverUnlinked(CStateManager& mgr, CEntity& ent, TUniqueId sender, EScriptObjectMessage msg) {
  const rstl::vector< SConnection > conns = ent.ConnectionList();
  ent.ConnectionList().clear();
  mgr.DeliverScriptMsg(&ent, sender, msg);
  ent.ConnectionList() = conns;
}

// Many classes do their setup in the kSM_Activate/kSM_Deactivate handler (Flaahgra builds
// its body controller there, bosses and turrets switch their collision actors, drones their
// lights), and the script that sent it never runs again after a state load, so a bare
// SetActive leaves them half set up (a mid-fight Flaahgra crashed in Think on a null body
// controller). A cinematic camera's handler starts the cinematic, so it only gets the flag.
// Same for handlers with one-shot side effects: memory relays (mailbox), HUD memos, streamed
// music/MIDI stop, EMP pulses (static interference) and maze nodes (barrier delete).
void RestoreActive(CStateManager& mgr, CEntity& ent, bool active) {
  if (ent.GetActive() == active)
    return;
  if (dynamic_cast< CCinematicCamera* >(&ent) != nullptr ||
      dynamic_cast< CScriptMemoryRelay* >(&ent) != nullptr ||
      dynamic_cast< CScriptHUDMemo* >(&ent) != nullptr ||
      dynamic_cast< CScriptStreamedMusic* >(&ent) != nullptr ||
      dynamic_cast< CScriptMidi* >(&ent) != nullptr ||
      dynamic_cast< CScriptEMPulse* >(&ent) != nullptr ||
      dynamic_cast< CScriptMazeNode* >(&ent) != nullptr) {
    ent.SetActive(active);
    return;
  }
  DeliverUnlinked(mgr, ent, kInvalidUniqueId, active ? kSM_Activate : kSM_Deactivate);
  if (ent.GetActive() != active)
    ent.SetActive(active);
}

// Sends the enemy the script messages it had been sent before the save, in order (what
// woke a boss: a trigger's Action to Thardus), and keeps them for the next save.
// Senders are the objects of the rebuilt room with the saved editor ids.
// With `deliver` false (an enemy its kTagFields block restores) the journal is only kept.
int ReplayScriptMsgs(CStateManager& mgr, CEntity& ent, const RoomObject& obj, bool deliver) {
  const RoomBlock* b = FindBlock(obj, kTagMessages);
  std::vector< ScriptMsgEntry > journal;
  if (b == nullptr || !DecodeScriptMsgs(b->data, journal) || journal.empty())
    return 0;
  const TUniqueId uid = ent.GetUniqueId();
  int replayed = 0;
  for (const ScriptMsgEntry& e : journal) {
    if (!deliver)
      break;
    const TUniqueId sender =
        e.sender == kNoEditorId ? kInvalidUniqueId : mgr.GetIdForScript(TEditorId(e.sender));
    DeliverUnlinked(mgr, ent, sender, static_cast< EScriptObjectMessage >(e.msg));
    ++replayed;
    // A handler may delete the object (an enemy told to go away).
    if (mgr.ObjectById(uid) != &ent || ent.IsInGraveyard())
      break;
  }
  NoteFor(mgr, uid).msgs = journal;
  return replayed;
}

// Plays the music a kTagMusic block says was playing at the save. A stream still
// playing that file (a load in the same world keeps it through the quit) goes on as it
// is. Returns whether the music is playing.
bool RestoreMusic(CStateManager& mgr, CEntity& ent, const RoomBlock& b) {
  if (!ent.GetActive())
    return false;
  BlockReader r(b.data);
  const uint8_t kind = r.U8();
  if (CScriptMidi* midi = dynamic_cast< CScriptMidi* >(&ent)) {
    if (!r.ok || kind != kMusicMidi)
      return false;
    if (!midi->PortPlaying())
      DeliverUnlinked(mgr, ent, kInvalidUniqueId, kSM_Play);
    PortLog::Write("[savestate] music: MIDI %s\n", ent.GetDebugName().data());
    return midi->PortPlaying();
  }
  if (dynamic_cast< CScriptStreamedMusic* >(&ent) != nullptr) {
    const std::string file = r.Str();
    if (!r.ok || kind != kMusicStream)
      return false;
    const bool playing =
        CStreamAudioManager::PortIsPlaying(CStreamAudioManager::kSC_Default, file.c_str());
    if (!playing)
      DeliverUnlinked(mgr, ent, kInvalidUniqueId, kSM_Play);
    PortLog::Write("[savestate] music: stream %s %s (%s)\n", ent.GetDebugName().data(),
                   file.c_str(), playing ? "plays on" : "started");
    return true;
  }
  return false;
}

void ApplyObject(CStateManager& mgr, CEntity& ent, const RoomObject& obj, RoomApplyStats& stats) {
  RestoreActive(mgr, ent, obj.active);
  // A cinematic camera saved mid-shot picks the shot up where it was (RestoreActive only
  // set its flag: its Activate handler would restart it). Saves without the block keep that.
  if (CCinematicCamera* cine = TCastToPtr< CCinematicCamera >(&ent)) {
    const RoomBlock* b = FindBlock(obj, kTagCinematic);
    if (b != nullptr && obj.active && cine->GetActive()) {
      BlockReader r(b->data);
      const float t = r.F32();
      const int viewPoint = static_cast< int >(r.U32());
      const int target = static_cast< int >(r.U32());
      cine->PortResumeCinematic(t, viewPoint, target, mgr);
    }
  }
  // An enemy whose fields block covers it gets its state (AI, body, animation, boss phase)
  // from that block, on top of the Activate above: replaying its messages and the per-boss
  // restore would only be undone (2026-10-23 sweep, 19 enemy/boss scenarios). Saves without
  // the block, or with a class this build changed, still go the old way.
  const RoomBlock* fields = FindBlock(obj, kTagFields);
  const bool generic = fields != nullptr && EntityFieldsCover(ent, fields->data);
  // Before the saved health and pose, which then win over whatever a message reset.
  stats.replayed += ReplayScriptMsgs(mgr, ent, obj, !generic);
  if (ent.IsInGraveyard())
    return;

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
      if (GetTransform(r, xf)) {
        physics->SetTransform(xf);
        // An enemy's path destination is where it stands, not the room origin (the
        // rebuilt object's zero). AI states only move it off that when the player is
        // out of range: Omega Pirate restored inside its attack range searched a path
        // to the origin, failed, and a shagged path keeps it from turning or attacking.
        if (CPatterned* patterned = TCastToPtr< CPatterned >(&ent))
          patterned->SetDestPos(xf.GetTranslation());
      }
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
  // Add-on effects and barrier progress are not serialized. This also clears a completed
  // group's suppression when restoring an earlier point in an already resident area.
  if (CScriptSpecialFunction* special = TCastToPtr< CScriptSpecialFunction >(&ent))
    special->PortInvalidateBarrierForRestore(mgr);
  // Omega Pirate keeps its health in a collision actor and needs its intro's Reset later.
  if (COmegaPirate* omega = dynamic_cast< COmegaPirate* >(&ent))
    omega->PortRestoredFromSave(mgr);
  // After health and pose: a boss's phase is worked out from them when the save has no block.
  if (!generic) {
    const RoomBlock* boss = FindBlock(obj, kTagBoss);
    RestoreBossState(mgr, ent, boss != nullptr ? &boss->data : nullptr);
  }
  // Last of an enemy's blocks: the saved values win over what the replayed messages and
  // the boss restore set up.
  if (fields != nullptr)
    stats.fields += ApplyEntityFields(mgr, ent, fields->data);
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
      DoorSaveState want;
      // No script message here: a door that still owes Closed sends it from Think's
      // own branch for the kind of close it was in (once the rest of the saved room
      // state has been applied). A payload this build did not write is ignored.
      if (DecodeDoorSaveState(b->data, want)) {
        CScriptDoor::PortDoorFlags flags;
        flags.open = want.open;
        flags.normalClosePending = want.pendingClose;
        flags.refusedClosePending = want.refusedClosePending;
        flags.doClose = want.doClose;
        flags.partner = want.partner;
        // Applied even when the door already reads closed: that is what restores
        // the message an interrupted close still owes, and the block on opening
        // again a close in flight carries.
        if (flags.normalClosePending || flags.refusedClosePending || flags.doClose ||
            flags.open != door->IsOpen())
          door->PortRestoreDoorState(flags);
      }
    }
  }
  if (CScriptPlatform* platform = TCastToPtr< CScriptPlatform >(&ent)) {
    if (const RoomBlock* b = FindBlock(obj, kTagPlatform)) {
      BlockReader r(b->data);
      PortRoomState::GetPlatform(mgr, *platform, r);
    }
  }
  if (CScriptActorRotate* rotate = dynamic_cast< CScriptActorRotate* >(&ent)) {
    if (const RoomBlock* b = FindBlock(obj, kTagRotate)) {
      BlockReader r(b->data);
      PortRoomState::GetRotate(mgr, *rotate, r);
    }
  }
  if (const RoomBlock* b = FindBlock(obj, kTagMusic)) {
    if (RestoreMusic(mgr, ent, *b))
      ++stats.music;
  }
}

// The saved platform poses of the areas applied now, each carrying its slaves,
// parents ahead of the platforms they carry. ApplyObject then sets the same pose
// again (a no-op) and the absolute pose of any saved slave. Returns whether a
// spider ball waypoint moved.
bool RestorePlatforms(CStateManager& mgr, const RoomState& room, const std::set< int >& now) {
  struct Saved {
    CScriptPlatform* platform;
    CTransform4f xf;
  };
  std::vector< Saved > saved;
  for (const RoomObject& obj : room.objects) {
    if (now.count(TEditorId(obj.editorId).AreaNum()) == 0)
      continue;
    const RoomBlock* b = FindBlock(obj, kTagTransform);
    if (b == nullptr)
      continue;
    CEntity* ent = mgr.ObjectById(mgr.GetIdForScript(TEditorId(obj.editorId)));
    CScriptPlatform* platform = ent != nullptr ? TCastToPtr< CScriptPlatform >(ent) : nullptr;
    if (platform == nullptr || platform->IsInGraveyard())
      continue;
    BlockReader r(b->data);
    Saved s = {platform, CTransform4f::Identity()};
    if (GetTransform(r, s.xf))
      saved.push_back(s);
  }
  std::vector< std::vector< size_t > > parents(saved.size());
  for (size_t child = 0; child < saved.size(); ++child) {
    for (size_t parent = 0; parent < saved.size(); ++parent) {
      if (parent != child && saved[parent].platform->IsSlave(saved[child].platform->GetUniqueId()))
        parents[child].push_back(parent);
    }
  }
  std::vector< size_t > order;
  if (!PlatformRestoreOrder(parents, order)) {
    // Each platform still gets its saved pose from ApplyObject, without its slaves.
    PortLog::Write("[savestate] platforms carry each other in a cycle; their slaves stay as loaded\n");
    return false;
  }
  bool waypoints = false;
  for (size_t i : order)
    waypoints |= PortRoomState::RestorePlatformPose(mgr, *saved[i].platform, saved[i].xf);
  return waypoints;
}


// Spawn layers (issue #161). The layer bits each area's script objects were
// last spawned with, keyed by world and area, and the override ArmSpawnLayers
// installed for a state load.
std::map< std::pair< uint32_t, int >, uint64_t > sSpawnMasks;
struct SpawnOverride {
  uint32_t world = 0;
  int area = 0;
  uint64_t spawn = 0;  // bits the area spawns with
  uint64_t live = 0;   // bits to put back afterwards
};
std::vector< SpawnOverride > sOverrides;

uint32_t WorldKey(CStateManager& mgr) {
  const CWorld* world = mgr.GetWorld();
  return world != nullptr ? static_cast< uint32_t >(world->IGetWorldAssetId()) : 0;
}

uint64_t LayerMask(const CScriptLayerManager& layers, int area) {
  return layers.GetAreaLayers()[area].m_layerBits;
}

void SetLayerMask(CScriptLayerManager& layers, int area, uint64_t mask) {
  const int count = layers.GetAreaLayerCount(TAreaId(area));
  for (int l = 0; l < count; ++l)
    layers.SetLayerActive(TAreaId(area), TLayerId(l), ((mask >> l) & 1) != 0);
}

CScriptLayerManager* WorldLayers(uint32_t worldId, int area) {
  if (gpGameState == nullptr)
    return nullptr;
  CScriptLayerManager* layers = gpGameState->StateForWorld(static_cast< CAssetId >(worldId)).GetLayerState().GetPtr();
  if (layers == nullptr || area < 0 || area >= static_cast< int >(layers->GetAreaLayers().size()))
    return nullptr;
  return layers;
}

void RestoreOverride(const SpawnOverride& o) {
  if (CScriptLayerManager* layers = WorldLayers(o.world, o.area))
    SetLayerMask(*layers, o.area, o.live);
}

} // namespace

void NoteScriptMsg(CStateManager& mgr, uint16_t dest, uint16_t sender, int msg) {
  if (!JournalsScriptMsg(msg))
    return;
  CEntity* ent = mgr.ObjectById(RawId(dest));
  if (ent == nullptr || TCastToPtr< CPatterned >(ent) == nullptr || ScriptId(*ent) == kNoEditorId)
    return;
  ScriptMsgEntry e;
  e.msg = static_cast< uint8_t >(msg);
  const TUniqueId from = RawId(sender);
  if (from != kInvalidUniqueId && mgr.ObjectById(from) != nullptr) {
    const TEditorId eid = mgr.GetEditorIdForUniqueId(from);
    if (eid != kInvalidEditorId)
      e.sender = eid.Value();
  }
  AppendScriptMsg(NoteFor(mgr, ent->GetUniqueId()).msgs, e);
}

void NoteGenerated(CStateManager& mgr, uint16_t uid) { NoteFor(mgr, RawId(uid)).generated = true; }

void NoteRemoved(CStateManager& mgr, uint16_t uid) {
  const auto it = sNoted.find(uid);
  if (it != sNoted.end() && it->second.mgr == &mgr)
    sNoted.erase(it);
}

void NoteAreaSpawnBegin(CStateManager& mgr, int area) {
  const CScriptLayerManager* layers = mgr.WorldLayerState().GetPtr();
  if (layers == nullptr || area < 0 || area >= static_cast< int >(layers->GetAreaLayers().size()))
    return;
  sSpawnMasks[{WorldKey(mgr), area}] = LayerMask(*layers, area);
}

void NoteAreaSpawnEnd(CStateManager& mgr, int area) {
  const uint32_t world = WorldKey(mgr);
  for (size_t i = 0; i < sOverrides.size(); ++i) {
    if (sOverrides[i].world == world && sOverrides[i].area == area) {
      RestoreOverride(sOverrides[i]);
      sOverrides.erase(sOverrides.begin() + static_cast< ptrdiff_t >(i));
      return;
    }
  }
}

void ArmSpawnLayers(const RoomState& room, uint32_t worldId) {
  // The game state was just rebuilt: an override left from before belongs to the old one.
  sOverrides.clear();
  if (!room.spawnLayersRecorded)
    return;
  for (const AreaSpawnLayers& sl : room.spawnLayers) {
    if (!sl.known)
      continue;
    CScriptLayerManager* layers = WorldLayers(worldId, sl.area);
    if (layers == nullptr)
      continue;
    SpawnOverride o{worldId, sl.area, sl.mask, LayerMask(*layers, sl.area)};
    if (o.spawn == o.live)
      continue;
    SetLayerMask(*layers, sl.area, sl.mask);
    sOverrides.push_back(o);
  }
}

void DisarmSpawnLayers() {
  for (const SpawnOverride& o : sOverrides)
    RestoreOverride(o);
  sOverrides.clear();
}

void SetSpawnLayersLive(bool live) {
  for (const SpawnOverride& o : sOverrides) {
    if (CScriptLayerManager* layers = WorldLayers(o.world, o.area))
      SetLayerMask(*layers, o.area, live ? o.live : o.spawn);
  }
}

void CaptureRoomState(CStateManager& mgr, RoomState& out) {
  out = {};
  const CWorld* world = mgr.GetWorld();
  if (world == nullptr)
    return;
  // This writer records one fingerprint per captured area; an area whose
  // scripting data is not readable stays without one.
  out.fingerprintsRecorded = true;
  out.spawnLayersRecorded = true;
  std::set< int > areas;
  for (int i = 0; i < world->GetNumAreas(); ++i) {
    if (AreaReady(*world, i)) {
      areas.insert(i);
      out.areas.push_back(i);
      const uint8_t* data = nullptr;
      size_t size = 0;
      LoadedScriptData(*world, i, data, size);
      out.fingerprints.push_back(MakeAreaFingerprint(i, data, size));
      AreaSpawnLayers sl;
      sl.area = i;
      const auto spawned = sSpawnMasks.find({WorldKey(mgr), i});
      if (spawned != sSpawnMasks.end()) {
        sl.known = true;
        sl.mask = spawned->second;
      }
      out.spawnLayers.push_back(sl);
    }
  }
  ForEachObject(mgr, [&](CEntity& ent) {
    const uint32_t id = ScriptId(ent);
    if (id == kNoEditorId || areas.count(OwnerArea(ent)) == 0 || IsGone(ent))
      return;
    const bool generated = IsGeneratedCopy(mgr, ent);
    if (generated && !SavesGeneratedCopy(ent))
      return;
    // A copy's editor id is its template's: the template decides whether that id is present.
    if (!generated)
      out.present.push_back(id);
    RoomObject obj;
    CaptureObject(mgr, ent, obj);
    out.objects.push_back(std::move(obj));
  });
}

static void HoldRestoredTimers(CStateManager& mgr, const RoomState& room,
                               const std::vector< int32_t >& done) {
  for (const RoomObject& obj : room.objects) {
    const RoomBlock* b = FindBlock(obj, kTagTimer);
    if (b == nullptr ||
        std::find(done.begin(), done.end(), TEditorId(obj.editorId).AreaNum()) == done.end())
      continue;
    CEntity* ent = mgr.ObjectById(mgr.GetIdForScript(TEditorId(obj.editorId)));
    if (CScriptTimer* timer = ent != nullptr ? TCastToPtr< CScriptTimer >(ent) : nullptr) {
      BlockReader r(b->data);
      PortRoomState::GetTimer(*timer, r);
    }
  }
}

// A saved area the rebuilt world has not loaded. The game streams in a room's
// neighbours from the load-other flags of the docks (CStateManager::Update runs
// TravelToArea(LoadAdjacent) every tick), and those flags are runtime state a state load
// does not bring back: only the room's own area comes up. Raises the flag of the current
// room's dock that leads to the saved area, as the dock does when the door nearest it is
// approached, so a neighbour that was resident at the save streams in again (and gets
// its saved state when it does). False for an area no dock of the current room leads to
// (or that the world cannot stream: inactive).
static bool RequestResidentArea(CStateManager& mgr, int current, int area) {
  CWorld* world = mgr.World();
  if (world == nullptr || area < 0 || area >= world->GetNumAreas())
    return false;
  CGameArea* target = world->Area(TAreaId(area));
  if (!target->IsActive())
    return false;
  CGameArea* from = world->Area(TAreaId(current));
  for (int d = 0; d < from->GetDockCount(); ++d) {
    const IGameArea::Dock& dock = from->GetDock(d);
    const int refs = static_cast< int >(dock.GetDockRefs().size());
    for (int r = 0; r < refs; ++r) {
      if (dock.GetConnectedAreaId(r).Value() == area) {
        if (!dock.ShouldLoadOtherArea(r))
          from->DockNC(d).SetShouldLoadOther(r, true);
        return true;
      }
    }
  }
  return false;
}

// Which neighbours an area streams in is the load-other flags of its docks, and a
// rebuilt dock starts from its script property (load connected): every neighbour of the
// room comes up, also the ones a script had shut (the Impact Crater relays close the
// Subchambers' docks for the fight). Put back as saved, so the world streams what it
// streamed at the save. A flag to a neighbour that was resident at the save stays up:
// RequestResidentArea is still bringing that one back.
static void RestoreDocks(CStateManager& mgr, const RoomState& room, const std::set< int >& now) {
  CWorld* world = mgr.World();
  for (const RoomObject& obj : room.objects) {
    if (now.count(TEditorId(obj.editorId).AreaNum()) == 0)
      continue;
    const RoomBlock* b = FindBlock(obj, kTagDock);
    if (b == nullptr)
      continue;
    CEntity* ent = mgr.ObjectById(mgr.GetIdForScript(TEditorId(obj.editorId)));
    CScriptDock* dock = ent != nullptr ? TCastToPtr< CScriptDock >(ent) : nullptr;
    if (dock == nullptr)
      continue;
    const int area = dock->GetAreaId().Value();
    if (area < 0 || area >= world->GetNumAreas() ||
        dock->GetDockId() >= world->Area(TAreaId(area))->GetDockCount())
      continue;
    IGameArea::Dock& d = world->Area(TAreaId(area))->DockNC(dock->GetDockId());
    BlockReader r(b->data);
    const int ref = r.U8();
    const int refs = r.U8();
    if (!r.ok || refs != static_cast< int >(d.GetDockRefs().size()) || ref >= refs)
      continue;
    std::vector< bool > load(refs);
    for (int i = 0; i < refs; ++i)
      load[i] = r.U8() != 0;
    if (!r.ok)
      continue;
    if (ref != d.GetReferenceCount())
      dock->SetDockReference(mgr, ref);
    for (int i = 0; i < refs; ++i) {
      const int other = d.GetConnectedAreaId(i).Value();
      const bool resident =
          std::find(room.areas.begin(), room.areas.end(), other) != room.areas.end();
      d.SetShouldLoadOther(i, load[i] || resident);
    }
  }
}

bool RoomAreaAwaitsRestore(CStateManager& mgr, const RoomState& room,
                           const std::vector< int32_t >& done, int area) {
  const CWorld* world = mgr.GetWorld();
  if (world == nullptr || area < 0 || area >= world->GetNumAreas() ||
      std::find(room.areas.begin(), room.areas.end(), area) == room.areas.end() ||
      std::find(done.begin(), done.end(), area) != done.end() || !AreaReady(*world, area))
    return false;
  const uint8_t* data = nullptr;
  size_t size = 0;
  LoadedScriptData(*world, area, data, size);
  return DecideAreaRestore(room, area, data, size) == RoomAreaRestore::kRestore;
}

int ApplyRoomState(CStateManager& mgr, const RoomState& room, std::vector< int32_t >& done,
                   RoomApplyStats& stats) {
  const CWorld* world = mgr.GetWorld();
  if (world == nullptr)
    return 0;
  std::set< int > now;
  int waiting = 0;
  stats.late = 0;
  const int current = world->GetCurrentAreaId().Value();
  const bool currentReady = current >= 0 && current < world->GetNumAreas() && AreaReady(*world, current);
  for (int32_t area : room.areas) {
    if (std::find(done.begin(), done.end(), area) != done.end())
      continue;
    if (!AreaReady(*world, area)) {
      // The room Samus stands in is held for; the neighbours that were resident at the
      // save are streamed in and take their state whenever they finish.
      if (currentReady && area != current && RequestResidentArea(mgr, current, area))
        ++stats.late;
      else
        ++waiting;
      continue;
    }
    // A rebuilt area may have loaded a different room script than the snapshot
    // was taken from (a mod reload swaps the patched script under the load).
    // Its saved editor ids and states then describe other objects, so nothing
    // is applied to or deleted from that area. It still counts as handled: the
    // data will not change, so there is nothing to wait for.
    const uint8_t* data = nullptr;
    size_t size = 0;
    LoadedScriptData(*world, area, data, size);
    const RoomAreaRestore decision = DecideAreaRestore(room, area, data, size);
    if (decision != RoomAreaRestore::kRestore) {
      PortLog::Write("[savestate] area %d: %s; its room state is left as loaded\n",
                     static_cast< int >(area), RoomAreaRestoreReason(decision));
      ++stats.skipped;
      done.push_back(area);
      continue;
    }
    now.insert(area);
  }
  // Areas load on different ticks: a timer put back early would count down the
  // ticks the rest still wait for. Held at its saved value until the last area.
  if (!done.empty())
    HoldRestoredTimers(mgr, room, done);
  if (now.empty())
    return waiting;

  const std::set< uint32_t > present(room.present.begin(), room.present.end());
  std::vector< TUniqueId > doomed;
  ForEachObject(mgr, [&](CEntity& ent) {
    const uint32_t id = ScriptId(ent);
    if (id == kNoEditorId || now.count(OwnerArea(ent)) == 0)
      return;
    if (present.count(id) == 0 || IsGone(ent))
      doomed.push_back(ent.GetUniqueId());
  });
  for (TUniqueId uid : doomed) {
    mgr.DeleteObjectRequest(uid);
    ++stats.removed;
  }
  const bool waypointsMoved = RestorePlatforms(mgr, room, now);
  std::vector< const RoomObject* > copies;
  for (const RoomObject& obj : room.objects) {
    if (FindBlock(obj, kTagGenerated) != nullptr) {
      if (now.count(TEditorId(obj.editorId).AreaNum()) != 0)
        copies.push_back(&obj);
      continue;
    }
    CEntity* ent = mgr.ObjectById(mgr.GetIdForScript(TEditorId(obj.editorId)));
    if (ent == nullptr || ent->IsInGraveyard()) {
      if (now.count(TEditorId(obj.editorId).AreaNum()) != 0)
        ++stats.unmatched;
      continue;
    }
    if (now.count(OwnerArea(*ent)) == 0)
      continue;
    ApplyObject(mgr, *ent, obj, stats);
    ++stats.applied;
  }
  // The enemies and pickups a script generator made (Hive Totem's wasps, the Essence's
  // drops): the rebuilt room has only their templates, and a fight counting their
  // deaths would wait forever. Made again from the template the way CScriptGenerator
  // does, then given the copy's saved state (ApplyObject sends the Activate the
  // generator sent).
  for (const RoomObject* obj : copies) {
    const bool wasGenerating = mgr.IsGeneratingObject();
    mgr.SetIsGeneratingObject(true);
    const TUniqueId uid = mgr.GenerateObject(TEditorId(obj->editorId)).second;
    mgr.SetIsGeneratingObject(wasGenerating);
    CEntity* ent = uid != kInvalidUniqueId ? mgr.ObjectById(uid) : nullptr;
    if (ent == nullptr) {
      ++stats.unmatched;
      continue;
    }
    // Also in the fields block; set here for saves without one.
    if (CScriptPickup* pickup = TCastToPtr< CScriptPickup >(ent))
      pickup->SetWasGenerated();
    ApplyObject(mgr, *ent, *obj, stats);
    ++stats.generated;
  }
  if (waypointsMoved)
    PortRoomState::RebuildSpiderBallWaypoints(mgr);
  RestoreDocks(mgr, room, now);
  done.insert(done.end(), now.begin(), now.end());
  return waiting;
}

// Read-only views of state the room save keeps, for the statedump console command.
bool IsScriptGeneratedCopy(const CStateManager& mgr, CEntity& ent) { return IsGeneratedCopy(mgr, ent); }

bool RoomStateHasMusic(const RoomState& room) {
  for (const RoomObject& obj : room.objects) {
    if (FindBlock(obj, kTagMusic) != nullptr)
      return true;
  }
  return false;
}

// A sound copy is left out: made again it would play from its start a second time.
bool SavesGeneratedCopy(CEntity& ent) {
  return TCastToPtr< CPatterned >(&ent) != nullptr || TCastToPtr< CScriptPickup >(&ent) != nullptr ||
         dynamic_cast< CScriptDebris* >(&ent) != nullptr;
}

bool ScriptTimerState(CEntity& ent, float& time, bool& timing) {
  return PortRoomState::TimerValue(ent, time, timing);
}

bool ScriptCounterState(CEntity& ent, int& current) {
  return PortRoomState::CounterValue(ent, current);
}

} // namespace PortSaveState
