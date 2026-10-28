#pragma once

// The map screen's legend and mark-prompt helpers that CAutoMapper.cpp calls from
// its frame setup and update (defined in platform/port_automapper.cpp, alongside
// the map screen's pointer input).

#include "rstl/string.hpp"

#include "types.h"

class CGuiTextPane;

// The settings key the shown legend was built for (~0u: the disc's legend).
extern uint sPortLegendKey;

// True when the port's legend can be laid out: its swatches are loaded, or it
// shows only the disc's text and needs none.
bool PortLegendReady();
// The settings the legend depends on, as one key: a change means a rebuild.
uint PortLegendKey();
// The disc's legend text (Trilogy builds its own from what Samus has).
rstl::wstring PortLegendBase();
// The disc's legend text plus the port's lines.
rstl::wstring PortLegendText();
// Sets the legend pane's text and fits the pane to its rows.
void PortSetLegend(CGuiTextPane* legend, const rstl::wstring& text);
// Drops the caches keyed on the map frame's panes (the frame was loaded or released).
void PortForgetMapPanes();
// Moves the disc's hint pane to the mark prompt's place, or back.
void PortPlaceMarkHint(CGuiTextPane* pane, bool show);
