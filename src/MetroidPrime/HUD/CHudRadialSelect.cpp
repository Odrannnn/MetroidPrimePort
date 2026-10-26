#include "MetroidPrime/HUD/CHudRadialSelect.hpp"

#include "GuiSys/CGuiCamera.hpp"
#include "GuiSys/CGuiFrame.hpp"
#include "GuiSys/CGuiWidget.hpp"
#include "GuiSys/CGuiWidgetDrawParms.hpp"
#include "Kyoto/Audio/CSfxManager.hpp"
#include "Kyoto/CSimplePool.hpp"
#include "Kyoto/Graphics/CColor.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "MetroidPrime/SFX/UI.h"

#include "port_radial.h"

#include <math.h>

// @ lbl_804707C0 (beam) / lbl_804710D8 (visor): the widget names, in load order: the main
// frame, the base widget (both menus use "basewidget_visiorSelect"), then (highlight, icon)
// for each slot. A slot is the beam id or the visor id.
static const char* const skFrameNames[] = {"FRME_VisorSelect", "FRME_BeamSelect"};
static const char* const skMainFrameName = "model_main_frame";
static const char* const skBaseWidgetName = "basewidget_visiorSelect";
static const char* const skSlotNames[2][4][2] = {
    {{"model_highlight_center", "model_combat_icon"},
     {"model_highlight_right", "model_xray_icon"},
     {"model_highlight_bottom", "model_scan_icon"},
     {"model_highlight_left", "model_thermal_icon"}},
    {{"model_highlight_center", "model_power_icon"},
     {"model_highlight_left", "model_ice_icon"},
     {"model_highlight_bottom", "model_wave_icon"},
     {"model_highlight_right", "model_plasma_icon"}},
};

static const float kEpsilon = 0.00001f; // @ lbl_805C4B24
// The centre takes the selection below this length; past it, the nearest highlight by angle.
static const float kPortCentreRadius = 0.35f;

static bool IsZero(float v) { return fabsf(v) < kEpsilon; }
static bool IsOne(float v) { return fabsf(v - 1.f) < kEpsilon; }

// @ fn_800E09A8 / fn_800E93B8: step toward 1 (on) or 0 (off), clamped.
static void Approach(bool on, float& value, float step) {
  if (on) {
    value = value + step < 1.f ? value + step : 1.f;
  } else {
    value = 0.f < value - step ? value - step : 0.f;
  }
}

// @ fn_800E04B4 / fn_800E8EDC
CHudRadialSelect::CHudRadialSelect(EType type)
: xPortType(type)
, x0_token(rs_new TCachedToken< CGuiFrame >(gpSimplePool->GetObj(skFrameNames[type])))
, x4_frame(nullptr)
, x8_open(0.f)
, xC_hold(0.f)
, x10_basewidget(nullptr)
, x14_mainFrame(nullptr)
, x60_sfx()
, x64_valid(false)
, xPortSlot(-1) {
  x0_token->Lock();
  for (int i = 0; i < 4; ++i) {
    x20_highlights[i] = nullptr;
    x34_icons[i] = nullptr;
    x48_highlightFrac[i] = 0.f;
  }
}

// @ fn_800E0F1C
CHudRadialSelect::~CHudRadialSelect() {}

// @ fn_800E058C / fn_800E8FB4 (the visor's also keeps textpane_scan_warning, which it never
// shows from these functions; not ported).
void CHudRadialSelect::Load() {
  x14_mainFrame = x4_frame->FindWidget(skMainFrameName);
  x10_basewidget = x4_frame->FindWidget(skBaseWidgetName);
  for (int i = 0; i < 4; ++i) {
    x20_highlights[i] = x4_frame->FindWidget(skSlotNames[xPortType][i][0]);
    x34_icons[i] = x4_frame->FindWidget(skSlotNames[xPortType][i][1]);
    x48_highlightFrac[i] = 0.f;
  }
  // As the port's other Trilogy frames: fit the frame's camera to the window's aspect.
  if (CGuiCamera* cam = x4_frame->GetFrameCamera()) {
    cam->SetAspectMatch(true);
  }
}

// @ fn_801A0B88 (beams: items 0..3) / fn_801A0E10 (visors: Combat always; Scan 5, XRay 13,
// Thermal 9).
bool CHudRadialSelect::Owned(const CStateManager& mgr, int slot) const {
  const CPlayerState& ps = *mgr.GetPlayerState();
  if (xPortType == kRS_Beam) {
    return ps.HasPowerUp(static_cast< CPlayerState::EItemType >(slot));
  }
  switch (slot) {
  case 0:
    return true;
  case 1:
    return ps.HasPowerUp(CPlayerState::kIT_XRayVisor);
  case 2:
    return ps.HasPowerUp(CPlayerState::kIT_ScanVisor);
  case 3:
    return ps.HasPowerUp(CPlayerState::kIT_ThermalVisor);
  }
  return false;
}

// @ fn_800E07A4 / fn_800E91DC
void CHudRadialSelect::OnOpen(const CStateManager& mgr) {
  // CTweakGuiColors+0x258 + 8 * (the visor's colour index): every entry is 0x89D6FFFF in
  // Trilogy's tweak (fn_802526A0), so the colour is the same whatever the visor.
  if (x10_basewidget != nullptr) {
    x10_basewidget->SetColor(CColor(0x89D6FFFFu));
  }
  // The beam menu shows every owned beam; the visor menu leaves Combat (slot 0) as it is.
  for (int i = xPortType == kRS_Beam ? 0 : 1; i < 4; ++i) {
    const bool owned = Owned(mgr, i);
    if (x20_highlights[i] != nullptr) x20_highlights[i]->SetIsVisible(owned);
    if (x34_icons[i] != nullptr) x34_icons[i]->SetIsVisible(owned);
  }
  x64_valid = false;
  xC_hold = 1.f;
}

// @ fn_800E08A0 / fn_800E92D8: the first pressed region's id, if it isn't the current one
// (beam: CPlayerState+0x8; visor: +0x18, the visor being switched to) and is owned. Wii
// region k lights and picks slot {0,1,2,3}[k] (beam) or {0,2,3,1}[k] (visor,
// lbl_804710C8): the PC picks the slot directly.
bool CHudRadialSelect::ValidSelection(const CStateManager& mgr) const {
  if (xPortSlot < 0) return false;
  const CPlayerState& ps = *mgr.GetPlayerState();
  const int current = xPortType == kRS_Beam ? static_cast< int >(ps.GetCurrentBeam())
                                            : static_cast< int >(ps.GetTransitioningVisor());
  return xPortSlot != current && Owned(mgr, xPortSlot);
}

// PC: the Wii's pointer regions (control codes 0x18..0x1B, 0x3A..0x37). The selection vector
// picks the centre near the middle, otherwise the slot whose icon is nearest its angle (the
// highlight models all sit at the centre, their shapes offset in the mesh).
int CHudRadialSelect::PortPressedSlot() const {
  if (!PortRadial::Held(xPortType == kRS_Beam ? PortRadial::kBeam : PortRadial::kVisor) &&
      IsZero(x8_open)) {
    return -1;
  }
  float sx = 0.f, sy = 0.f;
  PortRadial::GetSelection(sx, sy);
  if (sx * sx + sy * sy < kPortCentreRadius * kPortCentreRadius || x34_icons[0] == nullptr) {
    return 0;
  }
  const CVector3f centre = x34_icons[0]->GetIdlePosition();
  const float angle = atan2f(sy, sx);
  int best = 0;
  float bestDiff = 0.f;
  for (int i = 1; i < 4; ++i) {
    if (x34_icons[i] == nullptr) continue;
    const CVector3f pos = x34_icons[i]->GetIdlePosition();
    float diff = fabsf(atan2f(pos.GetZ() - centre.GetZ(), pos.GetX() - centre.GetX()) - angle);
    if (diff > 3.1415927f) diff = 2.f * 3.1415927f - diff;
    if (best == 0 || diff < bestDiff) {
      best = i;
      bestDiff = diff;
    }
  }
  return best;
}

// @ fn_800E09F8 (beam) / fn_800E9408 (visor)
void CHudRadialSelect::Update(float dt, const CStateManager& mgr) {
  if (x4_frame == nullptr) {
    if (!x0_token->TryCache()) return;
    x4_frame = x0_token->GetObject();
    if (x4_frame == nullptr || !x4_frame->GetIsFinishedLoading()) {
      x4_frame = nullptr;
      return;
    }
    Load();
  }

  const PortRadial::Kind kind = xPortType == kRS_Beam ? PortRadial::kBeam : PortRadial::kVisor;
  const float oldOpen = x8_open;
  const float oldHold = xC_hold;
  // The Wii holds the menu while the control mapper reads 0x4E (beam) / 0x4C (visor); the
  // PC holds it while its Beam menu / Visor menu action is down.
  const bool held = PortRadial::Held(kind);
  Approach(held, xC_hold, 3.f * dt);
  Approach(held || xC_hold > 0.f, x8_open, 10.f * dt);
  if (IsZero(oldOpen) && !IsZero(x8_open)) {
    OnOpen(mgr);
  }
  xPortSlot = PortPressedSlot();
  if (oldOpen != x8_open) {
    if (IsZero(oldOpen)) {
      CSfxManager::SfxStart(SFXui_x_freeon_00, 0x3C, 0x40, false, CSfxManager::kMedPriority, false,
                            CSfxManager::kAllAreas);
    } else if (IsOne(oldOpen)) {
      CSfxManager::SfxStart(SFXui_x_freeoff_00, 0x3C, 0x40, false, CSfxManager::kMedPriority, false,
                            CSfxManager::kAllAreas);
    }
  }
  if (!held && IsOne(oldHold)) {
    x64_valid = ValidSelection(mgr);
    if (x64_valid) {
      PortRadial::Pick(kind, xPortSlot);
      if (xPortSlot != 0) {
        if (x60_sfx && CSfxManager::IsPlaying(x60_sfx)) {
          CSfxManager::SfxStop(x60_sfx);
        }
        // The Wii then bends this handle's pitch (fn_80338754(&h, 0x8000)); not ported.
        x60_sfx = CSfxManager::SfxStart(SFXui_x_quitsel_00, 0x1E, 0x40, false,
                                        CSfxManager::kMedPriority, false, CSfxManager::kAllAreas);
      }
    }
  }

  if (x14_mainFrame != nullptr) {
    x14_mainFrame->SetLocalTransform(x14_mainFrame->GetIdleXform() *
                                     CTransform4f::Scale(x8_open, 1.f, x8_open));
    x14_mainFrame->RecalculateTransforms();
  }
  for (int i = 0; i < 4; ++i) {
    Approach(xPortSlot == i, x48_highlightFrac[i], dt / 0.25f);
  }
  // After a valid pick the highlight pulses while the hold fraction runs out.
  const float pulse = !held && x64_valid ? (1.f + cosf(1.5f * (2.f * (3.1415927f * xC_hold)))) * 0.5f
                                         : 1.f;
  for (int i = 0; i < 4; ++i) {
    if (x20_highlights[i] == nullptr) continue;
    // lbl_805C7BCC (opaque white) with its alpha replaced; lbl_805C4B3C is 255.
    const float alpha = x48_highlightFrac[i] * pulse * 255.f;
    x20_highlights[i]->SetColor(CColor(uchar(255), uchar(255), uchar(255), static_cast< uchar >(alpha)));
  }
}

// @ fn_800E0EE4 / fn_800E98F4
bool CHudRadialSelect::IsOpen() const { return !IsZero(x8_open); }

// @ fn_800E0E68 / fn_800E9878
void CHudRadialSelect::Draw() const {
  if (IsOpen() && x4_frame != nullptr) {
    x4_frame->Draw(CGuiWidgetDrawParms(x8_open));
  }
}
