#include "port_memory_log.h"

#include "port_log.h"

#if defined(__linux__)
#include <aurora/gfx.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#endif

#if defined(__ANDROID__)
#include <SDL3/SDL_system.h>
#include <jni.h>

#include <string>
#endif

namespace PortMemoryLog {
#if defined(__linux__)
namespace {

constexpr uint64_t kSampleNs = 1000000000ull;
constexpr uint64_t kPeriodNs = 30 * kSampleNs;
constexpr int64_t kProcessJumpKb = 64 * 1024;
constexpr int64_t kSystemJumpKb = 128 * 1024;

// Resident size of this process, in KiB; -1 when /proc can't say.
int64_t ProcessKb() {
  FILE* file = std::fopen("/proc/self/statm", "r");
  if (file == nullptr) {
    return -1;
  }
  long long size = 0;
  long long resident = 0;
  const int read = std::fscanf(file, "%lld %lld", &size, &resident);
  std::fclose(file);
  return read == 2 ? resident * (sysconf(_SC_PAGESIZE) / 1024) : -1;
}

struct SystemMemory {
  int64_t totalKb = -1;
  int64_t availableKb = -1;
};

SystemMemory ReadSystem() {
  SystemMemory memory;
  FILE* file = std::fopen("/proc/meminfo", "r");
  if (file == nullptr) {
    return memory;
  }
  char line[128];
  while (std::fgets(line, sizeof(line), file) != nullptr) {
    if (std::strncmp(line, "MemTotal:", 9) == 0) {
      memory.totalKb = std::strtoll(line + 9, nullptr, 10);
    } else if (std::strncmp(line, "MemAvailable:", 13) == 0) {
      memory.availableKb = std::strtoll(line + 13, nullptr, 10);
    }
  }
  std::fclose(file);
  return memory;
}

long long Mib(int64_t kb) { return kb < 0 ? -1 : static_cast< long long >(kb / 1024); }

void WriteLine(int64_t processKb, const SystemMemory& system, bool textures) {
  char gpu[128] = "";
  if (textures) {
    AuroraTextureStats stats{};
    aurora_get_texture_stats(&stats);
    std::snprintf(gpu, sizeof(gpu), ", textures %u (%llu MiB), targets %u (%llu MiB)", stats.count[0],
                  static_cast< unsigned long long >(stats.bytes[0] >> 20), stats.count[1],
                  static_cast< unsigned long long >(stats.bytes[1] >> 20));
  }
  PortLog::Write("port: memory: process %lld MiB, system %lld of %lld MiB available%s\n", Mib(processKb),
                 Mib(system.availableKb), Mib(system.totalKb), gpu);
}

#if defined(__ANDROID__)
// MetroidPrimeActivity.exitReport(): built in onCreate, before the log runs.
void LogPreviousExit() {
  JNIEnv* env = static_cast< JNIEnv* >(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast< jobject >(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    return;
  }
  std::string report;
  jclass cls = env->GetObjectClass(activity);
  jmethodID method = cls != nullptr ? env->GetStaticMethodID(cls, "exitReport", "()Ljava/lang/String;") : nullptr;
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    method = nullptr;
  }
  if (method != nullptr) {
    auto text = static_cast< jstring >(env->CallStaticObjectMethod(cls, method));
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      text = nullptr;
    }
    if (text != nullptr) {
      if (const char* chars = env->GetStringUTFChars(text, nullptr); chars != nullptr) {
        report = chars;
        env->ReleaseStringUTFChars(text, chars);
      }
      env->DeleteLocalRef(text);
    }
  }
  if (cls != nullptr) {
    env->DeleteLocalRef(cls);
  }
  env->DeleteLocalRef(activity);
  if (!report.empty()) {
    PortLog::Write("%s\n", report.c_str());
  }
}
#endif

} // namespace

void LogStartup() {
#if defined(__ANDROID__)
  LogPreviousExit();
#endif
  WriteLine(ProcessKb(), ReadSystem(), false);
}

void Tick(uint64_t nowNs) {
  static uint64_t sLastSampleNs = 0;
  static uint64_t sLastLineNs = 0;
  static int64_t sLineProcessKb = -1;
  static int64_t sLineAvailableKb = -1;
  if (sLastSampleNs != 0 && nowNs - sLastSampleNs < kSampleNs) {
    return;
  }
  sLastSampleNs = nowNs;
  const int64_t processKb = ProcessKb();
  const SystemMemory system = ReadSystem();
  const bool due = sLastLineNs == 0 || nowNs - sLastLineNs >= kPeriodNs ||
                   std::llabs(processKb - sLineProcessKb) >= kProcessJumpKb ||
                   std::llabs(system.availableKb - sLineAvailableKb) >= kSystemJumpKb;
  if (!due) {
    return;
  }
  sLastLineNs = nowNs;
  sLineProcessKb = processKb;
  sLineAvailableKb = system.availableKb;
  WriteLine(processKb, system, true);
}
#else
void LogStartup() {}
void Tick(uint64_t) {}
#endif
} // namespace PortMemoryLog

#if defined(__ANDROID__)
// Android's memory warnings (onTrimMemory/onLowMemory), from the UI thread. Android 14
// no longer sends the "running low" levels to a visible app; older versions do.
extern "C" JNIEXPORT void JNICALL Java_org_metroidprime_port_MetroidPrimeActivity_nativeMemoryWarning(
    JNIEnv*, jclass, jint level, jlong availableKb, jlong thresholdKb, jboolean low) {
  const char* name = "unknown";
  switch (level) {
  case -1: name = "low memory"; break;
  case 5: name = "running moderate"; break;
  case 10: name = "running low"; break;
  case 15: name = "running critical"; break;
  case 20: name = "ui hidden"; break;
  case 40: name = "background"; break;
  case 60: name = "moderate"; break;
  case 80: name = "complete"; break;
  default: break;
  }
  PortLog::Write("port: memory warning: %s (level %d), process %lld MiB, system %lld MiB available, "
                 "low-memory threshold %lld MiB%s\n",
                 name, static_cast< int >(level), PortMemoryLog::Mib(PortMemoryLog::ProcessKb()),
                 static_cast< long long >(availableKb / 1024), static_cast< long long >(thresholdKb / 1024),
                 low ? " (system is low on memory)" : "");
}
#endif
