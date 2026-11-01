#pragma once

// Suit colour customisation (issue #175): each suit's colours, picked by the
// player, baked into recoloured copies of the suit's own diffuse textures.
//
// Every suit has its own CMPR TXTRs, which the cutscene body, the pause
// screen's doll, the first-person arm and the morph ball all draw, so serving
// recoloured textures under the retail ids (and the generated balls' ids,
// port_ball_looks.h) changes every view with no render change. Samus's face
// (51E5B069) is shared and never touched, nor are the glow, visor and
// reflection maps.
//
// Each suit has two or three colour slots: the hue clusters of its textures,
// measured offline (the medians below). A texel is coloured when it is
// saturated and not dark, belongs to the slot whose hue is nearest (cleaned by
// a majority filter and by whole islands between dark seams), and moves to the
// picked colour: hue = picked + 0.35 x its offset from the slot's median,
// saturation and value scaled by picked / median. Warm suits (Power, Varia,
// Fusion Phazon) never touch hues 70-205 (the visor, the ball's glass). The
// Phazon Suit's body is nearly black: only its glow and accent move.
//
// PortMods serves the textures under every mod, from a cache in the user
// folder (generated/suit-colours/<suit>/), and leaves out a texture a mod
// brings (the Remastered import). Changing a colour takes a mod reload.
//
// The Dread Samus import (port_dread_samus_mod.h, issue #221) brings its own
// suits, with their own colours: four more entries, after the retail eight,
// recolour its .dds textures (CollectDread), which its dread-suits.txt lists.
// Its Power, Gravity and Varia suits have white plates, which no hue slot can
// reach (no saturation to scale), so their last slot is a base slot: light,
// unsaturated texels outside the hue slots take the picked hue and its
// saturation (added to theirs), value scaled by picked / median as above.

#include "port_mods.h"
#include "port_remastered_image.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace PortSuitColours {

// Bump when the output changes: the cache is remade when its stamp differs.
inline constexpr int kVersion = 1;

// CPlayerState::EPlayerSuit order, then the Dread Samus import's suits in the
// same order from kDreadFirst (Power, Gravity, Varia, Phazon's Metroid Suit).
inline constexpr int kDreadFirst = 8;
inline constexpr int kSuitCount = 12;
inline constexpr int kMaxSlots = 4;

// A slot's colour on the disc: the median of its texels, hue in degrees,
// saturation and value in 0..1.
struct Hsv {
  double h = 0.0;
  double s = 0.0;
  double v = 0.0;
};

struct Suit {
  const char* key;   // in settings keys and the cache folder
  const char* name;  // in the UI and the log
  int slots;
  std::array<Hsv, kMaxSlots> medians;
  std::array<const char*, kMaxSlots> slotKeys;
  std::array<const char*, kMaxSlots> slotNames;
  bool warm;      // never touch hues 70-205
  bool glowOnly;  // Phazon: only bright, saturated texels count as coloured
  bool base;      // the last slot is the white parts' (medians: their light texels')
};
const Suit& SuitInfo(int suit);

// The picked colours, 0xRRGGBB; nullopt = the original.
using Picks = std::array<std::optional<uint32_t>, kMaxSlots>;

// The slot's original colour as 0xRRGGBB (its median).
uint32_t DefaultColour(int suit, int slot);
std::optional<uint32_t> Colour(int suit, int slot);
void SetColour(int suit, int slot, std::optional<uint32_t> rgb);
Picks SuitPicks(int suit);

// Random picks for every slot of the suit: hues spread round the circle from
// a random start, saturated and bright enough to read.
void Randomise(int suit, uint32_t seed);

// Settings text of one slot: "RRGGBB", or empty for the original. Parse
// accepts an optional '#'; anything else reads as the original.
std::string FormatColour(std::optional<uint32_t> rgb);
std::optional<uint32_t> ParseColour(const std::string& text);

// A suit's picks as one string, a slot each, comma-separated, "-" for the
// original ("FFD000,-"; "" when none is set): the settings value, the cache
// stamp's, and what tells whether a reload is due. SetPicksKey reads one
// back; a missing or bad slot is the original.
std::string PicksKey(int suit);
std::string PicksKey(int suit, const Picks& picks);
void SetPicksKey(int suit, const std::string& key);

// What the last PortMods::Initialize served: each suit's PicksKey ("" for a
// suit it left as retail). Pending: the picks differ from it.
void SetServed(const std::array<std::string, kSuitCount>& served);
bool Pending(bool wanted);

// Recolours a decoded texture of `suit` towards `picks`. `transfer`, when
// given, is a texture of `transferSuit` with the same UVs whose slot map
// replaces this one's where both are coloured (Varia's textures take the
// Power Suit's cleaner split). Returns how many texels changed.
size_t Recolour(PortRemastered::Image& image, int suit, const Picks& picks,
                const PortRemastered::Image* transfer = nullptr, int transferSuit = 0);

// The suit's textures recoloured to `picks` (one snapshot of SuitPicks, so
// the stamp and the images agree), made or taken from the cache, to serve as
// loose resources. `loose` is what PortMods has so far: a texture a mod
// brings is left out, and a generated ball's texture (port_ball_looks.h) is
// read from there and recoloured too. `native` says whether a mod has a
// native image for a texture id. Empty when nothing is picked or the disc
// lacks the sources (logged). Needs the disc open; for PortMods::Initialize.
std::vector<PortMods::LooseResource> Collect(
    int suit, const Picks& picks, const std::map<std::pair<uint32_t, uint32_t>, PortMods::LooseResource>& loose,
    bool (*native)(uint32_t id));

// The Dread Samus import's textures of `suit` (kDreadFirst on) recoloured to
// `picks`, made or taken from the cache: (texture id, host path of the .dds).
// `modDir` is the import's folder (its dread-suits.txt lists each suit's
// textures); `source` gives the .dds a texture is served from now, or empty to
// leave it. Empty when nothing is picked or the import has no list (logged).
std::vector<std::pair<uint32_t, std::string>> CollectDread(int suit, const Picks& picks, const std::string& modDir,
                                                           const std::function<std::string(uint32_t)>& source);

}  // namespace PortSuitColours
