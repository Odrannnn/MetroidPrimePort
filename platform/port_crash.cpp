#include "port_crash.h"

#include "port_build_info.h"
#include "port_log_file.h"
#include "port_paths.h"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <exception>
#include <string>
#include <typeinfo>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// dbghelp.h needs windows.h first.
#include <dbghelp.h>

#include <string>
#else
#include <cxxabi.h>
#include <dlfcn.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <pthread.h>
#include <sys/ucontext.h> // <ucontext.h> refuses to compile there without _XOPEN_SOURCE
#else
#include <sys/syscall.h>
#include <ucontext.h>
#endif
#include <unwind.h>
#endif

namespace PortCrash {
namespace {
// Set by the first crash; a second one (another thread, or the report itself
// crashing) skips the report and goes straight on to the system.
std::atomic< bool > sCrashing{false};

constexpr int kMaxFrames = 64;

// One line of the report, made without the heap or stdio, which the crash may
// have left broken.
struct Line {
  char text[1024];
  size_t used = 0;

  Line& Add(const char* s) {
    while (s != nullptr && *s != '\0' && used + 1 < sizeof(text)) {
      text[used++] = *s++;
    }
    return *this;
  }
  Line& Hex(uintptr_t value) {
    char digits[2 * sizeof(value)];
    int n = 0;
    do {
      digits[n++] = "0123456789abcdef"[value & 15];
      value >>= 4;
    } while (value != 0);
    Add("0x");
    while (n > 0 && used + 1 < sizeof(text)) {
      text[used++] = digits[--n];
    }
    return *this;
  }
  Line& Dec(unsigned long value) {
    char digits[24];
    int n = 0;
    do {
      digits[n++] = static_cast< char >('0' + value % 10);
      value /= 10;
    } while (value != 0);
    while (n > 0 && used + 1 < sizeof(text)) {
      text[used++] = digits[--n];
    }
    return *this;
  }
};

const char* BaseName(const char* path) {
  const char* name = path;
  for (const char* c = path; *c != '\0'; ++c) {
    if (*c == '/' || *c == '\\') {
      name = c + 1;
    }
  }
  return name;
}

void CrashTest() {
  const char* test = std::getenv("MP_CRASH_TEST");
  if (test == nullptr) {
    return;
  }
  if (std::strcmp(test, "segv") == 0) {
    volatile int* volatile nowhere = nullptr;
    *nowhere = 1;
  } else if (std::strcmp(test, "abort") == 0) {
    std::abort();
  }
}

#if defined(_WIN32)
// Prepared at Install: the crash may have left the heap unusable.
std::wstring sDumpPath;
std::string sDumpPathUtf8;
std::wstring sExeFolder;

void Emit(Line& line) {
  line.text[line.used++] = '\n';
  // The log's pipe while the log runs; the copier drains it after the game has gone.
  const HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
  DWORD wrote = 0;
  if (error != nullptr && error != INVALID_HANDLE_VALUE) {
    WriteFile(error, line.text, static_cast< DWORD >(line.used), &wrote, nullptr);
  }
}

const char* CodeName(DWORD code) {
  switch (code) {
  case EXCEPTION_ACCESS_VIOLATION:
    return "access violation";
  case EXCEPTION_STACK_OVERFLOW:
    return "stack overflow";
  case EXCEPTION_ILLEGAL_INSTRUCTION:
    return "illegal instruction";
  case EXCEPTION_PRIV_INSTRUCTION:
    return "privileged instruction";
  case EXCEPTION_INT_DIVIDE_BY_ZERO:
    return "integer divide by zero";
  case EXCEPTION_IN_PAGE_ERROR:
    return "in-page error (the file behind the memory could not be read)";
  case EXCEPTION_BREAKPOINT:
    return "breakpoint";
  case 0xC0000374:
    return "heap corruption";
  case 0xE06D7363:
    return "uncaught C++ exception";
  default:
    return "exception";
  }
}

// "module+0xoffset", the form a symbolizer takes.
void Describe(Line& line, uintptr_t pc) {
  HMODULE module = nullptr;
  char path[MAX_PATH];
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast< LPCSTR >(pc), &module) &&
      GetModuleFileNameA(module, path, MAX_PATH) > 0) {
    line.Add(BaseName(path)).Add("+").Hex(pc - reinterpret_cast< uintptr_t >(module));
  } else {
    line.Hex(pc);
  }
}

// Function and line, when the module's PDB was found.
void AddSymbol(Line& line, HANDLE process, uintptr_t pc) {
  alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256];
  auto* symbol = reinterpret_cast< SYMBOL_INFO* >(storage);
  std::memset(storage, 0, sizeof(storage));
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = 255;
  DWORD64 displacement = 0;
  if (SymFromAddr(process, pc, &displacement, symbol)) {
    line.Add(" ").Add(symbol->Name).Add("+").Hex(static_cast< uintptr_t >(displacement));
    IMAGEHLP_LINE64 source{};
    source.SizeOfStruct = sizeof(source);
    DWORD column = 0;
    if (SymGetLineFromAddr64(process, pc, &column, &source)) {
      line.Add(" (").Add(BaseName(source.FileName)).Add(":").Dec(source.LineNumber).Add(")");
    }
  }
}

struct Report {
  EXCEPTION_POINTERS* pointers;
  DWORD threadId;
  const char* what; // instead of the exception code, for abort and the like
};

DWORD WINAPI WriteReport(void* argument) {
  const Report& report = *static_cast< const Report* >(argument);
  const EXCEPTION_RECORD& record = *report.pointers->ExceptionRecord;
  Line header;
  header.Add("port: crashed: ");
  if (report.what != nullptr) {
    header.Add(report.what);
  } else {
    header.Add(CodeName(record.ExceptionCode)).Add(" (").Hex(record.ExceptionCode).Add(")");
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
      const ULONG_PTR kind = record.ExceptionInformation[0];
      header.Add(kind == 0 ? " reading " : kind == 1 ? " writing " : " executing ")
          .Hex(static_cast< uintptr_t >(record.ExceptionInformation[1]));
    }
  }
  Emit(header);
  Line at;
  at.Add("port: at ");
  Describe(at, reinterpret_cast< uintptr_t >(record.ExceptionAddress));
  Emit(at);
  Line build;
  build.Add("port: build ").Add(MP_BUILD_REVISION).Add(", thread ").Dec(report.threadId);
  Emit(build);

  const HANDLE process = GetCurrentProcess();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS |
                SYMOPT_NO_PROMPTS);
  const bool symbols = SymInitializeW(process, sExeFolder.empty() ? nullptr : sExeFolder.c_str(), TRUE) != FALSE;
  const HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, report.threadId);
  CONTEXT context = *report.pointers->ContextRecord; // StackWalk64 changes it
  STACKFRAME64 frame{};
  DWORD machine = 0;
#if defined(_M_ARM64)
  machine = IMAGE_FILE_MACHINE_ARM64;
  frame.AddrPC.Offset = context.Pc;
  frame.AddrFrame.Offset = context.Fp;
  frame.AddrStack.Offset = context.Sp;
#else
  machine = IMAGE_FILE_MACHINE_AMD64;
  frame.AddrPC.Offset = context.Rip;
  frame.AddrFrame.Offset = context.Rbp;
  frame.AddrStack.Offset = context.Rsp;
#endif
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Mode = AddrModeFlat;
  Line title;
  Emit(title.Add("port: stack:"));
  for (int n = 0; n < kMaxFrames; ++n) {
    if (!StackWalk64(machine, process, thread != nullptr ? thread : GetCurrentThread(), &frame, &context, nullptr,
                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
        frame.AddrPC.Offset == 0) {
      break;
    }
    const auto pc = static_cast< uintptr_t >(frame.AddrPC.Offset);
    Line line;
    line.Add("port:   #").Dec(static_cast< unsigned long >(n)).Add(" ");
    Describe(line, pc);
    if (symbols) {
      // A return address is the instruction after the call; look up the call.
      AddSymbol(line, process, n == 0 ? pc : pc - 1);
    }
    Emit(line);
  }
  if (thread != nullptr) {
    CloseHandle(thread);
  }

  if (!sDumpPath.empty()) {
    const HANDLE file = CreateFileW(sDumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      MINIDUMP_EXCEPTION_INFORMATION exception{report.threadId, report.pointers, FALSE};
      const bool wrote = MiniDumpWriteDump(process, GetCurrentProcessId(), file,
                                           // Indirectly referenced memory: the bytes around each pointer on
                                           // the stacks, so a heap block being freed (and its header) is in
                                           // the dump without taking the whole heap.
                                           static_cast< MINIDUMP_TYPE >(MiniDumpNormal | MiniDumpWithThreadInfo |
                                                                        MiniDumpWithUnloadedModules |
                                                                        MiniDumpWithIndirectlyReferencedMemory),
                                           &exception, nullptr, nullptr) != FALSE;
      CloseHandle(file);
      Line line;
      Emit(line.Add(wrote ? "port: crash dump written to " : "port: cannot write a crash dump to ")
               .Add(sDumpPathUtf8.c_str()));
    }
  }
  if (symbols) {
    SymCleanup(process);
  }
  return 0;
}

void RunReport(EXCEPTION_POINTERS* pointers, const char* what) {
  Report report{pointers, GetCurrentThreadId(), what};
  // On a thread of its own, with a stack of its own: the crashed thread may have
  // run out of stack, and StackWalk64 must not walk the thread it runs on.
  const HANDLE thread = CreateThread(nullptr, 1 << 20, WriteReport, &report, 0, nullptr);
  if (thread != nullptr) {
    WaitForSingleObject(thread, 60000);
    CloseHandle(thread);
  }
}

LONG WINAPI OnException(EXCEPTION_POINTERS* pointers) {
  if (!sCrashing.exchange(true)) {
    RunReport(pointers, nullptr);
  }
  return EXCEPTION_CONTINUE_SEARCH; // on to Windows Error Reporting
}

// For the ways out that raise no exception: the report starts at the caller.
void ReportHere(const char* what) {
  if (sCrashing.exchange(true)) {
    return;
  }
  CONTEXT context{};
  RtlCaptureContext(&context);
  EXCEPTION_RECORD record{};
#if defined(_M_ARM64)
  record.ExceptionAddress = reinterpret_cast< void* >(context.Pc);
#else
  record.ExceptionAddress = reinterpret_cast< void* >(context.Rip);
#endif
  EXCEPTION_POINTERS pointers{&record, &context};
  RunReport(&pointers, what);
}

// abort(), which std::terminate and Aurora's FATAL end in. The C runtime ends
// the process once this returns.
void OnAbort(int) { ReportHere("abort() was called"); }

void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
  ReportHere("invalid parameter passed to a C runtime function");
  std::abort(); // what the default handler does, in effect
}
#else
// The handlers that were there before, which the signal goes on to: the default
// one (a core dump), or on Android the one that writes the tombstone.
constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGTRAP};
constexpr int kSignalCount = sizeof(kSignals) / sizeof(kSignals[0]);
// What RequestStack sends; not a crash, so it has no previous handler to chain to.
constexpr int kStackSignal = SIGUSR2;
struct sigaction sPrevious[kSignalCount];

void Emit(Line& line) {
  line.text[line.used++] = '\n';
  PortLogFile::WriteRaw(line.text, line.used);
}

const char* SignalName(int signal) {
  switch (signal) {
  case SIGSEGV:
    return "SIGSEGV (bad memory access)";
  case SIGBUS:
    return "SIGBUS (bad memory access)";
  case SIGFPE:
    return "SIGFPE (arithmetic error)";
  case SIGILL:
    return "SIGILL (illegal instruction)";
  case SIGABRT:
    return "SIGABRT (abort() was called)";
  case SIGTRAP:
    return "SIGTRAP (trap)";
  default:
    return "signal";
  }
}

// "module+0xoffset (symbol+0xoffset)". The offset is from the module's load
// address, which is what addr2line -e <module> takes. Symbols show only for
// exported functions (Android's system libraries, mostly).
void Describe(Line& line, uintptr_t pc) {
  Dl_info info{};
  if (dladdr(reinterpret_cast< void* >(pc), &info) != 0 && info.dli_fname != nullptr) {
    line.Add(BaseName(info.dli_fname)).Add("+").Hex(pc - reinterpret_cast< uintptr_t >(info.dli_fbase));
    if (info.dli_sname != nullptr) {
      line.Add(" (").Add(info.dli_sname).Add("+").Hex(pc - reinterpret_cast< uintptr_t >(info.dli_saddr)).Add(")");
    }
  } else {
    line.Hex(pc);
  }
}

uintptr_t FaultAddress(void* context) {
  const auto* user = static_cast< const ucontext_t* >(context);
#if defined(__APPLE__) && defined(__aarch64__)
  return static_cast< uintptr_t >(user->uc_mcontext->__ss.__pc);
#elif defined(__APPLE__) && defined(__x86_64__)
  return static_cast< uintptr_t >(user->uc_mcontext->__ss.__rip);
#elif defined(__x86_64__)
  return static_cast< uintptr_t >(user->uc_mcontext.gregs[REG_RIP]);
#elif defined(__aarch64__)
  return static_cast< uintptr_t >(user->uc_mcontext.pc);
#else
  (void)user;
  return 0;
#endif
}

struct Walk {
  const char* prefix;
  int frame;
};

_Unwind_Reason_Code OnFrame(_Unwind_Context* context, void* argument) {
  Walk& walk = *static_cast< Walk* >(argument);
  const uintptr_t pc = _Unwind_GetIP(context);
  if (pc == 0) {
    return _URC_END_OF_STACK;
  }
  Line line;
  line.Add(walk.prefix).Dec(static_cast< unsigned long >(walk.frame)).Add(" ");
  Describe(line, pc);
  Emit(line);
  return ++walk.frame < kMaxFrames ? _URC_NO_REASON : _URC_END_OF_STACK;
}

// The watchdog's request (RequestStack): runs on the thread that was asked, writes its
// stack and returns, so the game goes on. Same rules as the crash report's handler.
void OnStackSignal(int, siginfo_t*, void*) {
  const int savedErrno = errno;
  Line title;
  Emit(title.Add("watchdog: main thread stack:"));
  Walk walk{"watchdog:   #", 0};
  _Unwind_Backtrace(OnFrame, &walk);
  errno = savedErrno;
}

void OnSignal(int signal, siginfo_t* info, void* context) {
  if (!sCrashing.exchange(true)) {
    Line header;
    header.Add("port: crashed: ").Add(SignalName(signal));
    if (signal == SIGSEGV || signal == SIGBUS) {
      header.Add(" at address ").Hex(reinterpret_cast< uintptr_t >(info->si_addr));
    }
    Emit(header);
    if (const uintptr_t pc = FaultAddress(context); pc != 0) {
      Line at;
      at.Add("port: at ");
      Describe(at, pc);
      Emit(at);
    }
    Line build;
    Emit(build.Add("port: build ").Add(MP_BUILD_REVISION));
    // Starts in this handler; the frames after the signal frame are the crashed code.
    Line title;
    Emit(title.Add("port: stack:"));
    Walk walk{"port:   #", 0};
    _Unwind_Backtrace(OnFrame, &walk);
  }
  for (int i = 0; i < kSignalCount; ++i) {
    if (kSignals[i] == signal) {
      sigaction(signal, &sPrevious[i], nullptr);
    }
  }
  // A fault happens again on return, now to the previous handler; a signal that
  // was sent (abort, kill) has to be sent again.
  if (info->si_code <= 0) {
    raise(signal);
  }
}

_Unwind_Reason_Code CountFrame(_Unwind_Context*, void* argument) {
  ++*static_cast< int* >(argument);
  return _URC_NO_REASON;
}
#endif
} // namespace

void Install() {
#if defined(_WIN32)
  if (const std::string& folder = PortPaths::UserFolder(); !folder.empty()) {
    sDumpPathUtf8 = folder + "metroid_prime_port.dmp";
    const int size = MultiByteToWideChar(CP_UTF8, 0, sDumpPathUtf8.c_str(), -1, nullptr, 0);
    if (size > 0) {
      sDumpPath.resize(static_cast< size_t >(size));
      MultiByteToWideChar(CP_UTF8, 0, sDumpPathUtf8.c_str(), -1, sDumpPath.data(), size);
      sDumpPath.pop_back(); // the terminator
    }
  }
  wchar_t exe[4096];
  const DWORD exeSize = GetModuleFileNameW(nullptr, exe, 4096);
  if (exeSize > 0 && exeSize < 4096) {
    sExeFolder.assign(exe, exeSize);
    sExeFolder.erase(sExeFolder.find_last_of(L"\\/") + 1);
  }
  // Leaves room on the main thread's stack for the filter after a stack overflow.
  ULONG guarantee = 64 * 1024;
  SetThreadStackGuarantee(&guarantee);
  SetUnhandledExceptionFilter(OnException);
  std::signal(SIGABRT, OnAbort);
  _set_invalid_parameter_handler(OnInvalidParameter);
#else
  // The main thread's stack overflowing leaves no stack for the handler.
  static char alternate[64 * 1024];
  stack_t stack{};
  stack.ss_sp = alternate;
  stack.ss_size = sizeof(alternate);
  sigaltstack(&stack, nullptr);
  // The unwinder and dladdr load what they need now, not during a crash.
  int frames = 0;
  _Unwind_Backtrace(CountFrame, &frames);
  Dl_info info{};
  dladdr(reinterpret_cast< void* >(&Install), &info);
  struct sigaction stackAction {};
  stackAction.sa_sigaction = OnStackSignal;
  stackAction.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&stackAction.sa_mask);
  sigaction(kStackSignal, &stackAction, nullptr);
  struct sigaction action {};
  action.sa_sigaction = OnSignal;
  action.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&action.sa_mask);
  for (int i = 0; i < kSignalCount; ++i) {
    sigaction(kSignals[i], &action, &sPrevious[i]);
  }
#endif
  CrashTest();
}

bool RequestStack(long threadId) {
#if defined(_WIN32)
  (void)threadId;
  return false;
#else
  // A library loaded later (a GPU driver) may have taken the signal; don't run its handler.
  struct sigaction current {};
  if (threadId == 0 || sigaction(kStackSignal, nullptr, &current) != 0 || current.sa_sigaction != OnStackSignal) {
    return false;
  }
#if defined(__APPLE__)
  // macOS has no tgkill; the id the watchdog stores there is the thread's pthread_t.
  return pthread_kill(reinterpret_cast< pthread_t >(threadId), kStackSignal) == 0;
#else
  return syscall(SYS_tgkill, getpid(), static_cast< pid_t >(threadId), kStackSignal) == 0;
#endif
#endif
}

void AbortOnException(const char* where) {
  std::string what = "not a std::exception";
  try {
    throw;
  } catch (const std::exception& e) {
    what = e.what();
  } catch (...) {
  }
  const char* type = "unknown type";
#if !defined(_WIN32)
  char* demangled = nullptr;
  if (const std::type_info* info = abi::__cxa_current_exception_type(); info != nullptr) {
    int status = 0;
    demangled = abi::__cxa_demangle(info->name(), nullptr, nullptr, &status);
    type = demangled != nullptr ? demangled : info->name();
  }
#endif
  Line line;
  line.Add("port: uncaught C++ exception in ").Add(where).Add(": ").Add(type).Add(": ").Add(what.c_str());
  Emit(line);
  std::abort();
}

} // namespace PortCrash

#if defined(MP_WRAP_CXA_THROW)
// Linked with -Wl,--wrap=__cxa_throw: every throw passes through here first, so the
// log says what was thrown and from where, which nothing can tell once it is caught
// (or has unwound into a hang, as in issue #8).
extern "C" {
[[noreturn]] void __real___cxa_throw(void* object, void* type, void (*destructor)(void*));

[[noreturn]] void __wrap___cxa_throw(void* object, void* type, void (*destructor)(void*)) {
  constexpr int kThrowsLogged = 8;
  static std::atomic< int > sLogged{0};
  if (sLogged.fetch_add(1, std::memory_order_relaxed) < kThrowsLogged) {
    const char* name = static_cast< const std::type_info* >(type)->name();
    int status = 0;
    char* demangled = abi::__cxa_demangle(name, nullptr, nullptr, &status);
    PortCrash::Line line;
    line.Add("port: C++ exception thrown: ").Add(demangled != nullptr ? demangled : name);
    PortCrash::Emit(line);
    std::free(demangled);
    PortCrash::Walk walk{"port:   #", 0};
    _Unwind_Backtrace(PortCrash::OnFrame, &walk);
  }
  __real___cxa_throw(object, type, destructor);
}
}
#endif
