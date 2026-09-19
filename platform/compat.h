#pragma once

// The decompiled code targets the original Dolphin SDK. Aurora mirrors the SDK
// API but (a) hides OSContext/renames PADStatus members under TARGET_PC, (b)
// omits some Retro Studios convenience macros, and (c) does not aggregate the
// individual GX/SI/PAD headers the way the original umbrella headers did. This
// header is force-included ahead of every game translation unit (see
// CMakeLists.txt) to restore the SDK-facing view.

#include <math.h>
#include <stdlib.h>
#include <string.h>

// The game's global `operator delete` (routing to CMemory::Free) lives inline in
// this header; without it, `delete` on game-allocator memory reaches glibc free.
#include <Kyoto/Alloc/CMemory.hpp>

// MWCC-isms used throughout the decompiled code.
#define nofralloc
#define __abs(x) ((x) < 0 ? -(x) : (x))

// Umbrella inclusion, as the original SDK headers provided transitively.
#include <dolphin/gx.h>
#include <dolphin/gx/GXShims.h>
#include <dolphin/si.h>
#include <dolphin/pad.h>
#include <dolphin/os.h>
#include <dolphin/card.h>

// Aurora provides CARDFormat but not its async wrapper; see platform/shims.cpp.
#ifdef __cplusplus
extern "C" {
#endif
s32 CARDFormatAsync(s32 chan, CARDCallback callback);
// Port: drains Aurora's deferred ARQ completion callbacks (see AR.cpp).
void ARQPoll(void);
#ifdef __cplusplus
}
#endif

// Aurora names the PADStatus triggers triggerLeft/triggerRight under
// TARGET_PC; the game uses the SDK names triggerL/triggerR.
#ifdef TARGET_PC
#define triggerL triggerLeft
#define triggerR triggerRight
#endif



#ifndef AUTO
#if defined(__cplusplus) && __cplusplus >= 201103L
#define AUTO(name, val) auto name = val
#define AUTO_REF(name, val) auto& name = val
#define AUTO_CONST_REF(name, val) const auto& name = val
#else
#define AUTO(name, val) __typeof__(val) name = val
#define AUTO_REF(name, val) __typeof__(val)& name = val
#define AUTO_CONST_REF(name, val) const __typeof__(val)& name = val
#endif
#endif
