// The .sdfont file, the fit of a distance-field font to a bitmap one, and the
// FONT reader. With arguments it converts the user's own data, which is how a
// test mod gets its font without a full import:
//   port_hd_font_tests <FONT asset> <atlas.rgba> <width> <height> <out.sdfont>

#define PORT_HD_FONT_FILE_ONLY
#include "port_hd_font.h"
#include "port_remastered_font.h"

#include "port_font_accent.h"
#include "port_test.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

bool Close(float a, float b) { return std::fabs(a - b) < 0.01f; }

void Put32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(uint8_t(value >> (i * 8)));
  }
}

void PutFloat(std::vector<uint8_t>& out, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  Put32(out, bits);
}

// A capital H as Remastered stores it: 22 pixels of ink, 4 of field around it,
// and the ink one pixel below the baseline.
PortHdFont::Glyph CapitalH() {
  PortHdFont::Glyph glyph;
  glyph.character = 'H';
  glyph.left = -3.f;
  glyph.top = 25.f;
  glyph.width = 25.f;
  glyph.height = 30.f;
  glyph.u0 = 0.25f;
  glyph.v0 = 0.75f;
  glyph.u1 = 0.5f;
  glyph.v1 = 0.25f;
  glyph.advance = 20.f;
  return glyph;
}

PortHdFont::Font SmallFont() {
  PortHdFont::Font font;
  font.width = 8;
  font.height = 4;
  font.padding = 4.f;
  font.perPixel = 0.2f;
  font.glyphs.push_back(CapitalH());
  PortHdFont::Glyph comma = CapitalH();
  comma.character = 0x3001;
  font.glyphs.push_back(comma);
  for (int i = 0; i < 32; ++i) {
    font.distance.push_back(uint8_t(i * 8));
  }
  return font;
}

void TestFile() {
  const PortHdFont::Font font = SmallFont();
  std::vector<uint8_t> bytes;
  CHECK_MSG(PortHdFont::WriteFont(font, bytes), "a font is written");
  PortHdFont::Font read;
  std::string error;
  CHECK_MSG(PortHdFont::ReadFont(bytes.data(), bytes.size(), read, error), "and read back");
  CHECK_MSG(read.width == 8 && read.height == 4 && read.glyphs.size() == 2 && read.distance == font.distance,
            "with its atlas");
  CHECK_MSG(Close(read.padding, 4.f) && Close(read.perPixel, 0.2f), "its field");
  const PortHdFont::Glyph* const h = read.Find('H');
  CHECK_MSG(h != nullptr && Close(h->top, 25.f) && Close(h->u1, 0.5f) && Close(h->advance, 20.f), "and its glyphs");
  CHECK_MSG(read.Find(0x3001) != nullptr && read.Find('I') == nullptr, "glyphs are found by character");

  CHECK_MSG(!PortHdFont::ReadFont(bytes.data(), bytes.size() - 1, read, error), "a cut file is refused");
  std::vector<uint8_t> bad = bytes;
  bad[0] = 'X';
  CHECK_MSG(!PortHdFont::ReadFont(bad.data(), bad.size(), read, error), "as is another file");
  bad = bytes;
  bad[4] = 9;
  CHECK_MSG(!PortHdFont::ReadFont(bad.data(), bad.size(), read, error), "and another version");

  PortHdFont::Font empty = font;
  empty.glyphs.clear();
  CHECK_MSG(!PortHdFont::WriteFont(empty, bytes), "a font needs glyphs");
  PortHdFont::Font shortAtlas = font;
  shortAtlas.distance.pop_back();
  CHECK_MSG(!PortHdFont::WriteFont(shortAtlas, bytes), "and a whole atlas");

  CHECK_MSG(PortHdFont::ParseFileName("deface.sdfont") && PortHdFont::ParseFileName("Deface.SDFONT"), "the file's name");
  CHECK_MSG(!PortHdFont::ParseFileName("deface.sdf") && !PortHdFont::ParseFileName(".sdfont"), "and what is not one");
}

void TestFit() {
  const PortHdFont::Font font = SmallFont();
  const PortHdFont::Glyph& h = font.glyphs[0];
  PortHdFont::Fit fit;
  // A plain bitmap font: the H fills its 22 pixel cell and stands on the line.
  CHECK_MSG(PortHdFont::FitFont(font, 22, 22, false, fit), "a plain font fits");
  CHECK_MSG(Close(fit.scale, 1.f) && Close(fit.baseline, 0.f) && fit.edge == 128, "at its own size");
  PortHdFont::Box box = PortHdFont::GlyphBox(h, fit, 100.f, 50.f, 17, 22);
  // The ink is the box less its padding: it must be the cell, 50 to 72.
  CHECK_MSG(Close(box.top + 4.f, 50.f) && Close(box.bottom - 4.f, 72.f), "the ink fills the cell's height");
  CHECK_MSG(Close((box.left + box.right) / 2.f, 108.5f), "and is centred in its width");

  // Half the size.
  CHECK_MSG(PortHdFont::FitFont(font, 11, 11, false, fit) && Close(fit.scale, 0.5f), "a smaller font scales");
  box = PortHdFont::GlyphBox(h, fit, 0.f, 0.f, 9, 11);
  CHECK_MSG(Close(box.top + 2.f, 0.f) && Close(box.bottom - 2.f, 11.f), "with its padding");

  // An outlined font: a 13 pixel H in a 15 pixel cell whose line is 13 down.
  CHECK_MSG(PortHdFont::FitFont(font, 15, 13, true, fit), "an outlined font fits");
  CHECK_MSG(Close(fit.scale, 13.f / 22.f) && Close(fit.baseline, 1.f), "inside its outline");
  CHECK_MSG(fit.edge < 128 && fit.edge >= 16, "which the edge leaves room for");
  box = PortHdFont::GlyphBox(h, fit, 0.f, 0.f, 13, 13);
  CHECK_MSG(Close(box.top + 4.f * fit.scale, 1.f) && Close(box.bottom - 4.f * fit.scale, 14.f),
            "the ink is a pixel inside the cell");

  PortHdFont::Font noH = font;
  noH.glyphs.erase(noH.glyphs.begin());
  CHECK_MSG(!PortHdFont::FitFont(noH, 22, 22, false, fit), "a font without an H cannot be measured");
  CHECK_MSG(!PortHdFont::FitFont(font, 0, 0, false, fit), "nor an empty cell");
}

void PutGlyph(std::vector<uint8_t>& out, uint16_t character, uint16_t face, uint8_t page) {
  out.push_back(uint8_t(character));
  out.push_back(uint8_t(character >> 8));
  out.push_back(uint8_t(face));
  out.push_back(uint8_t(face >> 8));
  Put32(out, uint32_t(page) << 24);
  const float values[9] = {-3.f, 25.f, 25.f, 30.f, 0.25f, 0.75f, 0.5f, 0.25f, 20.f};
  for (float value : values) {
    PutFloat(out, value);
  }
  Put32(out, 0);
}

void TestAsset() {
  std::vector<uint8_t> asset(0x20, 0);
  std::memcpy(asset.data(), "RFRM", 4);
  std::memcpy(asset.data() + 0x14, "FONT", 4);
  Put32(asset, 2);
  for (int i = 0; i < 32; ++i) {
    asset.push_back(uint8_t(i + 1));
  }
  Put32(asset, 1);
  Put32(asset, 2);
  PutFloat(asset, 32.f);
  PutFloat(asset, 30.f);
  Put32(asset, 2);
  PutFloat(asset, 4.f);
  PutFloat(asset, 6.f);
  Put32(asset, 1);
  asset.insert(asset.end(), 16, 0);
  Put32(asset, 4);
  Put32(asset, 2);
  PutGlyph(asset, 'I', 0, 0);
  // Two kerning pairs after it, which read as a glyph if taken for a record.
  asset[asset.size() - 4] = 2;
  for (const char next : {'H', 'I'}) {
    asset.push_back(uint8_t(next));
    asset.push_back(0);
    PutFloat(asset, -0.78f);
  }
  PutGlyph(asset, 'H', 0, 0);
  PutGlyph(asset, 0x3042, 1, 1);
  PutGlyph(asset, 'H', 2, 3);

  PortRemastered::ModelUuid atlas{};
  PortHdFont::Font font;
  std::string error;
  CHECK_MSG(PortRemastered::ParseFont(asset.data(), asset.size(), atlas, font, error), "a FONT asset is read");
  CHECK_MSG(atlas[0] == 4 && atlas[3] == 1 && atlas[4] == 6 && atlas[15] == 16, "its first texture, in printed order");
  CHECK_MSG(font.glyphs.size() == 2 && font.glyphs[0].character == 'H' && font.glyphs[1].character == 'I',
            "and its first face, sorted");
  CHECK_MSG(Close(font.padding, 4.f) && font.perPixel > 0.f, "with that texture's padding");
  CHECK_MSG(!PortRemastered::ParseFont(asset.data(), asset.size() - 8, atlas, font, error), "a cut asset is refused");
  asset[0x14] = 'X';
  CHECK_MSG(!PortRemastered::ParseFont(asset.data(), asset.size(), atlas, font, error), "as is another asset");

  const uint8_t rgba[16] = {10, 0, 0, 255, 20, 0, 0, 255, 30, 0, 0, 255, 40, 0, 0, 255};
  CHECK_MSG(PortRemastered::SetFontAtlas(font, 2, 2, rgba, sizeof(rgba)) && font.distance[3] == 40,
            "the atlas is the texture's red");
  CHECK_MSG(!PortRemastered::SetFontAtlas(font, 3, 2, rgba, sizeof(rgba)), "of the size it says");
}

std::vector<uint8_t> ReadFile(const char* path) {
  std::ifstream file(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

int Convert(char** argv) {
  const std::vector<uint8_t> asset = ReadFile(argv[1]);
  const std::vector<uint8_t> rgba = ReadFile(argv[2]);
  PortRemastered::ModelUuid atlas{};
  PortHdFont::Font font;
  std::string error;
  if (!PortRemastered::ParseFont(asset.data(), asset.size(), atlas, font, error)) {
    std::fprintf(stderr, "%s: %s\n", argv[1], error.c_str());
    return 1;
  }
  std::vector<uint8_t> out;
  if (!PortRemastered::SetFontAtlas(font, uint32_t(std::atoi(argv[3])), uint32_t(std::atoi(argv[4])), rgba.data(),
                                    rgba.size()) ||
      !PortHdFont::WriteFont(font, out)) {
    std::fprintf(stderr, "%s: not a %sx%s RGBA image\n", argv[2], argv[3], argv[4]);
    return 1;
  }
  std::ofstream file(argv[5], std::ios::binary);
  file.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
  std::printf("%zu glyphs, padding %g, atlas texture %02x%02x%02x%02x..., %zu bytes\n", font.glyphs.size(),
              font.padding, atlas[0], atlas[1], atlas[2], atlas[3], out.size());
  return file ? 0 : 1;
}
}  // namespace

void TestStandIns() {
  bool e = false;
  bool euro = false;
  bool ascii = false;
  for (const PortHdFont::StandIn& standIn : PortHdFont::StandIns()) {
    e = e || (standIn.character == 0xE9 && standIn.base == 'e');
    euro = euro || (standIn.character == 0x20AC && standIn.base == 'E');
    ascii = ascii || standIn.character < 0x80 || uint8_t(standIn.base) < 0x20 || uint8_t(standIn.base) >= 0x7F;
  }
  CHECK_MSG(e && euro, "accented letters and symbols stand on their base letter");
  CHECK_MSG(!ascii, "stand-ins are for characters outside ASCII, on printable ones");

  PortHdFont::Font font = SmallFont();
  PortHdFont::Glyph wide = CapitalH();
  wide.character = 0x152;  // OE
  wide.advance = 30.f;
  font.glyphs.insert(font.glyphs.begin() + 1, wide);
  CHECK_MSG(Close(PortHdFont::AdvanceRatio(font, 0x152, 'H'), 1.5f), "the advance scales by the typeface's ratio");
  CHECK_MSG(Close(PortHdFont::AdvanceRatio(font, 0xE9, 'H'), 1.f), "a missing glyph keeps the base's advance");
  wide.advance = 1000.f;
  font.glyphs[1] = wide;
  CHECK_MSG(Close(PortHdFont::AdvanceRatio(font, 0x152, 'H'), 4.f), "the ratio is clamped");
}

void TestAccents() {
  using namespace PortFontAccent;
  CHECK_MSG(MarkFor(0xE9) == Mark::Acute, "e-acute takes an acute");
  CHECK_MSG(MarkFor(0xC0) == Mark::Grave, "A-grave takes a grave");
  CHECK_MSG(MarkFor(0xEA) == Mark::Circumflex, "e-circumflex takes a circumflex");
  CHECK_MSG(MarkFor(0xFC) == Mark::Diaeresis, "u-diaeresis takes a diaeresis");
  CHECK_MSG(MarkFor(0xF1) == Mark::Tilde, "n-tilde takes a tilde");
  CHECK_MSG(MarkFor(0xE5) == Mark::Ring, "a-ring takes a ring");
  CHECK_MSG(MarkFor(0xE7) == Mark::Cedilla, "c-cedilla takes a cedilla");
  CHECK_MSG(MarkFor(0xF8) == Mark::Slash, "o-stroke takes a slash");
  CHECK_MSG(MarkFor(0xE6) == Mark::None && MarkFor(0x153) == Mark::None, "ligatures keep the plain copy");
  CHECK_MSG(MarkFor(0xDF) == Mark::None && MarkFor(0xD0) == Mark::None && MarkFor(0xFE) == Mark::None,
            "eszett, eth and thorn keep the plain copy");
  CHECK_MSG(MarkFor(0x20AC) == Mark::None && MarkFor('e') == Mark::None, "symbols keep the plain copy");

  // A tiled C4 round trip: the high nibble comes first.
  std::vector<uint8_t> tiled(32, 0);
  CHECK_MSG(Encode(tiled.data(), tiled.size(), Format::C4, 8, 8, 0, 0, 1), "a texel is written");
  CHECK_MSG(Encode(tiled.data(), tiled.size(), Format::C4, 8, 8, 1, 0, 2), "and its neighbour");
  CHECK_MSG(Encode(tiled.data(), tiled.size(), Format::C4, 8, 8, 7, 7, 3), "and the last one");
  uint8_t v = 0;
  CHECK_MSG(Decode(tiled.data(), tiled.size(), Format::C4, 8, 8, 0, 0, v) && v == 1, "the first reads back");
  CHECK_MSG(Decode(tiled.data(), tiled.size(), Format::C4, 8, 8, 1, 0, v) && v == 2, "and the second");
  CHECK_MSG(Decode(tiled.data(), tiled.size(), Format::C4, 8, 8, 7, 7, v) && v == 3, "and the last");
  CHECK_MSG(tiled[0] == 0x12, "nibbles share a byte, high first");
  CHECK_MSG(!Decode(tiled.data(), tiled.size(), Format::C4, 8, 8, 8, 0, v), "outside is refused");
  int bw = 0, bh = 0, bpp = 0;
  CHECK_MSG(BlockInfo(Format::C4, bw, bh, bpp) && bw == 8 && bh == 8 && bpp == 4, "C4 blocks are 8x8");
  CHECK_MSG(BlockInfo(Format::C8, bw, bh, bpp) && bw == 8 && bh == 4 && bpp == 8, "C8 blocks are 8x4");
  CHECK_MSG(BlockInfo(Format::I8, bw, bh, bpp) && bpp == 8, "intensity formats decode too");

  // A fake lowercase 'e': a 6x8 block of ink (1) with an outline (2) above it,
  // in a 12x22 cell.
  Grid base;
  base.w = 12;
  base.h = 22;
  base.v.assign(12 * 22, 0);
  for (int y = 8; y < 16; ++y) {
    for (int x = 3; x < 9; ++x) {
      base.v[size_t(y) * 12 + size_t(x)] = 1;
    }
  }
  for (int x = 3; x < 9; ++x) {
    base.v[size_t(7) * 12 + size_t(x)] = 2;
  }
  Grid out;
  int shift = -1, cellH = -1;
  CHECK_MSG(Composite(base, Mark::Acute, 12, 22, 1, 2, true, out, shift, cellH), "an acute composites");
  CHECK_MSG(shift == 0 && cellH == 22, "a lowercase-topped letter needs no new rows");
  bool markAbove = false, inkKept = true, outlineMade = false;
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 12; ++x) {
      if (out.v[size_t(y) * 12 + size_t(x)] == 1) {
        markAbove = true;
      }
    }
  }
  for (int y = 8; y < 16; ++y) {
    for (int x = 3; x < 9; ++x) {
      if (out.v[size_t(y + shift) * 12 + size_t(x)] != 1) {
        inkKept = false;
      }
    }
  }
  for (uint8_t q : out.v) {
    outlineMade = outlineMade || q == 2;
  }
  CHECK_MSG(markAbove, "the mark sits above the ink");
  CHECK_MSG(inkKept, "the letter is kept where it was");
  CHECK_MSG(outlineMade, "the mark gets an outline");

  // A fake capital: ink from row 1 to row 20, no room for the mark.
  Grid cap = base;
  cap.v.assign(12 * 22, 0);
  for (int y = 1; y < 21; ++y) {
    for (int x = 3; x < 9; ++x) {
      cap.v[size_t(y) * 12 + size_t(x)] = 1;
    }
  }
  Grid capOut;
  int capShift = -1, capH = -1;
  CHECK_MSG(Composite(cap, Mark::Acute, 12, 22, 1, 1, false, capOut, capShift, capH),
            "a capital composites");
  CHECK_MSG(capShift == 6 && capH == 27, "a capital-topped letter shifts down and the cell grows");
  bool capMark = false, capInk = true;
  for (int y = 0; y < 6; ++y) {
    for (int x = 0; x < 12; ++x) {
      capMark = capMark || capOut.v[size_t(y) * 12 + size_t(x)] == 1;
    }
  }
  for (int y = 1; y < 21; ++y) {
    for (int x = 3; x < 9; ++x) {
      capInk = capInk && capOut.v[size_t(y + capShift) * 12 + size_t(x)] == 1;
    }
  }
  CHECK_MSG(capMark && capInk, "the mark is on top and the letter below it, on the same line");

  Grid low;
  int lowShift = -1, lowH = -1;
  CHECK_MSG(Composite(base, Mark::Diaeresis, 12, 22, 1, 2, false, low, lowShift, lowH),
            "a diaeresis composites");
  CHECK_MSG(lowShift == 0 && lowH == 22, "a lowercase-topped letter needs no new rows");
  int dots = 0;
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 12; ++x) {
      dots += low.v[size_t(y) * 12 + size_t(x)] == 1 ? 1 : 0;
    }
  }
  CHECK_MSG(dots >= 4, "both dots are drawn");

  Grid ced;
  int cedShift = 0, cedH = 0;
  CHECK_MSG(Composite(base, Mark::Cedilla, 12, 22, 1, 2, false, ced, cedShift, cedH),
            "a cedilla composites");
  bool hookBelow = false;
  for (int y = 16; y < cedH; ++y) {
    for (int x = 0; x < 12; ++x) {
      hookBelow = hookBelow || ced.v[size_t(y) * 12 + size_t(x)] == 1;
    }
  }
  CHECK_MSG(cedShift == 0 && hookBelow, "the cedilla hangs below the letter");

  Grid slash;
  int slashShift = -1, slashH = -1;
  CHECK_MSG(Composite(base, Mark::Slash, 12, 22, 1, 1, false, slash, slashShift, slashH),
            "a slash composites");
  CHECK_MSG(slashShift == 0 && slashH == 22, "the slash needs no new rows");

  Grid ring;
  int ringShift = -1, ringH = -1;
  CHECK_MSG(Composite(base, Mark::Ring, 12, 22, 1, 1, false, ring, ringShift, ringH),
            "a ring composites");
  int ringInk = 0;
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 12; ++x) {
      ringInk += ring.v[size_t(y) * 12 + size_t(x)] == 1 ? 1 : 0;
    }
  }
  CHECK_MSG(ringShift == 0 && ringInk >= 6, "the ring is a loop above the letter");

  for (Mark m : {Mark::Grave, Mark::Circumflex, Mark::Tilde}) {
    Grid g;
    int sh = -1, hh = -1;
    int before = 0;
    for (uint8_t q : base.v) {
      before += q == 1 ? 1 : 0;
    }
    CHECK_MSG(Composite(base, m, 12, 22, 1, 1, false, g, sh, hh), "every top mark composites");
    int after = 0;
    for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < 12; ++x) {
        after += g.v[size_t(y) * 12 + size_t(x)] == 1 ? 1 : 0;
      }
    }
    CHECK_MSG(sh == 0 && hh == 22 && after > 0 && int(g.v.size()) == 12 * 22, "above the ink, in place");
    (void)before;
  }

  Grid none;
  CHECK_MSG(!Composite(base, Mark::None, 12, 22, 1, 2, false, none, shift, cellH),
            "no mark composites nothing");
  Grid empty;
  empty.w = 12;
  empty.h = 22;
  empty.v.assign(12 * 22, 0);
  CHECK_MSG(!Composite(empty, Mark::Acute, 12, 22, 1, 2, false, none, shift, cellH),
            "a blank cell composites nothing");
}

// An 8: two stacked 2-pixel rings, 8 wide and 14 tall, in a 12x22 cell.
PortFontAccent::Grid FakeEight(bool outlined) {
  PortFontAccent::Grid g;
  g.w = 12;
  g.h = 22;
  g.v.assign(12 * 22, 0);
  for (int y = 4; y < 18; ++y) {
    for (int x = 2; x < 10; ++x) {
      const bool edge = x < 4 || x >= 8 || y < 6 || (y >= 10 && y < 12) || y >= 16;
      if (edge) {
        g.v[size_t(y) * 12 + size_t(x)] = 1;
      }
    }
  }
  if (outlined) {
    PortFontAccent::Grid ringed = g;
    for (int y = 0; y < 22; ++y) {
      for (int x = 0; x < 12; ++x) {
        bool near = false;
        for (int dy = -1; dy <= 1; ++dy) {
          for (int dx = -1; dx <= 1; ++dx) {
            const int nx = x + dx, ny = y + dy;
            near |= nx >= 0 && ny >= 0 && nx < 12 && ny < 22 && g.v[size_t(ny) * 12 + size_t(nx)] == 1;
          }
        }
        if (g.v[size_t(y) * 12 + size_t(x)] == 0 && near) {
          ringed.v[size_t(y) * 12 + size_t(x)] = 2;
        }
      }
    }
    g = ringed;
  }
  return g;
}

void TestInfinity() {
  using PortFontAccent::Grid;
  using PortFontAccent::Infinity;
  for (const bool outlined : {false, true}) {
    const Grid eight = FakeEight(outlined);
    Grid out;
    int extra = -1;
    CHECK_MSG(Infinity(eight, 1, 2, outlined, out, extra), "an infinity is drawn from the 8");
    CHECK_MSG(extra > 0 && out.w == eight.w + extra && out.h == eight.h,
              "the infinity cell is the 8's, widened");
    const auto at = [&out](int x, int y) { return out.v[size_t(y) * size_t(out.w) + size_t(x)]; };
    int left = out.w, right = -1, top = out.h, bottom = -1, inkCount = 0, outlineCount = 0;
    for (int y = 0; y < out.h; ++y) {
      for (int x = 0; x < out.w; ++x) {
        if (at(x, y) == 1) {
          ++inkCount;
          left = std::min(left, x);
          right = std::max(right, x);
          top = std::min(top, y);
          bottom = std::max(bottom, y);
        } else if (at(x, y) == 2) {
          ++outlineCount;
        }
      }
    }
    CHECK_MSG(inkCount > 0 && top == 4 && bottom == 17, "the infinity's ink spans the 8's rows");
    CHECK_MSG(left == 2 && right - left + 1 == 8 + extra, "the infinity is as wide as it claims");
    CHECK_MSG((outlineCount > 0) == outlined, "only an outlined 8 gives the infinity an outline");
    // Wider than tall, crossing in the middle, with both loops open.
    const int mid = (top + bottom) / 2;
    const int cx = (left + right) / 2;
    CHECK_MSG(at(cx, mid) == 1 || at(cx + 1, mid) == 1 || at(cx, mid + 1) == 1,
              "the strokes cross in the middle");
    const int qL = left + (right - left) / 4, qR = right - (right - left) / 4;
    CHECK_MSG(at(qL, mid) == 0 && at(qR, mid) == 0, "both loops are open");
  }
  Grid blank;
  blank.w = 12;
  blank.h = 22;
  blank.v.assign(12 * 22, 0);
  Grid out;
  int extra = 0;
  CHECK_MSG(!Infinity(blank, 1, 2, false, out, extra), "a blank 8 draws no infinity");
}

int main(int argc, char** argv) {
  if (argc == 6) {
    return Convert(argv);
  }
  TestFile();
  TestFit();
  TestAsset();
  TestStandIns();
  TestAccents();
  TestInfinity();
  if (port_test::Failures() == 0) {
    std::puts("port_hd_font tests passed");
  }
  return PORT_TEST_RESULT();
}
