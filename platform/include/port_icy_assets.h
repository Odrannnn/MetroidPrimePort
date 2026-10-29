#pragma once

#include <array>
#include <cstdint>

#include "port_shell_assets.h"

// Remastered's frozen-enemy material (RenderLooseDataMP1's icy MATI ba70a559, shader
// 700e8e71; build/mpr/kb topic/frozen-enemy-icy-render.md), as the Remastered import writes
// it to a mod's icy.bin and the port's frozen draw reads it.
namespace PortIcy {
// The material's samplers, in the MATI's order: the normal map, the metal/roughness map and
// the ice colour TCH0. Each is decoded to RGBA8 with every mip, as shell.bin's are.
enum : int { kNormal = 0, kMetal = 1, kColor = 2, kCount = 3 };
using Image = PortShell::Image;

// The material's constants, in the MATI's order: CCH0, CCH1, BKLT, BKLA.
enum : int { kCch0 = 0, kCch1 = 1, kBklt = 2, kBkla = 3, kVectors = 4 };

struct Assets {
  Image image[kCount];
  std::array<float, 4> vector[kVectors]{};
};

// The first loaded mod's icy.bin, read once on first call and kept; null when no mod
// supplies one or it does not parse (frozen enemies then draw as retail). Main thread.
const Assets* Get();
} // namespace PortIcy
