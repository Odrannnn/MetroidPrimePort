#pragma once

#include <cstdint>
#include "port_model_variant.h"

// What the game and the mod loader need to know of the "dread-samus" mod (port_dread_samus.h, the
// import that makes it), without the import's headers.
namespace PortDreadSamus {

// The mod's folder in the mods folder, and its stamp: kVersion, then the build's commit. Its own
// number, kept out of PortRemastered::kImportVersion, so a change here never asks for a Remastered
// re-import. Bump it when the output changes.
inline constexpr const char* kModName = "dread-samus";
inline constexpr const char* kStampName = ".dread-samus-version";
// 1: Power/Varia/Gravity, cutscene (CINF 19C53902) and in-game (A496DA02) bodies.
// 2: suit materials carry REFL (env specular on the metal plates).
// 3: shoulder pads skinned to the chest, not voted from Prime's collar.
// 4: Dread proportions: meshes keep their own bind, with Dread-proportioned CINFs (D7EAD001-006).
// 5: Phazon = Dread's Metroid Suit (samushyper), skeletons D7EAD007-008.
// 6: all four suit Morph Balls, normal/Spider and low-poly, with rigid animated skins.
// 7: the pause-screen doll's four suit bodies (CINF F76E70A6), skeletons D7EAD009-00C.
inline constexpr int kVersion = 7;

struct BallTarget {
  int suit;
  const char* source;
  bool spider;
  uint32_t ancs, cmdl, low;
};
inline constexpr BallTarget kBallTargets[] = {
    {0, "morphballpower", false, 0x9FE58F7A, 0x4E57CE08, 0x180418E0},
    {1, "morphballgravity", false, 0x9FE58F7A, 0x4E57CE08, 0x180418E0},
    {2, "morphballvaria", false, 0x9FE58F7A, 0x4E57CE08, 0x180418E0},
    {3, "morphballhyper", false, 0x9FE58F7A, 0x4E57CE08, 0x180418E0},
    {0, "morphballpower", true, 0xF37654A2, 0x223353FE, 0x0B57F9E8},
    {1, "morphballgravity", true, 0xF37654A2, 0x223353FE, 0x0B57F9E8},
    {2, "morphballvaria", true, 0xF37654A2, 0x223353FE, 0x0B57F9E8},
    {3, "morphballhyper", true, 0xD7F7C8EC, 0x1922EF7C, 0x0B57F9E8},
};

inline uint32_t BallModel(int suit, bool spider) {
  for (const auto& t : kBallTargets) {
    if (t.suit == suit && t.spider == spider) return PortModelVariant::Id(t.cmdl, suit);
  }
  return 0;
}
// The runtime selects ANCS and low CMDL variants independently, falling back to
// retail when absent. A Dread-owned ANCS has the importer's exact main binding.
// Query the final winning resources, never just the enabled mod list.
struct BallOwnership {
  bool main = false;
  bool low = false;
};
template <class Owns> BallOwnership ResolveBallOwnership(int suit, bool spider, const Owns& owns) {
  for (const auto& t : kBallTargets) {
    if (t.suit == suit && t.spider == spider) {
      return {owns(0x414E4353u, PortModelVariant::Id(t.ancs, suit)) &&
                  owns(0x434D444Cu, PortModelVariant::Id(t.cmdl, suit)),
              owns(0x434D444Cu, PortModelVariant::Id(t.low, suit))};
    }
  }
  return {};
}
// Main requires the winning variant ANCS AND its referenced main CMDL.
bool ReplacesBall(int suit, bool spider);
bool ReplacesLowBall(int suit, bool spider);

// The in-game body CMDL of a CPlayerState::EPlayerSuit (0 Power, 1 Gravity, 2 Varia, 3 Phazon;
// also BallTransitionsANCS's character index), 0 for the suits the mod leaves alone (Fusion).
inline uint32_t InGameBody(int suit) {
  switch (suit) {
  case 0:
    return 0x4A1A8FD3;
  case 1:
    return 0x57489AD3;
  case 2:
    return 0xE449874A;
  case 3:
    return 0x891E00FD;
  default:
    return 0;
  }
}

// The cutscene body CMDL of a cutscene ANCS character (0 Power, 1 Gravity, 2 Varia, 3 Phazon, as every
// cutscene ANCS and the elevator's order them), 0 for the others.
inline uint32_t CutsceneBody(int charIdx) {
  switch (charIdx) {
  case 0:
    return 0xF39476F2;
  case 1:
    return 0x57F5C930;
  case 2:
    return 0x776C1A96;
  case 3:
    return 0x2782648E;
  default:
    return 0;
  }
}

// The pause-screen doll's body CMDL of a CPlayerState::EPlayerSuit (ANCS_ItemScreenSamus's first
// part of the suit), 0 for the others.
inline uint32_t DollBody(int suit) {
  switch (suit) {
  case 0:
    return 0xCC25E002;
  case 1:
    return 0x11A5BC8A;
  case 2:
    return 0xA602C954;
  case 3:
    return 0x5644E7A1;
  default:
    return 0;
  }
}

// Whether the Dread body stands in for a retail body CMDL now: the dread-samus mod is loaded and
// serves that id (PortMods). Its skinned cannon replaces Prime's separate one, which is then not drawn.
bool Replaces(uint32_t bodyCmdl);

// The skin the mod ships for a cutscene body CMDL it serves (CINF 19C53902), 0 for any other. A
// Remastered install replaces the Samus ANCS and with it the skin ids, so a character's own skin id
// may be Remastered's, whose weights belong to Remastered's mesh, not the Dread one.
inline uint32_t CutsceneSkin(uint32_t bodyCmdl) {
  switch (bodyCmdl) {
  case 0xF39476F2:
    return 0x98BBD924;
  case 0x776C1A96:
    return 0x1C43B540;
  case 0x57F5C930:
    return 0x3CDA66E6;
  case 0x2782648E:
  case 0xB333E1D7: // the Phazon body of one Tallon ANCS (BAD39B59): the same skin, served as a copy
    return 0x4CADCB58;
  default:
    return 0;
  }
}

// The Dread-proportioned skeleton the mod ships for a body CMDL it serves: the retail CINF's joints
// with their heads moved to the Dread body's (one per suit and skeleton), 0 for any other. New ids,
// none of them on the disc.
inline uint32_t DreadCinf(uint32_t bodyCmdl) {
  switch (bodyCmdl) {
  case 0x4A1A8FD3:
    return 0xD7EAD001;
  case 0x57489AD3:
    return 0xD7EAD002;
  case 0xE449874A:
    return 0xD7EAD003;
  case 0xF39476F2:
    return 0xD7EAD004;
  case 0x57F5C930:
    return 0xD7EAD005;
  case 0x776C1A96:
    return 0xD7EAD006;
  case 0x2782648E:
  case 0xB333E1D7:
    return 0xD7EAD007;
  case 0x891E00FD:
    return 0xD7EAD008;
  case 0xCC25E002:
    return 0xD7EAD009;
  case 0x11A5BC8A:
    return 0xD7EAD00A;
  case 0xA602C954:
    return 0xD7EAD00B;
  case 0x5644E7A1:
    return 0xD7EAD00C;
  default:
    return 0;
  }
}

inline bool IsBody(uint32_t id) { return DreadCinf(id) != 0; }

// The Dread skeleton a body CMDL is skinned and posed on now: DreadCinf(bodyCmdl) when the mod serves
// the body and that CINF (an import from before the skeletons shipped has none), 0 otherwise.
// Animations still sample against the retail CINF; CAnimData::RecalcPoseBuilder retargets them.
uint32_t Skeleton(uint32_t bodyCmdl);

}  // namespace PortDreadSamus
