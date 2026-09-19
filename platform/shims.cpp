// Stubs for SDK facilities the Aurora compatibility layer does not provide.
// These exist so the decompiled game links on the port; a few are no-ops for
// now and must be implemented for real gameplay (see TODO notes).

#include <dolphin/card.h>
#include <dolphin/gba.h>
#include <dolphin/gx.h>
#include <dolphin/gx/GXShims.h>
#include <dolphin/PPCArch.h>

#include <cstdarg>
#include <cstdio>

// --- OS report --------------------------------------------------------------
// Aurora declares OSReport/OSVReport weak but does not define them for
// TARGET_PC. ELF tolerates an unresolved weak symbol; COFF does not, so the
// port supplies them here.
extern "C" void OSVReport(const char* msg, va_list args) {
    vfprintf(stderr, msg, args);
    fflush(stderr);
}

extern "C" void OSReport(const char* msg, ...) {
    va_list args;
    va_start(args, msg);
    OSVReport(msg, args);
    va_end(args);
}

// --- PowerPC architecture ---------------------------------------------------
extern "C" void PPCSync(void) {
#if defined(__GNUC__) || defined(__clang__)
    __sync_synchronize();
#endif
}
extern "C" void PPCSetFpIEEEMode(void) {}

// --- GX breakpoints / write-gather pipe -------------------------------------
// Aurora has no GX breakpoint path. The game uses GXEnableBreakPt to learn when
// the GPU has consumed the FIFO, then a VI retrace to flip buffers and advance
// its frame counter (CGraphics::VideoPostCallback). Simulate that completion
// synchronously: breakpoint callback, then the registered pre/post retrace
// callbacks. Without this, CGraphics::EndScene spins forever.
namespace {
GXBreakPtCallback s_breakPtCallback = nullptr;
} // namespace

extern "C" void AuroraRetracePulse(void);

extern "C" void GXEnableBreakPt(void* breakPt) {
    (void)breakPt;
    if (s_breakPtCallback != nullptr) {
        s_breakPtCallback();
    }
    AuroraRetracePulse();
}
extern "C" void GXDisableBreakPt(void) {}
extern "C" GXBreakPtCallback GXSetBreakPtCallback(GXBreakPtCallback cb) {
    GXBreakPtCallback previous = s_breakPtCallback;
    s_breakPtCallback = cb;
    return previous;
}
extern "C" volatile void* GXRedirectWriteGatherPipe(void* buf) {
    return buf;
}
extern "C" void GXRestoreWriteGatherPipe(void) {}

// --- Memory card ------------------------------------------------------------
// CARDFormatAsync is defined by Aurora (card.cpp) but not declared in its
// header; the declaration lives in platform/compat.h.

// --- GBA link cable ---------------------------------------------------------
// TODO: implement via Aurora when GBA connectivity is wanted.
extern "C" void GBAInit(void) {}
extern "C" s32 GBAGetStatus(s32 chan, u8* status) {
    (void)chan;
    if (status != nullptr) {
        *status = 0;
    }
    return GBA_NOT_READY;
}
extern "C" s32 GBAGetProcessStatus(s32 chan, u8* percentp) {
    (void)chan;
    if (percentp != nullptr) {
        *percentp = 0;
    }
    return GBA_NOT_READY;
}
extern "C" s32 GBARead(s32 chan, u8* dst, u8* status) {
    (void)chan;
    (void)dst;
    if (status != nullptr) {
        *status = 0;
    }
    return GBA_NOT_READY;
}
extern "C" s32 GBAWrite(s32 chan, u8* src, u8* status) {
    (void)chan;
    (void)src;
    if (status != nullptr) {
        *status = 0;
    }
    return GBA_NOT_READY;
}
extern "C" s32 GBAReset(s32 chan, u8* status) {
    (void)chan;
    if (status != nullptr) {
        *status = 0;
    }
    return GBA_NOT_READY;
}
extern "C" s32 GBAJoyBootAsync(s32 chan, s32 palette_color, s32 palette_speed, u8* programp,
                               s32 length, u8* status, GBACallback callback) {
    (void)chan;
    (void)palette_color;
    (void)palette_speed;
    (void)programp;
    (void)length;
    if (status != nullptr) {
        *status = 0;
    }
    (void)callback;
    return GBA_NOT_READY;
}
