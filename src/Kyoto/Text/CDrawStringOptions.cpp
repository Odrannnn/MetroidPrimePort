#include "Kyoto/Text/CDrawStringOptions.hpp"

static const uint col = 0;

CDrawStringOptions::CDrawStringOptions()
: x0_direction(kTD_Horizontal)
, x4_colors(col)
#if (VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02) || defined(TARGET_PC)
, x48_extraCharacterSpacing(0)
#endif
{}
