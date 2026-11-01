// Suit colour customisation's recolour on synthetic textures, and the picks'
// settings text.

#include "port_suit_colours.h"
#include "port_test.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

using PortRemastered::Image;
namespace PSC = PortSuitColours;

constexpr int kPower = 0, kVaria = 2;

void Rgb(double h, double s, double v, uint8_t* out) {
  h = std::fmod(h, 360.0) / 60.0;
  const double c = v * s, x = c * (1.0 - std::fabs(std::fmod(h, 2.0) - 1.0)), m = v - c;
  double r = 0, g = 0, b = 0;
  switch (int(h)) {
  case 0: r = c, g = x; break;
  case 1: r = x, g = c; break;
  case 2: g = c, b = x; break;
  case 3: g = x, b = c; break;
  case 4: r = x, b = c; break;
  default: r = c, b = x; break;
  }
  out[0] = uint8_t(std::lround((r + m) * 255));
  out[1] = uint8_t(std::lround((g + m) * 255));
  out[2] = uint8_t(std::lround((b + m) * 255));
  out[3] = 255;
}

double Hue(const uint8_t* t) {
  const double r = t[0] / 255.0, g = t[1] / 255.0, b = t[2] / 255.0;
  const double max = std::max({r, g, b}), range = max - std::min({r, g, b});
  if (range <= 0) {
    return 0;
  }
  const double h = max == r ? std::fmod((g - b) / range + 6, 6) : max == g ? (b - r) / range + 2 : (r - g) / range + 4;
  return h * 60;
}

uint8_t* At(Image& image, int x, int y) { return &image.rgba[(size_t(y) * image.width + x) * 4]; }
const uint8_t* At(const Image& image, int x, int y) { return &image.rgba[(size_t(y) * image.width + x) * 4]; }

// A Power Suit texture: gold on the left, red on the right, with a cyan
// visor patch in the gold and a grey patch in the red.
Image PowerTexture() {
  Image image;
  image.width = image.height = 64;
  image.rgba.resize(64 * 64 * 4);
  for (int y = 0; y < 64; ++y) {
    for (int x = 0; x < 64; ++x) {
      uint8_t* t = At(image, x, y);
      if (x >= 8 && x < 16 && y >= 8 && y < 16) {
        Rgb(180, 0.8, 0.9, t);
      } else if (x >= 48 && x < 56 && y >= 8 && y < 16) {
        Rgb(0, 0.0, 0.6, t);
      } else if (x < 32) {
        Rgb(43 + (y % 5) - 2, 0.83, 0.95, t);
      } else {
        Rgb(5, 0.9, 0.84, t);
      }
    }
  }
  return image;
}

void TestRecolour() {
  Image image = PowerTexture();
  const Image original = image;
  PSC::Picks picks{};
  CHECK_EQ(PSC::Recolour(image, kPower, picks), size_t(0));
  CHECK(image.rgba == original.rgba);

  picks[0] = 0x2050FF;  // blue primary, secondary left as it is
  const size_t changed = PSC::Recolour(image, kPower, picks);
  CHECK(changed > 1000);
  // The gold is blue now, its spread of hues kept (compressed).
  for (const int y : {20, 40, 60}) {
    const double hue = Hue(At(image, 20, y));
    CHECK_MSG(std::fabs(hue - 225.0) < 6.0, "gold texel hue " + std::to_string(hue));
  }
  // Red, the visor and grey stay.
  for (const auto& [x, y] : {std::pair{40, 30}, {60, 60}, {12, 12}, {52, 12}}) {
    CHECK(std::equal(At(image, x, y), At(image, x, y) + 4, &original.rgba[(size_t(y) * 64 + x) * 4]));
  }
}

void TestTransfer() {
  // Varia: all orange (its primary), but the Power texture with the same UVs
  // says the right half is secondary.
  Image varia;
  varia.width = varia.height = 64;
  varia.rgba.resize(64 * 64 * 4);
  for (int i = 0; i < 64 * 64; ++i) {
    Rgb(34, 0.87, 0.95, &varia.rgba[size_t(i) * 4]);
  }
  const Image power = PowerTexture();
  PSC::Picks picks{};
  picks[1] = 0x20C040;  // green secondary
  Image alone = varia;
  CHECK_EQ(PSC::Recolour(alone, kVaria, picks), size_t(0));
  PSC::Recolour(varia, kVaria, picks, &power, kPower);
  const uint8_t green[4] = {0x20, 0xC0, 0x40, 255};
  CHECK_MSG(std::fabs(Hue(At(varia, 44, 40)) - Hue(green)) < 8.0,
            "right half takes the secondary");
  CHECK(Hue(At(varia, 10, 40)) < 40.0);  // left half stays orange
}

// The Dread Power Suit's base slot: its white plates, and nothing else.
void TestBase() {
  constexpr int kDreadPower = PSC::kDreadFirst;
  const PSC::Suit& info = PSC::SuitInfo(kDreadPower);
  CHECK(info.base && info.slots == 3);
  CHECK(!PSC::SuitInfo(kPower).base && !PSC::SuitInfo(PSC::kDreadFirst + 3).base);
  Image image;
  image.width = image.height = 64;
  image.rgba.resize(64 * 64 * 4);
  for (int y = 0; y < 64; ++y) {
    for (int x = 0; x < 64; ++x) {
      uint8_t* t = At(image, x, y);
      if (y < 8) {
        Rgb(0, 0.0, 0.1, t);  // black under-suit
      } else if (x < 32) {
        Rgb(200, 0.6, 0.74, t);  // blue primary
      } else {
        Rgb(249, 0.05, 0.75 + 0.002 * y, t);  // white plates, shaded
      }
    }
  }
  const Image original = image;
  PSC::Picks picks{};
  picks[0] = 0x20C040;
  Image primary = image;
  PSC::Recolour(primary, kDreadPower, picks);
  CHECK(std::equal(At(primary, 40, 30), At(primary, 40, 30) + 4, At(image, 40, 30)));  // the plates stay

  picks = {};
  picks[2] = 0xFF2020;
  CHECK(PSC::Recolour(image, kDreadPower, picks) > 1000);
  const uint8_t red[4] = {0xFF, 0x20, 0x20, 255};
  CHECK_MSG(std::fabs(Hue(At(image, 40, 30)) - Hue(red)) < 3.0, "plates take the picked hue");
  CHECK(At(image, 40, 30)[0] > 200 && At(image, 40, 30)[1] < 80);
  CHECK(At(image, 40, 60)[0] > At(image, 40, 10)[0]);  // their shading kept
  for (const auto& [x, y] : {std::pair{10, 30}, {40, 2}}) {
    CHECK(std::equal(At(image, x, y), At(image, x, y) + 4, At(original, x, y)));
  }

  // Its own colour back is (nearly) the original.
  image = original;
  picks[2] = PSC::DefaultColour(kDreadPower, 2);
  PSC::Recolour(image, kDreadPower, picks);
  for (int c = 0; c < 3; ++c) {
    CHECK(std::abs(int(At(image, 40, 30)[c]) - int(At(original, 40, 30)[c])) <= 3);
  }
}

void TestPicks() {
  CHECK(PSC::ParseColour("#ff00aa") == std::optional<uint32_t>(0xFF00AA));
  CHECK(PSC::ParseColour("00FF00") == std::optional<uint32_t>(0x00FF00));
  CHECK(!PSC::ParseColour("-"));
  CHECK(!PSC::ParseColour("12345G"));
  CHECK_EQ(PSC::FormatColour(0x0A0B0C), std::string("0A0B0C"));

  CHECK_EQ(PSC::PicksKey(kPower), std::string());
  PSC::SetPicksKey(kPower, "2050FF,-");
  CHECK(PSC::Colour(kPower, 0) == std::optional<uint32_t>(0x2050FF));
  CHECK(!PSC::Colour(kPower, 1));
  CHECK_EQ(PSC::PicksKey(kPower), std::string("2050FF,-"));
  CHECK(!PSC::Colour(kPower, 2));  // Power has two slots
  PSC::SetPicksKey(1, "-,-,FF0000,ABCDEF");
  CHECK_EQ(PSC::PicksKey(1), std::string("-,-,FF0000"));

  std::array<std::string, PSC::kSuitCount> served{};
  CHECK(PSC::Pending(true));
  CHECK(!PSC::Pending(false));  // nothing served, nothing wanted
  served[kPower] = "2050FF,-";
  served[1] = "-,-,FF0000";
  PSC::SetServed(served);
  CHECK(!PSC::Pending(true));
  CHECK(PSC::Pending(false));
  PSC::SetPicksKey(kPower, "");
  PSC::SetPicksKey(1, "");
  CHECK_EQ(PSC::PicksKey(kPower), std::string());
  CHECK(PSC::Pending(true));
  PSC::SetServed({});
  CHECK(!PSC::Pending(true));

  PSC::Randomise(1, 1234);
  const std::optional<uint32_t> a = PSC::Colour(1, 0), b = PSC::Colour(1, 1), c = PSC::Colour(1, 2);
  CHECK(a && b && c && *a != *b && *b != *c);
  PSC::Randomise(1, 1234);
  CHECK(PSC::Colour(1, 0) == a);  // same seed, same picks
  PSC::SetPicksKey(1, "");

  for (int suit = 0; suit < PSC::kSuitCount; ++suit) {
    const PSC::Suit& info = PSC::SuitInfo(suit);
    CHECK(info.slots >= 2 && info.slots <= PSC::kMaxSlots);
    for (int slot = 0; slot < info.slots; ++slot) {
      CHECK(info.slotKeys[slot] != nullptr && info.slotNames[slot] != nullptr);
    }
  }
  CHECK_EQ(PSC::DefaultColour(kPower, 0) >> 16, uint32_t(247));  // gold: V 0.969
}

}  // namespace

int main() {
  TestRecolour();
  TestTransfer();
  TestBase();
  TestPicks();
  if (port_test::Failures() == 0) {
    std::printf("port_suit_colours: all passed\n");
  }
  return PORT_TEST_RESULT();
}
