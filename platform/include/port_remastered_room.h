#pragma once

// Writes a Metroid Prime Remastered world's room environments, one
// "<MREA id>.roomenv" per GameCube area (the format is read by port_room_env.h):
// each room's reflection probes with their HDR cubes, the world's tonemap, and
// the room's baked ambient grid (its LTPB), cropped to the room.
//
// Remastered's rooms are not the GameCube's areas. A world's master pak says
// where it puts each room (the RoomController components of its own ROOM), and
// the retail MLVL says where each area sits; Remastered moved the whole world's
// origin, so the shift that lands the most rooms on areas is found first. A room
// is then matched to the area at its place, using the doors (DoorMP1 against the
// area's door objects) to choose among areas that share an origin and as a check.
//
// Nothing here knows where bytes come from or go to, as with ConvertIO in
// port_remastered_convert.h: the retail resources come through RoomIO, the
// Remastered ones from the paks the caller opened.

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "port_remastered_pak.h"

namespace PortRemastered {

// A liquid's surface as its room describes it. The ids are in a pak's byte order. Water and
// poison carry every field of Remastered's WaterRenderVolume (build/mpr/water/B-cpu.md section
// 1), the room's or the loader's default; lava its model and its LavaRenderVolume's values.
struct RoomLiquid {
  enum Type { kWater = 0, kPoison = 1, kLava = 2 };
  int type = kWater;
  std::array<uint8_t, 16> model{};  // a WMDL, or a lava pool's CMDL
  uint8_t features[11] = {1, 1, 1, 1, 1, 0, 1, 0, 1, 0, 0};
  float waves[2][5] = {{0.0f, 0.2f, 0.2f, 1.0f, 0.0f}, {0.0f, 0.2f, 0.2f, 1.0f, 0.0f}};
  float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  float normalDir[2] = {0.0f, 0.0f};
  float normalSpeed = 0.5f;
  float normalScale = 1.0f;
  float fogColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  float fogDistance = 40.0f;
  float material[5] = {5.0f, 0.1f, 0.25f, 1.0f, 0.25f};
  float rain[10] = {0.75f, 1.0f, 0.5f, 0.8f, 0.1f, 1.0f, 0.75f, 1.5f, 0.5f, 1.0f};
  float flow[10] = {0.25f, 10.0f, 0.2f, 5.0f, 1.0f, 0.5f, 60.0f, -1.0f, -1.0f, -1.0f};
  float xrayOpacity = 1.0f;  // the entity's WaterMP1 (13264102), not the render volume's
  std::array<uint8_t, 16> normalMap{}, flowMap{}, rainNoise{};  // TXTRs, all zero for none
  // Lava (build/mpr/water/I-lava-cpu.md Q6): the flow's reach, its period in seconds, the
  // brightness, the flow map's tiling in u, the pattern's scale and the tiling in v.
  float lava[6] = {0.1f, 8.0f, 10.0f, 2.0f, 0.65f, 2.0f};
};

// What the importer makes of a water surface: its mesh (in Remastered's model space) and the
// ids its three maps were written under (0 for none).
struct RoomWaterAssets {
  struct Vertex {
    float pos[3];
    float uv[4];
    uint8_t color[4];
  };
  float boundsMin[3] = {0, 0, 0};
  float boundsMax[3] = {0, 0, 0};
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;
  uint32_t normalMap = 0, flowMap = 0, rainNoise = 0;
  uint32_t rainNoiseWidth = 0, rainNoiseHeight = 0;  // the source texture's
};

// A retail model Remastered keeps under its retail id (10000000-0000-f000-f000-0000XXXXXXXX),
// placed by an entity of a room, as seen from the retail script object of the matched area
// that names the same model and so draws it. `ok`: the object is paired with its entity (the
// n-th object of the area naming the model with the n-th entity of the room carrying it, the
// counts equal), and the model lands where the entity puts it with
// gc = orient * kR2G * remastered + offset, in the model's own space. Not ok: the counts differ.
struct RetailPlacement {
  std::array<uint8_t, 16> model{};  // in a pak's byte order
  uint32_t retail = 0;              // the CMDL id
  uint32_t mrea = 0;
  uint32_t object = 0;  // the script object's editor id
  bool ok = false;
  int entity = -1;  // the room's entity index
  double orient[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  double offset[3] = {0, 0, 0};
  std::string room;
  // The object (an Actor or Platform) names an animation set after the model: retail draws the
  // set's character (ScriptLoader's LoadActor, LoadPlatform), so the model is not what it shows.
  bool animated = false;
};

struct RoomIO {
  // A retail (GameCube) resource by FourCC type ('MLVL', 'MREA') and id, from
  // the unmodded disc, in the decompressed form the game's own loader sees.
  std::function<bool(uint32_t type, uint32_t id, std::vector<uint8_t>& out)> retail;
  // Stores one output file, "<MREA id as 8 upper case hex>.roomenv", ".roomgeo" or
  // ".roomliquid".
  std::function<bool(const std::string& name, const std::vector<uint8_t>& data)> write;
  // Optional; with it each room's static geometry is written too, as
  // "<MREA id>.roomgeo" (read by port_room_geo.h). Converts one Remastered model,
  // named by its id in a pak's byte order, into a GameCube CMDL and gives that
  // CMDL's id; false leaves the model out. It is asked once per model and room,
  // so the caller remembers what it has converted. These files are not counted
  // in `written`.
  std::function<bool(const std::array<uint8_t, 16>& model, uint32_t& cmdl)> model;
  // Optional, as `model` for one rigid piece of a skinned model: the triangles on `joint`
  // (TriangleJoint). Without it, animated actors of more than one bone are left out.
  std::function<bool(const std::array<uint8_t, 16>& model, int joint, uint32_t& cmdl)> piece;
  // Optional; with it each room's liquid surfaces are written too, as
  // "<MREA id>.roomliquid" (read by port_room_liquid.h). `liquid` converts a lava pool's model
  // and gives the CMDL's id, as `model` does; `water` reads a water or poison surface's mesh
  // and writes its maps. Each is asked once per surface; without `water` those are dropped.
  std::function<bool(const RoomLiquid& liquid, uint32_t& cmdl)> liquid;
  std::function<bool(const RoomLiquid& liquid, RoomWaterAssets& assets)> water;
  // Optional; with it each room's particle effect actors of Remastered's own are written too,
  // as "<MREA id>.roomfx" (read by port_room_effects.h). The PART the mod or the disc has for
  // a GENP (in a pak's byte order), 0 for none: such an actor is left out.
  std::function<uint32_t(const std::array<uint8_t, 16>& genp)> effect;
  // Optional; used with `effect`: the retail PARTs the effects step paired with a GENP (in a
  // pak's byte order) that several retail PARTs share (MatchedRetailEffects). A retail Effect
  // object drawing one of them keeps its own retail id, now the mod's conversion of that GENP,
  // and takes the effect actor's particle variables.
  std::function<std::vector<uint32_t>(const std::array<uint8_t, 16>& genp)> matched;
  // Optional; used with `effect` for the one room whose barrier Remastered drives from a
  // controller (the Parasite Queen's rings): each of the ten effects it names is checked
  // against its conversion. `part` is the PART `effect` gave that effect's GENP, `feature` its
  // shader feature (1 << 29 the refracting, two-variable family, 1 << 30 the glow one) and
  // `realVars` the real particle variables the controller animates, as the 16 bytes they are
  // stored as (Variable::guid's order): breakup then motion for the refraction effect, breakup
  // for the glow one. False fails the whole group, which is then left out and the retail rings
  // stay; nothing is added by halves.
  std::function<bool(uint32_t part, uint32_t feature, const std::vector<std::array<uint8_t, 16>>& realVars)>
      barrierEffect;
  // Optional: told each retail model placement (RetailPlacement) of every placed room, from
  // the world's worker thread. The entity of an ok one is not written into the room's geometry.
  std::function<void(const RetailPlacement& placement)> retailModel;
  // Optional: the rooms (by pak name) to write geometry for; all of them without.
  std::function<bool(const std::string& room)> wantsGeometry;
  std::function<void(const std::string& line)> log;  // optional
  std::function<bool()> cancelled;                   // optional; asked before each room
};

// One pak of a world, with its name: the file name without the '!' and '.pak'.
struct RoomPak {
  std::string name;
  const Pak* pak = nullptr;
};

// Remastered's world directory names and the retail MLVL each one is.
struct RoomWorld {
  const char* dir;  // "Intro_Master": the pak is Worlds/MP1/!Intro_Master/!Intro_Master.pak
  uint32_t mlvl;
};
const std::vector<RoomWorld>& RoomWorlds();

// The GENPs (in a pak's byte order) of the particle effect actors (EffectMP1) in `rooms` that
// no connection reaches, each once, in the order first met: those a room's .roomfx may add.
std::vector<std::array<uint8_t, 16>> RoomEffectGenps(const std::vector<RoomPak>& rooms);

// Writes the files of one world. `master` is the world's own pak, `rooms` its
// other paks; a room is a pak holding a ROOM asset, and `_Copy` rooms are
// skipped. A room that cannot be placed or has nothing to write is logged, not
// an error. `written` counts files stored; false (with `error`) when the world
// as a whole cannot be read. A room's reflection probes may name assets that
// live in another world's paks (a room used by two worlds is stored in both,
// not always completely), so `others` is searched for those after the world's
// own paks; nothing is written for them. The order of `rooms` only decides which
// rooms the world shift is read from, which moves it by rounding error.
bool WriteWorldRoomEnvs(uint32_t mlvl, const RoomPak& master, const std::vector<RoomPak>& rooms,
                        const std::vector<RoomPak>& others, const RoomIO& io, int& written, std::string& error);

}  // namespace PortRemastered
