#pragma once

#include "port_room_effects.h"
#include <array>
#include <cstdint>
#include <string>

class CStateManager;
class CScriptSpecialFunction;

namespace PortQueenBarrier {
struct RingInput {
  uint16_t uid = 0xffff;
  bool reached = false;
};

// Owned by the actual controller: no persistent UID registry survives an area unload.
class State {
public:
  void Configure(const PortRoomEffects::BarrierGroup& group, uint32_t mrea);
  void Update(float dt, int ringState, const std::array<RingInput, 5>& rings,
              CScriptSpecialFunction& controller, CStateManager& mgr);
  void Stop(CStateManager& mgr);
  bool HandlesBreakup() const { return started_ || finished_; }
  bool Visible(const CStateManager& mgr) const;
  bool Covers(uint16_t platform, const CStateManager& mgr) const;
  // One-line state plus one line per ring, for the console's `fx barrier`.
  std::string Describe() const;

private:
  bool ResourcesReady(const CStateManager& mgr) const;
  PortRoomEffects::BarrierGroup group_{};
  std::array<uint16_t, 5> platforms_{{0xffff, 0xffff, 0xffff, 0xffff, 0xffff}};
  std::array<uint16_t, 10> effects_{{0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
                                   0xffff, 0xffff, 0xffff, 0xffff, 0xffff}};
  std::array<float, 5> motion_{{1, 1, 1, 1, 1}};
  std::string provider_;
  uint64_t generation_ = 0;
  uint32_t mrea_ = 0;
  float elapsed_ = 0;
  int ringState_ = -1;  // the controller's x1a8_ringState as of the last Update
  bool configured_ = false;
  bool started_ = false;
  bool ready_ = false;  // ResourcesReady as of the last Update
  bool finished_ = false;
  bool failed_ = false;
};
} // namespace PortQueenBarrier
