#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class CGameArea;
class CStateManager;
class CVector3f;

// A room's liquid surfaces (water, poison, lava). A mod supplies them per area as
// <MREA id>.roomliquid; the port draws one in place of the fluid plane of the area's water
// object that stands where it does. The object itself is untouched: its splashes, ripples'
// sounds and damage are the game's.
//
// Water and poison are Remastered's WaterRenderVolume: its mesh and its material's fields
// as the room has them (build/mpr/water/B-cpu.md section 1, G-rainflags.md), drawn by
// aurora's water shader (aurora/water.hpp). Lava is a CMDL whose material is PBR kind 6.
//
// The file is little endian:
//   'MPRL', u32 version (5), u32 surfaces
//   surface: u32 type (0 water, 1 poison, 2 lava), u32 CMDL id (lava; 0 otherwise),
//            u32 owner (editor id, no layer, of the retail Water its entity matched; 0 none),
//            f32 transform[12] (rows of mesh -> area; a water mesh is in Remastered's
//            model space, so this includes the (x, y, z) -> (-x, z, y) axis change)
//   then, for water and poison only, a Water block:
//     u32 vertices, u32 indices, f32 boundsMin[3], f32 boundsMax[3] (mesh space)
//     u8 features[11] (SLdrWaterFeatureFlags, loader byte order b0..b10), u8 pad
//     f32 waves[2][5] (SLdrWaterWaveParams: angle in degrees, +4, +8, +0xC, +0x10)
//     f32 tint[4], normalDir[2], normalSpeed (522ABD8A), normalScale (D4483C7E),
//         fogColor[4], fogDistance, material[5] (SLdrWaterMaterialData +0xF0..+0x100),
//         rain[10] (RainDropRipple +0x10..+0x34), flow[10] (Flow +0x38..+0x5C),
//         xrayOpacity (its WaterMP1's 13264102, CScriptWaterMP1+0x548; H-opacity.md)
//     u32 normalMap, flowMap, rainNoise (TXTR ids, 0 for none; linear data),
//         u32 rainNoiseWidth, rainNoiseHeight (the source texture's, not the TXTR's)
//     vertex[vertices]: f32 pos[3], f32 uv[4] (TEXCOORD_0), u8 color[4] (COLOR, RGBA)
//     u32 index[indices] (triangle list)
//   u32 filters, then per WaterMP1: f32 position[3] (area space), f32 color[4] (its
//   CC1AF173, the CScriptWaterMP1+0x520 camera filter colour; build/mpr/water/E-under.md)
// Every value is the room's, or the loader's default where the room leaves it out. The
// transform's translation is where the water object is; the surface is drawn at the
// height the game gives its own.
namespace PortRoomLiquid {

struct Water {
  struct Vertex {
    float pos[3];
    float uv[4];
    uint8_t color[4];
  };
  float boundsMin[3];
  float boundsMax[3];
  uint8_t features[11];
  float waves[2][5];
  float tint[4];
  float normalDir[2];
  float normalSpeed;
  float normalScale;
  float fogColor[4];
  float fogDistance;
  float material[5];
  float rain[10];
  float flow[10];
  float xrayOpacity;
  uint32_t normalMap, flowMap, rainNoise;
  uint32_t rainNoiseWidth, rainNoiseHeight;
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;
};

struct Surface {
  uint32_t type;
  uint32_t model;
  uint32_t owner; // the editor id (no layer) of the retail Water that draws it; 0 when unmatched
  float transform[12];
  Water water; // type 0 and 1
};

struct Filter {
  float position[3];
  float color[4];
};

// "1A2B3C4D.roomliquid" (any case) -> 0x1A2B3C4D.
bool ParseFileName(const std::string& fileName, uint32_t& id);
bool Parse(const std::vector<uint8_t>& data, std::vector<Surface>& out, std::vector<Filter>& filters,
           std::string& error);

// The areas in memory now; frees the surfaces of areas that left.
void SetLoadedAreas(const uint32_t* mreas, size_t count);
// Once a frame, `dt` game seconds on: the water's clock (CStateManagerGameData's, which
// wraps at 5000 s) that its normal maps, waves and flow scroll with.
void Advance(float dt);
// Draws the surface of the area's water object `uid` (editor id `editorId`), which is at
// `position` and holds `fluidType` (CFluidPlane::EFluidType): the surface the importer
// matched to that object, else the nearest unmatched one of its fluid. False when there is
// none or its model has not loaded yet, and the object draws its own fluid plane.
bool Draw(const CStateManager& mgr, const CGameArea& area, uint32_t uid, uint32_t editorId,
          const CVector3f& position, int fluidType, float surfaceZ);
// Remastered's camera filter for the camera inside the area's water object `uid` at
// `position`: true, with its colour, when the area has a file with a water object
// within reach. CCameraManagerMP1::UpdateFilters then multiplies the screen by that
// colour (off in the X-Ray and Thermal visors) and sets no fog; false keeps retail's.
bool CameraFilter(const CGameArea& area, uint32_t uid, const CVector3f& position, float color[4]);
// Lets go of every model (the mods folder is about to change).
void Reset();

// MP_ROOM_LIQUID=0|1, the console's `roomliquid`.
void SetEnabled(bool enabled);
bool Enabled();
// Areas with a file, their surfaces, and those drawn in the last frame.
void Stats(int& areas, int& surfaces, int& drawn);

} // namespace PortRoomLiquid
