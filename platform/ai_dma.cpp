// PC implementation of the GameCube audio-interface (AI) DMA path.
//
// MusyX has its own SDL stream in the vendored runtime, but streamed audio
// (front-end/in-game music via CStaticAudioPlayer, movie audio) is produced by
// filling an AI DMA buffer from the guest. There is no AI hardware, so the
// registered DMA callback is driven from the game's main loop at the buffer rate
// and the submitted buffer is fed to an SDL audio stream. Driving it on the main
// thread matters: the guest mixer is not thread-safe.

#include <cstdint>
#include <cstdlib>

#include <SDL3/SDL.h>

#include <dolphin/ai.h>

namespace {
constexpr uint32_t kSampleRate = 32000;
// 16-bit stereo frames: 4 bytes per frame.
constexpr uint32_t kBytesPerFrame = 4;
constexpr uint64_t kDefaultFrameNs = 5000000ull; // 0x280 bytes at 32 kHz
constexpr int kTargetQueuedBytes = kSampleRate * kBytesPerFrame * 64 / 1000;

AIDCallback sCallback = nullptr;
uintptr_t sBuffer = 0;
uint32_t sLength = 0;
SDL_AudioStream* sStream = nullptr;
uint64_t sNextFrameNs = 0;
bool sStarted = false;
bool sOutputEnabled = true;

void EnsureStarted() {
  if (sStarted) {
    return;
  }
  sStarted = true;
  // Enabled by default; MP_DISABLE_AI_AUDIO=1 isolates streamed audio (music)
  // from the MusyX effects when diagnosing.
  sOutputEnabled = std::getenv("MP_DISABLE_AI_AUDIO") == nullptr;
  // Silence the AI is notionally playing before the first AIInitDMA, so the
  // guest's `AIGetDMAStartAddr` always yields a readable buffer.
  static uint8_t sSilence[0x280] = {};
  sBuffer = reinterpret_cast< uintptr_t >(sSilence);
  sLength = sizeof(sSilence);

  if (!sOutputEnabled) {
    return;
  }
  SDL_InitSubSystem(SDL_INIT_AUDIO);
  SDL_AudioSpec spec{SDL_AUDIO_S16, 2, kSampleRate};
  sStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  if (sStream != nullptr) {
    SDL_ResumeAudioStreamDevice(sStream);
  }
}
} // namespace

// The SDK's `AIGetDMAStartAddr` returns a 32-bit address, but on the port the DMA
// buffers are 64-bit host pointers. Guest code that needs the real pointer (the
// streamed-audio mixer) uses this instead.
extern "C" uintptr_t AIPortGetDMAStartAddr(void) { return sBuffer; }

// Runs pending AI DMA callbacks. Must be called from the thread that owns the
// guest audio state (the main loop).
extern "C" void AIPortPoll(void) {
  EnsureStarted();
  if (sCallback == nullptr) {
    // Nothing is playing; resynchronise so a later stream does not burst.
    sNextFrameNs = SDL_GetTicksNS();
    return;
  }

  // Keep enough decoded audio queued to absorb main-thread and disc-loading
  // jitter. A single 5 ms DMA buffer underruns whenever a frame runs long.
  if (sOutputEnabled && sStream != nullptr) {
    int queued = SDL_GetAudioStreamQueued(sStream);
    int budget = 16;
    while (sCallback != nullptr && queued < kTargetQueuedBytes && budget-- > 0) {
      sCallback();
      const uintptr_t buffer = sBuffer;
      const uint32_t length = sLength;
      if (buffer == 0 || length == 0 ||
          !SDL_PutAudioStreamData(sStream, reinterpret_cast< const void* >(buffer), length)) {
        break;
      }
      queued += static_cast<int>(length);
    }
    return;
  }

  const uint64_t now = SDL_GetTicksNS();
  if (sNextFrameNs == 0) {
    sNextFrameNs = now;
  }

  // Catch up to real time, bounded so a long stall cannot fire a burst.
  int budget = 8;
  while (sNextFrameNs <= now && budget-- > 0) {
    sCallback();
    const uintptr_t buffer = sBuffer;
    const uint32_t length = sLength;
    const uint64_t duration = length != 0
                                  ? static_cast< uint64_t >(length) * 1000000000ull /
                                        (static_cast< uint64_t >(kBytesPerFrame) * kSampleRate)
                                  : kDefaultFrameNs;
    sNextFrameNs += duration;
  }
  if (sNextFrameNs < now) {
    sNextFrameNs = now;
  }
}

extern "C" uint32_t AIGetDMAStartAddr(void) { return static_cast< uint32_t >(sBuffer); }

extern "C" void AIInit(u8* stack) {
  (void)stack;
  EnsureStarted();
}

extern "C" void AIInitDMA(uintptr_t start_addr, uint32_t length) {
  EnsureStarted();
  sBuffer = start_addr;
  sLength = length;
}

extern "C" AIDCallback AIRegisterDMACallback(AIDCallback callback) {
  EnsureStarted();
  AIDCallback previous = sCallback;
  sCallback = callback;
  return previous;
}

extern "C" void AISetStreamPlayState(uint32_t state) {
  if (sStream == nullptr) {
    return;
  }
  if (state == 0) {
    SDL_PauseAudioStreamDevice(sStream);
  } else {
    SDL_ResumeAudioStreamDevice(sStream);
  }
}

// Runtime mute of the streamed-audio path (used by the debug overlay).
extern "C" void AIPortSetOutputEnabled(int enabled) {
  EnsureStarted();
  sOutputEnabled = enabled != 0;
}
