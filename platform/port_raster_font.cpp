// CRasterFont's port-only members (the stand-in and accent glyphs, the layer
// unpacking), moved out of src/Kyoto/Text/CRasterFont.cpp so the decomp file
// keeps only its hooks (see docs/NATIVE_PORT.md, "Where port code goes"). They
// are declared under TARGET_PC in include/Kyoto/Text/CRasterFont.hpp.
//
// Built as part of mp_game, with the same flags and includes as the file it
// came from; the code was moved verbatim.

#include "Kyoto/Text/CRasterFont.hpp"
#include "Kyoto/Basics/CBasics.hpp"

#include "Kyoto/CFactoryFnReturn.hpp"
#include "Kyoto/CSimplePool.hpp"
#include "Kyoto/CVParamTransfer.hpp"
#include "Kyoto/Graphics/CColor.hpp"
#include "Kyoto/Graphics/CGX.hpp"
#include "Kyoto/Graphics/CGraphicsPalette.hpp"
#include "Kyoto/Streams/CInputStream.hpp"
#include "Kyoto/Text/CDrawStringOptions.hpp"
#include "Kyoto/Text/CTextRenderBuffer.hpp"
#include "Kyoto/Text/TextCommon.hpp"

#include "rstl/algorithm.hpp"
#include "rstl/string.hpp"
#include "rstl/vector.hpp"

#ifdef TARGET_PC
#include "Kyoto/Graphics/CTexture.hpp"
#include "Kyoto/Streams/CMemoryInStream.hpp"
#include "port_debug.h"
#include "port_disc.h"
#include "port_font_accent.h"
#include "port_hd_font.h"
#include "port_log.h"
#include "port_pal_languages.h"

#include <algorithm>
#include <string.h>
#include <vector>
#endif

#ifdef TARGET_PC
// The characters the port's languages need that the font lacks. Accented
// letters get a new cell in the font texture (below): the stand-in's cell with
// the diacritic drawn over it (port_font_accent.h). Anything else keeps a copy
// of its ASCII stand-in's cell (PortHdFont::StandIns), widened as the distance
// field's character is wider. Only the typeface the distance field holds.
// Every font with an 8 also gets an infinity sign drawn after it, for the
// infinite ammo cheat's HUD counters (PortFontAccent::Infinity).
void CRasterFont::PortAddStandIns() {
  std::vector< PortAccentPending > marked;
  std::vector< PortAccentPending > plain;
  const CGlyph* eight = GetGlyph(L'8');
  if (eight != nullptr && !HasGlyph(kPortInfinity)) {
    marked.push_back({kPortInfinity, eight, 1.f});
  }
  const std::vector< PortHdFont::StandIn >& standIns = PortHdFont::StandIns();
  const size_t standInCount = PortHdFont::SameTypeface(*this) ? standIns.size() : 0;
  for (size_t i = 0; i < standInCount; ++i) {
    const wchar_t chr = static_cast< wchar_t >(standIns[i].character);
    const CGlyph* base = GetGlyph(static_cast< wchar_t >(standIns[i].base));
    if (base == nullptr || HasGlyph(chr)) {
      continue;
    }
    const float widen = PortHdFont::ModAdvanceRatio(standIns[i].character, standIns[i].base);
    if (PortFontAccent::MarkFor(standIns[i].character) != PortFontAccent::Mark::None) {
      marked.push_back({chr, base, widen});
    } else {
      plain.push_back({chr, base, widen});
    }
  }
  std::vector< rstl::pair< wchar_t, CGlyph > > added;
  const auto addPlain = [&added](const PortAccentPending& p) {
    const int b = static_cast< int >(p.base->GetB() * p.widen + 0.5f);
    const int cellWidth = static_cast< int >(p.base->GetCellWidth() * p.widen + 0.5f);
    added.push_back(rstl::pair< wchar_t, CGlyph >(
        p.chr, CGlyph(p.base->GetA(), b, p.base->GetC(), p.base->GetStartU(), p.base->GetStartV(),
                      p.base->GetEndU(), p.base->GetEndV(), cellWidth, p.base->GetCellHeight(),
                      p.base->GetBaseLine(), p.base->GetKernStart())));
  };
  for (size_t i = 0; i < plain.size(); ++i) {
    addPlain(plain[i]);
  }
  int accented = 0;
  if (!marked.empty()) {
    const size_t before = added.size();
    if (PortAddAccents(marked, added)) {
      accented = static_cast< int >(added.size() - before);
    } else {
      for (size_t i = 0; i < marked.size(); ++i) {
        if (marked[i].chr != kPortInfinity) {  // an 8 is no stand-in for it
          addPlain(marked[i]);
        }
      }
    }
  }
  if (added.empty()) {
    return;
  }
  xc_glyphs.reserve(xc_glyphs.size() + static_cast< int >(added.size()));
  for (size_t i = 0; i < added.size(); ++i) {
    xc_glyphs.push_back(added[i]);
  }
  rstl::sort_by_key(xc_glyphs);
  if (accented > 0) {
    PortLog::Write("[font] %s: %d glyphs drawn (accents, infinity)\n", PortGetName(), accented);
  }
}

// Draws each pending letter's diacritic into a new texture cell. True unless
// nothing could be composited (the caller keeps plain copies then).
bool CRasterFont::PortAddAccents(const std::vector< PortAccentPending >& pending,
                                 std::vector< rstl::pair< wchar_t, CGlyph > >& added) {
  if (!x80_texture.valid()) {
    return false;
  }
  CTexture* tex = **x80_texture;
  PortFontAccent::Format format;
  switch (tex->GetTexelFormat()) {
  case kTF_I4:
    format = PortFontAccent::Format::I4;
    break;
  case kTF_I8:
    format = PortFontAccent::Format::I8;
    break;
  case kTF_C4:
    format = PortFontAccent::Format::C4;
    break;
  case kTF_C8:
    format = PortFontAccent::Format::C8;
    break;
  default:
    return false;
  }
  const bool indexed = format == PortFontAccent::Format::C4 || format == PortFontAccent::Format::C8;
  const bool outlined = indexed && x2c_mode == kFM_OneLayerOutline;
  const int texW = tex->GetWidth();
  const int oldH = tex->GetHeight();
  const uint8_t* texels = static_cast< const uint8_t* >(tex->GetConstBitMapData(0));
  const size_t texSize = tex->GetMemoryAllocated();
  struct Cell {
    wchar_t chr;
    CGlyph base;  // a copy: xc_glyphs is rebuilt below
    float widen;
    int cellW;
    PortFontAccent::Grid grid;
    int shift;
    int extraW;  // the infinity's widening of the 8's advance
  };
  std::vector< Cell > cells;
  for (size_t i = 0; i < pending.size(); ++i) {
    const CGlyph* base = pending[i].base;
    const int x0 = int(base->GetStartU() * texW + 0.5f);
    const int x1 = int(base->GetEndU() * texW + 0.5f);
    const int y0 = int(base->GetStartV() * oldH + 0.5f);
    const int y1 = int(base->GetEndV() * oldH + 0.5f);
    if (x0 < 0 || y0 < 0 || x1 > texW || y1 > oldH || x1 <= x0 || y1 <= y0) {
      continue;
    }
    PortFontAccent::Grid decoded;
    decoded.w = x1 - x0;
    decoded.h = y1 - y0;
    decoded.v.assign(size_t(decoded.w) * size_t(decoded.h), 0);
    bool ok = true;
    for (int y = 0; y < decoded.h && ok; ++y) {
      for (int x = 0; x < decoded.w && ok; ++x) {
        ok = PortFontAccent::Decode(texels, texSize, format, texW, oldH, x0 + x, y0 + y,
                                    decoded.v[size_t(y) * size_t(decoded.w) + size_t(x)]);
      }
    }
    if (!ok) {
      continue;
    }
    // The ink is the most common nonzero value; the outline (if any) the next.
    int count[256] = {0};
    for (size_t k = 0; k < decoded.v.size(); ++k) {
      ++count[decoded.v[k]];
    }
    int ink = 0, outline = 0, inkCount = 0, outlineCount = 0;
    for (int v = 1; v < 256; ++v) {
      if (count[v] > inkCount) {
        outline = ink;
        outlineCount = inkCount;
        ink = v;
        inkCount = count[v];
      } else if (count[v] > outlineCount) {
        outline = v;
        outlineCount = count[v];
      }
    }
    if (ink == 0) {
      continue;
    }
    const int maxValue = format == PortFontAccent::Format::C4 ? 15 : 255;
    if (ink > maxValue) {
      continue;
    }
    if (!outlined || outlineCount < 6 || outline > maxValue) {
      outline = ink;  // no outline pass
    }
    PortFontAccent::Grid grid;
    if (pending[i].chr == kPortInfinity) {
      int extraW = 0;
      if (PortFontAccent::Infinity(decoded, uint8_t(ink), uint8_t(outline), outlined, grid,
                                   extraW)) {
        cells.push_back({pending[i].chr, *base, 1.f, grid.w, grid, 0, extraW});
      }
      continue;
    }
    const int cellW =
        static_cast< int >(base->GetCellWidth() * pending[i].widen + 0.5f);
    int shift = 0, cellH = 0;
    if (cellW <= 0 ||
        !PortFontAccent::Composite(decoded, PortFontAccent::MarkFor(pending[i].chr), cellW,
                                   decoded.h, uint8_t(ink), uint8_t(outline), outlined, grid, shift,
                                   cellH)) {
      continue;
    }
    cells.push_back({pending[i].chr, *base, pending[i].widen, cellW, grid, shift, 0});
  }
  if (cells.empty()) {
    return false;
  }
  // Shelf-pack the new cells into rows across the texture's width.
  struct Row {
    size_t begin, end;
    int h;
  };
  std::vector< Row > rows;
  for (size_t i = 0; i < cells.size();) {
    size_t j = i;
    int rowH = 0, rowW = 0;
    while (j < cells.size() && rowW + cells[j].grid.w <= texW) {
      rowW += cells[j].grid.w;
      rowH = rowW == cells[j].grid.w ? cells[j].grid.h : (rowH > cells[j].grid.h ? rowH : cells[j].grid.h);
      ++j;
    }
    if (j == i) {  // wider than the texture: shrink the cell onto it
      j = i + 1;
      rowH = cells[i].grid.h;
    }
    rows.push_back({i, j, rowH});
    i = j;
  }
  int stripH = 0;
  for (size_t r = 0; r < rows.size(); ++r) {
    stripH += rows[r].h;
  }
  {
    // Keep the height a power of two, as retail's are (NPOT textures clamp).
    int total = 1;
    while (total < oldH + stripH) {
      total <<= 1;
    }
    stripH = total - oldH;
  }
  if (!tex->PortGrowHeight(stripH)) {
    return false;
  }
  const int newH = tex->GetHeight();
  const float vScale = float(oldH) / float(newH);
  // The taller texture stretches normalized V: rescale the old glyphs so their
  // pixel rows stay put.
  rstl::vector< rstl::pair< wchar_t, CGlyph > > scaled;
  scaled.reserve(xc_glyphs.size());
  for (int i = 0; i < xc_glyphs.size(); ++i) {
    const CGlyph& g = xc_glyphs[i].second;
    scaled.push_back(rstl::pair< wchar_t, CGlyph >(
        xc_glyphs[i].first,
        CGlyph(g.GetA(), g.GetB(), g.GetC(), g.GetStartU(), g.GetStartV() * vScale, g.GetEndU(),
               g.GetEndV() * vScale, g.GetCellWidth(), g.GetCellHeight(), g.GetBaseLine(),
               g.GetKernStart())));
  }
  // Copies made before the growth (the plain stand-ins) name the old rows too.
  for (size_t i = 0; i < added.size(); ++i) {
    const CGlyph& g = added[i].second;
    added[i].second = CGlyph(g.GetA(), g.GetB(), g.GetC(), g.GetStartU(), g.GetStartV() * vScale,
                             g.GetEndU(), g.GetEndV() * vScale, g.GetCellWidth(),
                             g.GetCellHeight(), g.GetBaseLine(), g.GetKernStart());
  }
  xc_glyphs = scaled;
  uint8_t* dest = static_cast< uint8_t* >(tex->GetBitMapData(0));
  const size_t destSize = tex->GetMemoryAllocated();
  int atY = oldH;
  for (size_t r = 0; r < rows.size(); ++r) {
    int atX = 0;
    for (size_t i = rows[r].begin; i < rows[r].end; ++i) {
      Cell& cell = cells[i];
      int w = cell.grid.w <= texW - atX ? cell.grid.w : texW - atX;
      for (int y = 0; y < cell.grid.h && atY + y < newH; ++y) {
        for (int x = 0; x < w; ++x) {
          PortFontAccent::Encode(dest, destSize, format, texW, newH, atX + x, atY + y,
                                 cell.grid.v[size_t(y) * size_t(cell.grid.w) + size_t(x)]);
        }
      }
      const float su = float(atX) / float(texW);
      const float eu = float(atX + w) / float(texW);
      const float sv = float(atY) / float(newH);
      const float ev = float(atY + cell.grid.h) / float(newH);
      const int bw = static_cast< int >(cell.base.GetB() * cell.widen + 0.5f) + cell.extraW;
      added.push_back(rstl::pair< wchar_t, CGlyph >(
          cell.chr, CGlyph(cell.base.GetA(), bw, cell.base.GetC(), su, sv, eu, ev, w,
                           cell.grid.h, cell.base.GetBaseLine() + cell.shift,
                           cell.base.GetKernStart())));
      atX += cell.grid.w;
    }
    atY += rows[r].h;
  }
  tex->UnLock();
  return true;
}

// PAL fonts (FONT v4) pack two or four layers of glyphs into the bits of each
// texel; their code picks a layer per glyph. The 1.00 renderer draws one
// layer, bit 0 the ink and bit 1 the outline. So each layer gets a band of
// rows of its own, its glyphs move there, and the font takes the one-layer
// mode. A texture shared by fonts is unpacked once.
void CRasterFont::PortUnpackLayers() {
  int layerCount = 0;
  EFontMode mode = kFM_OneLayer;
  switch (mPortLayerMode) {
  case 0:
  case 1:
    break;
  case kFM_FourLayers:
    layerCount = 4;
    break;
  case kFM_TwoLayersOutline:
  case 5:  // two layers, outline bits first
    layerCount = 2;
    mode = kFM_OneLayerOutline;
    break;
  case kFM_TwoLayers:
    layerCount = 2;
    break;
  default:
    PortLog::Write("[font] %s: unknown mode %d\n", PortGetName(), mPortLayerMode);
    break;
  }
  if (layerCount == 0) {
    mPortLayerMode = -1;
    mPortLayers.clear();
    PortAddStandIns();
    return;
  }
  CTexture* tex = **x80_texture;
  int bandH = tex->PortLayerBandHeight();
  if (bandH == 0) {
    PortFontAccent::Format format;
    switch (tex->GetTexelFormat()) {
    case kTF_C4:
      format = PortFontAccent::Format::C4;
      break;
    case kTF_C8:
      format = PortFontAccent::Format::C8;
      break;
    default:
      PortLog::Write("[font] %s: layered font with texel format %d\n", PortGetName(),
                     int(tex->GetTexelFormat()));
      return;
    }
    const int texW = tex->GetWidth();
    bandH = tex->GetHeight();
    std::vector< uint8_t > packed(size_t(texW) * size_t(bandH));
    {
      const uint8_t* texels = static_cast< const uint8_t* >(tex->GetConstBitMapData(0));
      const size_t size = tex->GetMemoryAllocated();
      for (int y = 0; y < bandH; ++y) {
        for (int x = 0; x < texW; ++x) {
          PortFontAccent::Decode(texels, size, format, texW, bandH, x, y,
                                 packed[size_t(y) * size_t(texW) + size_t(x)]);
        }
      }
    }
    // Keep the height a power of two, as retail's are (NPOT textures clamp).
    int newH = 1;
    while (newH < bandH * layerCount) {
      newH <<= 1;
    }
    if (!tex->PortGrowHeight(newH - bandH)) {
      PortLog::Write("[font] %s: could not unpack its layers\n", PortGetName());
      return;
    }
    uint8_t* dest = static_cast< uint8_t* >(tex->GetBitMapData(0));
    const size_t destSize = tex->GetMemoryAllocated();
    for (int layer = 0; layer < layerCount; ++layer) {
      for (int y = 0; y < bandH; ++y) {
        for (int x = 0; x < texW; ++x) {
          const int n = packed[size_t(y) * size_t(texW) + size_t(x)];
          int value;
          if (mPortLayerMode == 5) {
            value = ((n >> (2 + layer)) & 1) | (((n >> layer) & 1) << 1);
          } else if (mode == kFM_OneLayerOutline) {
            value = (n >> (2 * layer)) & 3;
          } else {
            value = (n >> layer) & 1;
          }
          PortFontAccent::Encode(dest, destSize, format, texW, newH, x, layer * bandH + y,
                                 uint8_t(value));
        }
      }
    }
    tex->UnLock();
    tex->PortSetLayerBandHeight(bandH);
  }
  const float bandV = float(bandH) / float(tex->GetHeight());
  std::sort(mPortLayers.begin(), mPortLayers.end(),
            [](const std::pair< wchar_t, int >& l, const std::pair< wchar_t, int >& r) {
              return l.first < r.first;
            });
  for (int i = 0; i < xc_glyphs.size(); ++i) {
    const wchar_t chr = xc_glyphs[i].first;
    const auto it = std::lower_bound(
        mPortLayers.begin(), mPortLayers.end(), chr,
        [](const std::pair< wchar_t, int >& l, wchar_t c) { return l.first < c; });
    const int layer = it != mPortLayers.end() && it->first == chr ? it->second : 0;
    const CGlyph& g = xc_glyphs[i].second;
    xc_glyphs[i].second =
        CGlyph(g.GetA(), g.GetB(), g.GetC(), g.GetStartU(), (layer + g.GetStartV()) * bandV,
               g.GetEndU(), (layer + g.GetEndV()) * bandV, g.GetCellWidth(), g.GetCellHeight(),
               g.GetBaseLine(), g.GetKernStart());
  }
  x2c_mode = mode;
  mPortLayerMode = -1;
  mPortLayers.clear();
  PortAddStandIns();
}
#endif
