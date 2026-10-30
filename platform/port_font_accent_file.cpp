// Accents for the disc bitmap fonts (port_font_accent.h): the marks and the
// compositing. No game headers here, so the unit test can use it directly.

#include "port_font_accent.h"

#include <algorithm>
#include <cmath>

namespace PortFontAccent {
namespace {

void Plot(Grid& grid, int x, int y, uint8_t value) {
  if (x >= 0 && y >= 0 && x < grid.w && y < grid.h) {
    grid.v[size_t(y) * size_t(grid.w) + size_t(x)] = value;
  }
}

uint8_t At(const Grid& grid, int x, int y) {
  if (x < 0 || y < 0 || x >= grid.w || y >= grid.h) {
    return 0;
  }
  return grid.v[size_t(y) * size_t(grid.w) + size_t(x)];
}

// A diagonal stroke through (x0, y1)-(x1, y0): `rising` goes up to the right
// (acute), otherwise up to the left (grave). Drawn `thick` pixels wide.
void Stroke(Grid& grid, int x0, int x1, int y0, int h, bool rising, int thick, uint8_t ink) {
  const int mw = x1 - x0 + 1;
  for (int r = 0; r < h; ++r) {
    const double t = h > 1 ? double(r) / double(h - 1) : 0.0;
    for (int i = 0; i < thick; ++i) {
      const int c = rising ? x1 - int(std::round(t * double(mw - thick))) - thick + 1 + i
                           : x0 + int(std::round(t * double(mw - thick))) + i;
      Plot(grid, c, y0 + r, ink);
    }
  }
}

}  // namespace

Mark MarkFor(uint32_t character) {
  switch (character) {
  case 0xC0:
  case 0xE0:  // A a grave
  case 0xC8:
  case 0xE8:  // E e grave
  case 0xCC:
  case 0xEC:  // I i grave
  case 0xD2:
  case 0xF2:  // O o grave
  case 0xD9:
  case 0xF9:  // U u grave
    return Mark::Grave;
  case 0xC1:
  case 0xE1:  // A a acute
  case 0xC9:
  case 0xE9:  // E e acute
  case 0xCD:
  case 0xED:  // I i acute
  case 0xD3:
  case 0xF3:  // O o acute
  case 0xDA:
  case 0xFA:  // U u acute
  case 0xDD:
  case 0xFD:  // Y y acute
    return Mark::Acute;
  case 0xC2:
  case 0xE2:  // A a circumflex
  case 0xCA:
  case 0xEA:  // E e circumflex
  case 0xCE:
  case 0xEE:  // I i circumflex
  case 0xD4:
  case 0xF4:  // O o circumflex
  case 0xDB:
  case 0xFB:  // U u circumflex
    return Mark::Circumflex;
  case 0xC4:
  case 0xE4:  // A a diaeresis
  case 0xCB:
  case 0xEB:  // E e diaeresis
  case 0xCF:
  case 0xEF:  // I i diaeresis
  case 0xD6:
  case 0xF6:  // O o diaeresis
  case 0xDC:
  case 0xFC:  // U u diaeresis
  case 0x178:
  case 0xFF:  // Y y diaeresis
    return Mark::Diaeresis;
  case 0xC3:
  case 0xE3:  // A a tilde
  case 0xD1:
  case 0xF1:  // N n tilde
  case 0xD5:
  case 0xF5:  // O o tilde
    return Mark::Tilde;
  case 0xC5:
  case 0xE5:  // A a ring
    return Mark::Ring;
  case 0xC7:
  case 0xE7:  // C c cedilla
    return Mark::Cedilla;
  case 0xD8:
  case 0xF8:  // O o stroke
    return Mark::Slash;
  default:
    return Mark::None;
  }
}

bool BlockInfo(Format format, int& blockW, int& blockH, int& bitsPerPixel) {
  switch (format) {
  case Format::I4:
  case Format::C4:
    blockW = 8;
    blockH = 8;
    bitsPerPixel = 4;
    return true;
  case Format::I8:
  case Format::IA4:
  case Format::C8:
    blockW = 8;
    blockH = 4;
    bitsPerPixel = 8;
    return true;
  default:
    return false;
  }
}

bool Decode(const uint8_t* data, size_t size, Format format, int texW, int texH, int x, int y,
            uint8_t& out) {
  int blockW, blockH, bpp;
  if (data == nullptr || !BlockInfo(format, blockW, blockH, bpp) || x < 0 || y < 0 || x >= texW ||
      y >= texH) {
    return false;
  }
  const int blocksPerRow = (texW + blockW - 1) / blockW;
  // In texels (nibbles at 4 bpp): whole blocks, then the position in the block.
  const size_t offset =
      (size_t(y / blockH) * size_t(blocksPerRow) + size_t(x / blockW)) * size_t(blockW * blockH) +
      size_t((y % blockH) * blockW + (x % blockW));
  if (bpp == 4) {
    const size_t byte = offset / 2;
    if (byte >= size) {
      return false;
    }
    out = (offset % 2 == 0) ? uint8_t(data[byte] >> 4) : uint8_t(data[byte] & 0xF);
    return true;
  }
  if (offset >= size) {
    return false;
  }
  out = data[offset];
  return true;
}

bool Encode(uint8_t* data, size_t size, Format format, int texW, int texH, int x, int y,
            uint8_t value) {
  int blockW, blockH, bpp;
  if (data == nullptr || !BlockInfo(format, blockW, blockH, bpp) || x < 0 || y < 0 || x >= texW ||
      y >= texH) {
    return false;
  }
  const int blocksPerRow = (texW + blockW - 1) / blockW;
  // In texels (nibbles at 4 bpp): whole blocks, then the position in the block.
  const size_t offset =
      (size_t(y / blockH) * size_t(blocksPerRow) + size_t(x / blockW)) * size_t(blockW * blockH) +
      size_t((y % blockH) * blockW + (x % blockW));
  if (bpp == 4) {
    const size_t byte = offset / 2;
    if (byte >= size || value > 0xF) {
      return false;
    }
    if (offset % 2 == 0) {
      data[byte] = uint8_t((data[byte] & 0x0F) | (value << 4));
    } else {
      data[byte] = uint8_t((data[byte] & 0xF0) | value);
    }
    return true;
  }
  if (offset >= size) {
    return false;
  }
  data[offset] = value;
  return true;
}

bool Composite(const Grid& base, Mark mark, int cellW, int baseH, uint8_t ink, uint8_t outline,
               bool outlined, Grid& out, int& shiftDown, int& cellH) {
  out = {};
  shiftDown = 0;
  cellH = baseH;
  if (mark == Mark::None || cellW <= 0 || baseH <= 0 || base.w <= 0 || base.h != baseH ||
      size_t(base.w) * size_t(base.h) != base.v.size() || ink == 0) {
    return false;
  }
  // The letter's ink box, in the base cell.
  int top = baseH, bottom = -1, left = base.w, right = -1;
  for (int y = 0; y < baseH; ++y) {
    for (int x = 0; x < base.w; ++x) {
      if (At(base, x, y) == ink) {
        top = std::min(top, y);
        bottom = std::max(bottom, y);
        left = std::min(left, x);
        right = std::max(right, x);
      }
    }
  }
  if (bottom < 0) {
    // No ink in the expected value (an intensity font's outline-only cell?):
    // fall back to any nonzero texel.
    for (int y = 0; y < baseH; ++y) {
      for (int x = 0; x < base.w; ++x) {
        if (At(base, x, y) != 0) {
          top = std::min(top, y);
          bottom = std::max(bottom, y);
          left = std::min(left, x);
          right = std::max(right, x);
        }
      }
    }
    if (bottom < 0) {
      return false;
    }
  }
  // The stroke: the commonest horizontal run of ink that is not a bar across the
  // letter (the width of a stem), so the mark is as heavy as the letter.
  int s = baseH >= 18 ? 2 : 1;
  {
    int runs[8] = {0};
    const int inkW = right - left + 1;
    for (int y = top; y <= bottom; ++y) {
      int run = 0;
      for (int x = left; x <= right + 1; ++x) {
        if (x <= right && At(base, x, y) != 0) {
          ++run;
        } else {
          if (run > 0 && run < 8 && run * 10 < inkW * 6) {
            ++runs[run];
          }
          run = 0;
        }
      }
    }
    int best = 0;
    for (int r = 1; r < 8; ++r) {
      if (runs[r] > best) {
        best = runs[r];
        s = std::min(r, 3);
      }
    }
  }
  const int mh = 2 * s + 1;           // the mark's height
  const int gap = s;
  const int inkCx = (left + right) / 2;

  int markTop = 0, markH = mh, markX0 = 0, markX1 = -1;
  int cedTop = -1, cedH = 0, cedCx = 0;
  if (mark == Mark::Cedilla) {
    cedH = 2 * s + 1;
    cedTop = bottom + 1;
    cedCx = inkCx;
    cellH = std::max(baseH, cedTop + cedH);
  } else if (mark == Mark::Slash) {
    cellH = baseH;  // struck through the letter itself, in place
  } else {
    int mw = std::max(right - left + 1 + 2, 3 * s + 1);
    if (mark == Mark::Ring) {
      mw = std::max(mw, 2 * mh + 1);
    }
    mw = std::min(cellW, mw);
    markX0 = std::clamp(inkCx - mw / 2, 0, std::max(0, cellW - mw));
    markX1 = markX0 + mw - 1;
    markTop = top - gap - mh;
    if (markTop < 0) {
      shiftDown = -markTop;
      markTop = 0;
    }
    markH = mh;
    cellH = std::max(baseH, bottom + 1 + shiftDown);
  }

  out.w = cellW;
  out.h = cellH;
  out.v.assign(size_t(cellW) * size_t(cellH), 0);
  // The letter, resampled across the new width when the advance was widened.
  for (int y = 0; y < baseH && y + shiftDown < cellH; ++y) {
    for (int x = 0; x < cellW; ++x) {
      const int sx = base.w == cellW ? x : x * base.w / cellW;
      out.v[size_t(y + shiftDown) * size_t(cellW) + size_t(x)] =
          base.v[size_t(y) * size_t(base.w) + size_t(sx)];
    }
  }

  const auto dx = markX1 - markX0 + 1;
  const int cxm = (markX0 + markX1) / 2;
  switch (mark) {
  case Mark::None:
    return false;
  case Mark::Acute:
    Stroke(out, markX0, markX1, markTop, markH, true, s, ink);
    break;
  case Mark::Grave:
    Stroke(out, markX0, markX1, markTop, markH, false, s, ink);
    break;
  case Mark::Circumflex: {
    // Two strokes meeting at the top centre.
    for (int r = 0; r < markH; ++r) {
      const double t = markH > 1 ? double(r) / double(markH - 1) : 0.0;
      const int lc = cxm - int(std::round(t * double(cxm - markX0)));
      const int rc = cxm + int(std::round(t * double(markX1 - cxm)));
      for (int i = 0; i < s; ++i) {
        Plot(out, lc - i, markTop + r, ink);
        Plot(out, rc + i, markTop + r, ink);
      }
    }
    break;
  }
  case Mark::Diaeresis: {
    const int dw = s + 1, dh = s + (s > 1 ? 1 : 0);
    const int yd = markTop + (markH - dh) / 2;
    for (int r = 0; r < dh; ++r) {
      for (int i = 0; i < dw; ++i) {
        Plot(out, markX0 + i, yd + r, ink);
        Plot(out, markX1 - i, yd + r, ink);
      }
    }
    break;
  }
  case Mark::Tilde: {
    const int yc = markTop + markH / 2;
    for (int c = 0; c < dx; ++c) {
      const double phase = std::sin(2.0 * 3.141592653589793 * double(c) / double(std::max(dx, 2)));
      const int y = yc - int(std::round(phase * double(markH - s) / 2.0));
      for (int i = 0; i < s; ++i) {
        Plot(out, markX0 + c, y + i, ink);
      }
    }
    break;
  }
  case Mark::Ring: {
    const double rx = double(markX1 - markX0) / 2.0;
    const double ry = double(markH) / 2.0;
    const double tol = s >= 2 ? 0.45 : 0.32;
    for (int r = 0; r < markH; ++r) {
      for (int c = 0; c < dx; ++c) {
        const double ex = (double(c) - rx) / (rx > 0.0 ? rx : 1.0);
        const double ey = (double(r) - (ry - 0.5)) / (ry > 0.0 ? ry : 1.0);
        if (std::fabs(ex * ex + ey * ey - 1.0) <= tol) {
          Plot(out, markX0 + c, markTop + r, ink);
        }
      }
    }
    break;
  }
  case Mark::Cedilla: {
    // A hook under the letter: a stub, a diagonal down to the right, and a
    // curl back to the left at the bottom.
    const int cw = 2 * s + 3;
    const int x0 = std::clamp(cedCx - cw / 2, 0, std::max(0, cellW - cw));
    for (int i = 0; i < s + 1; ++i) {
      Plot(out, x0 + i, cedTop, ink);
    }
    for (int r = 1; r < cedH - 1; ++r) {
      const int c = x0 + s + (r - 1) * (cw - s - 1) / std::max(cedH - 2, 1);
      for (int i = 0; i < s; ++i) {
        Plot(out, c - i, cedTop + r, ink);
      }
    }
    const int cBot = x0 + cw - 1;
    for (int i = 0; i < s + 1; ++i) {
      Plot(out, cBot - i, cedTop + cedH - 1, ink);
    }
    break;
  }
  case Mark::Slash: {
    // A stroke through the middle, a little past the ink on both ends.
    const int y0 = std::max(0, top - 1);
    const int y1 = std::min(cellH - 1, bottom + 1);
    const int run = (right - left + 1) + 2 * s;
    for (int y = y0; y <= y1; ++y) {
      const double t = y1 > y0 ? double(y - y0) / double(y1 - y0) : 0.0;
      const int cEnd = left - s + int(std::round(t * double(run - s)));
      for (int i = 0; i < s; ++i) {
        Plot(out, cEnd + i, y, ink);
      }
    }
    break;
  }
  }
  if (outlined && outline != 0 && outline != ink) {
    // A one pixel outline around the new ink, as the outlined fonts have.
    std::vector<std::pair<int, int>> added;
    for (int y = 0; y < out.h; ++y) {
      for (int x = 0; x < out.w; ++x) {
        if (At(out, x, y) != ink) {
          continue;
        }
        // Only the mark's pixels, not the letter's: above its ink, or the
        // below/through marks' rows.
        const bool isMark = mark == Mark::Cedilla ? y > bottom + shiftDown
                            : mark == Mark::Slash ? true
                                                  : y < top + shiftDown;
        if (!isMark) {
          continue;
        }
        for (int oy = -1; oy <= 1; ++oy) {
          for (int ox = -1; ox <= 1; ++ox) {
            if ((ox != 0 || oy != 0) && At(out, x + ox, y + oy) == 0) {
              added.emplace_back(x + ox, y + oy);
            }
          }
        }
      }
    }
    for (const auto& [x, y] : added) {
      Plot(out, x, y, outline);
    }
  }
  return true;
}

bool Infinity(const Grid& eight, uint8_t ink, uint8_t outline, bool outlined, Grid& out,
              int& extraW) {
  // The 8's ink box: anything drawn but the outline.
  const auto isInk = [&](int x, int y) {
    const uint8_t v = At(eight, x, y);
    return v != 0 && (!outlined || v != outline);
  };
  int left = eight.w, right = -1, top = eight.h, bottom = -1;
  for (int y = 0; y < eight.h; ++y) {
    for (int x = 0; x < eight.w; ++x) {
      if (isInk(x, y)) {
        left = std::min(left, x);
        right = std::max(right, x);
        top = std::min(top, y);
        bottom = std::max(bottom, y);
      }
    }
  }
  if (right < 0) {
    return false;
  }
  const int inkW = right - left + 1;
  const int inkH = bottom - top + 1;
  if (inkW < 3 || inkH < 4) {
    return false;
  }
  // The stroke: the 8's side run a quarter down, and its top run in the middle.
  const int sideY = top + inkH / 4;
  int sx = left;
  while (sx < eight.w && !isInk(sx, sideY)) {
    ++sx;
  }
  int side = 0;
  while (sx + side < eight.w && isInk(sx + side, sideY)) {
    ++side;
  }
  const int midX = left + inkW / 2;
  int topRun = 0;
  while (top + topRun < eight.h && isInk(midX, top + topRun)) {
    ++topRun;
  }
  int stroke = std::max(1, std::min(side > 0 ? side : topRun, topRun > 0 ? topRun : side));
  // The outline ring: how far the outline reaches out left of the ink.
  int ring = 0;
  if (outlined && outline != ink) {
    while (sx - ring - 1 >= 0 && At(eight, sx - ring - 1, sideY) == outline) {
      ++ring;
    }
    stroke = std::min(stroke, 2);  // the ring eats into the counters
  }
  // An 8 laid on its side is as wide as two of its loops; outlined fonts need
  // more room for the ring inside them.
  const int w = int(std::lround(double(inkW) * (ring > 0 ? 2.1 : 1.6)));
  extraW = w - inkW;
  // A Gerono lemniscate (x = cos t, y = sin t cos t) across the ink box,
  // drawn as the texels near enough to it; thinned until both loops stay open.
  constexpr int kSamples = 720;
  constexpr double kPi = 3.14159265358979323846;
  for (; stroke >= 1; --stroke) {
    const double s = double(stroke);
    const double ax = double(w) - s, ay = double(inkH) - s;
    std::vector<double> px(kSamples), py(kSamples);
    for (int i = 0; i < kSamples; ++i) {
      const double t = 2.0 * kPi * double(i) / double(kSamples);
      px[size_t(i)] = double(left) + s / 2.0 + (std::cos(t) + 1.0) / 2.0 * ax;
      py[size_t(i)] = double(top) + s / 2.0 + (std::sin(t) * std::cos(t) + 0.5) * ay;
    }
    out.w = eight.w + extraW;
    out.h = eight.h;
    out.v.assign(size_t(out.w) * size_t(out.h), 0);
    const double reach = (s / 2.0) * (s / 2.0) + 0.05;
    for (int y = top; y <= bottom; ++y) {
      for (int x = left; x < left + w; ++x) {
        const double cx = double(x) + 0.5, cy = double(y) + 0.5;
        for (int i = 0; i < kSamples; ++i) {
          const double dx = px[size_t(i)] - cx, dy = py[size_t(i)] - cy;
          if (dx * dx + dy * dy <= reach) {
            Plot(out, x, y, ink);
            break;
          }
        }
      }
    }
    // Each loop's middle (cos t = +-0.55 on the centre line) must stay empty.
    const int midY = int(double(top) + s / 2.0 + 0.5 * ay);
    const int loopL = int(double(left) + s / 2.0 + 0.225 * ax);
    const int loopR = int(double(left) + s / 2.0 + 0.775 * ax);
    if (At(out, loopL, midY) == 0 && At(out, loopR, midY) == 0) {
      break;
    }
  }
  if (stroke < 1) {
    return false;
  }
  if (ring > 0) {
    const int reach = ring * ring + ring;
    Grid inked = out;
    for (int y = 0; y < out.h; ++y) {
      for (int x = 0; x < out.w; ++x) {
        if (At(inked, x, y) != 0) {
          continue;
        }
        bool near = false;
        for (int dy = -ring; dy <= ring && !near; ++dy) {
          for (int dx = -ring; dx <= ring && !near; ++dx) {
            near = dx * dx + dy * dy <= reach && At(inked, x + dx, y + dy) == ink;
          }
        }
        if (near) {
          Plot(out, x, y, outline);
        }
      }
    }
  }
  return true;
}

}  // namespace PortFontAccent
