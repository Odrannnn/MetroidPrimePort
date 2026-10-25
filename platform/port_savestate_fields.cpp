#include "port_savestate.h"

#include "port_entity_schema.h"

#include "Kyoto/Animation/CAnimTreeNode.hpp"
#include "MetroidPrime/BodyState/CAdditiveBodyState.hpp"
#include "MetroidPrime/BodyState/CBSLocomotion.hpp"
#include "MetroidPrime/BodyState/CBodyController.hpp"
#include "MetroidPrime/BodyState/CBodyState.hpp"
#include "MetroidPrime/CAnimData.hpp"
#include "MetroidPrime/CAnimPlaybackParms.hpp"
#include "MetroidPrime/CEntity.hpp"
#include "MetroidPrime/CModelData.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Enemies/CPatterned.hpp"
#include "MetroidPrime/Enemies/CStateMachine.hpp"
#include "MetroidPrime/PathFinding/CPathFindSearch.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/TCastTo.hpp"

#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <vector>

// The kTagFields block: an enemy's own fields, read and written through the generated
// schema (port_entity_schema.h) instead of per-class code. Per class of the entity's chain,
// most derived first: its fingerprint, the payload length, then each saved field in table
// order. A reader skips a class whose fingerprint it does not have (the headers changed)
// and the whole block when the policy below differs, so an old save falls back to the
// restore the other tags do.
//
// Saved: the plain kinds (bool, int, float, bitfield, math), pod structs, and uids, which
// go by the editor id of the object they name (the player by a mark of its own). A uid
// naming an object without one (a collision actor or projectile the enemy made) is left
// as the rebuilt enemy has it.
// The state machine, body controller and animation go through the codecs below, each
// as a length and its payload. They are applied between two applications of the plain
// fields, in table order (the AI state, then the body state, then the animation), so
// that entering the saved states starts from the saved values and what it sets up is
// then overwritten by them. A path search is put back after the states, before the
// plain fields go in again.
// Not saved: pointers, smart pointers, containers, the kinds without a codec
// (docs/SAVESTATE_SCHEMA.md),
// CEntity's fields (the room record's own), most of CActor's and CPhysicsActor's (pose,
// motion and health are restored by their tags; the rest is render and collision
// bookkeeping the engine keeps), and the port's interpolation fields.

// Friend of CStateMachineState, CBodyController, CBodyStateInfo and CAnimData.
struct PortStateCodec {
  using BlockWriter = PortSaveState::BlockWriter;
  using BlockReader = PortSaveState::BlockReader;

  // State name and index (a machine can have two states of one name, with different
  // triggers), time, random, delay, fixed random, code trigger.
  static void PutStateMachine(const CStateMachineState& sm, BlockWriter& w) {
    w.Str(sm.x4_state != nullptr ? sm.x4_state->GetName() : "");
    int index = -1;
    if (sm.x4_state != nullptr && sm.x0_machine != nullptr) {
      const rstl::vector< CAiState >& states = sm.x0_machine->GetStateVector();
      for (int i = 0; i < static_cast< int >(states.size()) && index < 0; ++i)
        if (&states[i] == sm.x4_state)
          index = i;
    }
    w.U32(static_cast< uint32_t >(index));
    w.F32(sm.x8_time);
    w.F32(sm.xc_random);
    w.F32(sm.x10_delay);
    w.F32(sm.x14_fixedRandom);
    w.U8(sm.x18_24_codeTrigger ? 1 : 0);
  }

  // Enters the saved state through SetState (the current state's Exit, the saved one's
  // Start) when the enemy is in another, then puts back its time and randoms.
  static bool GetStateMachine(CStateManager& mgr, CAi& ai, CStateMachineState& sm, BlockReader& r) {
    const std::string name = r.Str();
    const int savedIdx = static_cast< int >(r.U32());
    const float time = r.F32();
    const float random = r.F32();
    const float delay = r.F32();
    const float fixedRandom = r.F32();
    const bool codeTrigger = r.U8() != 0;
    if (!r.ok || name.empty())
      return false;
    for (float v : {time, random, delay, fixedRandom})
      if (!(v == v) || v > 1e9f || v < -1e9f)
        return false;
    // The rebuilt enemy has not started its machine yet (its first Think does, at Start).
    if (sm.x0_machine == nullptr) {
      const CStateMachine* machine = ai.GetStateMachine();
      if (machine == nullptr)
        return false;
      sm.Setup(machine);
    }
    const rstl::vector< CAiState >& states = sm.x0_machine->GetStateVector();
    int idx = -1;
    if (savedIdx >= 0 && savedIdx < static_cast< int >(states.size()) && name == states[savedIdx].GetName())
      idx = savedIdx;
    for (int i = 0; i < static_cast< int >(states.size()) && idx < 0; ++i)
      if (name == states[i].GetName())
        idx = i;
    if (idx < 0)
      return false;
    if (sm.x4_state != &states[idx])
      sm.SetState(mgr, ai, idx);
    // Exit or Start may have moved the enemy into another state (or deleted it).
    if (sm.x4_state != &states[idx])
      return false;
    sm.x8_time = time;
    sm.xc_random = random;
    sm.x10_delay = delay;
    sm.x14_fixedRandom = fixedRandom;
    sm.x18_24_codeTrigger = codeTrigger;
    return true;
  }

  // Every command of the manager, in member order. A command keeps the values it was
  // last delivered with after the frame it was delivered on.
  template < class M, class F >
  static void ForEachCmd(M& m, F f) {
    f(m.xb8_getup), f(m.xc4_step), f(m.xd4_die), f(m.xdc_knockDown), f(m.xf4_knockBack);
    f(m.x10c_meleeAttack), f(m.x128_projectileAttack), f(m.x144_loopAttack);
    f(m.x154_loopReaction), f(m.x160_loopHitReaction), f(m.x16c_exitState);
    f(m.x174_leanFromCover), f(m.x17c_nextState), f(m.x184_maintainVelocity);
    f(m.x18c_generate), f(m.x1ac_hurled), f(m.x1d0_jump), f(m.x1f8_slide), f(m.x210_taunt);
    f(m.x21c_scripted), f(m.x230_cover), f(m.x254_wallHang), f(m.x260_locomotion);
    f(m.x268_additiveIdle), f(m.x270_additiveAim), f(m.x278_additiveFlinch);
    f(m.x284_additiveReaction), f(m.x298_stopReaction);
  }

  // A command's values: its bytes after the vtable pointer (all are plain values).
  template < class T >
  static constexpr size_t CmdSize() {
    return sizeof(T) - sizeof(void*);
  }
  template < class T >
  static const char* CmdBytes(const T& cmd) {
    return reinterpret_cast< const char* >(&cmd) + sizeof(void*);
  }

  static void PutBody(const CBodyController& bc, BlockWriter& w) {
    w.U32(static_cast< uint32_t >(bc.x2a4_bodyStateInfo.x14_state));
    w.U32(static_cast< uint32_t >(bc.x2a4_bodyStateInfo.x2c_additiveState));
    w.U32(static_cast< uint32_t >(bc.x2ec_locomotionType));
    w.U32(static_cast< uint32_t >(bc.x2f0_fallState));
    w.U32(static_cast< uint32_t >(bc.x2f8_curAnim));
    w.F32(bc.x2fc_turnSpeed);
    w.U8((bc.x300_24_animationOver ? 1 : 0) | (bc.x300_25_active ? 2 : 0) | (bc.x300_26_frozen ? 4 : 0) |
         (bc.x300_27_hasBeenFrozen ? 8 : 0) | (bc.x300_28_playDeathAnims ? 16 : 0));
    const float floats[] = {bc.x304_intoFreezeDur,     bc.x308_frozenDur,          bc.x30c_breakoutDur,
                            bc.x310_timeFrozen,        bc.x314_backedUpForce.GetX(), bc.x314_backedUpForce.GetY(),
                            bc.x314_backedUpForce.GetZ(), bc.x320_fireDur,          bc.x324_electrocutionDur,
                            bc.x328_timeOnFire,        bc.x32c_timeElectrocuting,  bc.x330_restrictedFlyerMoveSpeed,
                            bc.x2a4_bodyStateInfo.x30_maxPitch};
    for (float f : floats)
      w.F32(f);
    w.U8(bc.x2a4_bodyStateInfo.x34_24_changeLocoAtEndOfAnimOnly ? 1 : 0);
    BlockWriter cmds;
    int count = 0;
    ForEachCmd(bc.x4_cmdMgr, [&](const auto& cmd) {
      using T = std::remove_cvref_t< decltype(cmd) >;
      cmds.U32(static_cast< uint32_t >(CmdSize< T >()));
      cmds.bytes.append(CmdBytes(cmd), CmdSize< T >());
      ++count;
    });
    w.U8(static_cast< uint8_t >(count));
    w.bytes += cmds.bytes;
    // What the AI state machine delivered last tick and the next Update acts on (a flyer's
    // locomotion impulse, its animation's delta): the steering and the delivered mask.
    const CBodyStateCmdMgr& m = bc.x4_cmdMgr;
    for (const CVector3f* v : {&m.x0_move, &m.xc_face, &m.x18_target, &m.x24_additiveTarget})
      for (int i = 0; i < 3; ++i)
        w.F32((*v)[i]);
    w.U32(static_cast< uint32_t >(m.x30_steeringMode));
    w.F32(m.x34_steeringSpeedMin);
    w.F32(m.x38_steeringSpeedMax);
    w.F32(m.x3c_steeringSpeed);
    w.U32(m.xb4_deliveredCmdMask);
    // Which locomotion animation (idle/walk/run/strafe) the state is on and how long since it
    // last switched: Start resets both, and the next update then picks by speed alone.
    const CBodyStateInfo& info = bc.x2a4_bodyStateInfo;
    const CBodyState* cur =
        info.x0_stateMap.find(info.x14_state) != info.x0_stateMap.end() ? info.GetCurrentState() : nullptr;
    if (const CBSBiPedLocomotion* biped = dynamic_cast< const CBSBiPedLocomotion* >(cur)) {
      w.U8(1);
      w.U32(static_cast< uint32_t >(biped->x3c4_anim));
      w.F32(biped->x3c8_primeTime);
    } else if (const CBSRestrictedLocomotion* restricted = dynamic_cast< const CBSRestrictedLocomotion* >(cur)) {
      w.U8(2);
      w.U32(static_cast< uint32_t >(restricted->x44_anim));
      w.F32(0.f);
    } else {
      w.U8(0);
    }
  }

  // Moves the body into the saved state and additive state (the current one's Shutdown,
  // the saved one's Start, as UpdateBody does) and puts back the controller's values.
  // A state's Start reads the command that sent the body there, which is only there for
  // the frame it was delivered: the saved commands are put back first and Start sees
  // every command as delivered, so it makes the choices it made before the save (a
  // default command can pick animation -1, which SetAnimation indexes with). A state's
  // other members start over; its animation and phase are put back by the anim_data
  // codec after. Returns whether the controller's values were applied.
  static bool GetBody(CStateManager& mgr, CBodyController& bc, BlockReader& r) {
    const pas::EAnimationState state = static_cast< pas::EAnimationState >(r.U32());
    const pas::EAnimationState additive = static_cast< pas::EAnimationState >(r.U32());
    const pas::ELocomotionType loco = static_cast< pas::ELocomotionType >(r.U32());
    const pas::EFallState fall = static_cast< pas::EFallState >(r.U32());
    const int curAnim = static_cast< int >(r.U32());
    const float turnSpeed = r.F32();
    const uint8_t bits = r.U8();
    float f[13];
    for (float& v : f)
      v = r.F32();
    const bool changeLoco = r.U8() != 0;
    // The commands, all or none: a count or size this build does not have drops them.
    std::vector< std::string > saved(r.U8());
    for (std::string& cmd : saved) {
      const uint32_t size = r.U32();
      if (!r.ok || r.bytes.size() - r.pos < size) {
        r.ok = false;
        break;
      }
      cmd = r.bytes.substr(r.pos, size);
      r.pos += size;
    }
    float steer[12];
    for (float& v : steer)
      v = r.F32();
    const uint32_t steerMode = r.U32();
    const float steerParams[3] = {r.F32(), r.F32(), r.F32()};
    const uint32_t deliveredMask = r.U32();
    const uint8_t locoKind = r.U8();
    const uint32_t locoAnim = locoKind != 0 ? r.U32() : 0;
    const float primeTime = locoKind != 0 ? r.F32() : 0.f;
    if (!r.ok || !(primeTime == primeTime))
      return false;
    for (float v : f)
      if (!(v == v) || v > 1e9f || v < -1e9f)
        return false;
    for (float v : steer)
      if (!(v == v) || v > 1e9f || v < -1e9f)
        return false;
    size_t want = 0;
    bool fits = true;
    ForEachCmd(bc.x4_cmdMgr, [&](auto& cmd) {
      using T = std::remove_cvref_t< decltype(cmd) >;
      fits = fits && want < saved.size() && saved[want].size() == CmdSize< T >();
      ++want;
    });
    fits = fits && want == saved.size();
    if (fits) {
      size_t i = 0;
      ForEachCmd(bc.x4_cmdMgr, [&](auto& cmd) {
        using T = std::remove_cvref_t< decltype(cmd) >;
        std::memcpy(const_cast< char* >(CmdBytes(cmd)), saved[i++].data(), CmdSize< T >());
      });
    }
    CBodyStateInfo& info = bc.x2a4_bodyStateInfo;
    // The rebuilt enemy's controller is started by its first Think (into the default
    // state, which would then replace the saved one): start it here, as that Think would.
    // The default state may read a command the game delivers before that Think (Generate
    // does, and dereferences it): the saved commands stand in for it, as below.
    if (!bc.x300_25_active && (bits & 2) != 0) {
      CBodyStateCmdMgr& cmds = bc.x4_cmdMgr;
      const uint delivered = cmds.xb4_deliveredCmdMask;
      cmds.xb4_deliveredCmdMask = ~0u;
      bc.Activate(mgr);
      cmds.xb4_deliveredCmdMask = delivered;
    }
    if (bc.x300_25_active && (bits & 2) != 0) {
      if (fits && state != info.x14_state && info.x0_stateMap.find(state) != info.x0_stateMap.end()) {
        if (info.x0_stateMap.find(info.x14_state) != info.x0_stateMap.end())
          info.GetCurrentState()->Shutdown(bc);
        info.SetState(state);
        CBodyStateCmdMgr& cmds = bc.x4_cmdMgr;
        const uint delivered = cmds.xb4_deliveredCmdMask;
        cmds.xb4_deliveredCmdMask = ~0u;
        info.GetCurrentState()->Start(bc, mgr);
        cmds.xb4_deliveredCmdMask = delivered;
      }
      if (additive != info.x2c_additiveState && info.GetCurrentAdditiveState() != nullptr) {
        CAdditiveBodyState* now = info.GetCurrentAdditiveState();
        info.SetAdditiveState(additive);
        if (info.GetCurrentAdditiveState() != now) {
          now->Shutdown(bc);
          info.GetCurrentAdditiveState()->Start(bc, mgr);
        }
      }
    }
    if (fits) {
      CBodyStateCmdMgr& m = bc.x4_cmdMgr;
      m.x0_move = CVector3f(steer[0], steer[1], steer[2]);
      m.xc_face = CVector3f(steer[3], steer[4], steer[5]);
      m.x18_target = CVector3f(steer[6], steer[7], steer[8]);
      m.x24_additiveTarget = CVector3f(steer[9], steer[10], steer[11]);
      m.x30_steeringMode = static_cast< ESteeringBlendMode >(steerMode);
      m.x34_steeringSpeedMin = steerParams[0];
      m.x38_steeringSpeedMax = steerParams[1];
      m.x3c_steeringSpeed = steerParams[2];
      m.xb4_deliveredCmdMask = deliveredMask;
    }
    bc.x2ec_locomotionType = loco;
    bc.x2f0_fallState = fall;
    bc.x2f8_curAnim = curAnim;
    bc.x2fc_turnSpeed = turnSpeed;
    bc.x300_24_animationOver = (bits & 1) != 0;
    bc.x300_26_frozen = (bits & 4) != 0;
    bc.x300_27_hasBeenFrozen = (bits & 8) != 0;
    bc.x300_28_playDeathAnims = (bits & 16) != 0;
    bc.x304_intoFreezeDur = f[0];
    bc.x308_frozenDur = f[1];
    bc.x30c_breakoutDur = f[2];
    bc.x310_timeFrozen = f[3];
    bc.x314_backedUpForce = CVector3f(f[4], f[5], f[6]);
    bc.x320_fireDur = f[7];
    bc.x324_electrocutionDur = f[8];
    bc.x328_timeOnFire = f[9];
    bc.x32c_timeElectrocuting = f[10];
    bc.x330_restrictedFlyerMoveSpeed = f[11];
    info.x30_maxPitch = f[12];
    info.x34_24_changeLocoAtEndOfAnimOnly = changeLoco;
    // After the Start above, which reset them.
    if (info.x14_state == state && info.x0_stateMap.find(state) != info.x0_stateMap.end() &&
        (static_cast< int >(locoAnim) == pas::kLA_Invalid || locoAnim <= static_cast< uint32_t >(pas::kLA_StrafeDown))) {
      const pas::ELocomotionAnim anim = static_cast< pas::ELocomotionAnim >(static_cast< int >(locoAnim));
      CBodyState* cur = info.GetCurrentState();
      if (CBSBiPedLocomotion* biped = dynamic_cast< CBSBiPedLocomotion* >(cur); biped && locoKind == 1) {
        biped->x3c4_anim = anim;
        biped->x3c8_primeTime = primeTime;
      } else if (CBSRestrictedLocomotion* restricted = dynamic_cast< CBSRestrictedLocomotion* >(cur);
                 restricted && locoKind == 2) {
        restricted->x44_anim = anim;
      }
    }
    return true;
  }

  // Animation id, loop and animating, speed, and the phase of the root animation
  // (-1 without one). An animation blend in progress comes back as its target alone.
  static void PutAnim(const CAnimData& ad, BlockWriter& w) {
    w.U32(static_cast< uint32_t >(static_cast< int >(ad.x208_currentAnim)));
    w.U8((ad.x220_25_loop ? 1 : 0) | (ad.x220_24_animating ? 2 : 0));
    w.F32(ad.x200_speedScale);
    float phase = -1.f;
    if (ad.x1f8_animRoot.GetPtr() != nullptr) {
      const float duration = ad.x1f8_animRoot->VGetSteadyStateAnimInfo().GetDuration().GetSeconds();
      const float remaining = ad.x1f8_animRoot->VGetTimeRemaining().GetSeconds();
      if (duration > 0.f)
        phase = rstl::min_val(rstl::max_val(1.f - remaining / duration, 0.f), 1.f);
    }
    w.F32(phase);
  }

  // The path being followed: its waypoints, the current one and the search result.
  static void PutPath(const CPathFindSearch& ps, BlockWriter& w) {
    w.U8(static_cast< uint8_t >(ps.x4_waypoints.size()));
    for (const CVector3f& p : ps.x4_waypoints) {
      w.F32(p.GetX());
      w.F32(p.GetY());
      w.F32(p.GetZ());
    }
    w.U32(static_cast< uint32_t >(ps.xc8_curWaypoint));
    w.U32(static_cast< uint32_t >(ps.xcc_result));
  }

  // Applied after the AI state, whose Activate may have searched a new path.
  static bool GetPath(CPathFindSearch& ps, BlockReader& r) {
    const int count = r.U8();
    if (count > static_cast< int >(ps.x4_waypoints.capacity()))
      return false;
    CVector3f points[16];
    for (int i = 0; i < count; ++i) {
      const float x = r.F32(), y = r.F32(), z = r.F32();
      for (float v : {x, y, z})
        if (!(v == v) || v > 1e9f || v < -1e9f)
          return false;
      points[i] = CVector3f(x, y, z);
    }
    const int cur = static_cast< int >(r.U32());
    const uint32_t result = r.U32();
    if (!r.ok || cur < 0 || (count > 0 && cur >= count) || result > CPathFindSearch::kR_NoPath)
      return false;
    ps.x4_waypoints.clear();
    for (int i = 0; i < count; ++i)
      ps.x4_waypoints.push_back(points[i]);
    ps.xc8_curWaypoint = cur;
    ps.xcc_result = static_cast< CPathFindSearch::EResult >(result);
    return true;
  }

  static bool GetAnim(CAnimData& ad, BlockReader& r) {
    const int anim = static_cast< int >(r.U32());
    const uint8_t bits = r.U8();
    const float speed = r.F32();
    const float phase = r.F32();
    if (!r.ok || !(speed == speed) || !(phase == phase))
      return false;
    if (anim >= 0 && anim != ad.x208_currentAnim)
      ad.SetAnimation(CAnimPlaybackParms(anim, -1, 1.f, (bits & 2) != 0), false);
    ad.x220_25_loop = (bits & 1) != 0;
    ad.x220_24_animating = (bits & 2) != 0;
    ad.x200_speedScale = speed;
    if (phase >= 0.f && phase <= 1.f && ad.x1f8_animRoot.GetPtr() != nullptr)
      ad.SetPhase(phase);
    return anim < 0 || ad.x208_currentAnim == anim;
  }
};

namespace PortSaveState {
namespace {

// Bump when what SavedField picks or how a kind is written changes.
constexpr uint32_t kFieldPolicy = 7;

constexpr uint32_t kUidNone = 0xFFFFFFFFu;    // kInvalidUniqueId
constexpr uint32_t kUidDynamic = 0xFFFFFFFEu; // an object without an editor id: left alone
constexpr uint32_t kUidPlayer = 0xFFFFFFFDu;  // the player (no editor id, same uid every load)

bool Named(const char* name, std::initializer_list< const char* > names) {
  for (const char* n : names)
    if (std::strcmp(name, n) == 0)
      return true;
  return false;
}

bool SavedField(const PortSchemaClass& cls, const PortSchemaField& f) {
  if ((f.flags & kPortSchemaConst) != 0)
    return false;
  if (std::strncmp(f.name, "xPort", 5) == 0 || std::strncmp(f.name, "xpc_", 4) == 0)
    return false;
  switch (f.kind) {
  case PortSchemaKind::bool_:
  case PortSchemaKind::int_:
  case PortSchemaKind::float_:
  case PortSchemaKind::bitfield:
  case PortSchemaKind::math:
  case PortSchemaKind::uid:
  case PortSchemaKind::pod:
  case PortSchemaKind::state_machine:
  case PortSchemaKind::body_controller:
  case PortSchemaKind::anim_data:
    break;
  default:
    return false;
  }
  if (f.kind != PortSchemaKind::bitfield && f.addr == nullptr)
    return false;
  if (std::strcmp(cls.name, "CEntity") == 0)
    return false;
  // Asset streaming is this run's, not the game's: Flaahgra's copies, still loading
  // after a load, given the saved "loaded" never built their body controller.
  if (std::strstr(f.name, "oaded") != nullptr || std::strstr(f.name, "oading") != nullptr)
    return false;
  if (std::strcmp(cls.name, "CActor") == 0)
    return Named(f.name, {"x64_modelData", "xb4_drawFlags", "xd0_damageMag", "xe5_26_muted", "xe5_28_callTouch",
                          "xe6_27_thermalVisorFlags", "xe6_29_renderParticleDBInside",
                          "xe6_31_targetableVisorFlags", "xe7_27_enableRender", "xe7_29_drawEnabled",
                          "xe7_30_doTargetDistanceTest", "xe7_31_targetable"});
  if (std::strcmp(cls.name, "CPhysicsActor") == 0)
    // Motion too, though kTagMotion has velocity and momentum: these are applied after
    // the codecs, and the force/impulse a Think queued for the next physics step would
    // otherwise be lost (war wasps' x168_impulse).
    return Named(f.name, {"xf8_24_movable", "xf8_25_angularEnabled", "xfc_constantForce",
                          "x108_angularMomentum", "x138_velocity", "x144_angularVelocity",
                          "x150_momentum", "x15c_force", "x168_impulse", "x174_torque",
                          "x180_angularImpulse", "x18c_moveImpulse", "x198_moveAngularImpulse",
                          "x24c_numTicksStuck"});
  // CAi's health is the kTagHealth block's (Omega Pirate keeps its own elsewhere).
  if (std::strcmp(cls.name, "CAi") == 0)
    return std::strcmp(f.name, "x258_healthInfo") != 0;
  return true;
}

bool IsCodec(PortSchemaKind kind) {
  return kind == PortSchemaKind::state_machine || kind == PortSchemaKind::body_controller ||
         kind == PortSchemaKind::anim_data || kind == PortSchemaKind::path_search;
}

// The pass a field is applied on (ApplyEntityFields): the plain fields on 1, the AI, body
// and animation codecs on 0, the path on 2 (after the AI state's Activate searched one).
int FieldPass(PortSchemaKind kind) {
  return kind == PortSchemaKind::path_search ? 2 : IsCodec(kind) ? 0 : 1;
}

// The animation of an anim_data field: CModelData or a single_ptr to one.
CAnimData* FieldAnim(CEntity& ent, const PortSchemaField& f) {
  CModelData* model = nullptr;
  if (std::strcmp(f.type, "CModelData") == 0)
    model = static_cast< CModelData* >(f.addr(ent));
  else if (std::strcmp(f.type, "rstl::single_ptr<CModelData>") == 0)
    model = static_cast< rstl::single_ptr< CModelData >* >(f.addr(ent))->get();
  return model != nullptr ? model->AnimationData() : nullptr;
}

CBodyController* FieldBody(CEntity& ent, const PortSchemaField& f) {
  if (std::strcmp(f.type, "rstl::single_ptr<CBodyController>") != 0)
    return nullptr;
  return static_cast< rstl::single_ptr< CBodyController >* >(f.addr(ent))->get();
}

CStateMachineState* FieldStateMachine(CEntity& ent, const PortSchemaField& f) {
  if (std::strcmp(f.type, "CStateMachineState") != 0)
    return nullptr;
  return static_cast< CStateMachineState* >(f.addr(ent));
}

// The payload of a codec field; empty when the entity has none there.
std::string PutCodec(CEntity& ent, const PortSchemaField& f) {
  BlockWriter w;
  switch (f.kind) {
  case PortSchemaKind::state_machine:
    if (const CStateMachineState* sm = FieldStateMachine(ent, f))
      PortStateCodec::PutStateMachine(*sm, w);
    break;
  case PortSchemaKind::body_controller:
    if (const CBodyController* bc = FieldBody(ent, f))
      PortStateCodec::PutBody(*bc, w);
    break;
  case PortSchemaKind::path_search:
    if (std::strcmp(f.type, "CPathFindSearch") == 0)
      PortStateCodec::PutPath(*static_cast< const CPathFindSearch* >(f.addr(ent)), w);
    break;
  default:
    if (const CAnimData* ad = FieldAnim(ent, f))
      PortStateCodec::PutAnim(*ad, w);
    break;
  }
  return std::move(w.bytes);
}

bool GetCodec(CStateManager& mgr, CEntity& ent, const PortSchemaField& f, const std::string& data) {
  if (data.empty())
    return false;
  BlockReader r(data);
  switch (f.kind) {
  case PortSchemaKind::state_machine: {
    CStateMachineState* sm = FieldStateMachine(ent, f);
    CAi* ai = TCastToPtr< CPatterned >(&ent);
    return sm != nullptr && ai != nullptr && PortStateCodec::GetStateMachine(mgr, *ai, *sm, r);
  }
  case PortSchemaKind::body_controller: {
    CBodyController* bc = FieldBody(ent, f);
    return bc != nullptr && PortStateCodec::GetBody(mgr, *bc, r);
  }
  case PortSchemaKind::path_search:
    return std::strcmp(f.type, "CPathFindSearch") == 0 &&
           PortStateCodec::GetPath(*static_cast< CPathFindSearch* >(f.addr(ent)), r);
  default: {
    CAnimData* ad = FieldAnim(ent, f);
    return ad != nullptr && PortStateCodec::GetAnim(*ad, r);
  }
  }
}

bool Enabled() {
  static const bool on = [] {
    const char* v = std::getenv("MP_SAVESTATE_FIELDS");
    return v == nullptr || std::strcmp(v, "0") != 0;
  }();
  return on;
}

// The schema classes of `ent`, most derived first.
std::vector< const PortSchemaClass* > Chain(const CEntity& ent) {
  std::vector< const PortSchemaClass* > chain;
  for (const PortSchemaClass* c = PortEntitySchema::For(ent); c != nullptr && chain.size() < 32;
       c = PortEntitySchema::Base(*c))
    chain.push_back(c);
  return chain;
}

void PutVar(BlockWriter& w, long long v) {
  uint64_t z = (static_cast< uint64_t >(v) << 1) ^ static_cast< uint64_t >(v >> 63);
  while (z >= 0x80) {
    w.U8(static_cast< uint8_t >(z | 0x80));
    z >>= 7;
  }
  w.U8(static_cast< uint8_t >(z));
}

long long GetVar(BlockReader& r) {
  uint64_t z = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    const uint8_t b = r.U8();
    if (!r.ok)
      return 0;
    z |= static_cast< uint64_t >(b & 0x7F) << shift;
    if ((b & 0x80) == 0)
      return static_cast< long long >((z >> 1) ^ (~(z & 1) + 1));
  }
  r.ok = false;
  return 0;
}

void PutBytes(BlockWriter& w, const void* p, size_t n) {
  w.bytes.append(static_cast< const char* >(p), n);
}

bool GetBytes(BlockReader& r, void* p, size_t n) {
  if (!r.ok || r.bytes.size() - r.pos < n) {
    r.ok = false;
    return false;
  }
  std::memcpy(p, r.bytes.data() + r.pos, n);
  r.pos += n;
  return true;
}

void PutField(const CStateManager& mgr, CEntity& ent, const PortSchemaField& f, BlockWriter& w) {
  if (IsCodec(f.kind)) {
    const std::string payload = PutCodec(ent, f);
    w.U32(static_cast< uint32_t >(payload.size()));
    w.bytes += payload;
    return;
  }
  switch (f.kind) {
  case PortSchemaKind::bool_:
  case PortSchemaKind::int_:
  case PortSchemaKind::bitfield:
    PutVar(w, f.getInt != nullptr ? f.getInt(ent) : 0);
    return;
  case PortSchemaKind::uid: {
    const TUniqueId uid = *static_cast< const TUniqueId* >(f.addr(ent));
    uint32_t id = kUidNone;
    if (uid != kInvalidUniqueId) {
      // A script generator's copy carries its template's editor id: not a name for it.
      const CEntity* target = mgr.GetObjectById(uid);
      const TEditorId eid = mgr.GetEditorIdForUniqueId(uid);
      if (target != nullptr && target == mgr.GetPlayer())
        id = kUidPlayer;
      else
        id = target != nullptr && eid != kInvalidEditorId &&
                     !IsScriptGeneratedCopy(mgr, const_cast< CEntity& >(*target))
                 ? eid.Value()
                 : kUidDynamic;
    }
    w.U32(id);
    return;
  }
  default:
    // float, math and pod: the bytes, after their size (a pod's differs between
    // compilers when its padding does; a reader skips one it cannot take).
    w.U32(f.size);
    PutBytes(w, f.addr(ent), f.size);
    return;
  }
}

// Reads past one field as PutField wrote it.
void SkipField(const PortSchemaField& f, BlockReader& r) {
  switch (f.kind) {
  case PortSchemaKind::bool_:
  case PortSchemaKind::int_:
  case PortSchemaKind::bitfield:
    GetVar(r);
    return;
  case PortSchemaKind::uid:
    r.U32();
    return;
  default: {
    const uint32_t size = r.U32();
    if (!r.ok || r.bytes.size() - r.pos < size)
      r.ok = false;
    else
      r.pos += size;
    return;
  }
  }
}

// Returns whether the field was set; false with r.ok still true is a field skipped.
// Only a field of the given pass is set (FieldPass); the rest are read past.
bool GetField(CStateManager& mgr, CEntity& ent, const PortSchemaField& f, BlockReader& r, int pass) {
  if (FieldPass(f.kind) != pass) {
    SkipField(f, r);
    return false;
  }
  if (IsCodec(f.kind)) {
    const uint32_t size = r.U32();
    if (!r.ok || r.bytes.size() - r.pos < size) {
      r.ok = false;
      return false;
    }
    const std::string payload = r.bytes.substr(r.pos, size);
    r.pos += size;
    return GetCodec(mgr, ent, f, payload);
  }
  switch (f.kind) {
  case PortSchemaKind::bool_:
  case PortSchemaKind::int_:
  case PortSchemaKind::bitfield: {
    const long long v = GetVar(r);
    if (!r.ok || f.setInt == nullptr)
      return false;
    if (f.kind == PortSchemaKind::bool_ && v != 0 && v != 1)
      return false;
    f.setInt(ent, v);
    return true;
  }
  case PortSchemaKind::uid: {
    const uint32_t id = r.U32();
    if (!r.ok || id == kUidDynamic)
      return false;
    TUniqueId uid = kInvalidUniqueId;
    if (id == kUidPlayer) {
      if (mgr.GetPlayer() == nullptr)
        return false;
      uid = mgr.GetPlayer()->GetUniqueId();
    } else if (id != kUidNone) {
      uid = mgr.GetIdForScript(TEditorId(id));
      if (uid == kInvalidUniqueId)
        return false;
    }
    *static_cast< TUniqueId* >(f.addr(ent)) = uid;
    return true;
  }
  default: {
    const uint32_t size = r.U32();
    if (!r.ok || r.bytes.size() - r.pos < size) {
      r.ok = false;
      return false;
    }
    if (size != f.size) {
      r.pos += size;
      return false;
    }
    std::vector< unsigned char > buf(size);
    GetBytes(r, buf.data(), size);
    // A vector or transform that went NaN or huge is not put back.
    for (uint32_t i = 0; f.kind == PortSchemaKind::math && i + 4 <= size; i += 4) {
      float v;
      std::memcpy(&v, buf.data() + i, 4);
      if (!(v == v) || v > 1e9f || v < -1e9f)
        return false;
    }
    std::memcpy(f.addr(ent), buf.data(), size);
    return true;
  }
  }
}

// One pass over the classes of a kTagFields block (GetField's `pass`).
int ApplyClasses(CStateManager& mgr, CEntity& ent, const std::vector< const PortSchemaClass* >& chain,
                 int classes, BlockReader& r, int pass) {
  int set = 0;
  for (int c = 0; c < classes && r.ok; ++c) {
    const uint64_t lo = r.U32();
    const uint64_t fingerprint = lo | (static_cast< uint64_t >(r.U32()) << 32);
    const uint32_t len = r.U32();
    if (!r.ok || r.bytes.size() - r.pos < len)
      break;
    const PortSchemaClass* cls = nullptr;
    for (const PortSchemaClass* k : chain)
      if (k->fingerprint == fingerprint)
        cls = k;
    const size_t end = r.pos + len;
    if (cls != nullptr) {
      const std::string body = r.bytes.substr(r.pos, len);
      BlockReader br(body);
      for (int i = 0; i < cls->fieldCount && br.ok && !ent.IsInGraveyard(); ++i)
        if (SavedField(*cls, cls->fields[i]) && GetField(mgr, ent, cls->fields[i], br, pass))
          ++set;
    }
    r.pos = end;
  }
  return set;
}

} // namespace

bool EntityFieldsSaved(const CStateManager& mgr, CEntity& ent) {
  // Enemies, and the pickups and debris a generator made (life timers, generated flag).
  return Enabled() && (TCastToConstPtr< CPatterned >(&ent) != nullptr ||
                       (IsScriptGeneratedCopy(mgr, ent) && SavesGeneratedCopy(ent)));
}

bool SchemaFieldSaved(const PortSchemaClass& cls, const PortSchemaField& f) { return SavedField(cls, f); }

bool CaptureEntityFields(const CStateManager& mgr, CEntity& ent, std::string& out) {
  if (!EntityFieldsSaved(mgr, ent))
    return false;
  const std::vector< const PortSchemaClass* > chain = Chain(ent);
  BlockWriter w;
  w.U32(kFieldPolicy);
  w.U8(static_cast< uint8_t >(chain.size()));
  for (const PortSchemaClass* cls : chain) {
    BlockWriter body;
    for (int i = 0; i < cls->fieldCount; ++i)
      if (SavedField(*cls, cls->fields[i]))
        PutField(mgr, ent, cls->fields[i], body);
    w.U32(static_cast< uint32_t >(cls->fingerprint));
    w.U32(static_cast< uint32_t >(cls->fingerprint >> 32));
    w.U32(static_cast< uint32_t >(body.bytes.size()));
    w.bytes += body.bytes;
  }
  out = std::move(w.bytes);
  return true;
}

int ApplyEntityFields(CStateManager& mgr, CEntity& ent, const std::string& data) {
  if (!Enabled())
    return 0;
  BlockReader r(data);
  if (r.U32() != kFieldPolicy || !r.ok)
    return 0;
  const std::vector< const PortSchemaClass* > chain = Chain(ent);
  const int classes = r.U8();
  const size_t start = r.pos;
  const TUniqueId uid = ent.GetUniqueId();
  // The plain fields go in before the codecs too, so the saved state's Activate starts
  // from the saved fields (a path searched to the saved destination, a saved target),
  // and again after, over what entering the states changed. Only the last three count.
  int set = 0;
  const int passes[] = {1, 0, 2, 1};
  for (int i = 0; i < 4; ++i) {
    r.pos = start;
    // Entering a saved state may delete the enemy (an enemy told to go away).
    if (i >= 2 && (mgr.ObjectById(uid) != &ent || ent.IsInGraveyard()))
      break;
    const int n = ApplyClasses(mgr, ent, chain, classes, r, passes[i]);
    if (i > 0)
      set += n;
  }
  return set;
}

bool EntityFieldsCover(CEntity& ent, const std::string& data) {
  if (!Enabled())
    return false;
  BlockReader r(data);
  if (r.U32() != kFieldPolicy || !r.ok)
    return false;
  const std::vector< const PortSchemaClass* > chain = Chain(ent);
  const int classes = r.U8();
  int matched = 0;
  for (int c = 0; c < classes && r.ok; ++c) {
    const uint64_t lo = r.U32();
    const uint64_t fingerprint = lo | (static_cast< uint64_t >(r.U32()) << 32);
    const uint32_t len = r.U32();
    if (!r.ok || r.bytes.size() - r.pos < len)
      return false;
    for (const PortSchemaClass* k : chain)
      if (k->fingerprint == fingerprint)
        ++matched;
    r.pos += len;
  }
  return !chain.empty() && matched == static_cast< int >(chain.size());
}

} // namespace PortSaveState
