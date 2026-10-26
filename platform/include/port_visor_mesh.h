#pragma once

#include <cstdint>

// Remastered's per-visor mesh filter. A CMDL mesh carries a flag word (unkC | unkE << 16);
// InitMeshFilter (0xf9e764) excludes the meshes whose bit for the active visor is set (table
// 0x1d2d7f4: combat and scan bit 16, thermal bit 17, X-ray bit 18), and the shadow passes
// (0x3bbbd0, 0x3b7314, 0x3b820c) also exclude bit 5. The thermal visor's cold pass
// (init_render_query_for_cold_pass 0xfb1190) further includes bit 22 and removes 19 from the
// required set, its hot pass (0xfb11f0) includes bit 19 and removes 22: the cold pass draws
// only meshes with bit 22, the hot pass only meshes with bit 19. The converter keeps those
// bits per surface as a tagged word at surface header offset 0x5c ('VM' in the top half, a
// bit per filter in the bottom; bit 22 as its absence, since nearly every mesh has it); 0
// means a surface with bit 22 and none of the others.
namespace PortVisorMesh {

constexpr uint32_t kTag = 0x564d0000;
constexpr uint32_t kShadow = 1;  // flag bit 5: not cast into shadows
constexpr uint32_t kVisor16 = 2; // flag bit 16: hidden in the combat and scan visors
constexpr uint32_t kVisor17 = 4; // flag bit 17: hidden in the thermal visor
constexpr uint32_t kVisor18 = 8; // flag bit 18: hidden in the X-ray visor
constexpr uint32_t kThermalHot = 0x10;   // flag bit 19: drawn in the thermal hot pass
constexpr uint32_t kNoThermalCold = 0x20; // flag bit 22 clear: not drawn in the thermal cold pass

// The surface word for a mesh flag word (0 when bit 22 is set and no other filter bit is).
inline uint32_t FromFlags(uint32_t flags) {
  const uint32_t bits = ((flags >> 5) & 1 ? kShadow : 0) | ((flags >> 16) & 1 ? kVisor16 : 0) |
                        ((flags >> 17) & 1 ? kVisor17 : 0) | ((flags >> 18) & 1 ? kVisor18 : 0) |
                        ((flags >> 19) & 1 ? kThermalHot : 0) | ((flags >> 22) & 1 ? 0 : kNoThermalCold);
  return bits != 0 ? kTag | bits : 0;
}

// The filter bits of a surface word (0 for an untagged word).
inline uint32_t Bits(uint32_t word) { return (word & 0xffff0000u) == kTag ? (word & 0x3f) : 0; }

// The exclusion bit of a visor in CPlayerState::EPlayerVisor order (combat, X-ray, scan, thermal).
inline uint32_t VisorBit(int visor) {
  return visor == 1 ? kVisor18 : visor == 3 ? kVisor17 : kVisor16;
}

// A surface is skipped when its bit for the active visor is set, and, with the shadow caster
// mode 2 (casts only), when it has the shadow bit.
inline bool SkipsVisor(uint32_t word, int visor) { return (Bits(word) & VisorBit(visor)) != 0; }
inline bool SkipsShadow(uint32_t word) { return (Bits(word) & kShadow) != 0; }

// The thermal visor's passes (0 none, 1 cold, 2 hot): the cold pass skips a surface without
// bit 22, the hot pass one without bit 19.
enum ThermalPass { kTP_None, kTP_Cold, kTP_Hot };
inline bool SkipsThermal(uint32_t word, int pass) {
  return pass == kTP_Cold ? (Bits(word) & kNoThermalCold) != 0
                          : pass == kTP_Hot && (Bits(word) & kThermalHot) == 0;
}

} // namespace PortVisorMesh
