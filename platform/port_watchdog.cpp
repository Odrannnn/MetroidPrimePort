#include "port_watchdog.h"

#include "port_crash.h"
#include "port_env.h"
#include "port_log.h"
#include "port_log_file.h"

#include <aurora/aurora.h>
#include <aurora/phase.hpp>

#include <SDL3/SDL_timer.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#if defined(__APPLE__)
#include <pthread.h>
#else
#include <sys/syscall.h>
#endif
#endif

namespace PortWatchdog {
namespace {
constexpr int64_t kSecondNs = 1000000000;
constexpr int64_t kReportsNs[] = {5 * kSecondNs, 15 * kSecondNs, 30 * kSecondNs, 60 * kSecondNs};
constexpr int kReportCount = sizeof(kReportsNs) / sizeof(kReportsNs[0]);

std::atomic< unsigned > sFrame{0};
std::atomic< int64_t > sBeatNs{0};
std::atomic< long > sMainThread{0};

int64_t NowNs() { return aurora::phase::now_ns(); }

double Seconds(int64_t ns) { return static_cast< double >(ns) / 1e9; }

void ReportStall(int level, int64_t stalledNs, unsigned frame) {
  const int64_t now = NowNs();
  const auto& main = aurora::phase::state(aurora::phase::Main);
  const auto& render = aurora::phase::state(aurora::phase::Render);
  const auto ms = [now](const aurora::phase::State& s) {
    return static_cast< long long >((now - s.sinceNs.load(std::memory_order_relaxed)) / 1000000);
  };
  PortLog::Write("watchdog: no frame for %.1f s (frame %u); main thread in '%s' for %lld ms\n", Seconds(stalledNs),
                 frame, main.name.load(std::memory_order_relaxed), ms(main));
  PortLog::Write("watchdog: render thread last in '%s' for %lld ms\n", render.name.load(std::memory_order_relaxed),
                 ms(render));
  // The stack once at the start and once again at 30 s, to show whether it moved.
  if (level == 0 || level == 2) {
    if (!PortCrash::RequestStack(sMainThread.load(std::memory_order_relaxed))) {
      PortLog::Write("watchdog: no main thread stack on this platform\n");
    }
  }
  if (level == 0) {
    PortWatchdog::LogcatDump("first stall report", false);
  }
}

void Watch() {
  int reported = 0;
  bool stalled = false;
  bool warningsDumped = false;
  int64_t firstFrameNs = 0;
  int64_t lastBeatNs = sBeatNs.load(std::memory_order_relaxed);
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const int64_t now = NowNs();
    const int64_t beat = sBeatNs.load(std::memory_order_relaxed);
    const unsigned frame = sFrame.load(std::memory_order_relaxed);
    if (firstFrameNs == 0 && frame > 1) {
      firstFrameNs = now;
    }
    if (beat != lastBeatNs) {
      if (stalled) {
        PortLog::Write("watchdog: frames resumed after %.1f s (frame %u)\n", Seconds(beat - lastBeatNs), frame);
      }
      lastBeatNs = beat;
      stalled = false;
      reported = 0;
    }
    // A backgrounded app has no frames to wait for; count from when it came back.
    if (aurora_is_suspended()) {
      lastBeatNs = now;
      if (stalled) {
        stalled = false;
        reported = 0;
      }
      continue;
    }
    // Driver warnings (shader compiles and the like) show up without any hang.
    if (!warningsDumped && firstFrameNs != 0 && now - firstFrameNs > 10 * kSecondNs) {
      warningsDumped = true;
      PortWatchdog::LogcatDump("10 s after the first frame", true);
    }
    const int64_t stalledNs = now - lastBeatNs;
    if (reported < kReportCount && stalledNs >= kReportsNs[reported]) {
      ReportStall(reported, stalledNs, frame);
      stalled = true;
      ++reported;
    }
  }
}
} // namespace

void Heartbeat(unsigned frame) {
  static const bool sEnabled = port::EnvFlag("MP_WATCHDOG", true);
  if (!sEnabled) {
    return;
  }
  aurora::phase::set(aurora::phase::Main, "frame start");
  sFrame.store(frame, std::memory_order_relaxed);
  sBeatNs.store(NowNs(), std::memory_order_relaxed);
  static std::once_flag sStarted;
  std::call_once(sStarted, [] {
#if defined(_WIN32)
    sMainThread.store(0);
#else
#if defined(__APPLE__)
    // PortCrash::RequestStack takes the pthread_t here (no tgkill on macOS).
    sMainThread.store(reinterpret_cast< long >(pthread_self()));
#else
    sMainThread.store(static_cast< long >(syscall(SYS_gettid)));
#endif
#endif
    std::thread(Watch).detach();
  });
  static const int sTestStall = port::EnvInt("MP_WATCHDOG_TEST_STALL", 0);
  if (sTestStall > 0 && frame == static_cast< unsigned >(sTestStall)) {
    aurora::phase::set(aurora::phase::Main, "test stall (MP_WATCHDOG_TEST_STALL)");
    SDL_Delay(8000);
  }
  static const int sTestThrow = port::EnvInt("MP_WATCHDOG_TEST_THROW", 0);
  if (sTestThrow > 0 && frame == static_cast< unsigned >(sTestThrow)) {
    throw std::runtime_error("test exception (MP_WATCHDOG_TEST_THROW)");
  }
}

#if defined(__ANDROID__)
namespace {
std::atomic< bool > sDumping{false};

// Lines our own writers already put in the log (PortLog, stdout/stderr, Aurora's
// callback, SDL), and Android UI chatter that repeats on every launch, in
// `logcat -v brief` form: "I/tag( pid): message".
bool IsSkipped(const std::string& line) {
  static const char* const kTags[] = {"stdout",       "aurora",        "metroidprime",
                                      "SDL",          "touchpad",      "MetroidPrime",
                                      "InsetsSource", "InteractionJankMonitor"};
  if (line.size() < 3 || line[1] != '/') {
    return false;
  }
  for (const char* tag : kTags) {
    const size_t length = std::char_traits< char >::length(tag);
    if (line.compare(2, length, tag) == 0 && length + 2 < line.size() &&
        (line[2 + length] == '(' || line[2 + length] == '/' || line[2 + length] == ' ')) {
      return true;
    }
  }
  return false;
}
} // namespace

void LogcatDump(const char* why, bool warningsOnly) {
  if (sDumping.exchange(true)) {
    return;
  }
  std::string command = "logcat -d -t 300 -v brief --pid=" + std::to_string(getpid());
  if (warningsOnly) {
    command += " *:W";
  }
  command += " 2>&1";
  // Repeats of a line (same tag and message) are counted on its first copy.
  std::vector< std::string > kept;
  std::vector< int > repeats;
  std::unordered_map< std::string, size_t > seen;
  if (FILE* pipe = popen(command.c_str(), "r")) {
    char buffer[2048];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
      std::string line(buffer);
      while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
      }
      if (line.empty() || line[0] == '-' || IsSkipped(line)) {
        continue;
      }
      std::string key = line;
      if (const size_t open = key.find('('), close = key.find("): "); open != std::string::npos &&
                                                                       close != std::string::npos && open < close) {
        key.erase(open, close + 1 - open);
      }
      if (const auto [it, added] = seen.try_emplace(std::move(key), kept.size()); !added) {
        ++repeats[it->second];
        continue;
      }
      kept.push_back(std::move(line));
      repeats.push_back(1);
    }
    pclose(pipe);
  }
  constexpr size_t kMaxLines = 150;
  const size_t first = kept.size() > kMaxLines ? kept.size() - kMaxLines : 0;
  PortLogFile::Write("logcat", (std::string("--- ") + why + ": " + std::to_string(kept.size() - first) +
                                (warningsOnly ? " distinct warning lines" : " distinct lines from other tags") + " ---")
                                   .c_str());
  for (size_t i = first; i < kept.size(); ++i) {
    if (repeats[i] > 1) {
      kept[i] += " (x" + std::to_string(repeats[i]) + ")";
    }
    PortLogFile::Write("logcat", kept[i].c_str());
  }
  sDumping.store(false);
}
#else
void LogcatDump(const char*, bool) {}
#endif

} // namespace PortWatchdog
