#ifndef _CHUDRADIALSELECT
#define _CHUDRADIALSELECT

#include "Kyoto/Audio/CSfxHandle.hpp"
#include "Kyoto/TToken.hpp"
#include "rstl/single_ptr.hpp"
#include "types.h"

class CGuiFrame;
class CGuiWidget;
class CStateManager;

// Metroid Prime Trilogy (Wii) only: the radial beam and visor menus, FRME_BeamSelect and
// FRME_VisorSelect. A behaviour port of the Wii code (beam fn_800E04B4..fn_800E0F1C, visor
// fn_800E8EDC..fn_800E9AB8; notes/radial-menus.md in the RE workspace). The Wii points at a
// region with the remote; the PC picks one from PortRadial's selection vector.
class CHudRadialSelect {
public:
  enum EType { kRS_Visor, kRS_Beam };
  explicit CHudRadialSelect(EType type);
  ~CHudRadialSelect();

  void Update(float dt, const CStateManager& mgr);
  void Draw() const;
  bool IsOpen() const;
  float GetOpenFraction() const { return x8_open; }

private:
  void Load();
  void OnOpen(const CStateManager& mgr);
  bool ValidSelection(const CStateManager& mgr) const;
  bool Owned(const CStateManager& mgr, int slot) const;
  int PortPressedSlot() const;

  EType xPortType;
  rstl::single_ptr< TCachedToken< CGuiFrame > > x0_token;
  CGuiFrame* x4_frame;
  float x8_open;
  float xC_hold;
  CGuiWidget* x10_basewidget;
  CGuiWidget* x14_mainFrame;
  CGuiWidget* x20_highlights[4];
  CGuiWidget* x34_icons[4];
  float x48_highlightFrac[4];
  CSfxHandle x60_sfx;
  bool x64_valid;
  int xPortSlot; // the pressed slot this tick, -1 for none
};

#endif // _CHUDRADIALSELECT
