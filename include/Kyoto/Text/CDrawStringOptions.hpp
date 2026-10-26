#ifndef _CDRAWSTRINGOPTIONS
#define _CDRAWSTRINGOPTIONS

#include "Kyoto/Graphics/CColor.hpp"
#include "Kyoto/Text/TextCommon.hpp"
#include "rstl/reserved_vector.hpp"

class CDrawStringOptions {
public:
  CDrawStringOptions();

  void SetTextDirection(ETextDirection dir) { x0_direction = dir; }
  ETextDirection GetTextDirection() const { return x0_direction; }
  void SetPaletteEntry(int idx, uint color) { x4_colors[idx] = color; }
  uint GetPaletteEntry(int idx) const { return x4_colors[idx]; }
#ifdef TARGET_PC
  // Trilogy (and PAL) add this after every glyph's advance: rs5mp1 GetSize fn_8032750C,
  // SinglePassDrawString fn_803277F0. 0 everywhere on GC 1.00.
  void SetExtraCharacterSpacing(int space) { x48_extraCharacterSpacing = space; }
  int GetExtraCharacterSpacing() const { return x48_extraCharacterSpacing; }
#endif

private:
  ETextDirection x0_direction;
  rstl::reserved_vector< u32, 16 > x4_colors;
#if (VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02) || defined(TARGET_PC)
  int x48_extraCharacterSpacing;
#endif
};

#endif // _CDRAWSTRINGOPTIONS
