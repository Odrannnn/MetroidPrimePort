// Suit colour customisation's image work and picks (port_suit_colours.h). The
// disc reading and the cache are in port_suit_colours_disc.cpp.

#include "port_suit_colours.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>

namespace PortSuitColours {
namespace {

// The slot medians were measured on USA 1.00 over each suit's body, arm and
// ball textures (hue clusters seeded per suit, refined by circular k-means).
const Suit kSuits[kSuitCount] = {
    {"power", "Power Suit", 2, {{{43.4, 0.826, 0.969}, {5.3, 0.912, 0.839}, {}}},
     {"primary", "secondary", nullptr}, {"Primary", "Secondary", nullptr}, true, false, false},
    {"gravity", "Gravity Suit", 3, {{{246.4, 0.356, 0.482}, {43.4, 0.839, 0.969}, {8.2, 0.906, 0.871}}},
     {"primary", "secondary", "accent"}, {"Primary", "Secondary", "Accent"}, false, false, false},
    {"varia", "Varia Suit", 2, {{{33.7, 0.866, 0.969}, {13.1, 0.906, 0.882}, {}}},
     {"primary", "secondary", nullptr}, {"Primary", "Secondary", nullptr}, true, false, false},
    {"phazon", "Phazon Suit", 2, {{{107.1, 1.0, 0.49}, {12.4, 0.81, 0.741}, {}}},
     {"glow", "accent", nullptr}, {"Glow", "Accent", nullptr}, false, true, false},
    {"fusion", "Fusion Suit", 3, {{{188.0, 1.0, 0.737}, {54.9, 0.68, 0.871}, {337.6, 0.851, 0.871}}},
     {"primary", "secondary", "accent"}, {"Primary", "Secondary", "Accent"}, false, false, false},
    {"fusion_gravity", "Fusion Gravity Suit", 3,
     {{{166.3, 0.725, 0.647}, {263.7, 0.364, 0.482}, {337.6, 0.853, 0.871}}},
     {"primary", "secondary", "accent"}, {"Primary", "Secondary", "Accent"}, false, false, false},
    {"fusion_varia", "Fusion Varia Suit", 3, {{{67.2, 1.0, 0.969}, {315.0, 0.631, 0.761}, {337.6, 0.853, 0.871}}},
     {"primary", "secondary", "accent"}, {"Primary", "Secondary", "Accent"}, false, false, false},
    {"fusion_phazon", "Fusion Phazon Suit", 3, {{{32.8, 0.869, 0.969}, {54.9, 0.746, 0.871}, {4.1, 0.851, 0.871}}},
     {"primary", "secondary", "accent"}, {"Primary", "Secondary", "Accent"}, true, false, false},
    // The Dread Samus import's, measured the same way over its BCLR textures
    // (cutscene, game and doll bodies, and the ball). Its Power Suit's green
    // lights (90-105) and its Varia's olive under-suit stay out of the gates.
    // A base slot's median is of the light texels (saturation under 0.12,
    // value over 0.6).
    {"dread_power", "Power Suit (Dread)", 3, {{{200.2, 0.606, 0.741}, {8.9, 0.651, 0.776}, {249.0, 0.079, 0.804}}},
     {"primary", "secondary", "base"}, {"Primary", "Secondary", "White"}, false, false, true},
    {"dread_gravity", "Gravity Suit (Dread)", 4,
     {{{267.4, 0.579, 0.71}, {303.9, 0.85, 0.773}, {8.8, 0.623, 0.902}, {242.9, 0.035, 0.937}}},
     {"primary", "secondary", "accent", "base"}, {"Primary", "Secondary", "Accent", "White"}, false, false, true},
    {"dread_varia", "Varia Suit (Dread)", 3, {{{38.9, 0.867, 0.937}, {6.6, 0.644, 0.843}, {260.0, 0.054, 0.882}}},
     {"primary", "secondary", "base"}, {"Primary", "Secondary", "White"}, true, false, true},
    // The Metroid Suit has no white parts (under 0.1% of its texels).
    {"dread_phazon", "Metroid Suit (Dread)", 2, {{{90.6, 0.621, 0.592}, {10.8, 0.462, 0.388}, {}}},
     {"primary", "accent", nullptr}, {"Primary", "Accent", nullptr}, false, false, false},
};

// Tuned by eye on contact sheets of every suit.
constexpr double kGateFull = 25.0;     // degrees from the nearest median: full recolour below
constexpr double kGateZero = 35.0;     // and none from here
constexpr double kCompress = 0.35;     // how much of a texel's offset from its median it keeps
constexpr double kIslandShare = 0.70;  // an island takes one slot when this share of it has it
constexpr int kIslandMin = 40;         // smaller islands take their majority whatever it is
constexpr double kIslandHue = 60.0;    // but only texels this near that slot's hue
constexpr double kSeamValue = 0.18;    // darker texels part islands
constexpr double kScaleMax = 4.0;      // saturation and value scale at most this much
constexpr double kBaseLow = 0.45;      // a base slot takes no texel darker than this
constexpr double kBaseHigh = 0.60;     // and all of one this light (or lighter)

std::mutex sMutex;
std::array<Picks, kSuitCount> sPicks{};
std::array<std::string, kSuitCount> sServed{};

// The slots that go by hue: all but a base slot.
int HueSlots(const Suit& info) { return info.slots - (info.base ? 1 : 0); }

double Smooth(double x, double a, double b) {
  const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

double HueDistance(double a, double b) {
  const double d = std::fmod(std::fabs(a - b), 360.0);
  return std::min(d, 360.0 - d);
}

// -180..180.
double Wrap(double d) {
  double w = std::fmod(d + 180.0, 360.0);
  if (w < 0.0) {
    w += 360.0;
  }
  return w - 180.0;
}

Hsv ToHsv(double r, double g, double b) {
  const double max = std::max({r, g, b});
  const double min = std::min({r, g, b});
  const double range = max - min;
  Hsv out;
  out.v = max;
  out.s = max > 1e-6 ? range / max : 0.0;
  if (range > 1e-6) {
    if (max == r) {
      out.h = 60.0 * std::fmod((g - b) / range + 6.0, 6.0);
    } else if (max == g) {
      out.h = 60.0 * ((b - r) / range + 2.0);
    } else {
      out.h = 60.0 * ((r - g) / range + 4.0);
    }
  }
  return out;
}

// 0..255 floats.
void FromHsv(double h, double s, double v, double* rgb) {
  h = std::fmod(std::fmod(h, 360.0) + 360.0, 360.0) / 60.0;
  const int i = int(std::floor(h)) % 6;
  const double f = h - std::floor(h);
  const double p = v * (1.0 - s), q = v * (1.0 - s * f), t = v * (1.0 - s * (1.0 - f));
  double r = v, g = t, b = p;
  switch (i) {
  case 1: r = q, g = v, b = p; break;
  case 2: r = p, g = v, b = t; break;
  case 3: r = p, g = q, b = v; break;
  case 4: r = t, g = p, b = v; break;
  case 5: r = v, g = p, b = q; break;
  default: break;
  }
  rgb[0] = r * 255.0;
  rgb[1] = g * 255.0;
  rgb[2] = b * 255.0;
}

// The sums of `in` over the k x k window round each texel (zero outside).
std::vector<int> BoxSum(const std::vector<uint8_t>& in, int width, int height, int k) {
  const int r = k / 2;
  std::vector<int> table(size_t(width + 1) * size_t(height + 1), 0);
  for (int y = 0; y < height; ++y) {
    int row = 0;
    for (int x = 0; x < width; ++x) {
      row += in[size_t(y) * width + x];
      table[size_t(y + 1) * (width + 1) + (x + 1)] = table[size_t(y) * (width + 1) + (x + 1)] + row;
    }
  }
  std::vector<int> out(in.size());
  for (int y = 0; y < height; ++y) {
    const int y0 = std::max(y - r, 0), y1 = std::min(y + r + 1, height);
    for (int x = 0; x < width; ++x) {
      const int x0 = std::max(x - r, 0), x1 = std::min(x + r + 1, width);
      out[size_t(y) * width + x] = table[size_t(y1) * (width + 1) + x1] - table[size_t(y0) * (width + 1) + x1] -
                                   table[size_t(y1) * (width + 1) + x0] + table[size_t(y0) * (width + 1) + x0];
    }
  }
  return out;
}

// A texture as HSV, and which slot each texel belongs to.
struct Analysis {
  int width = 0, height = 0;
  std::vector<Hsv> hsv;
  std::vector<double> coloured;  // 0..1
  std::vector<double> nearest;   // degrees to the nearest slot median
  std::vector<int> slot;         // -1: not recoloured
  std::vector<uint8_t> hard;     // the slot came from a whole island
};

Analysis Analyse(const PortRemastered::Image& image, int suit, const std::vector<int>* transfer) {
  const Suit& info = kSuits[suit];
  const int n = HueSlots(info);
  Analysis a;
  a.width = image.width;
  a.height = image.height;
  const size_t count = size_t(image.width) * size_t(image.height);
  a.hsv.resize(count);
  a.coloured.resize(count);
  a.nearest.resize(count);
  a.slot.assign(count, -1);
  a.hard.assign(count, 0);
  std::vector<int> label(count);
  std::vector<uint8_t> in(count);
  for (size_t i = 0; i < count; ++i) {
    const uint8_t* t = &image.rgba[i * 4];
    const Hsv hsv = ToHsv(t[0] / 255.0, t[1] / 255.0, t[2] / 255.0);
    a.hsv[i] = hsv;
    const double opaque = t[3] > 127 ? 1.0 : 0.0;
    a.coloured[i] = info.glowOnly ? Smooth(hsv.s, 0.45, 0.55) * Smooth(hsv.v, 0.25, 0.35) * opaque
                                  : Smooth(hsv.s, 0.12, 0.25) * Smooth(hsv.v, 0.08, 0.16) * opaque;
    int best = 0;
    double bestDistance = 1e9;
    for (int j = 0; j < n; ++j) {
      const double d = HueDistance(hsv.h, info.medians[j].h);
      if (d < bestDistance) {
        bestDistance = d, best = j;
      }
    }
    label[i] = best;
    a.nearest[i] = bestDistance;
    in[i] = a.coloured[i] > 0.5 && bestDistance < kGateZero;
  }

  // Majority of the coloured texels round each, ties kept as they are.
  const int k = image.width >= 256 ? 5 : 3;
  std::vector<std::vector<int>> votes(n);
  for (int j = 0; j < n; ++j) {
    std::vector<uint8_t> mask(count);
    for (size_t i = 0; i < count; ++i) {
      mask[i] = in[i] && label[i] == j;
    }
    votes[j] = BoxSum(mask, image.width, image.height, k);
  }
  std::vector<int> cleaned(count);
  for (size_t i = 0; i < count; ++i) {
    int best = 0, top = -1, second = -1;
    for (int j = 0; j < n; ++j) {
      const int v = votes[j][i];
      if (v > top) {
        second = top, top = v, best = j;
      } else if (v > second) {
        second = v;
      }
    }
    cleaned[i] = (top <= 0 || (n > 1 && top == second)) ? label[i] : best;
    if (transfer != nullptr && in[i] && (*transfer)[i] >= 0 && (*transfer)[i] < n) {
      cleaned[i] = (*transfer)[i];
    }
  }

  // Islands: 4-connected coloured texels between dark seams. One that is
  // mostly one slot (or small) is that slot, near enough in hue.
  std::vector<int> island(count, -1);
  std::vector<size_t> stack;
  for (size_t start = 0; start < count; ++start) {
    if (!in[start] || a.hsv[start].v < kSeamValue || island[start] >= 0) {
      continue;
    }
    std::vector<int> tally(n, 0);
    std::vector<size_t> members;
    const int id = int(start);
    island[start] = id;
    stack.push_back(start);
    while (!stack.empty()) {
      const size_t p = stack.back();
      stack.pop_back();
      members.push_back(p);
      ++tally[cleaned[p]];
      const int x = int(p % image.width), y = int(p / image.width);
      const auto visit = [&](int qx, int qy) {
        if (qx < 0 || qy < 0 || qx >= image.width || qy >= image.height) {
          return;
        }
        const size_t q = size_t(qy) * image.width + qx;
        if (in[q] && a.hsv[q].v >= kSeamValue && island[q] < 0) {
          island[q] = id;
          stack.push_back(q);
        }
      };
      visit(x, y - 1);
      visit(x, y + 1);
      visit(x - 1, y);
      visit(x + 1, y);
    }
    const int best = int(std::max_element(tally.begin(), tally.end()) - tally.begin());
    const double share = double(tally[best]) / double(members.size());
    if (share < kIslandShare && int(members.size()) >= kIslandMin) {
      continue;
    }
    for (const size_t p : members) {
      if (HueDistance(a.hsv[p].h, info.medians[best].h) < kIslandHue) {
        cleaned[p] = best;
        a.hard[p] = 1;
      }
    }
  }
  for (size_t i = 0; i < count; ++i) {
    if (in[i]) {
      a.slot[i] = cleaned[i];
    }
  }
  return a;
}

}  // namespace

const Suit& SuitInfo(int suit) { return kSuits[std::clamp(suit, 0, kSuitCount - 1)]; }

uint32_t DefaultColour(int suit, int slot) {
  const Hsv& m = SuitInfo(suit).medians[std::clamp(slot, 0, kMaxSlots - 1)];
  double rgb[3];
  FromHsv(m.h, m.s, m.v, rgb);
  uint32_t out = 0;
  for (const double c : rgb) {
    out = (out << 8) | uint32_t(std::clamp(std::lround(c), 0l, 255l));
  }
  return out;
}

std::optional<uint32_t> Colour(int suit, int slot) {
  if (suit < 0 || suit >= kSuitCount || slot < 0 || slot >= kSuits[suit].slots) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(sMutex);
  return sPicks[suit][slot];
}

void SetColour(int suit, int slot, std::optional<uint32_t> rgb) {
  if (suit < 0 || suit >= kSuitCount || slot < 0 || slot >= kSuits[suit].slots) {
    return;
  }
  std::lock_guard<std::mutex> lock(sMutex);
  sPicks[suit][slot] = rgb ? std::optional<uint32_t>(*rgb & 0xFFFFFFu) : std::nullopt;
}

Picks SuitPicks(int suit) {
  if (suit < 0 || suit >= kSuitCount) {
    return {};
  }
  std::lock_guard<std::mutex> lock(sMutex);
  return sPicks[suit];
}

void Randomise(int suit, uint32_t seed) {
  if (suit < 0 || suit >= kSuitCount) {
    return;
  }
  std::mt19937 random(seed);
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  const double start = unit(random) * 360.0;
  // The golden angle apart, jittered, so the slots never share a hue. A base
  // slot stays pale, so the white parts still read against the rest.
  for (int slot = 0; slot < kSuits[suit].slots; ++slot) {
    const bool pale = kSuits[suit].base && slot == kSuits[suit].slots - 1;
    const double h = start + slot * 137.5 + (unit(random) - 0.5) * 30.0;
    const double s = pale ? 0.1 + 0.25 * unit(random) : 0.55 + 0.45 * unit(random);
    const double v = pale ? 0.8 + 0.2 * unit(random) : 0.6 + 0.4 * unit(random);
    double rgb[3];
    FromHsv(h, s, v, rgb);
    uint32_t value = 0;
    for (const double c : rgb) {
      value = (value << 8) | uint32_t(std::clamp(std::lround(c), 0l, 255l));
    }
    SetColour(suit, slot, value);
  }
}

std::string FormatColour(std::optional<uint32_t> rgb) {
  if (!rgb) {
    return {};
  }
  char text[8];
  std::snprintf(text, sizeof(text), "%06X", unsigned(*rgb & 0xFFFFFFu));
  return text;
}

std::optional<uint32_t> ParseColour(const std::string& text) {
  size_t at = !text.empty() && text[0] == '#' ? 1 : 0;
  if (text.size() - at != 6) {
    return std::nullopt;
  }
  uint32_t value = 0;
  for (; at < text.size(); ++at) {
    const char c = text[at];
    const int digit = c >= '0' && c <= '9'   ? c - '0'
                      : c >= 'a' && c <= 'f' ? c - 'a' + 10
                      : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                             : -1;
    if (digit < 0) {
      return std::nullopt;
    }
    value = (value << 4) | uint32_t(digit);
  }
  return value;
}

std::string PicksKey(int suit) { return PicksKey(suit, SuitPicks(suit)); }

std::string PicksKey(int suit, const Picks& picks) {
  std::string key;
  bool any = false;
  for (int j = 0; j < SuitInfo(suit).slots; ++j) {
    key += (j ? "," : "") + (picks[j] ? FormatColour(picks[j]) : std::string("-"));
    any |= picks[j].has_value();
  }
  return any ? key : std::string();
}

void SetPicksKey(int suit, const std::string& key) {
  if (suit < 0 || suit >= kSuitCount) {
    return;
  }
  size_t at = 0;
  for (int j = 0; j < kSuits[suit].slots; ++j) {
    const size_t begin = std::min(at, key.size());
    const size_t end = std::min(key.find(',', begin), key.size());
    SetColour(suit, j, ParseColour(key.substr(begin, end - begin)));
    at = end + 1;
  }
}

void SetServed(const std::array<std::string, kSuitCount>& served) {
  std::lock_guard<std::mutex> lock(sMutex);
  sServed = served;
}

bool Pending(bool wanted) {
  std::array<std::string, kSuitCount> served;
  {
    std::lock_guard<std::mutex> lock(sMutex);
    served = sServed;
  }
  for (int suit = 0; suit < kSuitCount; ++suit) {
    if ((wanted ? PicksKey(suit) : std::string()) != served[suit]) {
      return true;
    }
  }
  return false;
}

size_t Recolour(PortRemastered::Image& image, int suit, const Picks& picks, const PortRemastered::Image* transfer,
                int transferSuit) {
  if (suit < 0 || suit >= kSuitCount || image.width <= 0 || image.height <= 0 ||
      image.rgba.size() != size_t(image.width) * size_t(image.height) * 4) {
    return 0;
  }
  const Suit& info = kSuits[suit];
  std::vector<int> transferSlots;
  if (transfer != nullptr && transfer->width == image.width && transfer->height == image.height &&
      transfer->rgba.size() == image.rgba.size() && transferSuit >= 0 && transferSuit < kSuitCount) {
    transferSlots = Analyse(*transfer, transferSuit, nullptr).slot;
  }
  const Analysis a = Analyse(image, suit, transferSlots.empty() ? nullptr : &transferSlots);
  const int n = HueSlots(info);
  const int base = info.base ? info.slots - 1 : -1;
  const size_t count = a.hsv.size();

  // Each slot's share of a texel: the 3x3 mix of slots round it, so a border
  // blends; one-hot where a whole island decided it.
  std::vector<uint8_t> any(count);
  for (size_t i = 0; i < count; ++i) {
    any[i] = a.slot[i] >= 0;
  }
  const std::vector<int> around = BoxSum(any, a.width, a.height, 3);
  std::vector<std::vector<int>> share(n);
  for (int j = 0; j < n; ++j) {
    std::vector<uint8_t> mask(count);
    for (size_t i = 0; i < count; ++i) {
      mask[i] = a.slot[i] == j;
    }
    share[j] = BoxSum(mask, a.width, a.height, 3);
  }

  std::array<Hsv, kMaxSlots> targets{};
  bool anyPick = false;
  for (int j = 0; j < info.slots; ++j) {
    if (picks[j]) {
      const uint32_t c = *picks[j];
      targets[j] = ToHsv(((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0);
      anyPick = true;
    }
  }
  if (!anyPick) {
    return 0;
  }
  const bool basePicked = base >= 0 && picks[base].has_value();

  size_t changed = 0;
  for (size_t i = 0; i < count; ++i) {
    const Hsv& t = a.hsv[i];
    double gate = (1.0 - Smooth(a.nearest[i], kGateFull, kGateZero)) * a.coloured[i];
    if (info.warm) {
      gate *= 1.0 - Smooth(t.h, 70.0, 75.0) * (1.0 - Smooth(t.h, 200.0, 205.0));
    }
    uint8_t* texel = &image.rgba[i * 4];
    // The white parts: light texels the hue slots leave.
    const double baseGate =
        basePicked && texel[3] > 127 ? (1.0 - a.coloured[i]) * Smooth(t.v, kBaseLow, kBaseHigh) : 0.0;
    if (gate <= 0.0 && baseGate <= 0.0) {
      continue;
    }
    const double original[3] = {double(texel[0]), double(texel[1]), double(texel[2])};
    // A soft-edge texel outside every slot moves with its nearest slot.
    std::array<double, kMaxSlots> weight{};
    if (a.slot[i] < 0 || a.hard[i]) {
      int nearest = a.slot[i];
      if (nearest < 0) {
        double best = 1e9;
        for (int j = 0; j < n; ++j) {
          const double d = HueDistance(t.h, info.medians[j].h);
          if (d < best) {
            best = d, nearest = j;
          }
        }
      }
      weight[nearest] = 1.0;
    } else {
      double sum = 0.0;
      for (int j = 0; j < n; ++j) {
        weight[j] = double(share[j][i]) / double(std::max(around[i], 1));
        sum += weight[j];
      }
      for (int j = 0; j < n; ++j) {
        weight[j] = sum > 0.0 ? weight[j] / sum : 0.0;
      }
    }
    double out[3] = {0.0, 0.0, 0.0};
    for (int j = 0; j < n; ++j) {
      if (weight[j] <= 0.0) {
        continue;
      }
      double rgb[3] = {original[0], original[1], original[2]};
      if (picks[j]) {
        const Hsv& m = info.medians[j];
        const Hsv& to = targets[j];
        const double h = to.h + kCompress * Wrap(t.h - m.h);
        const double s = std::clamp(t.s * std::min(kScaleMax, to.s / std::max(m.s, 1e-3)), 0.0, 1.0);
        const double v = std::clamp(t.v * std::min(kScaleMax, to.v / std::max(m.v, 1e-3)), 0.0, 1.0);
        FromHsv(h, s, v, rgb);
      }
      for (int c = 0; c < 3; ++c) {
        out[c] += weight[j] * rgb[c];
      }
    }
    // Their hue means nothing, so the picked one; their saturation is added to.
    double white[3] = {original[0], original[1], original[2]};
    if (baseGate > 0.0) {
      const Hsv& m = info.medians[base];
      const Hsv& to = targets[base];
      const double s = std::clamp(t.s + to.s - m.s, 0.0, 1.0);
      const double v = std::clamp(t.v * std::min(kScaleMax, to.v / std::max(m.v, 1e-3)), 0.0, 1.0);
      FromHsv(to.h, s, v, white);
    }
    bool differs = false;
    for (int c = 0; c < 3; ++c) {
      double mixed = gate * out[c] + (1.0 - gate) * original[c];
      mixed = baseGate * white[c] + (1.0 - baseGate) * mixed;
      const uint8_t byte = uint8_t(std::clamp(std::floor(mixed + 0.5), 0.0, 255.0));
      differs |= byte != texel[c];
      texel[c] = byte;
    }
    changed += differs;
  }
  return changed;
}

}  // namespace PortSuitColours
