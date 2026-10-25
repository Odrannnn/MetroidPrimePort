#pragma once

#include <cstdint>
#include <vector>

// Remastered's silhouette-shell textures (the Phazon Suit's aura; build/mpr/kb
// topic/model-black-silhouette.md), as the Remastered import writes them to a mod's
// shell.bin and the port's shell draw reads them.
namespace PortShell {
// One texture, every mip, decoded to RGBA8: the top level first, then each half down to
// 1x1, tightly packed (level i is max(width >> i, 1) x max(height >> i, 1), no padding).
struct Image {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t mips = 0;
  bool srgb = false;         // pixels are sRGB-encoded, so average them in linear light
  std::vector<uint8_t> rgba; // every mip, top first
};

// The shell shader's samplers (NMaterialSilhouette::Model_SilhouetteShell): TCH0, then
// TCH1, the indirect noise map a body and the Samus doll each have their own of.
enum : int { kStrength = 0, kIndirectBody = 1, kIndirectDoll = 2, kCount = 3 };

struct Assets {
  Image image[kCount];
};

// The first loaded mod's shell.bin, read once on first call and kept; null when no mod
// supplies one or it does not parse (the caller then draws no shell). Main thread.
const Assets* Get();
} // namespace PortShell
