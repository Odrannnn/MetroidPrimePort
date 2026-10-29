// Remastered's silhouette shell over the Phazon Suit (port_shell.h).

#include "port_shell.h"

#include "port_debug.h"
#include "port_mods.h"
#include "port_room_env.h"
#include "port_shell_assets.h"

#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Math/CAABox.hpp"

#include <aurora/vfx.hpp>
#include <dolphin/gx.h>
#include <dolphin/gx/GXExtra.h>

#include <utility>
#include <vector>

namespace PortShell {
namespace {
// CCH0..CCH4 of each silhouette type, as CModelDataMP1::RenderSilhouette sets them (KB
// topic/model-black-silhouette.md): type 1 the player actor's, type 2 the Samus doll's.
constexpr float kParams[2][20] = {
    {.22f, .03f, .95f, .5f, 0.f, 0.f, .015f, 1.3f, .1f, .04f, 1.f, -1.f, 8.f, 0.f, 8.f, .8f, 0.f, 20.f, 5.f, 0.f},
    {.25f, .025f, .95f, .5f, 0.f, 0.f, .002f, 4.f, .05f, .04f, 1.f, -1.f, 16.f, 0.f, 1.f, .2f, 1.f, 3.f, 4.f, 0.f},
};
// The depth bias in view units: CScriptPlayerActorMP1::Render's flags carry 0.35; the doll's are
// CModelFlagsMPT::Normal().WithOtherFlags(0x400), no bias.
constexpr float kBias[2] = {.35f, 0.f};

struct Pending {
  Type type;
  CAABox bounds;
  std::function<void()> draw;
};
std::vector<Pending> sPending;

bool sTexInit = false;
GXTexObj sTex[kCount];

bool InitTextures() {
  const Assets* assets = Get();
  if (assets == nullptr) {
    return false;
  }
  if (!sTexInit) {
    for (int i = 0; i < kCount; ++i) {
      const Image& img = assets->image[i];
      if (img.width == 0 || img.height == 0 || img.mips == 0 || img.rgba.empty()) {
        return false;
      }
      GXInitTexObj(&sTex[i], const_cast<uint8_t*>(img.rgba.data()), u16(img.width), u16(img.height),
                   static_cast<GXTexFmt>(GX_TF_RGBA8_PC), GX_REPEAT, GX_REPEAT, img.mips > 1 ? GX_TRUE : GX_FALSE);
      GXInitTexObjLOD(&sTex[i], img.mips > 1 ? GX_LIN_MIP_LIN : GX_LINEAR, GX_LINEAR, 0.f, float(img.mips - 1), 0.f,
                      GX_FALSE, GX_FALSE, GX_ANISO_1);
    }
    sTexInit = true;
  }
  return true;
}

void Draw(Type type, const CAABox& bounds, const std::function<void()>& draw) {
  const int t = type == kDoll ? 1 : 0;
  const Assets* assets = Get();
  const int indirect = type == kDoll ? kIndirectDoll : kIndirectBody;
  float params[24];
  for (int i = 0; i < 20; ++i) {
    params[i] = kParams[t][i];
  }
  const CVector3f centre = bounds.GetCenterPoint();
  params[20] = CGraphics::GetSecondsMod900();
  params[21] = kBias[t];
  params[22] = assets->image[kStrength].srgb ? 1.f : 0.f;
  params[23] = CVector3f::Dot(centre - CGraphics::GetViewPoint(), CGraphics::GetViewMatrix().GetForward());

  GXLoadTexObj(&sTex[kStrength], GX_TEXMAP0);
  GXLoadTexObj(&sTex[indirect], GX_TEXMAP1);
  GXPortShell(1, params);
  draw();
  GXPortShell(2, params);
  draw();
  GXPortShell(0, nullptr);
}
} // namespace

bool Enabled() {
  return PortDebug::PhazonShell() && PortRoomEnv::Enabled() && PortMods::RoomEnvLoaded() && InitTextures();
}

bool Defer(Type type, const CAABox& bounds, std::function<void()> draw) {
  if (!Enabled()) {
    return false;
  }
  sPending.push_back({type, bounds, std::move(draw)});
  return true;
}

void Clear() { sPending.clear(); }

void Flush() {
  std::vector<Pending> pending = std::move(sPending);
  sPending.clear();
  if (pending.empty() || !Enabled() || !aurora::gfx::vfx::scene_copy_available()) {
    return;
  }
  for (const Pending& p : pending) {
    Draw(p.type, p.bounds, p.draw);
  }
}

bool DrawNow(Type type, const CAABox& bounds, const std::function<void()>& draw) {
  if (!Enabled()) {
    return false;
  }
  // RenderPauseScreenElements copies the frame after every doll part (CopyOpaqueColorBuffer), then
  // draws the silhouettes over that copy.
  aurora::gfx::vfx::capture_scene(true);
  if (!aurora::gfx::vfx::scene_copy_available()) {
    return false;
  }
  Draw(type, bounds, draw);
  return true;
}
} // namespace PortShell
