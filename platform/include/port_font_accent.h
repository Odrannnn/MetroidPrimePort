#pragma once

// Accents for the disc bitmap (Deface) fonts.
//
// The retail fonts hold ASCII only; PortAddStandIns gives each accented letter
// the cell of an ASCII stand-in. This module draws the diacritic into a copy
// of that cell, so the bitmap font shows the real character while the layout
// (advances, baselines) stays the disc's.
//
// Everything here works on plain grids of texel values (palette indices for
// the CI fonts, intensities for the I ones) so it can be unit tested without
// the game: the caller decodes its tiled texture into grids and encodes the
// results back. Coordinate origin is the cell's top left.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace PortFontAccent {

// The GameCube texel formats a font texture may use, with ETexelFormat's values.
enum class Format : int {
  I4 = 0,
  I8 = 1,
  IA4 = 2,
  C4 = 4,
  C8 = 5,
};

// What is drawn over (or under) the base letter. None keeps the plain copy:
// ligatures (ae, oe), eszett, eth, thorn and punctuation.
enum class Mark {
  None,
  Acute,
  Grave,
  Circumflex,
  Diaeresis,
  Tilde,
  Ring,
  Cedilla,
  Slash,  // through the letter (o with stroke)
};

// The mark for a character, from its Unicode code point. None when the plain
// stand-in copy is kept.
Mark MarkFor(uint32_t character);

// A decoded cell: w x h texel values in row order.
struct Grid {
  int w = 0;
  int h = 0;
  std::vector<uint8_t> v;  // size w*h
};

// The block geometry of a tiled format. False for formats fonts don't use.
bool BlockInfo(Format format, int& blockW, int& blockH, int& bitsPerPixel);

// One texel of mip 0, in pixels from the texture's top left.
bool Decode(const uint8_t* data, size_t size, Format format, int texW, int texH, int x, int y,
            uint8_t& out);
bool Encode(uint8_t* data, size_t size, Format format, int texW, int texH, int x, int y,
            uint8_t value);

// Copies `base` (the stand-in's cell, baseW x baseH values) into a new cell
// `cellW` wide carrying the mark, and describes the result:
//   `shiftDown`  rows the letter moved down (its ink stays on the same line)
//   `cellH`      the new cell's height (baseH grown as needed, never shrunk)
//   `ink`        the texel value to draw the mark with
// `outlined` draws a one pixel outline around the mark with `outline`, as the
// outlined Deface fonts have. Cedilla hangs below the letter and Slash strikes
// through it; every other mark sits above the ink. Advances are untouched.
// False when the mark is None or the base holds no ink (keep the plain copy).
bool Composite(const Grid& base, Mark mark, int cellW, int baseH, uint8_t ink, uint8_t outline,
               bool outlined, Grid& out, int& shiftDown, int& cellH);

// The infinity sign (U+221E) for the HUD's infinite ammo, which no disc font
// has. Drawn in the style of the font's '8' (`eight`, its decoded cell): as
// tall as the 8's ink, with its stroke, and the outline ring the outlined
// fonts put around it. `out` is the 8's cell widened by `extraW` columns, ink
// on the same rows, so the 8's baseline holds; the caller widens the advance
// by `extraW` too. False when the 8 holds no ink or is too small to keep the
// loops open.
bool Infinity(const Grid& eight, uint8_t ink, uint8_t outline, bool outlined, Grid& out,
              int& extraW);

}  // namespace PortFontAccent
