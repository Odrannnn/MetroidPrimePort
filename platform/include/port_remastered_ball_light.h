#pragma once

// Remastered's morph ball light (the inner glow's particle light, read from its main.nso:
// CMorphBallMP1::UpdateEffects). Remastered lights its PBR scenery with it as an HDR point
// light: the suit's light colour made linear, an intensity that rises from 30 to 60 (90 for
// the Phazon suit) as the ball sinks into normal water and to 450 while the boost charges or
// drains, a quadratic falloff to nothing at 2.3 units, at the ball's centre. The GX light
// the original game lights with stays as it was for everything else.
namespace PortRemasteredBallLight {

// On unless MP_REMASTERED_BALL_LIGHT=0 (the console's `roomenv balllight`).
bool Enabled();
void SetEnabled(bool on);
// Multiplies the light's intensity; 1 unless MP_REMASTERED_BALL_LIGHT_SCALE or the console's
// `roomenv balllight <scale>` sets it.
float Scale();
void SetScale(float scale);

constexpr float kOuterRadius = 2.3f;

struct Inputs {
  int glowIndex;         // CMorphBall::x8_ballGlowColorIdx: 0 Power .. 4 Phazon
  const float* srgb;     // the suit's light colour (CMorphBall::skBallLightModulationColors), 0..1
  float boost;           // the boost's charge (or 1 - its drain), 0..1
  float water;           // CMorphBall's water factor (Remastered's CMorphBallMP1 + 0x22a8), 0..1
  float fade;            // the morph transition's fade, 0..1
  float dt;
};

// Writes the light's linear colour as the PBR shader takes it (its Lambert has no 1/pi, so
// the intensity is divided by pi here).
void Update(const Inputs& in, float outLinearColor[3]);

} // namespace PortRemasteredBallLight
