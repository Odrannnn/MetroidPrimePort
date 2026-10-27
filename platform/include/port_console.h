#pragma once

class CStateManager;

// The debug command console (platform/port_console.cpp, MP_CONSOLE=<port>).
// In every build; it does nothing unless MP_CONSOLE is set.
#include <span>
#include <string_view>

// The items `give` and `take` accept, for the Remap page's custom action picker:
// the console's own name of each (what the commands take) and the player-facing
// name, e.g. "WaveBeam" and "Wave Beam".
struct PortConsoleItem {
  const char* key;
  const char* label;
};
std::span<const PortConsoleItem> PortConsoleItems();

bool PortConsoleEnabled();
// Queues one command line from the game itself (Remap's custom actions), with or
// without MP_CONSOLE. It runs in order with the client's; its reply goes to the
// log. False when the queue is full.
bool PortConsoleRun(std::string_view line);
bool PortConsoleFrame(unsigned frame);
void PortConsoleTick(CStateManager& mgr);
// Damage queued by the console `hurt` command; PreThink clears the player's damage flags, so the
// hit is applied right after it. Returns 0 when none is pending.
float PortConsoleTakePendingHurt();
