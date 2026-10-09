#pragma once

class CStateManager;

// The debug command console (platform/port_console.cpp, MP_CONSOLE=<port>).
// In every build; it does nothing unless MP_CONSOLE is set.
bool PortConsoleEnabled();
bool PortConsoleFrame(unsigned frame);
void PortConsoleTick(CStateManager& mgr);
// Damage queued by the console `hurt` command; PreThink clears the player's damage flags, so the
// hit is applied right after it. Returns 0 when none is pending.
float PortConsoleTakePendingHurt();
