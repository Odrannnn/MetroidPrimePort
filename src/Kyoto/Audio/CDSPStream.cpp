#include "Kyoto/Audio/CDSPStream.hpp"

#include "Kyoto/Alloc/CMemory.hpp"

#include "musyx/musyx.h"

#include "dolphin/dvd.h"
#include "dolphin/os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__ANDROID__)
#include <android/log.h>
// Port diagnostic: streamed-audio (music) refill tracing. Android apps have no
// usable stderr, so report to logcat there. The per-chunk and per-mix detail
// needs `MP_STREAM_TRACE=1`; stream starts, stops and stale completions are
// always reported because they are rare and explain a stalled stream.
#define MP_STREAM_LOG(...) __android_log_print(ANDROID_LOG_INFO, "mpstream", __VA_ARGS__)
#else
#define MP_STREAM_LOG(...)                        \
  do {                                            \
    std::fprintf(stderr, "mpstream: ");           \
    std::fprintf(stderr, __VA_ARGS__);            \
    std::fputc('\n', stderr);                     \
  } while (0)
#endif

namespace {
bool StreamTraceVerbose() {
  static const bool verbose = getenv("MP_STREAM_TRACE") != nullptr;
  return verbose;
}
} // namespace

#define MP_STREAM_TRACE_LOG(...)     \
  do {                               \
    if (StreamTraceVerbose()) {      \
      MP_STREAM_LOG(__VA_ARGS__);    \
    }                                \
  } while (0)

#ifdef TARGET_PC
// The guest guarded the stream table with OSDisableInterrupts, which is a no-op on
// PC, but three threads touch it here: the game (allocate, silence, volume), the
// MusyX mixer (UpdateStream, called with the MusyX lock held) and Aurora's DVD
// worker (ReadCompleted). Unlocked, the worker could count xec_readsPending down
// while the mixer tested it, so a stream was freed twice or its slot leaked. Every
// section takes the MusyX lock instead: the mixer already holds it, it is recursive
// (the snd* calls inside lock it again), and the heap lock CMemory takes under it
// is never held while waiting for the MusyX lock, so the order stays one way.
extern "C" void hwDisableIrq();
extern "C" void hwEnableIrq();
static BOOL StreamLock() {
  hwDisableIrq();
  return TRUE;
}
static void StreamUnlock(BOOL) { hwEnableIrq(); }
namespace {
struct StreamLockScope {
  StreamLockScope() { hwDisableIrq(); }
  ~StreamLockScope() { hwEnableIrq(); }
  StreamLockScope(const StreamLockScope&) = delete;
  StreamLockScope& operator=(const StreamLockScope&) = delete;
};
} // namespace
#else
static BOOL StreamLock() { return OSDisableInterrupts(); }
static void StreamUnlock(BOOL ints) { OSRestoreInterrupts(ints); }
#endif

static struct {
  CDSPStream streams[4];
  int handleCounter;
  int pad;
  int unkCounter;
  int pad2;
} g_StreamData;

#define g_Streams g_StreamData.streams
#define sHandleCounter g_StreamData.handleCounter

void CDSPStream::OpenFiles(const char* fileName, CDSPStream& stream) {
  DVDOpen(const_cast< char* >(fileName), &stream.x50_fileInfo1);
  DVDOpen(const_cast< char* >(fileName), &stream.x8c_fileInfo2);
}

void CDSPStream::CloseFiles() {
  DVDClose(&x50_fileInfo1);
  DVDClose(&x8c_fileInfo2);
}

int CDSPStream::PickFreeStream(CDSPStream*& streamOut, int oneshot) {
  const CDSPStream* streams = g_Streams;
  for (int i = 0; i < 4; ++i) {
    if (g_Streams[i].x0_state != 0 || oneshot != g_Streams[i].x1_oneshot) {
      continue;
    }

    uint handle;
    for (;;) {
      handle = sHandleCounter++;
      if (handle == static_cast< uint >(-1)) {
        handle = sHandleCounter++;
      }

      uint j = 0;
      for (; j < 4; ++j) {
        if (streams[j].x0_state != 0 && handle == streams[j].x4_handle) {
          break;
        }
      }
      if (j == 4) {
        break;
      }
    }

    g_Streams[i].x0_state = 1;
    g_Streams[i].x4_handle = handle;
    g_Streams[i].x8_right = nullptr;
    g_Streams[i].xc_left = nullptr;
    streamOut = &g_Streams[i];
    return handle;
  }

  return -1;
}

int CDSPStream::FindStreamIdx(int handle) {
  for (int i = 0; i < 4; ++i) {
    if (g_Streams[i].x0_state != 0 && static_cast< uint >(handle) == g_Streams[i].x4_handle) {
      return i;
    }
  }
  return -1;
}

void CDSPStream::DoAllocateStream() {
  xd4_buffer = CMemory::Alloc(0x11DC0, IAllocator::kHI_RoundUpLen, IAllocator::kSC_Unk1,
                              IAllocator::kTP_Heap, CCallStack(-1, "??(??)"));
  xc8_streamId = sndStreamAllocEx(
      static_cast< u8 >(0xFF), xd4_buffer, xdc_streamSamples, static_cast< u32 >(32000),
      static_cast< u8 >(0), static_cast< u8 >(0x40), static_cast< u8 >(0), static_cast< u8 >(0),
      static_cast< u8 >(0), static_cast< u8 >(0), static_cast< u32 >(0x30001), UpdateStream,
#if UINTPTR_MAX > UINT32_MAX
      static_cast< u32 >(this - g_Streams), static_cast< SND_ADPCMSTREAM_INFO* >(nullptr));
#else
      reinterpret_cast< uintptr_t >(this), static_cast< SND_ADPCMSTREAM_INFO* >(nullptr));
#endif
}

void CDSPStream::Initialize() {
  for (uint i = 0; i < 4; ++i) {
    CDSPStream& stream = g_Streams[i];
    stream.x0_state = 0;
    stream.xd4_buffer = nullptr;
    stream.xd8_bufferBytes = 0x11DC0;
    stream.xdc_streamSamples = 0x1F410;
    stream.xc8_streamId = static_cast< uint >(-1);
    if (i < 2) {
      stream.x1_oneshot = 0;
      stream.DoAllocateStream();
    } else {
      stream.x1_oneshot = 1;
    }
  }
  sHandleCounter = 0;
  g_StreamData.unkCounter = 0;
}

void CDSPStream::FreeAllStreams() {
  for (uint i = 0; i < 4; ++i) {
    sndStreamFree(g_Streams[i].xc8_streamId);
    CMemory::Free(g_Streams[i].xd4_buffer);
  }
}

uint CDSPStream::AllocateStream(const SStreamInfo& info, char vol, char pan) {
  x10_fileName = info.x0_fileName;
  x14_sampleRate = info.x4_sampleRate;
  x18_headerSize = info.x8_headerSize;
  x1c_adpcmBytes = info.xc_adpcmBytes;
  x20_loopFlag = info.x10_loopFlag;
  x24_loopStartByte = info.x14_loopStartByte;
  x28_loopEndByte = info.x18_loopEndByte;
  x2c_adpcmInfo = info.x1c_adpcmInfo;

  if (xd4_buffer == nullptr) {
    DoAllocateStream();
  }

  x4c_vol = vol;
  x4d_pan = pan;
  xe4_needsPrime = 1;
  xe8_silenced = 0;
  xf0_stopRequested = 0;
  xec_readsPending = 1;
  xe0_curBuffer = 1;
  xd8_bufferBytes = 0x11DC0;
  xdc_streamSamples = (xd8_bufferBytes >> 3) * 14;
  if (xd8_bufferBytes > 0x11DC0) {
    xd8_bufferBytes = 0x11DC0;
  }

  uint fileCur = xd8_bufferBytes >> 1;
  xcc_fileCur = fileCur;
  xd0_remaining = x1c_adpcmBytes;
  uint ret;
  if (xd0_remaining < fileCur) {
    ret = xd0_remaining;
    memset(static_cast< char* >(xd4_buffer) + ret, 0, fileCur - ret);
  } else {
    ret = fileCur;
  }

  x0_state = 2;
  MP_STREAM_LOG("alloc name=%s rate=%u adpcm=%u loop=%d loopStart=%u loopEnd=%u oneshot=%d",
                x10_fileName.data(), x14_sampleRate, x1c_adpcmBytes,
                x20_loopFlag, x24_loopStartByte, x28_loopEndByte, x1_oneshot);
  return ret;
}

int CDSPStream::AllocateMono(const SStreamInfo& info, char vol, char pan, int oneshot) {
  int handle;
  BOOL ints = StreamLock();
  CDSPStream* stream;
  handle = PickFreeStream(stream, oneshot);
  if (static_cast< uint >(handle) != static_cast< uint >(-1)) {
    uint readLen = stream->AllocateStream(info, vol, pan);
    StreamUnlock(ints);
    OpenFiles(stream->x10_fileName.data(), *stream);
    DVDReadAsyncPrio(&stream->x50_fileInfo1, stream->xd4_buffer, static_cast< s32 >(readLen),
                     static_cast< s32 >(stream->x18_headerSize), ReadCompleted, 1);
    return handle;
  }
  StreamUnlock(ints);
  return handle;
}

void CDSPStream::DeallocateStream() {
  x0_state = 0;
  switch (x1_oneshot) {
  case 0:
    if (xc8_streamId != static_cast< uint >(-1)) {
      sndStreamDeactivate(xc8_streamId);
    }
    break;
  case 1:
    if (xd4_buffer != nullptr) {
      CMemory::Free(xd4_buffer);
      xd4_buffer = nullptr;
      if (xc8_streamId != static_cast< uint >(-1)) {
        sndStreamDeactivate(xc8_streamId);
        sndStreamFree(xc8_streamId);
        xc8_streamId = static_cast< uint >(-1);
      }
    }
    break;
  }
}

int CDSPStream::AllocateStereo(const SStreamInfo& leftInfo, const SStreamInfo& rightInfo, char vol,
                               int oneshot) {
  BOOL ints = StreamLock();
  CDSPStream* streams[2];
  int handle = PickFreeStream(streams[0], oneshot);
  if (static_cast< uint >(handle) != static_cast< uint >(-1)) {
    if (static_cast< uint >(PickFreeStream(streams[1], oneshot)) != static_cast< uint >(-1)) {
      streams[0]->x8_right = streams[1];
      streams[1]->xc_left = streams[0];
      uint readLen[2];
      readLen[0] = streams[0]->AllocateStream(leftInfo, vol, 0);
      readLen[1] = streams[1]->AllocateStream(rightInfo, vol, 0x7F);
      StreamUnlock(ints);
      OpenFiles(streams[0]->x10_fileName.data(), *streams[0]);
      OpenFiles(streams[1]->x10_fileName.data(), *streams[1]);
      DVDReadAsyncPrio(&streams[0]->x50_fileInfo1, streams[0]->xd4_buffer,
                       static_cast< s32 >(readLen[0]),
                       static_cast< s32 >(streams[0]->x18_headerSize), ReadCompleted, 1);
      DVDReadAsyncPrio(&streams[1]->x50_fileInfo1, streams[1]->xd4_buffer,
                       static_cast< s32 >(readLen[1]),
                       static_cast< s32 >(streams[1]->x18_headerSize), ReadCompleted, 1);
      return handle;
    }
    streams[0]->DeallocateStream();
    handle = -1;
  }
  StreamUnlock(ints);
  return handle;
}

void CDSPStream::SilenceStream() {
  if (x0_state != 0 && xe8_silenced == 0) {
    sndStreamMixParameterEx(xc8_streamId, 0, x4d_pan, 0, 0, 0);
    xe8_silenced = 1;
  }
}

void CDSPStream::Silence(int handle) {
  BOOL ints = StreamLock();
  int idx = FindStreamIdx(handle);
  if (static_cast< uint >(idx) != 0xFFFFFFFF) {
    g_Streams[idx].SilenceStream();
    if (g_Streams[idx].x8_right != nullptr) {
      g_Streams[idx].x8_right->SilenceStream();
    }
    if (g_Streams[idx].xc_left != nullptr) {
      g_Streams[idx].xc_left->SilenceStream();
    }
  }
  StreamUnlock(ints);
}

void CDSPStream::UpdateStreamVolume(int vol) {
  x4c_vol = vol;
  if (x0_state != 0 && xe8_silenced == 0) {
    sndStreamMixParameterEx(xc8_streamId, x4c_vol, x4d_pan, 0, 0, 0);
  }
}

void CDSPStream::UpdateVolume(int handle, int vol) {
  BOOL ints = StreamLock();
  int idx = FindStreamIdx(handle);
  if (static_cast< uint >(idx) != 0xFFFFFFFF) {
    CDSPStream& stream = g_Streams[idx];
    stream.UpdateStreamVolume(vol);
    if (stream.x8_right != nullptr) {
      stream.x8_right->UpdateStreamVolume(vol);
    }
    if (stream.xc_left != nullptr) {
      stream.xc_left->UpdateStreamVolume(vol);
    }
  }
  StreamUnlock(ints);
}

bool CDSPStream::IsStreamActive(int handle) {
  bool ret = false;
  BOOL ints = StreamLock();
  int idx = FindStreamIdx(handle);
  if (static_cast< uint >(idx) != 0xFFFFFFFF) {
    ret = g_Streams[idx].x0_state != 0;
  }
  StreamUnlock(ints);
  return ret;
}

bool CDSPStream::IsStreamAvailable(int handle) {
  bool ret = false;
  BOOL ints = StreamLock();
  int idx = FindStreamIdx(handle);
  if (static_cast< uint >(idx) != 0xFFFFFFFF) {
    ret = g_Streams[idx].x0_state == 4;
  }
  StreamUnlock(ints);
  return ret;
}

#ifdef TARGET_PC
// True while a silenced stream of this kind still holds its slot. Silence only
// mutes it; the mixer's next UpdateStream (or the pending read's completion)
// frees it.
bool CDSPStream::IsReleasing(int oneshot) {
  bool ret = false;
  BOOL ints = StreamLock();
  for (int i = 0; i < 4; ++i) {
    const CDSPStream& stream = g_Streams[i];
    if (stream.x1_oneshot == oneshot && stream.x0_state != 0 &&
        (stream.xe8_silenced != 0 || stream.xf0_stopRequested != 0)) {
      ret = true;
    }
  }
  StreamUnlock(ints);
  return ret;
}
#endif

void CDSPStream::StopStream() {
  if (xec_readsPending == 0) {
    CloseFiles();
    DeallocateStream();
  } else {
    xf0_stopRequested = 1;
  }
}

void CDSPStream::BufferStream() {
  void* buf;
  uint readLen = xd8_bufferBytes >> 1;
  uint secondReadLen = 0;
  if (xe0_curBuffer != 0) {
    buf = static_cast< char* >(xd4_buffer) + readLen;
  } else {
    buf = xd4_buffer;
  }

  uint endByte;
  if (x20_loopFlag != 0) {
    endByte = x28_loopEndByte;
  } else {
    endByte = xd0_remaining;
  }

  if (xcc_fileCur + readLen > endByte) {
    if (endByte > xcc_fileCur) {
      readLen = endByte - xcc_fileCur;
      if (x20_loopFlag != 0) {
        secondReadLen = (xd8_bufferBytes >> 1) - readLen;
      } else {
        memset(static_cast< char* >(buf) + readLen, 0, (xd8_bufferBytes >> 1) - readLen);
      }
    } else if (x20_loopFlag != 0) {
      xcc_fileCur = x24_loopStartByte;
    } else {
      memset(buf, 0, xd8_bufferBytes >> 1);
      sndStreamARAMUpdate(xc8_streamId, xe0_curBuffer != 0 ? xdc_streamSamples >> 1 : 0,
                          xdc_streamSamples >> 1, 0, 0);
      readLen = 0;
    }
  }

  BOOL ints = StreamLock();
  // Advance the destination selector before starting the read: the completion
  // (which may run on Aurora's DVD worker thread before this call returns)
  // uploads the half that was just read based on this flag, so leaving the
  // toggle until afterwards could upload the other half instead.
  xe0_curBuffer ^= 1;
  if (readLen != 0) {
    if (xec_readsPending == 0) {
      MP_STREAM_TRACE_LOG("read issue file=%s off=%u len=%u half=%u fileCur=%u loop=%d",
                          this->x10_fileName.data(), x18_headerSize + xcc_fileCur, readLen, xe0_curBuffer,
                          xcc_fileCur, static_cast< int >(x20_loopFlag));
      // Publish the outstanding-read count *before* starting the read. The
      // completion runs on Aurora's DVD worker thread, so starting the read
      // first let it decrement the count before this call assigned it: the
      // uchar wrapped to 255, every later refill saw reads "outstanding" and
      // stopped, and the mixer looped the buffer it already had. The guest
      // guarded this with OSDisableInterrupts, which is a no-op on PC.
      xec_readsPending = secondReadLen != 0 ? 2 : 1;
      DVDReadAsyncPrio(&x50_fileInfo1, buf, static_cast< s32 >(readLen),
                       static_cast< s32 >(x18_headerSize + xcc_fileCur), ReadCompleted, 1);
      if (secondReadLen != 0) {
        DVDReadAsyncPrio(&x8c_fileInfo2, static_cast< char* >(buf) + readLen,
                         static_cast< s32 >(secondReadLen),
                         static_cast< s32 >(x18_headerSize + x24_loopStartByte), ReadCompleted, 1);
        xcc_fileCur = secondReadLen + x24_loopStartByte;
      } else {
        xcc_fileCur += readLen;
      }
    } else {
      MP_STREAM_LOG("read SKIPPED (one pending) file=%s pending=%u cur=%u fileCur=%u",
                    this->x10_fileName.data(), xec_readsPending, xe0_curBuffer, xcc_fileCur);
    }
  } else if (xe4_needsPrime != 0) {
    MP_STREAM_TRACE_LOG("prime consumed file=%s", this->x10_fileName.data());
    xe4_needsPrime = 0;
  } else {
    MP_STREAM_LOG("end of stream file=%s fileCur=%u remaining=%u loop=%d",
                  this->x10_fileName.data(),
                  xcc_fileCur, xd0_remaining, static_cast< int >(x20_loopFlag));
    StopStream();
  }

  StreamUnlock(ints);
}

u32 CDSPStream::UpdateStream(void*, u32 destOffset, void*, u32 len, u32 user) {
#if UINTPTR_MAX > UINT32_MAX
  if (user >= sizeof(g_Streams) / sizeof(g_Streams[0])) {
    return 0;
  }
  CDSPStream* stream = &g_Streams[user];
#else
  CDSPStream* stream = reinterpret_cast< CDSPStream* >(user);
#endif
  if (stream->xe8_silenced != 0) {
    stream->StopStream();
    return 0;
  }

  MP_STREAM_TRACE_LOG("update state=%u pending=%u adv=%u len=%u half=%u cur=%u fileCur=%u rem=%u",
                      stream->x0_state, stream->xec_readsPending, destOffset, len,
                      stream->xdc_streamSamples >> 1, stream->xe0_curBuffer, stream->xcc_fileCur,
                      stream->xd0_remaining);

  if (destOffset + len < (stream->xdc_streamSamples >> 1)) {
    return 0;
  }

  if (stream->xec_readsPending == 0) {
    MP_STREAM_TRACE_LOG("refill triggered pending=0 state=%u cur=%u fileCur=%u", stream->x0_state,
                  stream->xe0_curBuffer, stream->xcc_fileCur);
    stream->BufferStream();
    // End-of-stream can synchronously free the MusyX stream in BufferStream.
    // Do not ask its caller to upload another chunk to the retired buffer.
    return stream->x0_state == 4 ? stream->xdc_streamSamples >> 1 : 0;
  }

  return 0;
}

int CDSPStream::InitializeStream() {
  if (x0_state == 0) {
    return 0;
  }

  sndStreamMixParameterEx(xc8_streamId, x4c_vol, x4d_pan, 0, 0, 0);
  sndStreamFrq(xc8_streamId, x14_sampleRate);
  sndStreamADPCMParameter(xc8_streamId, &x2c_adpcmInfo);
  sndStreamARAMUpdate(xc8_streamId, 0, xdc_streamSamples >> 1, 0, 0);
  if (sndStreamActivate(xc8_streamId)) {
    x0_state = 4;
    MP_STREAM_LOG("activate ok file=%s id=%u rate=%u", x10_fileName.data(), xc8_streamId,
                 x14_sampleRate);
    BufferStream();
    return 1;
  }

  MP_STREAM_LOG("activate FAILED file=%s id=%u", x10_fileName.data(), xc8_streamId);
  DeallocateStream();
  CloseFiles();
  return 0;
}

void CDSPStream::ReadCompleted(s32, DVDFileInfo* fileInfo) {
#ifdef TARGET_PC
  // Runs on Aurora's DVD worker thread; see StreamLock.
  StreamLockScope lock;
#endif
  int idx = 0;
  CDSPStream* s = g_Streams;
  for (; idx < 4; ++idx, ++s) {
    if (&s->x50_fileInfo1 == fileInfo || &s->x8c_fileInfo2 == fileInfo) {
      break;
    }
  }

  if (idx == 4) {
    // A late completion from a retired stream: the slot has been recycled and
    // nothing here owns this file info any more. Without this the code below
    // indexes g_Streams[4] and corrupts whatever follows it.
    MP_STREAM_LOG("read completion for an unowned file info (ignored)");
    return;
  }

  CDSPStream& stream = g_Streams[idx];
  if (stream.xec_readsPending == 0) {
    // A completion this stream no longer owns (a late callback for a recycled
    // slot). Counting it down would wrap the uchar and stop every later
    // refill, which is heard as the music looping the buffer it already had.
    MP_STREAM_LOG("stale read completion ignored (idx=%d state=%u)", idx, stream.x0_state);
    return;
  }
  stream.xec_readsPending--;
  MP_STREAM_TRACE_LOG("read done idx=%d pending=%u state=%u status=%d fileCur=%u", idx,
                stream.xec_readsPending, stream.x0_state, DVDGetCommandBlockStatus(&fileInfo->cb),
                stream.xcc_fileCur);
  if (stream.xec_readsPending != 0) {
    return;
  }

  if (stream.xf0_stopRequested != 0) {
    stream.xf0_stopRequested = 0;
    stream.CloseFiles();
    stream.DeallocateStream();
    return;
  }

  if (DVDGetCommandBlockStatus(&fileInfo->cb) == 0) {
    switch (stream.x0_state) {
    case 2:
      if (stream.x8_right != nullptr) {
        if (stream.x8_right->x0_state != 3) {
          stream.x0_state = 3;
          return;
        }
        if (!stream.x8_right->InitializeStream()) {
          stream.x8_right = nullptr;
        }
      }
      if (stream.xc_left != nullptr) {
        if (stream.xc_left->x0_state != 3) {
          stream.x0_state = 3;
          return;
        }
        if (!stream.xc_left->InitializeStream()) {
          stream.xc_left = nullptr;
        }
      }
      if (!stream.InitializeStream()) {
        if (stream.x8_right != nullptr) {
          stream.x8_right->xc_left = nullptr;
        }
        if (stream.xc_left != nullptr) {
          stream.xc_left->x8_right = nullptr;
        }
      }
      break;
    case 4:
      sndStreamARAMUpdate(stream.xc8_streamId,
                          stream.xe0_curBuffer != 0 ? 0 : stream.xdc_streamSamples >> 1,
                          stream.xdc_streamSamples >> 1, 0, 0);
      break;
    }
  } else {
    if (stream.x0_state == 4) {
      sndStreamDeactivate(stream.xc8_streamId);
    }
    stream.CloseFiles();
    stream.DeallocateStream();
    if (stream.x8_right != nullptr) {
      stream.x8_right->xc_left = nullptr;
    }
    if (stream.xc_left != nullptr) {
      stream.xc_left->x8_right = nullptr;
    }
  }
}
