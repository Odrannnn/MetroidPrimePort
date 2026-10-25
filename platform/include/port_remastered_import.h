#pragma once

#include "port_remastered_effect.h"

// Builds the "remastered-models" mod inside the port, from the user's own
// copy of Metroid Prime Remastered (.nsp) and their own Switch key file.
//
// Nothing is extracted to disk on the way: the paks are read straight out of
// the encrypted image (port_remastered_nsp.h), the retail models they replace
// come off the unmodded disc that is already open, and each model in
// port_remastered_table.h goes through port_remastered_convert.h. The output
// is the user's, for their use only; the port ships none of it.
//
// The mod is written to a hidden staging folder in the import's root and only
// takes the place of <root>/remastered-models when nothing is reading the one it
// replaces: at the next start, before mods are scanned (ApplyPendingImport), or in
// the background once the mods are unloaded (StartInstall, on the import's own
// thread). The root is the mods folder on desktop and the app's own internal
// storage on Android (port_remastered_storage.h); a legacy <mods>/remastered-models
// keeps loading as the fallback until a full import has written the private one.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PortRemastered {

struct ImportState {
  bool running = false;
  bool finished = false;   // a run ended; ok, message and lines are its result
  bool ok = false;         // at least one model converted and the mod is staged
  bool cancelled = false;
  int done = 0;            // models tried so far
  int total = 0;
  int failed = 0;
  // Where a full import is: step `step` of `steps` (0 when not counted, as for a movies-only
  // import), and how far into that step, out of stepTotal (0 when it cannot say).
  int step = 0;
  int steps = 0;
  int stepDone = 0;
  int stepTotal = 0;
  bool cancelling = false; // cancel was asked for and the import has not stopped yet
  bool installing = false; // StartInstall() is moving the finished import into place; it can't be cancelled
  std::string message;     // what it is doing, or how it ended
  std::vector<std::string> lines;  // the newest kImportMaxLines of its log
};

inline constexpr size_t kImportMaxLines = 200;

// The parts of the game the panel leaves out of the next import, on top of the
// MP_REMASTERED_* switches (docs/MODS.md): a part is imported only when both want it.
struct ImportParts {
  bool hud = true;
  bool text = true;
  bool effects = true;
  bool movies = true;
  bool gallery = true;
  bool geometry = true; // the rooms' static geometry (MP_REMASTERED_GEOMETRY)
};
void SetImportParts(const ImportParts& parts);

// The mod folder an import ends up as.
inline constexpr const char* kImportModName = "remastered-models";

// An import is made in stages, and a re-import links in the previous import's output of every
// stage whose number, options and inputs are unchanged instead of making it again. Bump the
// number of the stage a change needs re-imported (all of them that it touches):
namespace ImportStage {
// port_remastered_convert/cmdl: how models and materials are written (models, effects, rooms,
// room models, HUD).
// 57: HoloGlass_DX11 (03407341, kind 11) carries DIFC (PBR8 trailer), blend One / InvSrc1Color.
// 56: HoloGlass (2f95a061) TCH1 is a raw colour map (sRGB, alpha kept).
// 55: textures are used as authored (no flat-texture statistic, no :bare retail fallback).
// 54: the glow added without a room's tone data is unscaled (kPbrEmissive 1, was 0.10).
// 58: the beam glow (kind 9) is drawn as kind 9 without vertex colours too (no ramp mean).
// 59: a retail TEV material's diffuse slot takes the Remastered base map as authored (no AO,
//     cavity or sheen baked into it).
// 60: the materials report has the mesh class, the blend written and the texcoord slots.
// 61: a retail model's materials get a texcoord slot for every Remastered set (no fold onto 0).
// 62: a retail model's PBR material blends as its mesh class does, not as its retail material.
// 63: a retail model's materials read vertex colour as a room's do (tinted, or a vertex-colour kind).
// 64: ... and its alpha, unlit, mask and height as a room's, sorted and depth-written by its class.
// 65: ... and its kind, macro and second layer as a room's (no kind dropped).
// 66: a retail model's fx material with a port kind draws as its Remastered shader, not its retail TEV.
// 67: 5854e91b and bda080c0 draw as Remastered's unlit Surface with a reflection cube (kind 38).
// 68: af330de4 draws as Remastered's unlit DIFT x DIFC x vertex colour (kind 39).
// 69: 664ee5ca draws as Remastered's unlit matcap, DIFT x DIFC + REFS x REFV (kind 40).
// 70: 67053ffe draws as Remastered's unlit METL-weighted BCLR + ICAN (kind 41).
// 71: 8c7a939f draws as kind 25 (its maths), 1fbafbca as kind 42 (25 plus ICAN x ICNC).
// 72: 084a8db5 draws as kind 43 (HoloGlass less TCH1/TCH2/DIFC).
// 73: 0c0b6119 draws as kind 31 (fb2bc671's maths, DIFC for ICNC).
// 74: every retail fx material draws through its Remastered shader (kShaderFxNoKind gone).
inline constexpr int kConverter = 74;
// What a converted texture holds: port_remastered_image/txtr/dds/astc, and Converter's Get and
// Cube. Each converted texture is kept across imports under its tag and this number, so a
// kConverter bump re-imports the models without converting their textures again.
// 5: a BC6H colour map (the skies) is written as BC6H (RGBA16F without BC), its 8-bit
//    form decoded by value instead of by the halves' high bytes.
inline constexpr int kTextures = 5;
// 17: the inventory and cinematic guns draw the animated beam glow (kConverter 58).
// 16: placements bake as one only when float-identical; any difference places per object.
// 15: a per-instance retail model converts once, untransformed; retailplacements.bin places
//     each of its objects (port_retail_placement.h).
// 14: normals go through the orient's inverse transpose (a non-uniform orient kept them skewed).
// 13: a retail model placed by the rooms takes its placements' orient as well as their offset.
// 12: the retail models Remastered keeps under their retail ids, placed by the rooms, are
//     converted too (retailmodels stage); 3C26F42B's table row goes.
// 11: the Elite Quarters stasis tank 3C26F42B is in the table.
// 10: a plain cutout over a retail fx material is drawn opaque, cut by the shader.
// 9: a ColorUnlit mesh (the door shields) gets a back copy.
// 19: remove the obsolete model-driven Queen barrier.
// 20: a skin joint whose pivot sits on one of its retail bones drops that bone's ancestors
//     (the Parasite Queen's jaw); the 12 artifacts each take their own Remastered model
//     (CHPR FAA9C708's model node N is name-pool character N), Truth/Lifegiver/Chozo/World added;
//     the elevator platforms 32103B70/F33A6018 take 6FA561D0's model, not a Frigate rubble sheet.
// 21: intra-Mines elevator C0A4DF30 uses the shipped E8E0AB8D platform, not a VFX model.
inline constexpr int kModels = 21;
// 63: ELPS reads its CNST surface flag; the system tween is 0 only at a 0x1000000 lifetime.
// 62: NEGT, CRCV, SPEV, ELPS, ROTV, PNCV, KEYF, SLTW, CRLN octaves, KESP, one-stop GRAD, PAP0/9,
//     ADV9 and IAV0..3 are converted; PLNV counts come from MAXP.
// 30: PLNV writes its shuffler size (0 none), EMRV under XFMD 5 with a constant POFS is PEMV mode 2.
// 29: PLNE, PLNV (plane emitters) and EMRV (PEMV) are converted, no longer dropped.
// 28: CREL(LTHN) is retail's CLTN; MPAC, MPCN and ANCM are the exact port-only PMAC, PMCN and PANM.
// 27: MPXV is converted as the port-only PMXV, no longer dropped.
// 26: ABS_, COSE, RSGN, IRSN (PIRS) and CODE are converted, no longer dropped.
// 25: swoosh GRAD is the exact PGRD element, no longer a 101-key KEYP bake.
// 64: DIVD/T2S_/SCLE/SDTR parse, T2S_ and SCLE convert, swoosh SUST, ELC2 NFOM byte.
// 65: EBDM read as a byte, GPUA exact, ELC2 COLR GRAD lifetime, recipes 20a05041/3fe57e12/f0e276eb/a0482072.
// 67: recipes 8278c22d/7498e54a (VFX_Pyro, the vfx Pyro feature).
// 68: recipe d749057f (VFX_Disintegration, the vfx Disintegration feature).
// 69: recipes cc155f99/04276ae1/d955396d (VFX_FrameBuffer_Indirect_*, the vfx SceneWarp feature).
// 70: recipe 9e2cbd46 (VFX_VisorSplat, the vfx VisorSplat feature).
// 71: recipe 9e0605cd (VFX_IceChargeBeam, the vfx IceChargeBeam feature; VMSH v2, VPRM).
// 72: Remastered's swooshes the disc's WPSC has no field for (plasma ASW1/ASW2) are imported under synthetic ids,
//     and the WPSC is written into the mod with the field added (AddRetailLink).
// 73: recipe 1c651f97 (VFX_IceChargeCollision_Unlit, the vfx IceChargeCollision feature; VPRM v2, SMTR colours).
// 74: the root PART's visor effect data (VESN/VESM as PVSD/PVSN/PVSM) for the VisorGoo splat manager.
// 75: recipes b5e12668 and d85d4169 (VFX_VisorRunOff, the vfx VisorRunOff/VisorRunOffLit features; CCH0..4).
// 76: a generator's SVCn/SVRn/SVIn/SVVn tag-1 ILPT is the system's PSLT, which PortEvalSys holds while it runs (PLPT offset 0).
// 77: recipe 390fc33b (VFX_IceChargeCollision_Lit, the vfx IceChargeCollisionLit feature; the runtime's clustered
//     light gather from the room lights and the probe from the room env).
// 78: a VFX material's BC5 texture keeps its red and green as (R, G, 0, 1), not grey plus alpha.
// 79: INTS is the system scale (port-only PINS).
// 80: INTR and INTT are the system orientation and translation (port-only PINR/PINT); XFMD 5's INTT is POFS.
// 85: DFOG 1 is the port-only PDFG (the particles are not volumetric-fogged).
// 88: ZTES/IVSE/ISVF/ITPE, DBIS, SHTM/SHTT/PMOV/PMPV/SDTR as port-only tags; a DVVN sprite without SIZE is 1.0.
// 89: a PART without ITPE under XFMD 2/5 gets Remastered's default 1 (PITP flagged 0x100).
// 90: a swoosh generator's SHTM 1 (shutdown destroys the live particles) is the port-only PSHM.
// 91: ISDT 0 (a shutdown reaches the children as their own ShutdownEffect) is the port-only PISD 0.
// 92: the GENPs of the rooms' own effect actors, under RoomEffectId (for the .roomfx files).
// 93: LIT_ 1 on a VFX_*_Lit sprite shader is the vfx ParticleLit feature (AlreadyExposed under IEXP).
// 94: a ParticleLit material is never AlreadyExposed (the lit perm reads no inverse exposure).
// 95: BLIT 1 (a lit sprite takes the room's baked probe) is the port-only PBLT 1.
// 97: a sprite/swoosh/ELC2 VMAT takes its MATI's EBlendMode over the PBDM/SBDM/EBDM row (Fogged pins the row's fog).
// 98: a PART keeps its visor effect override data (PVSC/PVMC/PVTC/PVBS/PVDT).
// 100: missile and energy tank pickups paired (#loose-chpr, kMatchedEffects).
// 101: the lava spit's two PARTs paired with one GENP (#shared-genp), the actor's variables on them.
// 102: visor rain splash (VFX_VisorRain 0ec41df0, BC978532 paired).
// 103: the FlickerBat's 0B9E48EB paired with 3b29b0b3 (#visual-inferred: inferred from the
// look, not proven by any CHPR or other serialized reference).
// 106: a VMSH whose material's shader reads COLOR (perm 016) is tinted by its vertex colour (VMSH flag 4).
inline constexpr int kEffects = 106;   // port_remastered_effect_import and the particle converters
// 20: two rooms matching one area write only the closer match's files, not the later room's.
// 21: roomenv version 22, the world master's hints driven by its camera volumes (the table in every roomenv).
// 22: a master's InventoryActivator fired by the camera going under water drives its hints (kSenderCameraWaterItem).
// 23: a hint (grade, backlight, fog) marks whether it is the master's, so the room's own wins (HintOutranks).
// 24: a room's effect actors of Remastered's own, as <MREA>.roomfx (port_room_effects.h).
// 25: .roomfx v2 carries each actor's particle variable values (property 3def0de6).
// 26: roomenv version 23, the world shift (the fog's noise is read in Remastered's world).
// 28: retail model placements (RetailPlacement); their entities leave the room's geometry.
// 29: a retail model's objects pair with its entities by order (RetailPlacement orient, entity).
// 30: roomenv version 26, the EnvironmentFX colour intensity (a RelayAutoFire-fired density controller).
// 34: a retail Effect object drawing a PART its GENP is shared with takes the actor's variables (the lava spit).
// 36: actors with a model of Remastered's own under a retail-style id no retail object names
//     are drawn, hiding the retail Actors they stand in for (Central Dynamo's grates).
inline constexpr int kRooms = 36;      // Remastered stand-in actors (m_syntheticEntities)
inline constexpr int kRoomModels = 1;  // the rooms' own models and their levels of detail
inline constexpr int kText = 1;        // port_remastered_text, and the font (port_remastered_font)
// 9: envfx.bin, the EnvironmentFX flake textures (port_envfx_file.h).
// 10: shell.bin, the silhouette-shell textures (port_shell_file.h).
inline constexpr int kHud = 10;        // port_remastered_hud, port_remastered_map, envfx.bin, shell.bin
inline constexpr int kMovies = 0;      // port_remastered_movie
inline constexpr int kGallery = 0;     // the gallery pictures (port_gallery)
}  // namespace ImportStage

// The USA retail Missile WPSC's COLR collision response defaults to the
// Remastered Missile WPSM's COLR response's DEFS GENP. Keep this one pinned
// role mapping separate from the broader projectile APSM/ASW rules below.
namespace ImportDetail {
inline constexpr uint32_t kMissileWpsc = 0x466208AB;
inline constexpr uint32_t kMissileCrsc = 0xD27D6EDD;
inline constexpr uint32_t kMissileDefaultPart = 0xA594B6AE;
// Pak asset-index / EffectPairing order (printed UUID byte order).
inline constexpr std::array<uint8_t, 16> kMissileCrsm = {0xAC, 0xE0, 0xF7, 0xA8, 0x67, 0xC6, 0x4B, 0xD6,
                                                         0xB3, 0xE3, 0xC1, 0x8E, 0x1D, 0xC1, 0x10, 0x54};
inline constexpr std::array<uint8_t, 16> kMissileDefaultGenp = {0xE2, 0xCF, 0x01, 0x68, 0x90, 0xA2, 0x4E, 0xFA,
                                                                0x82, 0x3C, 0xAB, 0x99, 0xC0, 0x2D, 0x85, 0x50};
// The link records carry the same GUIDs in uuid.UUID(...).bytes_le order.
inline constexpr std::array<uint8_t, 16> kMissileCrsmLink = {0xA8, 0xF7, 0xE0, 0xAC, 0xC6, 0x67, 0xD6, 0x4B,
                                                             0xB3, 0xE3, 0xC1, 0x8E, 0x1D, 0xC1, 0x10, 0x54};
inline constexpr std::array<uint8_t, 16> kMissileDefaultGenpLink = {0x68, 0x01, 0xCF, 0xE2, 0xA2, 0x90, 0xFA, 0x4E,
                                                                    0x82, 0x3C, 0xAB, 0x99, 0xC0, 0x2D, 0x85, 0x50};

struct MissileDefaultRoleInput {
  std::string_view weaponName;  // the lower-case FrameKey, not a filename guess
  uint32_t retailWpscId = 0;
  bool retailWpscExists = false;
  bool retailCrscExists = false;
  bool retailPartExists = false;
  bool remasteredWpsmExists = false;
  bool remasteredCrsmExists = false;
  bool remasteredGenpExists = false;
  const uint8_t* retailWpsc = nullptr;
  size_t retailWpscSize = 0;
  const uint8_t* retailCrsc = nullptr;
  size_t retailCrscSize = 0;
  const uint8_t* remasteredWpsm = nullptr;
  size_t remasteredWpsmSize = 0;
  const uint8_t* remasteredCrsm = nullptr;
  size_t remasteredCrsmSize = 0;
};

struct MissileDefaultRolePair {
  std::array<uint8_t, 16> genp{};
  uint32_t retailPart = 0;
};

// The USA 1.00 PowerBall collision-response default is independent of its
// APSM projectile effect: retail WPSC COLR -> CRSC DEFS PART, and Remastered
// WPSM COLR -> CRSM DEFS GENP. These exact identities prevent cross-region or
// other-default pairing.
inline constexpr uint32_t kPowerBallWpsc = 0x87D4CA7C;
inline constexpr uint32_t kPowerBallCrsc = 0x39EB7161;
inline constexpr uint32_t kPowerBallDefaultPart = 0x09C2016D;
inline constexpr std::array<uint8_t, 16> kPowerBallCrsm = {0x76, 0x69, 0x0A, 0x3F, 0x48, 0x95, 0x4F, 0xD9,
                                                           0x8C, 0x6E, 0xB3, 0x4F, 0x15, 0x6D, 0xEC, 0x58};
inline constexpr std::array<uint8_t, 16> kPowerBallDefaultGenp = {0xDA, 0x57, 0xEF, 0xF5, 0xBB, 0xEB, 0x41, 0xDE,
                                                                 0x9C, 0x9B, 0xCA, 0x90, 0xEF, 0x19, 0x22, 0x6E};
// Reversed-guid byte order in Remastered COLR/DEFS links.
inline constexpr std::array<uint8_t, 16> kPowerBallCrsmLink = {0x3F, 0x0A, 0x69, 0x76, 0x95, 0x48, 0xD9, 0x4F,
                                                              0x8C, 0x6E, 0xB3, 0x4F, 0x15, 0x6D, 0xEC, 0x58};
inline constexpr std::array<uint8_t, 16> kPowerBallDefaultGenpLink = {0xF5, 0xEF, 0x57, 0xDA, 0xEB, 0xBB, 0xDE, 0x41,
                                                                      0x9C, 0x9B, 0xCA, 0x90, 0xEF, 0x19, 0x22, 0x6E};

struct PowerBallDefaultRoleInput {
  std::string_view weaponName;  // lower-case FrameKey for the named PowerBall WPSM
  uint32_t retailWpscId = 0;
  bool retailWpscExists = false;
  bool retailCrscExists = false;
  bool retailPartExists = false;
  bool remasteredWpsmExists = false;
  bool remasteredCrsmExists = false;
  bool remasteredGenpExists = false;
  const uint8_t* retailWpsc = nullptr;
  size_t retailWpscSize = 0;
  const uint8_t* retailCrsc = nullptr;
  size_t retailCrscSize = 0;
  const uint8_t* remasteredWpsm = nullptr;
  size_t remasteredWpsmSize = 0;
  const uint8_t* remasteredCrsm = nullptr;
  size_t remasteredCrsmSize = 0;
};

struct PowerBallDefaultRolePair {
  std::array<uint8_t, 16> genp{};
  uint32_t retailPart = 0;
};

// The USA 1.00 PlasmaBeam collision-response default is independent of its
// APSM projectile effect: retail WPSC COLR -> CRSC DEFS PART, and Remastered
// WPSM COLR -> CRSM DEFS GENP. Asset ids use printed UUID order; serialized
// role links use UUID little-endian order.
inline constexpr uint32_t kPlasmaBeamWpsc = 0x310D8B94;
inline constexpr uint32_t kPlasmaBeamCrsc = 0x48036137;
inline constexpr uint32_t kPlasmaBeamDefaultPart = 0x650D4419;
// Printed UUID order: CRSM e42c4b33-8618-44ab-ab5c-d88dd71d9ee6,
// GENP 249c566b-6276-4f00-b905-3c3a73c8c647.
inline constexpr std::array<uint8_t, 16> kPlasmaBeamCrsm = {0xE4, 0x2C, 0x4B, 0x33, 0x86, 0x18, 0x44, 0xAB,
                                                           0xAB, 0x5C, 0xD8, 0x8D, 0xD7, 0x1D, 0x9E, 0xE6};
inline constexpr std::array<uint8_t, 16> kPlasmaBeamDefaultGenp = {0x24, 0x9C, 0x56, 0x6B, 0x62, 0x76, 0x4F, 0x00,
                                                                  0xB9, 0x05, 0x3C, 0x3A, 0x73, 0xC8, 0xC6, 0x47};
// The stored links are the same UUIDs in uuid.UUID(...).bytes_le order.
inline constexpr std::array<uint8_t, 16> kPlasmaBeamCrsmLink = {0x33, 0x4B, 0x2C, 0xE4, 0x18, 0x86, 0xAB, 0x44,
                                                               0xAB, 0x5C, 0xD8, 0x8D, 0xD7, 0x1D, 0x9E, 0xE6};
inline constexpr std::array<uint8_t, 16> kPlasmaBeamDefaultGenpLink = {0x6B, 0x56, 0x9C, 0x24, 0x76, 0x62, 0x00, 0x4F,
                                                                      0xB9, 0x05, 0x3C, 0x3A, 0x73, 0xC8, 0xC6, 0x47};

struct PlasmaBeamDefaultRoleInput {
  std::string_view weaponName;  // the lower-case FrameKey for PlasmaBeam.WPSM
  uint32_t retailWpscId = 0;
  bool retailWpscExists = false;
  bool retailCrscExists = false;
  bool retailPartExists = false;
  bool remasteredWpsmExists = false;
  bool remasteredCrsmExists = false;
  bool remasteredGenpExists = false;
  const uint8_t* retailWpsc = nullptr;
  size_t retailWpscSize = 0;
  const uint8_t* retailCrsc = nullptr;
  size_t retailCrscSize = 0;
  const uint8_t* remasteredWpsm = nullptr;
  size_t remasteredWpsmSize = 0;
  const uint8_t* remasteredCrsm = nullptr;
  size_t remasteredCrsmSize = 0;
};

struct PlasmaBeamDefaultRolePair {
  std::array<uint8_t, 16> genp{};
  uint32_t retailPart = 0;
};

namespace detail {
inline bool ReadUniqueRetailAssetId(const uint8_t* data, size_t size, const char (&classId)[5],
                                    const char (&field)[5], uint32_t& id, std::string* error = nullptr) {
  return PortRemastered::ReadRetailRoleAssetId(data, size, PortRemastered::EffectFourCC(classId),
                                               PortRemastered::EffectFourCC(field), id, error);
}

inline bool ReadUniqueReversedGuidLink(const uint8_t* data, size_t size, const char (&form)[5],
                                       const char (&field)[5], uint8_t kind, std::array<uint8_t, 16>& guid,
                                       std::string* error = nullptr) {
  PortRemastered::EffectNode asset;
  std::string parseError;
  if (!PortRemastered::ParseRemasteredRoleAsset(data, size, PortRemastered::EffectFourCC(form), asset, parseError)) {
    if (error != nullptr) {
      *error = std::move(parseError);
    }
    return false;
  }
  size_t found = 0;
  std::array<uint8_t, 16> parsed{};
  const uint32_t wanted = PortRemastered::EffectFourCC(field);
  for (const PortRemastered::EffectProperty& property : asset.properties) {
    if (property.fourcc != wanted) {
      continue;
    }
    if (++found != 1) {
      if (error != nullptr) {
        *error = "duplicate top-level " + std::string(field) + " fields";
      }
      return false;
    }
    if (property.tag != kind || property.value.size() != 1 ||
        property.value.front().kind != PortRemastered::EffectValue::Kind::Guid) {
      if (error != nullptr) {
        *error = "top-level " + std::string(field) + " has the wrong kind or value type";
      }
      return false;
    }
    parsed = property.value.front().guid;
  }
  if (found != 1) {
    if (error != nullptr) {
      *error = "top-level " + std::string(field) + " field is missing";
    }
    return false;
  }
  guid = parsed;
  return true;
}
}  // namespace detail

// A retail WPSC (class WPSM, tagged fields, _END) with the asset field `field` (ASW1...) set to `id`:
// a NONE field takes the id, a missing one is added before _END. The result must read back with that
// id as the field's one top-level link, else the WPSC is unusual (padding, an unknown field) and is
// left alone. A WPSC that already links the field is not changed.
inline bool AddRetailLink(const std::vector<uint8_t>& wpsc, const char (&field)[5], uint32_t id,
                          std::vector<uint8_t>& out, std::string* error = nullptr) {
  auto fail = [error](std::string message) {
    if (error != nullptr) {
      *error = std::move(message);
    }
    return false;
  };
  const uint32_t wpscClass = PortRemastered::EffectFourCC("WPSM");
  const uint32_t fieldTag = PortRemastered::EffectFourCC(field);
  uint32_t have = 0;
  if (PortRemastered::ReadRetailRoleAssetId(wpsc.data(), wpsc.size(), wpscClass, fieldTag, have)) {
    return fail("the WPSC already links " + std::string(field));
  }
  const uint8_t link[] = {'C', 'N', 'S', 'T', uint8_t(id >> 24), uint8_t(id >> 16), uint8_t(id >> 8), uint8_t(id)};
  size_t at = std::string::npos;  // where the field's value starts, and how much of it is replaced
  size_t replaced = 0;
  for (size_t i = 4; i + 12 <= wpsc.size() && at == std::string::npos; ++i) {
    if (std::memcmp(wpsc.data() + i, field, 4) == 0 && std::memcmp(wpsc.data() + i + 4, "NONE", 4) == 0) {
      at = i + 4;
      replaced = 4;
    }
  }
  std::vector<uint8_t> candidate = wpsc;
  if (at != std::string::npos) {
    candidate.erase(candidate.begin() + std::ptrdiff_t(at), candidate.begin() + std::ptrdiff_t(at + replaced));
    candidate.insert(candidate.begin() + std::ptrdiff_t(at), link, link + sizeof(link));
  } else {
    for (size_t i = wpsc.size(); i >= 4 + 4 && at == std::string::npos; --i) {
      if (std::memcmp(wpsc.data() + i - 4, "_END", 4) == 0) {
        at = i - 4;
      }
    }
    if (at == std::string::npos) {
      return fail("the WPSC has no _END");
    }
    candidate.insert(candidate.begin() + std::ptrdiff_t(at), field, field + 4);
    candidate.insert(candidate.begin() + std::ptrdiff_t(at + 4), link, link + sizeof(link));
  }
  std::string readError;
  if (!PortRemastered::ReadRetailRoleAssetId(candidate.data(), candidate.size(), wpscClass, fieldTag, have,
                                             &readError) ||
      have != id) {
    return fail("the WPSC with " + std::string(field) + " added does not read back (" + readError + ")");
  }
  out = std::move(candidate);
  return true;
}

// Returns a pair only for the exact, uniquely identified USA default-role
// chain. The RFRM and retail class ids, field tags/kinds, complete GUIDs, and
// typed resource-presence flags are all part of the gate; an incomplete or
// ambiguous chain safely leaves retail's collision response in place.
inline bool ResolveMissileDefaultRole(const MissileDefaultRoleInput& input, MissileDefaultRolePair& out,
                                      std::string* error = nullptr) {
  auto fail = [error](std::string message) {
    if (error != nullptr) {
      *error = std::move(message);
    }
    return false;
  };
  if (input.weaponName != "missile" || input.retailWpscId != kMissileWpsc || !input.retailWpscExists ||
      !input.retailCrscExists || !input.retailPartExists || !input.remasteredWpsmExists ||
      !input.remasteredCrsmExists || !input.remasteredGenpExists) {
    return fail("pinned identity or typed-resource presence gate failed");
  }
  uint32_t crsc = 0;
  uint32_t part = 0;
  std::array<uint8_t, 16> crsm{};
  std::array<uint8_t, 16> genp{};
  std::string why;
  if (!detail::ReadUniqueRetailAssetId(input.retailWpsc, input.retailWpscSize, "WPSM", "COLR", crsc, &why)) {
    return fail("retail WPSC: " + why);
  }
  if (crsc != kMissileCrsc) {
    return fail("retail WPSC COLR does not name the pinned CRSC");
  }
  if (!detail::ReadUniqueRetailAssetId(input.retailCrsc, input.retailCrscSize, "CRSM", "DEFS", part, &why)) {
    return fail("retail CRSC: " + why);
  }
  if (part != kMissileDefaultPart) {
    return fail("retail CRSC DEFS does not name the pinned PART");
  }
  if (!detail::ReadUniqueReversedGuidLink(input.remasteredWpsm, input.remasteredWpsmSize, "WPSM", "COLR", 0,
                                          crsm, &why)) {
    return fail("Remastered WPSM: " + why);
  }
  if (crsm != kMissileCrsmLink) {
    return fail("Remastered WPSM COLR does not name the pinned CRSM");
  }
  if (!detail::ReadUniqueReversedGuidLink(input.remasteredCrsm, input.remasteredCrsmSize, "CRSM", "DEFS", 4,
                                          genp, &why)) {
    return fail("Remastered CRSM: " + why);
  }
  if (genp != kMissileDefaultGenpLink) {
    return fail("Remastered CRSM DEFS does not name the pinned GENP");
  }
  out = {kMissileDefaultGenp, part};
  return true;
}

// Resolve only the raw, typed USA 1.00 PowerBall chain. The caller supplies
// uniqueness/type checks from the real retail and PAK indexes; every link is
// then independently parsed and compared against the pinned chain.
inline bool ResolvePowerBallDefaultRole(const PowerBallDefaultRoleInput& input, PowerBallDefaultRolePair& out,
                                        std::string* error = nullptr) {
  auto fail = [error](std::string message) {
    if (error != nullptr) {
      *error = std::move(message);
    }
    return false;
  };
  if (input.weaponName != "powerball" || input.retailWpscId != kPowerBallWpsc || !input.retailWpscExists ||
      !input.retailCrscExists || !input.retailPartExists || !input.remasteredWpsmExists ||
      !input.remasteredCrsmExists || !input.remasteredGenpExists) {
    return fail("pinned USA identity or unique typed-resource presence gate failed");
  }
  uint32_t crsc = 0;
  uint32_t part = 0;
  std::array<uint8_t, 16> crsm{};
  std::array<uint8_t, 16> genp{};
  std::string why;
  if (!detail::ReadUniqueRetailAssetId(input.retailWpsc, input.retailWpscSize, "WPSM", "COLR", crsc, &why)) {
    return fail("retail WPSC: " + why);
  }
  if (crsc != kPowerBallCrsc) {
    return fail("retail WPSC COLR does not name the pinned USA CRSC");
  }
  if (!detail::ReadUniqueRetailAssetId(input.retailCrsc, input.retailCrscSize, "CRSM", "DEFS", part, &why)) {
    return fail("retail CRSC: " + why);
  }
  if (part != kPowerBallDefaultPart) {
    return fail("retail CRSC DEFS does not name the pinned USA PART");
  }
  if (!detail::ReadUniqueReversedGuidLink(input.remasteredWpsm, input.remasteredWpsmSize, "WPSM", "COLR", 0,
                                          crsm, &why)) {
    return fail("Remastered WPSM: " + why);
  }
  if (crsm != kPowerBallCrsmLink) {
    return fail("Remastered WPSM COLR does not name the pinned CRSM");
  }
  if (!detail::ReadUniqueReversedGuidLink(input.remasteredCrsm, input.remasteredCrsmSize, "CRSM", "DEFS", 4,
                                          genp, &why)) {
    return fail("Remastered CRSM: " + why);
  }
  if (genp != kPowerBallDefaultGenpLink) {
    return fail("Remastered CRSM DEFS does not name the pinned GENP");
  }
  out = {kPowerBallDefaultGenp, part};
  return true;
}

// Resolve only the raw, typed USA 1.00 PlasmaBeam chain. The caller supplies
// uniqueness/type checks from the real retail and PAK indexes; every link is
// then independently parsed and compared against the pinned chain.
inline bool ResolvePlasmaBeamDefaultRole(const PlasmaBeamDefaultRoleInput& input, PlasmaBeamDefaultRolePair& out,
                                         std::string* error = nullptr) {
  auto fail = [error](std::string message) {
    if (error != nullptr) {
      *error = std::move(message);
    }
    return false;
  };
  if (input.weaponName != "plasmabeam" || input.retailWpscId != kPlasmaBeamWpsc || !input.retailWpscExists ||
      !input.retailCrscExists || !input.retailPartExists || !input.remasteredWpsmExists ||
      !input.remasteredCrsmExists || !input.remasteredGenpExists) {
    return fail("pinned USA identity or unique typed-resource presence gate failed");
  }
  uint32_t crsc = 0;
  uint32_t part = 0;
  std::array<uint8_t, 16> crsm{};
  std::array<uint8_t, 16> genp{};
  std::string why;
  if (!detail::ReadUniqueRetailAssetId(input.retailWpsc, input.retailWpscSize, "WPSM", "COLR", crsc, &why)) {
    return fail("retail WPSC: " + why);
  }
  if (crsc != kPlasmaBeamCrsc) {
    return fail("retail WPSC COLR does not name the pinned USA CRSC");
  }
  if (!detail::ReadUniqueRetailAssetId(input.retailCrsc, input.retailCrscSize, "CRSM", "DEFS", part, &why)) {
    return fail("retail CRSC: " + why);
  }
  if (part != kPlasmaBeamDefaultPart) {
    return fail("retail CRSC DEFS does not name the pinned USA PART");
  }
  if (!detail::ReadUniqueReversedGuidLink(input.remasteredWpsm, input.remasteredWpsmSize, "WPSM", "COLR", 0,
                                          crsm, &why)) {
    return fail("Remastered WPSM: " + why);
  }
  if (crsm != kPlasmaBeamCrsmLink) {
    return fail("Remastered WPSM COLR does not name the pinned CRSM");
  }
  if (!detail::ReadUniqueReversedGuidLink(input.remasteredCrsm, input.remasteredCrsmSize, "CRSM", "DEFS", 4,
                                          genp, &why)) {
    return fail("Remastered CRSM: " + why);
  }
  if (genp != kPlasmaBeamDefaultGenpLink) {
    return fail("Remastered CRSM DEFS does not name the pinned GENP");
  }
  out = {kPlasmaBeamDefaultGenp, part};
  return true;
}

inline bool ConflictsWithPlasmaBeamDefaultRolePair(const PlasmaBeamDefaultRolePair& candidate,
                                                   const std::array<uint8_t, 16>& existingGenp,
                                                   uint32_t existingRetail) {
  return (existingGenp == candidate.genp || existingRetail == candidate.retailPart) &&
         !(existingGenp == candidate.genp && existingRetail == candidate.retailPart);
}

// A default-role mapping must not override another established mapping that
// claims either side. An exact existing pair is handled as already resolved.
inline bool ConflictsWithPowerBallDefaultRolePair(const PowerBallDefaultRolePair& candidate,
                                                  const std::array<uint8_t, 16>& existingGenp,
                                                  uint32_t existingRetail) {
  return (existingGenp == candidate.genp || existingRetail == candidate.retailPart) &&
         !(existingGenp == candidate.genp && existingRetail == candidate.retailPart);
}
}  // namespace ImportDetail
// Any stage's bump raises it. A full import writes it to kImportStampName in the mod; the mod scan
// (port_mods.h) tells the player when an import carries a lower number, or none.
inline constexpr int kImportVersion = 38 + ImportStage::kConverter + ImportStage::kTextures + ImportStage::kModels +
                                      ImportStage::kEffects + ImportStage::kRooms + ImportStage::kRoomModels +
                                      ImportStage::kText + ImportStage::kHud + ImportStage::kMovies +
                                      ImportStage::kGallery;
inline constexpr const char* kImportStampName = ".import-version";

// ~/.switch/prod.keys if it is there, else empty.
std::string DefaultKeysPath();

// Starts an import on worker threads; `threads` 0 leaves a couple of cores to
// the game. The disc must be open. False (with the reason in the state's
// message) when one is already running or there is no import root.
bool StartImport(const std::string& nspPath, const std::string& keysPath, int threads = 0);
// Only the menu movies (port_remastered_movie.h) and the Extras gallery, into the mod an earlier
// import made: for a player who had no ffmpeg then, or imported before the gallery. Same state and cancelling;
// false when one is running or there is no such mod. It never writes into a legacy
// <mods>/remastered-models on Android, where the import belongs to the app: a player who still has
// only that one is asked for a full import instead.
bool StartMovieImport(const std::string& nspPath, const std::string& keysPath);
ImportState ImportStatus();
// Asks the running import to stop; it ends at the next model.
void CancelImport();
// The next imports make every stage again instead of reusing the previous import's unchanged
// ones (as MP_REMASTERED_REUSE=0). Not saved: it lasts until the game is closed.
void SetRemakeAll(bool remake);
bool RemakeAll();
// Cancels and waits. Call before the disc is closed.
void StopImport();

// The root the import is made in and read from (port_remastered_storage.h): the mods folder on
// desktop, where the import is one of the mods, and <internal storage>/remastered-imports on
// Android, outside the shared mods folder. Empty when there is none, with `error` (when given)
// saying why: on Android that is the app's internal storage being unavailable, and it is never
// worked around by writing to the shared mods folder. Created when it is missing.
std::string ImportRootFolder(std::string* error = nullptr);
// Whether the root is outside the mods folder, so a legacy import there is only a fallback.
bool UsesPrivateImportRoot();
// <root>/remastered-models when that folder is installed, for the mod scan to load
// (port_mods.cpp); empty when there is none, or when the root is the mods folder the scan already
// reads. The staging folder is never answered.
std::string InstalledModFolder();

// Moves a finished import into place. Call before PortMods::Initialize().
// True if a mod was installed.
bool ApplyPendingImport();
// Android only (a no-op elsewhere): deletes the legacy <mods>/remastered-models once the app's own
// import is installed and usable (PortRemasteredStorage::ShouldDeleteLegacy), and resumes an
// interrupted deletion. Call after ApplyPendingImport(), before PortMods::Initialize().
void RemoveLegacyImport();
// The same on the import's thread, reported as running with `installing` set, after which the state
// is the import's result again. Nothing may have a file of the installed mod open (unload the mods
// first); inside the root that is one rename, where the mods folder was shared storage it was a
// file-by-file walk through Android's media provider. False when an import is running or none is
// staged.
bool StartInstall();

// `--import-remastered <nsp> [keys]`: imports with every core, progress on
// stdout, and installs the mod. The disc must be open. Returns the exit code.
// `--import-remastered-movies` is StartMovieImport() the same way.
int RunImportFromCommandLine(const std::string& nspPath, const std::string& keysPath, bool moviesOnly = false);

// The Dread Samus import (port_dread_samus.h): Samus's Power/Varia/Gravity bodies from the user's
// own Metroid Dread .nsp/.xci into <mods>/dread-samus, with the disc open. It shares this import's
// state, cancelling and thread (ImportStatus, CancelImport, StopImport), so only one of the two runs
// at a time; it installs itself when it ends, or at the next start (ApplyPendingImport) when a file
// was in the way. MP_DREAD_ROMFS=<dir> reads an extracted RomFS instead of the image (development).
bool StartDreadSamusImport(const std::string& nspPath, const std::string& keysPath);
// `--import-dread-samus <nsp> [keys]`, as RunImportFromCommandLine. Returns the exit code.
int RunDreadSamusImportFromCommandLine(const std::string& nspPath, const std::string& keysPath);

}  // namespace PortRemastered
