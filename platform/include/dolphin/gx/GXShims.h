#pragma once

// GX API surface the Aurora compatibility layer does not provide. Some are real
// gaps (breakpoints, write-gather pipe redirection); others are values the
// original SDK headers exposed transitively.

#include <dolphin/gx.h>

#ifndef GX_MAX_VTXDESCLIST_SZ
#define GX_MAX_VTXDESCLIST_SZ (GX_VA_MAX_ATTR + 1)
#endif

#ifndef GX_BREAKPT_CALLBACK_DEFINED
#define GX_BREAKPT_CALLBACK_DEFINED
typedef void (*GXBreakPtCallback)(void);
#endif

#ifdef __cplusplus
extern "C" {
#endif
void GXEnableBreakPt(void* breakPt);
void GXDisableBreakPt(void);
GXBreakPtCallback GXSetBreakPtCallback(GXBreakPtCallback cb);
volatile void* GXRedirectWriteGatherPipe(void* buf);
void GXRestoreWriteGatherPipe(void);
#ifdef __cplusplus
}
#endif
