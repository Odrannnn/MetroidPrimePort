#pragma once

#include "port_dread_material.h"
#include "port_dread_reskin.h"
#include "port_dread_samus_mod.h"
#include "port_remastered_cmdl.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// The "dread-samus" mod (step 6 of the Dread Samus plan): Samus's Power, Varia, Gravity and Phazon bodies
// from the user's own Metroid Dread RomFS, re-skinned onto Prime's two Samus skeletons
// (port_dread_reskin.h), given the suit materials (port_dread_material.h) and written by
// PortRemastered::Converter as CMDL/CSKR/TXTR under the retail body ids, so the game's own
// characters draw them. Phazon is Dread's Metroid Suit (samushyper); the Fusion suits keep their models.
//
// Everything comes and goes through Io, so the same code runs in the game (the NSP and the open
// disc, port_dread_samus_import.cpp) and in the tests (an extracted RomFS and dumped resources).
// The output is the user's, made from their own copies; nothing of either game ships.
namespace PortDreadSamus {

// One body the mod replaces: a Dread suit model re-skinned onto a Prime skeleton and written over
// the retail body CMDL and every CSKR a character binds it with (0 = unused slot).
struct Target {
  const char* suit;      // the Dread model's stem, actors/characters/samus/models/<suit>.bcmdl
  const char* skeleton;  // "cine50" or "game20", for the log
  uint32_t cinf;
  uint32_t cmdl;
  uint32_t cskr[2];
};

// Cutscene bodies (every cutscene ANCS, e.g. 06B034E2: Power/Varia/Gravity on CINF 19C53902; Varia
// is bound with 8C72FDAC in ANCS 863C1B6A too) and BallTransitionsANCS 836C33B3's characters
// PowerSuit/VariaSuit/GravitySuit/PhazonSuit on CINF A496DA02. Phazon is Dread's Metroid Suit.
// Tallon's ANCS BAD39B59 draws its Phazon character with CMDL B333E1D7 on the same skin 4CADCB58,
// so the mod serves the cine50 Phazon body under that id too: the retail mesh would not fit the
// Dread skin.
inline constexpr uint32_t kPhazonCutsceneBody = 0x2782648E;
inline constexpr uint32_t kPhazonCutsceneAlias = 0xB333E1D7;

inline constexpr Target kTargets[] = {
    {"samuspower", "cine50", 0x19C53902, 0xF39476F2, {0x98BBD924, 0}},
    {"samusvaria", "cine50", 0x19C53902, 0x776C1A96, {0x1C43B540, 0x8C72FDAC}},
    {"samusgravity", "cine50", 0x19C53902, 0x57F5C930, {0x3CDA66E6, 0}},
    {"samushyper", "cine50", 0x19C53902, 0x2782648E, {0x4CADCB58, 0}},
    {"samuspower", "game20", 0xA496DA02, 0x4A1A8FD3, {0x21352005, 0}},
    {"samusvaria", "game20", 0xA496DA02, 0xE449874A, {0x8F66289C, 0}},
    {"samusgravity", "game20", 0xA496DA02, 0x57489AD3, {0x3C673505, 0}},
    {"samushyper", "game20", 0xA496DA02, 0x891E00FD, {0xE231AF2B, 0}},
    // The pause screen's doll (ANCS_ItemScreenSamus 72DCEC67, NoARAM.pak): each suit's first
    // part on CINF F76E70A6. The doll then leaves out the suit's other parts, boots, beam and visor.
    {"samuspower", "doll49", 0xF76E70A6, 0xCC25E002, {0xA70A4FD4, 0}},
    {"samusvaria", "doll49", 0xF76E70A6, 0xA602C954, {0xCD2D6682, 0}},
    {"samusgravity", "doll49", 0xF76E70A6, 0x11A5BC8A, {0x7A8A135C, 0}},
    {"samushyper", "doll49", 0xF76E70A6, 0x5644E7A1, {0x3D6B4877, 0}},
};

struct Io {
  // A file of the Dread RomFS by its path ("packs/players/samus.pkg", "textures/actors/...").
  std::function<bool(const std::string& path, std::vector<uint8_t>& out, std::string& error)> dread;
  // A resource of the unmodded Prime disc by type FourCC and id ('CMDL', 'CSKR', 'CINF', 'TXTR').
  std::function<bool(uint32_t type, uint32_t id, std::vector<uint8_t>& out)> retail;
  // Whether any disc resource has this id (new texture ids avoid them).
  std::function<bool(uint32_t id)> retailId;
  // Stores one output file ("<ID>.CMDL", "<ID>.CSKR", "<ID>.TXTR", "<ID>.dds").
  std::function<bool(const std::string& name, const std::vector<uint8_t>& data)> write;
  std::function<void(const std::string& line)> log;     // optional: what the import decided
  std::function<void(const std::string& line)> detail;  // optional: the converter's notes (else `log`)
  std::function<bool()> cancelled;                     // optional
  std::function<void(size_t done, size_t total)> progress;  // optional, per target
};

struct Result {
  size_t converted = 0;               // targets written
  size_t failed = 0;
  std::vector<std::string> files;     // every file written, in order
  // The cannon tip of the cutscene Power body against the 50-joint skeleton's GUN_LCTR and
  // Charge_LCTR, bind pose, metres (for the locator work of step 7); measured only when it converts.
  bool measured = false;
  float cannonTip[3] = {};
  float tipFromGun[3] = {};
  float tipFromCharge[3] = {};
};

// Runs the whole import. Every target must succeed before writes start. False on any failed
// target, cancellation or write failure; the caller must discard staging on false. Body target
// failures are logged and counted in Result::failed; ball failures carry their reason in error.
bool Import(const Io& io, Result& result, std::string& error);

// A retail body's per-vertex weights for the re-skin's votes: the CMDL's positions (the section
// after its material sets) with the CSKR's groups expanded over them.
bool RetailSkinFromCmdl(const std::vector<uint8_t>& cmdl, const std::vector<uint8_t>& cskr,
                        PortDread::RetailSkinRef& out, std::string* error);

// The converter's input for one re-skinned body: a vertex buffer per mesh (already Prime space,
// metres, so the conversion's orient is identity), a mesh per submesh, a material per distinct
// entry of `meshMaterials` (null skips that mesh), and the re-skin's weights per buffer for
// ConvertOptions::boneWeights. False when no mesh is left.
bool BuildModel(const PortDread::ReskinResult& reskin,
                const std::vector<const PortDread::SuitMaterial*>& meshMaterials, PortRemastered::Model& out,
                std::vector<std::vector<std::vector<std::pair<uint32_t, float>>>>& boneWeights,
                 std::string* error);

// Enabled shell only (not sonar/FX), measured uniformly against the retail CMDL bounds;
// rigid weight on the retail ball shell's joint. No humanoid re-targeting.
bool FitBall(const PortDread::Bcmdl& source, const std::vector<uint8_t>& retailCmdl,
             uint32_t joint, PortDread::ReskinResult& out, std::string* error);

}  // namespace PortDreadSamus
