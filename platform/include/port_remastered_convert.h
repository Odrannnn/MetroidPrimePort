#pragma once

// Converts one Metroid Prime Remastered model into the files a port mod holds:
// a GameCube CMDL (and CSKR, for a skinned retail model) plus its textures.
//
// The Remastered model replaces a retail one, so the retail model decides
// everything the game's own code depends on: its material sets, vertex
// descriptors, skin bones and resource id. The Remastered side supplies the
// geometry and the maps. A material becomes one of two things: a PBR material
// (base, metal/roughness, normal and emissive maps, flagged for the port's PBR
// shading, with the port's material record appended) or, where that cannot be
// done (a blended effect, no base map), the retail material with its texture
// slots refilled.
//
// Nothing here knows where bytes come from or go to. Retail resources, decoded
// Remastered textures and the output files all pass through ConvertIO, so the
// same code serves the in-game importer and the test tool.
//
// The output is derived from the player's own copy of both games and is for
// their use only; nothing produced here may ship.

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "port_remastered_cmdl.h"
#include "port_remastered_image.h"
#include "port_remastered_report.h"

namespace PortRemastered {

struct ConvertOptions {
  uint32_t retail = 0;  // CMDL id the model replaces
  std::string source;   // a label for the Remastered model, for the materials report
  // The model replaces nothing (a piece of room geometry): `retail` is only the
  // id it is written under, and every surface gets one opaque lit PBR material.
  // Surfaces with no base map are dropped, having no retail material to keep.
  bool standalone = false;
  // Standalone PBR materials also carry the model's lightmap UV (UV0.zw) as an extra texcoord
  // attribute, named by an 'LMUV' record trailer.
  bool lightmapUv = false;
  int nativeMax = 0;  // largest edge of a native .dds, 0 for the converter's own
  int lod = 0;        // level of detail to convert, 0 the finest (a model with fewer has none)
  // 0 or more: keep only the triangles TriangleJoint puts on this joint, one rigid piece of
  // a skinned model that the room's animation moves (standalone only).
  int joint = -1;
  // A lava pool's LavaRenderVolume values (RoomLiquid::lava), which the game puts in the
  // pool material's CCH0 and CCH1 (x, y, z of each) in place of the material's own.
  bool hasLava = false;
  float lava[6] = {};
  // gc = orient * remastered + offset. The default is Remastered's y-up frame
  // onto the GameCube's z-up one.
  double orient[3][3] = {{-1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, {0.0, 1.0, 0.0}};
  double offset[3] = {0.0, 0.0, 0.0};
  // Material name substrings to drop: the low-detail copies a model carries.
  std::vector<std::string> skip = {"simple"};
  int material = -1;      // force every surface onto this retail material
  bool pbr = true;        // false keeps every material on the retail TEV path
  int maxTexture = 2048;  // largest edge of a TEV path texture
  // Remaps U of one role's coordinates (TEV path): from [u0, u1] onto [lo, hi].
  bool squeeze = false;
  std::string squeezeRole;
  double squeezeFrom[2] = {0.0, 1.0};
  double squeezeTo[2] = {0.0, 1.0};
  // Every CSKR id a character binds this model to; empty for a static model.
  // The weights are written under each one whose skeleton covers the first's.
  std::vector<uint32_t> skins;
  // Ids the output is written under instead, for a second look of one retail
  // model (another suit's ball, another beam's gun): the CMDL's (0 for
  // `retail`), and one per id in `skins`, in that order (empty for the same).
  // `retail` and `skins` are still read for the materials and the skeleton.
  uint32_t outputModel = 0;
  std::vector<uint32_t> outputSkins;
  // A texture's id is hashed from a tag naming its source. These wrap the
  // Remastered texture's uuid in that tag; the test tool sets them to
  // reproduce the reference converter's ids, the game leaves them empty.
  std::string texturePrefix;
  std::string textureSuffix;
  // Optional: receives the converted level of detail as one plain mesh (the VMSH blob a
  // model particle draws, see build/mpr/vfx/DESIGN.md), in the written CMDL's model space:
  // big-endian u32 version (1), vertex count, triangle count; per vertex position[3],
  // normal[3], uv[2] as f32; then the indices, u16 up to 65535 vertices, else u32. Back-face
  // copies are left out. Cleared when the model has no usable mesh.
  std::vector<uint8_t>* vmsh = nullptr;
};

struct ConvertIO {
  // A retail resource by type FourCC ('CMDL', 'CSKR', 'TXTR') and id, from the
  // unmodded disc. False when there is none.
  std::function<bool(uint32_t type, uint32_t id, std::vector<uint8_t>& out)> retail;
  // Whether any retail resource has this id, so a new texture never takes one.
  std::function<bool(uint32_t id)> retailId;
  // The top mip of a Remastered texture as RGBA8. The id is in a pak's byte
  // order (what IdToString prints), not the order a model stores it in.
  std::function<bool(const ModelUuid& id, Image& out, std::string& error)> texture;
  // Optional: the top mip of every face of a Remastered cube map as linear RGBA floats,
  // 6 faces of edge * edge texels (DecodeTxtrCubeLinear). Without it no material gets a
  // reflection cube of its own.
  std::function<bool(const ModelUuid& id, uint32_t& edge, std::vector<float>& rgba, std::string& error)> cube;
  // Optional: a Remastered 3D texture of 64 slices of 64 x 64 as one 512 x 512 RGBA8 atlas, slice z at tile
  // (z % 8, z / 8) (DecodeTxtrVolume). Without it a material that needs one keeps the generic shader.
  std::function<bool(const ModelUuid& id, Image& out, std::string& error)> volume;
  // Optional, for converters that run side by side into one folder: asked once
  // for each PBR map a converter is about to write, by the id it will have;
  // false when another converter has taken it, and this one then only names it.
  std::function<bool(uint32_t id)> claim;
  // Stores one output file: "<ID>.CMDL", "<ID>.CSKR", "<ID>.TXTR", "<ID>.dds".
  std::function<bool(const std::string& name, const std::vector<uint8_t>& data)> write;
  std::function<void(const std::string& line)> log;  // optional
  // Optional, what an earlier import converted (a texture's tag stays the same as long as its
  // output does): `recall` gives the value stored under a key, `relink` puts the files stored
  // with it in the folder (false when it cannot), and `remember` stores a value with the files
  // just written for it. Keys are "tex:<tag>" (the value is the texture's id as 8 hex digits,
  // or "-" for none) and facts about a Remastered texture ("size:", "mean:", "metal:" + map).
  std::function<bool(const std::string& key, std::string& value)> recall;
  std::function<bool(const std::string& key)> relink;
  std::function<void(const std::string& key, const std::string& value, const std::vector<std::string>& files)>
      remember;
  // Optional: called once per output material, in output order (the materials report).
  std::function<void(const MaterialDecision& decision)> decision;
};

// Holds what is shared between the models of one import: the textures already
// written, so a map used by several models is converted once.
class Converter {
public:
  explicit Converter(ConvertIO io);
  ~Converter();
  Converter(const Converter&) = delete;
  Converter& operator=(const Converter&) = delete;

  bool Convert(const Model& model, const ConvertOptions& options, std::string& error);

  // How many output materials took the PBR path and how many kept the TEV one.
  int PbrMaterials() const;
  int TevMaterials() const;

private:
  struct State;
  State* m_state;
};

} // namespace PortRemastered
