#pragma once

#include <array>
#include <cstdint>

// Landing Site adds these unrigged cinematic hulls alongside retail's animated ship.
// Their cinematic poses are not imported. Leave the original ship in charge of the
// landing, hatch/platform, collision and save sequence until a posed replacement exists.
namespace PortGunshipScenery {
inline constexpr uint32_t kLandingSite = 0xB2701146;
// Pak byte order, after the ROOM property's SwapUuid conversion.
inline constexpr std::array<uint8_t, 16> kHull = {
    0xA4, 0xDE, 0x14, 0x1B, 0xEA, 0x3B, 0x44, 0xAD,
    0x81, 0x23, 0x46, 0xE4, 0x5C, 0x64, 0x9B, 0x06};
inline constexpr std::array<uint8_t, 16> kUnlitHull = {
    0xF3, 0x18, 0xAC, 0xF8, 0x3D, 0x2B, 0x46, 0x1B,
    0x8A, 0xF7, 0xA6, 0x6E, 0x61, 0xDB, 0xB4, 0xD0};

// Import-time policy for added static actors: callers preserve animation/transform bindings,
// platforms and synthetic replacements. Existing imports are refreshed by rebuilding rooms.
// Match source UUIDs, never allocated numeric CMDL IDs, which can be collision-probed.
inline constexpr bool ExcludeSource(uint32_t mrea, const std::array<uint8_t, 16>& model,
                                    bool bound = false) {
  return !bound && mrea == kLandingSite && (model == kHull || model == kUnlitHull);
}
} // namespace PortGunshipScenery
