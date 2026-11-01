#pragma once

#include "MetroidPrime/CStateManager.hpp"

class CEntity;
class CPlayer;
class CVector3f;

// Permanent, near-free motion diagnostic. A ring of the player's last ticks (position and
// velocity before and after the mover, the impulse it was fed, which mover ran, the retained
// Remastered object state, knockbacks) that is written to the port log as `[motion]` lines
// when the player moves impossibly far or fast in one tick, or jumps between ticks outside
// CGameCollision::MovePlayer. See docs/DEBUGGING.md.
namespace PortMotionWatch {

enum class Branch : unsigned char {
  None,
  RemasteredGround, // MoveGroundedRemastered
  RemasteredAir,    // MoveAirborneStepped
  GroundNew,        // retail MoveGroundCollider_New
  GroundOld,        // retail MoveGroundCollider
  Ball,             // standard collider (morph ball) MoveAndCollide
  Air,              // retail airborne MoveAndCollide
};

// One player MovePlayer call. Construct before the first early return, Set() the branch
// where it is chosen; the destructor records the tick and decides whether to dump.
class TickScope {
public:
  TickScope(CStateManager& mgr, CPlayer* player, const TEntityList* colliderList);
  ~TickScope();
  TickScope(const TickScope&) = delete;
  TickScope& operator=(const TickScope&) = delete;
  void Set(Branch branch) { mBranch = branch; }

private:
  CStateManager& mMgr;
  CPlayer* mPlayer;
  const TEntityList* mColliderList;
  Branch mBranch = Branch::None;
};

// The actor whose damage is being applied (ApplyDamage / ApplyRadiusDamage), so a knockback
// recorded inside it can name its source.
class DamagerScope {
public:
  DamagerScope(const CStateManager& mgr, TUniqueId damagerId);
  ~DamagerScope();
  DamagerScope(const DamagerScope&) = delete;
  DamagerScope& operator=(const DamagerScope&) = delete;

private:
  const CEntity* mPrev;
};

// CStateManager::KnockBackPlayer applied `impulse` (dir * power).
void NoteKnockback(const CVector3f& dir, float usePower);
// A legitimate warp (CPlayer::Teleport, save-state placement): the next tick's jump is tagged
// `warp` and not dumped.
void NoteWarp();

} // namespace PortMotionWatch
