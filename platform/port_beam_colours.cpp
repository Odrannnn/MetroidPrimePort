// Beam colour customisation (port_beam_colours.h).

#include "port_beam_colours.h"

#include "port_debug.h"

#include <dolphin/gx/GXExtra.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace PortBeamColours {
namespace {

struct Beam {
  const char* key;
  const char* name;
  uint32_t reference;
};

// The saturated texels' mean hue and saturation of each beam's glow textures, at value 1:
// Power 2BA2C91C/683B5A1C (h 31 and 56), Ice 2EE307AE/142BF3A2, Wave 73934A6F/DCA4E794,
// Plasma E58C24C1/20BEA729, Phazon 3FC8598C/3F4AD85A/24244332/B86EB9FA.
constexpr std::array<Beam, kBeamCount> kBeams = {{
    {"power", "Power Beam", 0xFFC933},
    {"ice", "Ice Beam", 0xA3E0FF},
    {"wave", "Wave Beam", 0xB299FF},
    {"plasma", "Plasma Beam", 0xFF210D},
    {"phazon", "Phazon Beam", 0x38A5FF},
}};

std::array<std::optional<uint32_t>, kBeamCount> sPicks;
bool sSuppressed = false;
int sBeam = -1;
bool sGlowOnly = false;

struct Rgb {
  double r, g, b;
};

Rgb Unpack(uint32_t rgb) {
  return {double((rgb >> 16) & 0xFF) / 255.0, double((rgb >> 8) & 0xFF) / 255.0, double(rgb & 0xFF) / 255.0};
}

// The chroma (the colour less its grey) as an angle about the grey axis from red, its length over the
// grey (a saturation that a scale of the colour keeps), and the HSV value.
struct Polar {
  double angle, sat, value;
};

Polar ToPolar(const Rgb& c) {
  const double grey = (c.r + c.g + c.b) / 3.0;
  const double x = c.r - grey;
  const double y = c.g - grey;
  const double z = c.b - grey;
  // Coordinates in the plane normal to (1,1,1): along (2,-1,-1)/sqrt6 and (0,1,-1)/sqrt2.
  const double u = (2.0 * x - y - z) / std::sqrt(6.0);
  const double v = (y - z) / std::sqrt(2.0);
  const double length = std::sqrt(u * u + v * v);
  return {std::atan2(v, u), grey > 0.0 ? length / grey : 0.0, std::max({c.r, c.g, c.b})};
}

void Apply(int beam, bool glowOnly) {
  float rows[3][3];
  if (beam >= 0 && !sSuppressed && Matrix(beam, rows)) {
    GXSetColorMatrix(rows, glowOnly ? GX_TRUE : GX_FALSE);
  } else {
    GXSetColorMatrix(nullptr, GX_FALSE);
  }
}

}  // namespace

const char* Key(int beam) { return beam >= 0 && beam < kBeamCount ? kBeams[beam].key : ""; }
const char* Name(int beam) { return beam >= 0 && beam < kBeamCount ? kBeams[beam].name : ""; }

uint32_t DefaultColour(int beam) { return beam >= 0 && beam < kBeamCount ? kBeams[beam].reference : 0xFFFFFF; }

std::optional<uint32_t> Colour(int beam) {
  return beam >= 0 && beam < kBeamCount ? sPicks[beam] : std::nullopt;
}

void SetColour(int beam, std::optional<uint32_t> rgb) {
  if (beam >= 0 && beam < kBeamCount) {
    sPicks[beam] = rgb ? std::optional<uint32_t>(*rgb & 0xFFFFFFu) : std::nullopt;
  }
}

bool Matrix(int beam, float rows[3][3]) {
  if (beam < 0 || beam >= kBeamCount || !sPicks[beam] || *sPicks[beam] == kBeams[beam].reference ||
      PortDebug::OriginalExperience()) {
    return false;
  }
  const Polar from = ToPolar(Unpack(kBeams[beam].reference));
  const Polar to = ToPolar(Unpack(*sPicks[beam]));
  // M = v (G + s R(theta) (I - G)), G the projection onto grey (all 1/3), R the rotation about it.
  const double theta = to.sat > 0.0 ? to.angle - from.angle : 0.0;
  const double s = from.sat > 0.0 ? to.sat / from.sat : 0.0;
  const double v = from.value > 0.0 ? to.value / from.value : 0.0;
  const double c = std::cos(theta);
  const double sn = std::sin(theta) / std::sqrt(3.0);
  const double t = (1.0 - c) / 3.0;
  // Rodrigues' rotation about k = (1,1,1)/sqrt3: c I + sin [k]x + (1 - c) k k^T.
  const double rot[3][3] = {
      {c + t, t - sn, t + sn},
      {t + sn, c + t, t - sn},
      {t - sn, t + sn, c + t},
  };
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      // R (I - G) = R - G, since R keeps grey.
      const double g = 1.0 / 3.0;
      rows[i][j] = float(v * (g + s * (rot[i][j] - g)));
    }
  }
  return true;
}

void TintRgb(int beam, float rgb[3]) {
  float rows[3][3];
  if (!Matrix(beam, rows)) {
    return;
  }
  const float in[3] = {rgb[0], rgb[1], rgb[2]};
  for (int i = 0; i < 3; ++i) {
    rgb[i] = std::clamp(rows[i][0] * in[0] + rows[i][1] * in[1] + rows[i][2] * in[2], 0.f, 1.f);
  }
}

void SetSuppressed(bool suppressed) { sSuppressed = suppressed; }

Scope::Scope(int beam, bool glowOnly) : mPrevBeam(sBeam), mPrevGlowOnly(sGlowOnly) {
  if (beam < 0 && beam != kNoTint) {
    return;
  }
  sBeam = beam < 0 ? -1 : beam;
  sGlowOnly = glowOnly;
  if (sBeam != mPrevBeam || sGlowOnly != mPrevGlowOnly) {
    Apply(sBeam, sGlowOnly);
  }
}

Scope::~Scope() {
  if (sBeam != mPrevBeam || sGlowOnly != mPrevGlowOnly) {
    sBeam = mPrevBeam;
    sGlowOnly = mPrevGlowOnly;
    Apply(sBeam, sGlowOnly);
  }
}

int Current() { return sBeam; }

void Reapply() { Apply(sBeam, sGlowOnly); }

}  // namespace PortBeamColours
