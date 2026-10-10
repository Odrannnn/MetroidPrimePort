// Temporary SDK stubs for functions the Aurora compatibility layer does not
// implement. These let the Metroid Prime port link; several will need real
// implementations for correct audio/VI/OS behaviour. Signatures are taken from
// the same headers the game compiled against (Aurora's dolphin/*).

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include "port_debug.h"

#include <dolphin/ai.h>
#include <dolphin/ar.h>
#include <dolphin/dtk.h>
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>

// CElementGen.cpp declares this SDK entry point directly.
extern "C" float frsqrte(float x);

// AI (audio DMA) is implemented in platform/ai_dma.cpp.

// --- AR (ARAM) --------------------------------------------------------------
extern "C" u32 ARGetDMAStatus(void) {
    return 0;
}

// --- DTK (streamed audio tracks) --------------------------------------------
extern "C" void DTKInit(void) {}
extern "C" u32 DTKGetState(void) {
    return 0;
}
extern "C" int DTKSetState(u32 state) {
    (void)state;
    return 0;
}
extern "C" int DTKNextTrack(void) {
    return 0;
}
extern "C" int DTKFlushTracks(DTKFlushCallback callback) {
    (void)callback;
    return 0;
}
extern "C" u32 DTKQueueTrack(char* fileName, DTKTrack* track, u32 eventMask, DTKCallback callback) {
    (void)fileName;
    (void)track;
    (void)eventMask;
    (void)callback;
    return 0;
}
extern "C" void DTKSetRepeatMode(u32 repeat) {
    (void)repeat;
}
extern "C" void DTKSetSampleRate(u32 samplerate) {
    (void)samplerate;
}
extern "C" void DTKSetVolume(u8 left, u8 right) {
    (void)left;
    (void)right;
}

// --- GX gaps ----------------------------------------------------------------
extern "C" void GXAbortFrame(void) {}
extern "C" void GXInitFifoLimits(GXFifoObj* fifo, u32 hiWaterMark, u32 loWaterMark) {
    (void)fifo;
    (void)hiWaterMark;
    (void)loWaterMark;
}
extern "C" void GXInvalidateTexRegion(const GXTexRegion* region) {
    (void)region;
}
extern "C" void GXSetMisc(GXMiscToken token, u32 val) {
    (void)token;
    (void)val;
}

// --- OS gaps ----------------------------------------------------------------
extern "C" void OSCancelAlarm(OSAlarm* alarm) {
    (void)alarm;
}
extern "C" BOOL OSDisableInterrupts(void) {
    return TRUE;
}
extern "C" BOOL OSEnableInterrupts(void) {
    return TRUE;
}
extern "C" u32 OSGetConsoleType(void) {
    return 0;
}
extern "C" OSThread* OSGetCurrentThread(void) {
    // Non-null dummy so callers that register/unregister do not dereference
    // null. Give it a plausible guest stack range inside MEM1.
    static OSThread s_dummyThread{};
    static bool init = false;
    if (!init) {
        init = true;
        s_dummyThread.stackBase = reinterpret_cast< u8* >(0x81800000u);
        s_dummyThread.stackEnd = reinterpret_cast< u8* >(0x81700000u);
        s_dummyThread.state = OS_THREAD_STATE_RUNNING;
    }
    return &s_dummyThread;
}
extern "C" u32 OSGetProgressiveMode(void) {
    return 0;
}
extern "C" BOOL OSGetResetButtonState(void) {
    return FALSE;
}
extern "C" void OSGetSavedRegion(void** start, void** end) {
    if (start != nullptr) {
        *start = nullptr;
    }
    if (end != nullptr) {
        *end = nullptr;
    }
}
extern "C" u32 OSGetSoundMode(void) {
    return OS_SOUND_MODE_STEREO;
}
extern "C" BOOL OSLink(OSModuleInfo* newModule, void* bss) {
    (void)newModule;
    (void)bss;
    return TRUE;
}
extern "C" void OSProtectRange(u32 chan, void* addr, u32 nBytes, u32 control) {
    (void)chan;
    (void)addr;
    (void)nBytes;
    (void)control;
}
extern "C" void OSResetSystem(int reset, u32 resetCode, BOOL forceMenu) {
    (void)reset;
    (void)resetCode;
    (void)forceMenu;
    PortDebug::RequestReset();
}
extern "C" BOOL OSRestoreInterrupts(BOOL level) {
    (void)level;
    return TRUE;
}
extern "C" u32 OSSaveContext(OSContext* context) {
    (void)context;
    return 0;
}
extern "C" OSErrorHandler OSSetErrorHandler(OSError error, OSErrorHandler handler) {
    (void)error;
    (void)handler;
    return nullptr;
}
extern "C" void OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period, OSAlarmHandler handler) {
    (void)alarm;
    (void)start;
    (void)period;
    (void)handler;
}
extern "C" void OSSetProgressiveMode(u32 on) {
    (void)on;
}
extern "C" void OSSetSaveRegion(void* start, void* end) {
    (void)start;
    (void)end;
}
extern "C" void OSSetSoundMode(u32 mode) {
    (void)mode;
}
extern "C" BOOL OSUnlink(OSModuleInfo* oldModule) {
    (void)oldModule;
    return TRUE;
}
extern "C" void OSYieldThread(void) { std::this_thread::yield(); }

// --- VI gaps ----------------------------------------------------------------
extern "C" u32 VIGetDTVStatus(void) {
    return 0;
}
extern "C" u32 VIGetNextField(void) {
    return 0;
}
extern "C" void VISetBlack(BOOL black) {
    (void)black;
}
extern "C" void VISetNextFrameBuffer(void* fb) {
    (void)fb;
}
// Port: the game registers VI retrace callbacks that flip framebuffers and
// advance its frame pacing. With no real retrace, GXEnableBreakPt (see
// platform/shims.cpp) pulses them to complete each frame.
namespace {
VIRetraceCallback s_preRetraceCallback = nullptr;
VIRetraceCallback s_postRetraceCallback = nullptr;
} // namespace

extern "C" void AuroraRetracePulse(void) {
    if (s_preRetraceCallback != nullptr) {
        s_preRetraceCallback(0);
    }
    if (s_postRetraceCallback != nullptr) {
        s_postRetraceCallback(0);
    }
}

extern "C" VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb) {
    VIRetraceCallback previous = s_postRetraceCallback;
    s_postRetraceCallback = cb;
    return previous;
}
extern "C" VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb) {
    VIRetraceCallback previous = s_preRetraceCallback;
    s_preRetraceCallback = cb;
    return previous;
}
extern "C" void VIWaitForRetrace(void) {}

// --- Aurora-declared but not linked -----------------------------------------
// Aurora lists these in headers but leaves the implementations as TODOs (and
// OSFatal's definition does not link), so provide them here.
extern "C" void GXInitTexCacheRegion(GXTexRegion* region, GXBool is_32b_mipmap, u32 tmem_even,
                                     GXTexCacheSize size_even, u32 tmem_odd,
                                     GXTexCacheSize size_odd) {
    (void)region;
    (void)is_32b_mipmap;
    (void)tmem_even;
    (void)size_even;
    (void)tmem_odd;
    (void)size_odd;
}
extern "C" GXTexRegionCallback GXSetTexRegionCallback(GXTexRegionCallback callback) {
    (void)callback;
    return nullptr;
}
extern "C" void OSFatal(GXColor fg, GXColor bg, const char* msg) {
    (void)fg;
    (void)bg;
    if (msg != nullptr) {
        std::fprintf(stderr, "OSFatal: %s\n", msg);
    }
    // Declared noreturn: returning would run off into undefined behaviour.
    std::abort();
}

// --- PowerPC math -----------------------------------------------------------
extern "C" float frsqrte(float x) {
    return 1.0f / std::sqrt(x);
}
