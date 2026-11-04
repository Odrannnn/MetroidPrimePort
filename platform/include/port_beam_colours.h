#pragma once

// Beam colour customisation (issue #223): each beam's colour, picked by the
// player, applied at draw time to its shots, charge and muzzle effects, their
// lights, and the glow of the arm cannon (not its body).
//
// A pick is a 3x3 colour matrix (GXSetColorMatrix) taking the beam's own
// colour to the picked one: a rotation about the grey axis by the hue
// difference, the chroma scaled by the saturation ratio and everything by the
// value ratio, so greys stay grey (a white core stays white unless the pick is
// darker) and the reference colour lands on the pick's hue and saturation.
// The reference colours are the mean of the saturated texels of each beam's
// gun glow textures (CMDL 19AE626C's material sets 0-4), at full value.
//
// The retail gun's glow is the materials whose textures change between the
// beams' material sets (CModel::PortBeamGlowMask); the Remastered gun's is the
// PBR glow term, which the glow-only mode recolours alone. The thermal visor
// and the original experience draw everything as it is.

#include <cstdint>
#include <optional>

namespace PortBeamColours {

// CPlayerState::EBeamId order: Power, Ice, Wave, Plasma, Phazon.
inline constexpr int kBeamCount = 5;

const char* Key(int beam);   // in settings keys
const char* Name(int beam);  // in the UI

// The beam's own colour as 0xRRGGBB.
uint32_t DefaultColour(int beam);
// The picked colour; nullopt = the original.
std::optional<uint32_t> Colour(int beam);
void SetColour(int beam, std::optional<uint32_t> rgb);

// The matrix taking the beam's colour to its pick (rows, rgb' = rows * rgb).
// False when it is the identity: nothing picked, a pick equal to the beam's
// colour, the original experience, or no such beam.
bool Matrix(int beam, float rows[3][3]);
// A light's colour through the beam's matrix, clamped to 0..1.
void TintRgb(int beam, float rgb[3]);

// Set while the scene is drawn through the thermal visor: no tint then.
void SetSuppressed(bool suppressed);

// A Scope beam that draws untinted inside a tinted one (the gun's frozen
// effect).
inline constexpr int kNoTint = -2;

// Draws inside take the beam's matrix (glowOnly: on a PBR draw's glow term
// alone); -1 keeps the enclosing one, kNoTint none. The enclosing state comes
// back at the end.
class Scope {
public:
  explicit Scope(int beam, bool glowOnly = false);
  ~Scope();
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

private:
  int mPrevBeam;
  bool mPrevGlowOnly;
};

// The current scope's beam (-1 = none).
int Current();
// Sets the GX colour matrix to the current scope's again, after a draw that
// changed it (CCubeModel's per-surface gun glow).
void Reapply();

}  // namespace PortBeamColours
