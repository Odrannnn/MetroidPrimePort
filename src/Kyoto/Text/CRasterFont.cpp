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
#include "port_font_accent.h"
#include "port_hd_font.h"
#include "port_log.h"

#include <algorithm>
#include <vector>
#endif

CRasterFont::CRasterFont(CInputStream& in, IObjectStore* store)
: x0_initialized(false)
, x4_monoWidth(16)
, x8_monoHeight(16)
, x2c_mode(kFM_OneLayer)
, x90_lineMargin(0)
#ifdef TARGET_PC
, mPortLayerMode(-1)
#endif
{
  if (in.ReadInt32() == 'FONT') {
    int version = in.ReadInt32();
#ifdef TARGET_PC
    // PAL discs have version 4 (bytes per glyph field, a layer each).
    const int maxVersion = 4;
#else
    const int maxVersion = 2;
#endif
    if (version >= 0 && version <= maxVersion) {
      x4_monoWidth = in.ReadInt32();
      x8_monoHeight = in.ReadInt32();
      if (version >= 1) {
        x8c_baseline = in.ReadInt32();
      } else {
        x8c_baseline = x8_monoHeight;
      }

      if (version >= 2) {
        x90_lineMargin = in.ReadInt32();
      }

      bool fInfoA = in.ReadBool();
      bool fInfoB = in.ReadBool();
      int fInfoC = in.ReadInt32();
      int fontSize = in.ReadInt32();
      rstl::string fontName(in);
      CAssetId fontId = in.ReadInt32();

      if (store != nullptr) {
        x80_texture = store->GetObj(SObjectTag('TXTR', fontId));
        x80_texture->Lock();
      }

      x30_fontInfo = CFontInfo(fInfoA, fInfoB, fInfoC, fontSize, fontName.data());

      int mode = in.ReadInt32();
      switch (mode) {
      case 0:
        x2c_mode = kFM_OneLayer;
        break;
      case 1:
        x2c_mode = kFM_OneLayerOutline;
        break;
      }
#ifdef TARGET_PC
      if (version >= 4) {
        mPortLayerMode = mode;
      }
#endif

      int glyphCount = in.ReadInt32();
      xc_glyphs.reserve(glyphCount);

      for (int i = 0; i < glyphCount; ++i) {
        wchar_t chr = in.Get< ushort >();
        float startU = in.ReadFloat();
        float startV = in.ReadFloat();
        float endU = in.ReadFloat();
        float endV = in.ReadFloat();
#ifdef TARGET_PC
        if (version >= 4) {
          // char is unsigned on ARM: the spacings and baseline are signed bytes.
          const int layer = static_cast< unsigned char >(in.ReadChar());
          const int a = static_cast< signed char >(in.ReadChar());
          const int b = static_cast< signed char >(in.ReadChar());
          const int c = static_cast< signed char >(in.ReadChar());
          const int cellWidth = static_cast< unsigned char >(in.ReadChar());
          const int cellHeight = static_cast< unsigned char >(in.ReadChar());
          const int baseline = static_cast< signed char >(in.ReadChar());
          const int kernStart = static_cast< short >(in.ReadShort());
          xc_glyphs.push_back(rstl::pair< wchar_t, CGlyph >(
              chr, CGlyph(a, b, c, startU, startV, endU, endV, cellWidth, cellHeight, baseline,
                          kernStart)));
          mPortLayers.push_back(std::pair< wchar_t, int >(chr, layer));
          continue;
        }
#endif
        int a = in.ReadInt32();
        int b = in.ReadInt32();
        int c = in.ReadInt32();
        int cellWidth = in.ReadInt32();
        int cellHeight = in.ReadInt32();
        int baseline = in.ReadInt32();
        int kernStart = in.ReadInt32();
        xc_glyphs.push_back(
            rstl::pair< wchar_t, CGlyph >(chr, CGlyph(a, b, c, startU, startV, endU, endV,
                                                      cellWidth, cellHeight, baseline, kernStart)));
      }
      rstl::sort_by_key(xc_glyphs);
#ifdef TARGET_PC
      if (mPortLayerMode < 0) {
        PortAddStandIns();
      } else if (x80_texture.valid()) {
        PortUnpackLayers();  // adds the stand-ins once the cells are in place
      }  // else SetTexture does (the default font)
#endif

      int kerningCount = in.ReadInt32();
      x1c_kerning.reserve(kerningCount);

      for (int i = 0; i < kerningCount; ++i) {
        short first = in.Get< short >();
        short second = in.Get< short >();
        int howMuch = in.ReadInt32();
        x1c_kerning.push_back(CKernPair(first, second, howMuch));
      }

      x0_initialized = true;
    }
  }
}

#ifdef TARGET_PC
// The characters the port's languages need that the font lacks. Accented
// letters get a new cell in the font texture (below): the stand-in's cell with
// the diacritic drawn over it (port_font_accent.h). Anything else keeps a copy
// of its ASCII stand-in's cell (PortHdFont::StandIns), widened as the distance
// field's character is wider. Only the typeface the distance field holds.
void CRasterFont::PortAddStandIns() {
  if (!PortHdFont::SameTypeface(*this)) {
    return;
  }
  std::vector< PortAccentPending > marked;
  std::vector< PortAccentPending > plain;
  const std::vector< PortHdFont::StandIn >& standIns = PortHdFont::StandIns();
  for (size_t i = 0; i < standIns.size(); ++i) {
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
        addPlain(marked[i]);
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
    PortLog::Write("[font] %s: %d accented glyphs with diacritics\n", PortGetName(), accented);
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
    const int cellW =
        static_cast< int >(base->GetCellWidth() * pending[i].widen + 0.5f);
    PortFontAccent::Grid grid;
    int shift = 0, cellH = 0;
    if (cellW <= 0 ||
        !PortFontAccent::Composite(decoded, PortFontAccent::MarkFor(pending[i].chr), cellW,
                                   decoded.h, uint8_t(ink), uint8_t(outline), outlined, grid, shift,
                                   cellH)) {
      continue;
    }
    cells.push_back({pending[i].chr, *base, pending[i].widen, cellW, grid, shift});
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
      const int bw = static_cast< int >(cell.base.GetB() * cell.widen + 0.5f);
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

EFontMode CRasterFont::GetMode() const { return x2c_mode; }

void CRasterFont::GetSize(const CDrawStringOptions& options, int& width, int& height,
                          const wchar_t* str, int length) const {
  width = 0;
  height = 0;
  int curWidth = 0;
  const CGlyph* prevGlyph = nullptr;
#if NONMATCHING || defined(TARGET_PC)
  for (const wchar_t* ptr = str; (length == -1 || ptr - str < length) && *ptr != 0; ++ptr) {
#else
  for (const wchar_t* ptr = str; *ptr != 0 && (length == -1 || ptr - str < length); ++ptr) {
#endif
    const CGlyph* glyph = GetGlyph(*ptr);
    if (glyph != nullptr) {
      int kerning =
          prevGlyph != nullptr ? KernLookup(x1c_kerning, prevGlyph->GetKernStart(), *ptr) : 0;
      int newWidth = curWidth + glyph->GetA() + glyph->GetB() + glyph->GetC() + kerning;
      int newHeight = x8_monoHeight - glyph->GetBaseLine() + glyph->GetCellHeight();
      if (options.GetTextDirection() == kTD_Horizontal) {
        width = newWidth;
        curWidth = newWidth;
        if (newHeight > height) {
          height = newHeight;
        }
      }
    }
    prevGlyph = glyph;
  }
}

int CRasterFont::GetMonoWidth() const { return x4_monoWidth; }
int CRasterFont::GetMonoHeight() const { return x8_monoHeight; }
int CRasterFont::GetCarriageAdvance() { return GetMonoHeight() + GetLineMargin(); }

const CGlyph* CRasterFont::GetGlyph(wchar_t c) const { return InternalGetGlyph(c); }

void CRasterFont::DrawString(const CDrawStringOptions& options, int x, int y, int& xOut, int& yOut,
                             CTextRenderBuffer* buffer, const wchar_t* str, int length) const {
  if (!x0_initialized) {
    return;
  }

  if (buffer != nullptr) {
    CGraphicsPalette pal(kPF_RGB5A3, 4);
    ushort* data = reinterpret_cast< ushort* >(pal.Lock());
    data[0] = CBasics::SwapBytes(CColor(0.f, 0.f, 0.f, 0.f).ToRGB5A3());
    data[1] = CBasics::SwapBytes(CColor(options.GetPaletteEntry(0)).ToRGB5A3());
    data[2] = CBasics::SwapBytes(CColor(options.GetPaletteEntry(1)).ToRGB5A3());
    data[3] = CBasics::SwapBytes(CColor(0.f, 0.f, 0.f, 0.f).ToRGB5A3());
    pal.UnLock();
    buffer->AddPaletteChange(pal);
  }

  SinglePassDrawString(options, x, y, xOut, yOut, buffer, str, length);
}

void CRasterFont::DrawSpace(const CDrawStringOptions& options, int x, int y, int& xOut, int& yOut,
                            int length) const {
  if (options.GetTextDirection() != kTD_Horizontal) {
    return;
  }

  xOut = x + length;
  yOut = y;
}

int CRasterFont::KernLookup(const rstl::vector< CKernPair >& kern, const int start, const int chr) {

  rstl::vector< CKernPair >::const_iterator it = kern.begin() + start;
  for (; it != kern.end() && it->GetFirst() == kern[start].GetFirst(); ++it) {
    if (it->GetSecond() == chr) {
      return it->GetHowMuch();
    }
  }

  return 0;
}

void CRasterFont::SinglePassDrawString(const CDrawStringOptions& options, const int x, const int y,
                                       int& xOut, int& yOut, CTextRenderBuffer* buffer,
                                       const wchar_t* str, const int length) const {
  if (x0_initialized) {
    int curX = x;
    const CGlyph* prevGlyph = nullptr;
#if NONMATCHING || defined(TARGET_PC)
    for (const wchar_t* ptr = str; (length == -1 || ptr - str < length) && *ptr != 0; ++ptr) {
#else
    for (const wchar_t* ptr = str; *ptr != 0 && (length == -1 || (ptr - str) < length); ++ptr) {
#endif
      const CGlyph* curGlyph = GetGlyph(*ptr);
      if (curGlyph != nullptr) {
        int xOffset = 0;
        int yOffset = 0;
        if (options.GetTextDirection() == kTD_Horizontal) {
          curX += curGlyph->GetA();
          if (prevGlyph != nullptr) {
            curX += KernLookup(x1c_kerning, prevGlyph->GetKernStart(), *ptr);
          }
          xOffset = 0;
          yOffset = 0;
        }

        if (buffer) {
          buffer->AddCharacter(CVector2i(curX + xOffset, yOffset + (y - curGlyph->GetBaseLine())),
                               *ptr, options.GetPaletteEntry(2));
        }

        if (options.GetTextDirection() == kTD_Horizontal) {
          curX += curGlyph->GetB() + curGlyph->GetC();
        }
      }
      prevGlyph = curGlyph;
    }

    xOut = curX;
    yOut = y;
  }
}

const CGlyph* CRasterFont::InternalGetGlyph(const wchar_t chr) const {
  rstl::vector< rstl::pair< wchar_t, CGlyph > >::const_iterator it =
      rstl::find_by_key(xc_glyphs, chr);

  if (it == xc_glyphs.end()) {
    return nullptr;
  }

  return &it->second;
}

const CFactoryFnReturn FRasterFontFactory(const SObjectTag& tag, CInputStream& in,
                                    const CVParamTransfer& xfer) {
  const rstl::rc_ptr< IVParamObj > obj = xfer.x0_obj;
  CSimplePool* pool = static_cast< TObjOwnerParam< CSimplePool* >* >(obj.GetPtr())->GetData();

  return rs_new CRasterFont(in, pool);
}

void CRasterFont::SetupRenderState() {
  static const GXVtxDescList skDescList[] = {
      {GX_VA_POS, GX_DIRECT},
      {GX_VA_TEX0, GX_DIRECT},
      {GX_VA_NULL, GX_NONE},
  };

  TLockedToken< CTexture > texture = *x80_texture;
  texture->Load(GX_TEXMAP0, CTexture::kCM_Clamp);
  CGX::SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_K0_A);
  CGX::SetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
  CGX::SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_ZERO);
  CGX::SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_KONST, GX_CA_ZERO);
  CGX::SetStandardTevColorAlphaOp(GX_TEVSTAGE0);
  CGX::SetTevDirect(GX_TEVSTAGE0);
  CGX::SetVtxDescv(skDescList);
  CGX::SetNumChans(0);
  CGX::SetNumTexGens(1);
  CGX::SetNumTevStages(1);
  CGX::SetNumIndStages(0);
  CGX::SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
  CGX::SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
}

int CRasterFont::GetBaseLine() const { return x8c_baseline; }
int CRasterFont::GetLineMargin() { return x90_lineMargin; }

bool CRasterFont::IsFinishedLoading() { return x80_texture && x80_texture->IsLoaded(); }

void CRasterFont::SetTexture(TToken< CTexture > texture) {
  x80_texture = texture;
  x80_texture->Lock();
#ifdef TARGET_PC
  if (mPortLayerMode >= 0) {
    PortUnpackLayers();
  }
#endif
}
