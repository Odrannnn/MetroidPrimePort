#pragma once

// Trilogy's radial beam and visor menus (CHudRadialSelect, Controls > Options "Radial
// beam and visor menus"): the input side. On the Wii the pointer picks a region of the
// open menu; here the mouse, touch, gyro and right-stick travel (aim frozen meanwhile) or
// the C-stick move a selection from the centre, and the HUD turns it into a region.
namespace PortRadial {

enum Kind { kVisor = 0, kBeam = 1 };

// Once per game tick: whether each menu's action is held, and the C-stick (x right,
// y up), which points the selection while a menu captures input.
void Tick(bool beamHeld, bool visorHeld, float cx, float cy);
bool Held(Kind kind);
// fn_80075BE0: Held means latched after >0.175 seconds, not the raw button.
// fn_80235C98 tests the latch regardless of which menu owns it.
bool LatchActive();
// The console's `radial`: hold menu `kind` (-1 releases) with the selection at x, y.
void Force(int kind, float x, float y);
// From CSamusHud each update: whether the menus exist (a Trilogy disc, the setting on)
// and whether each is open (drawn).
void SetState(bool available, bool visorOpen, bool beamOpen);
// fn_80075B58 per menu, from the game update: whether a held menu may latch (and stay latched).
void SetEligible(bool visor, bool beam);
bool Eligible(Kind kind);
// A menu is held or still open: the pointer travel and the C-stick go to the selection.
bool Capturing();
// Pointer travel in mouse counts (y down).
void AddTravel(float dx, float dy);
// A touch menu button's finger: the selection itself (x right, y up, 1 = full radius), taken at
// the next Tick while a menu captures input. Any thread.
void SetPoint(float x, float y);
// The selection, x right, y up, length at most 1; reset to the centre when a menu opens.
void GetSelection(float& x, float& y);
// The HUD picked `id` (EBeamId or EPlayerVisor) on release; the input poll sends it.
void Pick(Kind kind, int id);
// The pending pick, or -1; clears it.
int TakePick(Kind kind);

} // namespace PortRadial
