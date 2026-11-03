#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "port_dread_reskin.h"
#include "port_echoes_suits_mod.h"

// The Echoes suits import: Samus's suit bodies from the user's own Metroid Prime 2: Echoes disc
// (GameCube, G2ME01) in place of Prime's, written as the "echoes-suits" mod.
//
// Echoes keeps Prime's formats one step on: its PAKs hold LZO-compressed resources, its CMDLs are
// version 4 (matrix-index bytes in every vertex, two more words per material) and its CSKR/CINF
// carry extra data Prime does not read. Prime's own skeletons are Echoes' with fewer joints, so a
// body converts without re-skinning: every vertex keeps its weights, each Echoes joint renamed to
// Prime's joint of the same name (or its nearest ancestor that has one). Textures are the same
// format and are copied under their Echoes ids.
//
// Everything comes and goes through Io, so the same code runs in the game
// (port_echoes_suits_import.cpp) and in the tests. The output is the user's, made from their own
// copies; nothing of either game ships.
namespace PortEchoesSuits {

// LZO1X: `destLen` is the capacity on entry and the bytes written on return. False on a corrupt
// stream (what was decoded so far is still counted in destLen).
bool LzoInflate(const uint8_t* source, size_t sourceLen, uint8_t* dest, size_t& destLen);

// One compressed Echoes PAK entry: a big-endian u32 size, then segments, each a big-endian s16
// length and that many bytes: LZO1X for a positive length (it inflates to min(0x4000, what is
// left)), stored bytes for a negative one.
bool Decompress(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::string* error);

// An Echoes CMDL (version 4) as Prime's (version 2): the materials' PBR bit (14) and the vertex
// flags' matrix-index bits (24-31) cleared and their two extra words dropped, each surface header
// without its extra word and its display list without the matrix-index bytes. `textures` gets the
// TXTR ids of every material set, sorted, without repeats.
bool ConvertCmdl(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, std::vector<uint32_t>& textures,
                 std::string* error);

struct EchoesJoint {
  uint32_t id = 0;
  uint32_t parent = 0;
  std::string name;
};
// An Echoes CINF: u32 count, {u32 id, u32 parent, f32 position[3], f32 rotation[4],
// f32 local rotation[4], u32 n, u32 links[n]}, u32 count, u32 build order[], u32 count,
// {string name, u32 id}. Joints in file order, named.
bool ParseCinf(const uint8_t* data, size_t size, std::vector<EchoesJoint>& out, std::string* error);

// An Echoes CSKR on Prime's skeleton `to`: each weight moves to the Prime joint of its Echoes
// joint's name (the Varia shoulder pads' L/R_varia_SDK to Prime's L/R_varias2_SDK), else of its
// nearest named ancestor's; weights landing on one joint add up. Echoes' footer is replaced by
// Prime's. `folded` (optional) gets "echoes->prime" for every joint that had no namesake.
bool ConvertCskr(const std::vector<uint8_t>& in, const std::vector<EchoesJoint>& from,
                 const PortDread::PrimeSkeleton& to, std::vector<uint8_t>& out, std::vector<std::string>* folded,
                 std::string* error);

// One body: Echoes' CMDL and CSKR on its CINF, written over Prime's on Prime's CINF.
struct Target {
  const char* suit;      // Echoes' suit, for the log
  const char* skeleton;  // "cine50" or "game20"
  uint32_t echoesCmdl, echoesCskr, echoesCinf;
  uint32_t cmdl, cskr, cinf;
};
// Power stays Prime's. Varia is Echoes' Varia, Gravity its Dark Suit (the Dark+Gravity body),
// Phazon its Light Suit. Cutscene bodies on Echoes' A27AFE83 / Prime's 19C53902; in-game ones on F86193FE /
// A496DA02 (BallTransitionsANCS).
inline constexpr Target kTargets[] = {
    {"Varia", "cine50", 0xCDF1C541, 0xA6DE6A97, 0xA27AFE83, 0x776C1A96, 0x1C43B540, 0x19C53902},
    {"Dark", "cine50", 0x3A93B6F2, 0x51BC1924, 0xA27AFE83, 0x57F5C930, 0x3CDA66E6, 0x19C53902},
    {"Light", "cine50", 0x726FB01A, 0x19401FCC, 0xA27AFE83, 0x2782648E, 0x4CADCB58, 0x19C53902},
    {"Varia", "game20", 0xE31FD3AD, 0x88307C7B, 0xF86193FE, 0xE449874A, 0x8F66289C, 0xA496DA02},
    {"Dark", "game20", 0xC8146EFF, 0xA33BC129, 0xF86193FE, 0x57489AD3, 0x3C673505, 0xA496DA02},
    {"Light", "game20", 0xACDFD2CA, 0xC7F07D1C, 0xF86193FE, 0x891E00FD, 0xE231AF2B, 0xA496DA02},
};
// Copies the mod also serves (port_dread_samus.h): the cutscene Varia skin under ANCS 863C1B6A's
// 8C72FDAC, and the cutscene Phazon body under Tallon ANCS BAD39B59's B333E1D7.
struct Copy {
  uint32_t type;
  uint32_t from, to;
};
inline constexpr Copy kCopies[] = {
    {0x43534B52, 0x1C43B540, 0x8C72FDAC},  // 'CSKR'
    {0x434D444C, 0x2782648E, 0xB333E1D7},  // 'CMDL'
};

struct Io {
  // A resource of the Echoes disc by type FourCC and id, decompressed.
  std::function<bool(uint32_t type, uint32_t id, std::vector<uint8_t>& out)> echoes;
  // A resource of the unmodded Prime disc ('CINF', 'TXTR').
  std::function<bool(uint32_t type, uint32_t id, std::vector<uint8_t>& out)> retail;
  // Whether any Prime disc resource has this id (an Echoes texture must not shadow one).
  std::function<bool(uint32_t id)> retailId;
  // Stores one output file ("<ID>.CMDL", "<ID>.CSKR", "<ID>.TXTR").
  std::function<bool(const std::string& name, const std::vector<uint8_t>& data)> write;
  std::function<void(const std::string& line)> log;     // optional
  std::function<bool()> cancelled;                     // optional
  std::function<void(size_t done, size_t total)> progress;  // optional, per target
};

struct Result {
  size_t converted = 0;            // bodies written
  std::vector<std::string> files;  // every file written, in order
};

// Converts every target, then writes them, their copies and their textures. False on any failure
// or cancellation, with the reason in `error`; the caller then discards what was written.
bool Import(const Io& io, Result& result, std::string& error);

}  // namespace PortEchoesSuits
