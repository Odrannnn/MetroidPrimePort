#include "port_queen_barrier.h"
#include "port_queen_barrier_assets.h"
#include "port_mods.h"
#include "port_room_env.h"
#include "port_vfx_particles.h"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/ScriptObjects/CScriptEffect.hpp"
#include "MetroidPrime/ScriptObjects/CScriptPlatform.hpp"
#include "MetroidPrime/ScriptObjects/CScriptSpecialFunction.hpp"
#include "MetroidPrime/TCastTo.hpp"
#include <algorithm>
#include <cmath>

namespace PortQueenBarrier {
namespace {
PortGuid Guid(const std::array<uint8_t, 16>& bytes) {
  PortGuid g;
  g.bytes = bytes;
  return g;
}

// Raw 16-bit unique id (version << 10 | id) to TUniqueId: TUniqueId takes (version, id).
TUniqueId RawUid(uint16_t raw) { return TUniqueId(raw >> 10, raw & 0x3ff); }

const CScriptEffect* Effect(const CStateManager& mgr, uint16_t uid) {
  return TCastToConstPtr<CScriptEffect>(mgr.GetObjectById(RawUid(uid)));
}
CScriptEffect* Effect(CStateManager& mgr, uint16_t uid) {
  return TCastToPtr<CScriptEffect>(mgr.ObjectById(RawUid(uid)));
}
} // namespace

void State::Configure(const PortRoomEffects::BarrierGroup& group, uint32_t mrea) {
  if (configured_ || mrea != kMrea || (group.controller & 0x3ffffff) != kController ||
      !std::isfinite(group.duration) || group.duration <= 0 || group.duration > 60) return;
  for (size_t i = 0; i < 5; ++i) {
    if ((group.rings[i].platform & 0x3ffffff) != kRings[i].platform) return;
  }
  group_ = group;
  mrea_ = mrea;
  provider_ = PortMods::RoomEffectsProvider(mrea);
  generation_ = PortMods::ResourceGeneration();
  configured_ = !provider_.empty();
}

bool State::ResourcesReady(const CStateManager& mgr) const {
  if (!configured_ || failed_ || finished_ || generation_ != PortMods::ResourceGeneration() ||
      provider_ != PortMods::RoomEffectsProvider(mrea_) || !PortRoomEffects::Enabled()) return false;
  for (size_t i = 0; i < 5; ++i) {
    if (!TCastToConstPtr<CScriptPlatform>(mgr.GetObjectById(RawUid(platforms_[i])))) return false;
    const auto& ring = group_.rings[i];
    for (size_t j = 0; j < 2; ++j) {
      const CScriptEffect* e = Effect(mgr, effects_[2 * i + j]);
      if (!e || PortMods::ResourceProvider(0x50415254, e->PortPartId()) != provider_) return false;
      const CElementGen* gen = e->PortParticleSystem();
      if (!gen || !gen->PortDesc()) return false;
      const CGenDescription& desc = *gen->PortDesc();
      if (!PortVfxHasMesh(desc) || desc.xPortXfmd != kPortXfmdFollow) return false;
      const CPortVfxData& vfx = *desc.xPortVfx;
      const uint32_t feature = j == 0 ? kRefractionFeature : kGlowFeature;
      if ((vfx.mat.features & (kRefractionFeature | kGlowFeature)) != feature ||
          gen->PortGetRealHandle(Guid(j == 0 ? ring.breakupRefraction : ring.breakupGlow)) == kPortNoVar ||
          (j == 0 && gen->PortGetRealHandle(Guid(ring.motion)) == kPortNoVar)) return false;
      for (const auto& tex : vfx.mat.tex) {
        if (!tex.token.HasReference() || !tex.token.IsLoaded() ||
            PortMods::ResourceProvider(0x54585452, tex.token.GetTag().GetId()) != provider_) return false;
      }
      if (started_ && gen->GetParticleCount() == 0) return false;
    }
  }
  return true;
}

bool State::Visible(const CStateManager&) const {
  // Every visor draws in the HDR scene, so the group shows in all of them; each member's own
  // roomfx visor flags (all 1/1/1 here) decide per visor in CScriptEffect, as Remastered's do.
  // Every member asks this twice a frame; the full check runs once per tick in Update.
  return started_ && ready_ && generation_ == PortMods::ResourceGeneration() && PortRoomEffects::Enabled() &&
         PortRoomEnv::Enabled();
}

bool State::Covers(uint16_t platform, const CStateManager& mgr) const {
  return std::find(platforms_.begin(), platforms_.end(), platform) != platforms_.end() &&
         (finished_ || Visible(mgr));
}

void State::Stop(CStateManager& mgr) {
  started_ = false;
  ready_ = false;
  finished_ = false;
  failed_ = true;
  for (uint16_t& uid : effects_) {
    if (uid != 0xffff && Effect(mgr, uid)) mgr.DeleteObjectRequest(RawUid(uid));
    uid = 0xffff;
  }
}

void State::Update(float dt, int ringState, const std::array<RingInput, 5>& inputs,
                   CScriptSpecialFunction& controller, CStateManager& mgr) {
  if (!configured_ || failed_ || finished_ || !std::isfinite(dt) || dt < 0) return;
  if (generation_ != PortMods::ResourceGeneration() || !PortRoomEffects::Enabled()) {
    Stop(mgr);
    return;
  }
  // Resolve against this area's live objects. Repeated calls while textures stream remain invisible.
  if (!started_) {
    bool complete = true;
    for (size_t i = 0; i < 5; ++i) {
      platforms_[i] = mgr.GetIdForScript(TEditorId(group_.rings[i].platform)).value;
      effects_[2 * i] = mgr.GetIdForScript(TEditorId(group_.rings[i].refraction)).value;
      effects_[2 * i + 1] = mgr.GetIdForScript(TEditorId(group_.rings[i].glow)).value;
      bool member = false;
      for (const auto& input : inputs) member |= input.uid == platforms_[i] && input.uid != 0xffff;
      complete &= member && effects_[2 * i] != 0xffff && effects_[2 * i + 1] != 0xffff;
      for (size_t j = 0; j < 2; ++j) {
        if (CScriptEffect* e = Effect(mgr, effects_[2 * i + j])) {
          e->PortAttachBarrier(controller.GetUniqueId(), RawUid(platforms_[i]));
        }
      }
    }
    if (!complete || ringState == 3) {
      Stop(mgr);
      return;
    }
    if (!ResourcesReady(mgr)) return;
    // All members are available before any is activated. Rendering stays gated until primed.
    for (size_t i = 0; i < 5; ++i) {
      CScriptPlatform* platform = TCastToPtr<CScriptPlatform>(mgr.ObjectById(RawUid(platforms_[i])));
      platform->PortSetBarrierController(controller.GetUniqueId());
      const auto& ring = group_.rings[i];
      for (size_t j = 0; j < 2; ++j) {
        CScriptEffect* e = Effect(mgr, effects_[2 * i + j]);
        e->PortSetExternalReal(Guid(j == 0 ? ring.breakupRefraction : ring.breakupGlow), 0.f);
        if (j == 0) e->PortSetExternalReal(Guid(ring.motion), 1.f);
        e->PortFollowBarrier(*platform);
        mgr.DeliverScriptMsg(e, controller.GetUniqueId(), kSM_Activate);
        if (e->PortParticleSystem()->GetParticleCount() == 0) e->PortParticleSystem()->ForceParticleCreation(1);
        e->CalculateRenderBounds();
      }
    }
    started_ = true;
  }
  if (!ResourcesReady(mgr)) {
    Stop(mgr);
    return;
  }
  ready_ = true;
  if (ringState == 3) elapsed_ = std::min(group_.duration, elapsed_ + dt);
  for (size_t i = 0; i < 5; ++i) {
    CScriptPlatform* platform = TCastToPtr<CScriptPlatform>(mgr.ObjectById(RawUid(platforms_[i])));
    bool reached = false;
    for (const auto& input : inputs) if (input.uid == platforms_[i]) reached = input.reached;
    const bool down = std::fabs(platform->GetVelocityWR().GetZ()) < 1e-5f && (ringState == 2 || reached);
    motion_[i] = std::clamp(motion_[i] + (down ? -2.5f : 2.5f) * dt, 0.f, 1.f);
    const auto& ring = group_.rings[i];
    CScriptEffect* refract = Effect(mgr, effects_[2 * i]);
    CScriptEffect* glow = Effect(mgr, effects_[2 * i + 1]);
    if (platform->GetActive()) {
      refract->PortFollowBarrier(*platform);
      glow->PortFollowBarrier(*platform);
    }
    refract->PortSetExternalReal(Guid(ring.motion), motion_[i]);
    if (ringState == 3) {
      refract->PortSetExternalReal(Guid(ring.breakupRefraction), elapsed_ / group_.duration);
      glow->PortSetExternalReal(Guid(ring.breakupGlow), elapsed_ / group_.duration);
    }
  }
  if (ringState == 3 && elapsed_ >= group_.duration) {
    for (uint16_t uid : platforms_) {
      if (CScriptPlatform* platform = TCastToPtr<CScriptPlatform>(mgr.ObjectById(RawUid(uid)))) platform->SetActive(false);
    }
    Stop(mgr);
    finished_ = true;
    controller.SetActive(false);
  }
}
} // namespace PortQueenBarrier
