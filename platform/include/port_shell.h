#pragma once

#include <functional>

class CAABox;

// Remastered's Phazon Suit aura: NMaterialSilhouette::Model_SilhouetteShell drawn over a model by
// CModelDataMP1::RenderSilhouette 0xfc5bec (build/mpr/kb topic/model-black-silhouette.md), in
// place of retail's DrawPhazonSuitIndirectEffect, wherever the Remastered room environment
// renders the frame and the mod supplies shell.bin. The shell is the model pushed out along its
// normals, drawn twice (depth only, then colour through the scene copy without depth writes)
// after the frame's sorted geometry, where Remastered's RenderPrimaryPass draws it.
namespace PortShell {
enum Type : int {
  kBody = 1, // the player actor's body and gun (CScriptPlayerActorMP1::Render), and the port's third-person body
  kDoll = 2, // the pause screen's Samus doll (CSamusDollMP1::RenderSuit)
};

// Whether a shell can draw now (Remastered room environment on, shell.bin loaded); false: the
// caller draws retail's effect.
bool Enabled();

// `draw` draws the model (its own transform and flags; the shell overrides the shading, blend and
// depth state) and is called twice from Flush, once a pass. `bounds` is the model's world bounds,
// whose centre the depth bias is measured at. True when queued.
bool Defer(Type type, const CAABox& bounds, std::function<void()> draw);

// Drops the queue (start of CStateManager::DrawWorld), so a shell queued outside it never draws a
// frame late.
void Clear();

// Draws the queued shells (CStateManager::DrawWorld, after the sorted geometry) and empties the
// queue. Without this frame's scene copy it draws none of them.
void Flush();

// Draws a shell now (the doll, which draws in the pause screen's own pass): copies the scene first
// if this frame has no copy yet. False when it cannot (no copy, not enabled): nothing drawn.
bool DrawNow(Type type, const CAABox& bounds, const std::function<void()>& draw);
} // namespace PortShell
