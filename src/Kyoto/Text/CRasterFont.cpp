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
// CRasterFont::PortAddStandIns, PortAddAccents and PortUnpackLayers live in
// platform/port_raster_font.cpp.
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
#ifdef TARGET_PC
      newWidth += options.GetExtraCharacterSpacing(); // @ fn_8032750C (opts+0x48)
#endif
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
#ifdef TARGET_PC
          curX += options.GetExtraCharacterSpacing(); // @ fn_803277F0 (opts+0x48)
#endif
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

#ifdef TARGET_PC
  // A French, German, Spanish or Italian text from an imported PAL disc draws
  // with that disc's font (same id): the USA fonts have no accented letters.
  if (!PortDisc::HasPalData()) {
    const char* code = PortDebug::TextLanguage();
    static const char* const kPalLanguages[] = {"EUFR", "USFR", "EUGE", "EUSP", "USSP", "EUIT"};
    bool palLanguage = false;
    for (size_t i = 0; i < sizeof(kPalLanguages) / sizeof(kPalLanguages[0]); ++i) {
      palLanguage = palLanguage || strncmp(code, kPalLanguages[i], 4) == 0;
    }
    std::vector< uint8_t > fontData, textureData;
    if (palLanguage && PortPalLanguages::ReadFont(tag.GetId(), fontData, textureData)) {
      CMemoryInStream fontStream(fontData.data(), fontData.size());
      CRasterFont* font = rs_new CRasterFont(fontStream, nullptr);
      CMemoryInStream textureStream(textureData.data(), textureData.size());
      font->SetTexture(rs_new CTexture(textureStream, CTexture::kAM_Zero, CTexture::kBK_Zero));
      return font;
    }
  }
#endif
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
