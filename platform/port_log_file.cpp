#include "port_log_file.h"
#include "port_env.h"

#include "port_log_redact.h"
#include "port_paths.h"
#include "port_watchdog.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <fcntl.h>
#include <condition_variable>
#include <io.h>
#include <mutex>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#if !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
#if defined(__ANDROID__)
#include <android/log.h>
#include <thread>

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_system.h>
#elif !defined(_WIN32)
#include <csignal>
#include <poll.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#endif

namespace PortLogFile {
namespace {
bool sActive = false;
// Whether this process holds the log's instance lock (TakeInstanceLock).
bool sInstanceLock = false;

// Holds an advisory lock on "<log>.lock" for the life of the process; false when
// another live instance already holds it (two instances on one user folder). A
// lock that cannot be made at all counts as free, so a read-only folder keeps
// the old behaviour.
bool TakeInstanceLock(const std::filesystem::path& file) {
  std::filesystem::path lock = file;
  lock += ".lock";
#if defined(_WIN32)
  // Share mode 0: a second open fails with a sharing violation while the first
  // is alive.
  const HANDLE handle = CreateFileW(lock.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
  return handle != INVALID_HANDLE_VALUE || GetLastError() != ERROR_SHARING_VIOLATION;
#else
  const int fd = open(lock.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) {
    return true;
  }
  if (flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK) {
    close(fd);
    return false;
  }
  return true; // fd stays open: the lock lasts until the process exits
#endif
}

#if !defined(__ANDROID__)
// What the file copy takes out of the log (port_log_redact.h). Android's paths
// name the package, not the person, so it has none.
PortLogRedact::Rules MakeRules() {
  PortLogRedact::Rules rules;
#if defined(_WIN32)
  rules.Add(std::getenv("USERPROFILE"), "%USERPROFILE%", true);
  if (const char* name = std::getenv("USERNAME")) {
    // Another drive's Users folder, or a profile moved off C:.
    const std::string part = std::string(":\\Users\\") + name;
    rules.Add(part.c_str(), ":\\Users\\<user>", true);
  }
#else
  const char* home = port::HomeFolder();
  rules.Add(home, "~", false);
  if (home != nullptr) {
    if (char* real = realpath(home, nullptr)) {
      rules.Add(real, "~", false);
      std::free(real);
    }
  }
  const char* name = std::getenv("USER");
  if (const passwd* entry = getpwuid(getuid()); entry != nullptr && entry->pw_name != nullptr) {
    name = entry->pw_name;
  }
  if (name != nullptr && name[0] != '\0') {
    // Removable drives and other disks are mounted under the account's name.
    const std::string runMedia = std::string("/run/media/") + name;
    const std::string media = std::string("/media/") + name;
    rules.Add(runMedia.c_str(), "/run/media/<user>", false);
    rules.Add(media.c_str(), "/media/<user>", false);
  }
#endif
  return rules;
}

// Collects output into lines and passes each on with the rules applied, so a
// path split across two reads is still matched. Allocation-free (see Copy).
struct RedactedLines {
  using Sink = void (*)(void* context, const char* data, size_t size);
  const PortLogRedact::Rules* rules = nullptr;
  Sink sink = nullptr;
  void* context = nullptr;
  char line[8192];
  size_t used = 0;
  char out[sizeof(line) * 2];

  void Feed(const char* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
      line[used++] = data[i];
      if (data[i] == '\n' || used == sizeof(line)) {
        Flush();
      }
    }
  }
  void Flush() {
    if (used != 0) {
      sink(context, out, PortLogRedact::Apply(*rules, line, used, out, sizeof(out)));
      used = 0;
    }
  }
};
#endif

#if !defined(_WIN32)
void WriteAll(int fd, const char* data, ssize_t size) {
  while (size > 0) {
    const ssize_t n = write(fd, data, static_cast< size_t >(size));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;
    }
    data += n;
    size -= n;
  }
}
#endif

#if !defined(_WIN32) && !defined(__ANDROID__)
// Writes to the terminal, dropping what it will not take. The terminal copy is
// opened non-blocking (or is a socket, sent to with MSG_DONTWAIT), so a terminal
// that stops reading - Ctrl+S, a pager left waiting - loses its copy of the
// output instead of filling the pipe and stalling every write the game makes. A
// reader that is only slow gets kTerminalWaitMs to make room; one that has not
// is skipped without waiting until it takes something again.
constexpr int kTerminalWaitMs = 100;
bool sTerminalStalled = false;

void WriteTerminal(int fd, const char* data, ssize_t size, bool socket) {
  int waits = 0; // without progress, in case "ready" is not followed by room
  while (size > 0) {
    const ssize_t n = socket ? send(fd, data, static_cast< size_t >(size), MSG_DONTWAIT)
                             : write(fd, data, static_cast< size_t >(size));
    if (n >= 0) {
      sTerminalStalled = false;
      waits = 0;
      data += n;
      size -= n;
      continue;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      return;
    }
    pollfd descriptor{};
    descriptor.fd = fd;
    descriptor.events = POLLOUT;
    if (++waits > 8 || poll(&descriptor, 1, sTerminalStalled ? 0 : kTerminalWaitMs) <= 0) {
      sTerminalStalled = true;
      return; // this much never reaches the terminal; the file has it
    }
  }
}

// A descriptor for the terminal copy of the log, of its own so that making it
// non-blocking leaves the shell's descriptor (which a tty's stdin, stdout and
// stderr usually all share) alone. `socket` is set when it is a socket, which
// is written per call without blocking instead. Falls back to a plain,
// blocking dup when no such descriptor can be had.
int OpenTerminal(bool& socket) {
  socket = false;
  struct stat info {};
  if (fstat(STDERR_FILENO, &info) == 0) {
    const char* reopen = nullptr;
    if (isatty(STDERR_FILENO)) {
      reopen = ttyname(STDERR_FILENO);
#if defined(__linux__)
    } else if (S_ISFIFO(info.st_mode)) {
      reopen = "/proc/self/fd/2"; // a new open of the same pipe
#endif
    } else if (S_ISSOCK(info.st_mode)) {
      socket = true;
    }
    if (reopen != nullptr) {
      const int fd = open(reopen, O_WRONLY | O_NOCTTY | O_NONBLOCK);
      if (fd >= 0) {
        return fd;
      }
    }
  }
  return dup(STDERR_FILENO);
}

// The copying process: pipe -> terminal + file until every writer has gone. Only
// async-signal-safe calls, since the game may already have threads when it forks.
// The terminal gets the output as it is; the file, which is what gets shared,
// with the user's name taken out.
PortLogRedact::Rules sRules; // made before the fork
RedactedLines sFileLines;

void ToFile(void* context, const char* data, size_t size) {
  WriteAll(*static_cast< const int* >(context), data, static_cast< ssize_t >(size));
}

[[noreturn]] void Copy(int in, int terminal, bool terminalSocket, int file, int maxFd) {
  for (int fd = 3; fd < maxFd; ++fd) {
    if (fd != in && fd != terminal && fd != file) {
      close(fd);
    }
  }
  // Ctrl+C in the terminal reaches this process too; it must keep draining until
  // the game has printed its last line. Aurora renames the game's main thread
  // "Main thread", which left this process the only one named after the
  // executable: a kill by name (pkill metroid_prime_port) hit it instead of the
  // game, which then died of SIGPIPE on its next line. It has its own name and
  // shrugs off SIGTERM/SIGHUP; it still exits when the game closes the pipe.
#if defined(__linux__)
  prctl(PR_SET_NAME, "mp-log-copier", 0, 0, 0);
#endif
  signal(SIGINT, SIG_IGN);
  signal(SIGQUIT, SIG_IGN);
  signal(SIGTERM, SIG_IGN);
  signal(SIGHUP, SIG_IGN);
  signal(SIGPIPE, SIG_IGN);
  sFileLines.rules = &sRules;
  sFileLines.sink = ToFile;
  sFileLines.context = &file;
  char buffer[8192];
  for (;;) {
    const ssize_t n = read(in, buffer, sizeof(buffer));
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      break;
    }
    WriteTerminal(terminal, buffer, n, terminalSocket);
    sFileLines.Feed(buffer, static_cast< size_t >(n)); // the file keeps everything
  }
  sFileLines.Flush(); // a last line without its newline, as before an abort
  _exit(0);
}
#endif

#if defined(__ANDROID__)
// The open log file. Every line is one write() of its own, so whatever was logged
// before a crash or a kill by Android is already in the file.
int sFile = -1;
// The copy in shared storage (Documents/MetroidPrime), -1 when it could not be opened.
int sShared = -1;
std::string sSharedOpened; // its path, which may be a numbered fallback
SDL_LogOutputFunction sSdlDefault = nullptr;
void* sSdlDefaultData = nullptr;

void SdlLog(void* userdata, int category, SDL_LogPriority priority, const char* message) {
  if (sSdlDefault != nullptr) {
    sSdlDefault(sSdlDefaultData, category, priority, message);
  }
  Write("SDL", message);
}

// Nothing reads a process's stdout and stderr on Android, so printf output (the
// game's own reports among it) went nowhere. While the log runs both go through
// a pipe into logcat (tag "stdout") and the file.
void CopyStdio(int in) {
  char line[2048];
  size_t used = 0;
  char buffer[4096];
  for (;;) {
    const ssize_t n = read(in, buffer, sizeof(buffer));
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return;
    }
    for (ssize_t i = 0; i < n; ++i) {
      const char c = buffer[i];
      if (c != '\n' && used + 1 < sizeof(line)) {
        line[used++] = c;
        continue;
      }
      if (c != '\n') {
        line[used++] = c;
      }
      line[used] = '\0';
      __android_log_write(ANDROID_LOG_INFO, "stdout", line);
      Write("stdout", line);
      used = 0;
    }
  }
}

// Opens the shared copy, keeping the previous run's as .old.log like the main
// file. A file left by an earlier install of the app is not this install's to
// write (scoped storage), so numbered names are tried after it.
int OpenShared(const std::string& path, std::string& opened) {
  std::error_code ec;
  const std::filesystem::path file(path);
  std::filesystem::create_directories(file.parent_path(), ec);
  // Only the instance that owns the main log rotates this copy and truncates it;
  // a second instance takes one of the numbered names below instead.
  int fd = -1;
  if (sInstanceLock) {
    if (std::filesystem::exists(file, ec)) {
      std::filesystem::path old = file;
      old.replace_extension(".old.log");
      std::filesystem::rename(file, old, ec);
    }
    fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
    opened = path;
  }
  for (int n = 2; fd < 0 && n <= 5; ++n) {
    std::filesystem::path other = file;
    other.replace_filename(file.stem().string() + " (" + std::to_string(n) + ").log");
    fd = open(other.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
    if (fd >= 0) {
      opened = other.string();
    }
  }
  return fd;
}
#endif
} // namespace

std::string Path() {
#if defined(__ANDROID__)
  // The app's private folder is out of reach without root, so unless the data
  // was moved to shared storage the log goes to the app's external folder,
  // Android/data/org.metroidprime.port/files, which a USB file transfer shows.
  if (!PortPaths::IsPortable()) {
    const char* external = SDL_GetAndroidExternalStoragePath();
    if (external == nullptr || external[0] == '\0') {
      return {};
    }
    std::string folder = external;
    if (folder.back() != '/') {
      folder += '/';
    }
    return folder + "metroid_prime_port.log";
  }
#endif
  const std::string& folder = PortPaths::UserFolder();
  return folder.empty() ? std::string() : folder + "metroid_prime_port.log";
}

std::string SharedPath() {
#if defined(__ANDROID__)
  // A moved data folder is already in shared storage, with the log in it.
  if (PortPaths::IsPortable()) {
    return {};
  }
  // The external folder is <storage>/Android/data/<package>/files; Documents is
  // beside Android. Android 11 and later let any app create files there without a
  // permission, and the phone's own file manager shows them.
  const char* external = SDL_GetAndroidExternalStoragePath();
  if (external == nullptr) {
    return {};
  }
  const std::string folder = external;
  const size_t at = folder.find("/Android/data/");
  if (at == std::string::npos) {
    return {};
  }
  return folder.substr(0, at) + "/Documents/MetroidPrime/metroid_prime_port.log";
#else
  return {};
#endif
}

bool Active() { return sActive; }

#if defined(_WIN32)
namespace {
void ToHandle(void* context, const char* data, size_t size) {
  while (size > 0) {
    DWORD wrote = 0;
    if (!WriteFile(static_cast< HANDLE >(context), data, static_cast< DWORD >(size), &wrote, nullptr) || wrote == 0) {
      return;
    }
    data += wrote;
    size -= wrote;
  }
}

HANDLE ParseHandle(const char* text) {
  return reinterpret_cast< HANDLE >(static_cast< uintptr_t >(std::strtoull(text, nullptr, 10)));
}

// The terminal copy, written on a thread of its own: a console that stops
// reading (a selection in QuickEdit mode) blocks its writer, and that must not
// fill the pipe and stall the game. What does not fit in kTerminalQueue while it
// is blocked is dropped; the file has it.
constexpr size_t kTerminalQueue = 1 << 20;
struct Terminal {
  HANDLE handle = nullptr;
  std::mutex mutex;
  std::condition_variable ready;
  std::string queued;
  bool done = false;

  void Push(const char* data, size_t size) {
    {
      std::lock_guard< std::mutex > lock(mutex);
      if (queued.size() + size > kTerminalQueue) {
        return;
      }
      queued.append(data, size);
    }
    ready.notify_one();
  }
  void Run() {
    std::string writing;
    for (;;) {
      {
        std::unique_lock< std::mutex > lock(mutex);
        ready.wait(lock, [this] { return done || !queued.empty(); });
        if (queued.empty()) {
          return;
        }
        writing.swap(queued);
      }
      ToHandle(handle, writing.data(), writing.size());
      writing.clear();
    }
  }
};
} // namespace

void AttachParentConsole() {
  const HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
  if ((error != nullptr && error != INVALID_HANDLE_VALUE) || !AttachConsole(ATTACH_PARENT_PROCESS)) {
    return;
  }
  FILE* reopened = nullptr;
  freopen_s(&reopened, "CONOUT$", "w", stdout);
  freopen_s(&reopened, "CONOUT$", "w", stderr);
  SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast< HANDLE >(_get_osfhandle(_fileno(stdout))));
  SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast< HANDLE >(_get_osfhandle(_fileno(stderr))));
}

int RunCopy(const char* pipe, const char* file, const char* terminal) {
  const HANDLE in = ParseHandle(pipe);
  const HANDLE out = ParseHandle(file);
  static PortLogRedact::Rules rules;
  rules = MakeRules();
  static RedactedLines lines;
  lines.rules = &rules;
  lines.sink = ToHandle;
  lines.context = out;
  static Terminal console;
  console.handle = terminal != nullptr ? ParseHandle(terminal) : nullptr;
  std::thread consoleThread;
  if (console.handle != nullptr && console.handle != INVALID_HANDLE_VALUE) {
    consoleThread = std::thread([] { console.Run(); });
  }
  char buffer[8192];
  DWORD n = 0;
  // Ends when every writer has gone: the game, and anything it started that
  // inherited its output.
  while (ReadFile(in, buffer, sizeof(buffer), &n, nullptr) && n > 0) {
    if (consoleThread.joinable()) {
      console.Push(buffer, n); // as it is, like the Linux terminal copy
    }
    lines.Feed(buffer, n);
  }
  lines.Flush();
  if (consoleThread.joinable()) {
    {
      std::lock_guard< std::mutex > lock(console.mutex);
      console.done = true;
    }
    console.ready.notify_one();
    consoleThread.join();
  }
  return 0;
}
#endif

void Write(const char* tag, const char* text) {
#if defined(__ANDROID__)
  if (sFile < 0 || text == nullptr) {
    return;
  }
  // A lost device is fatal and the line before the abort; the drivers' last words are
  // in logcat. The dump writes through here under its own tag, which is not "aurora".
  const bool deviceLost = std::strcmp(tag, "aurora") == 0 && std::strstr(text, "Device lost") != nullptr;
  char line[2304];
  size_t size = static_cast< size_t >(std::snprintf(line, sizeof(line), "%s: %s", tag, text));
  if (size >= sizeof(line)) {
    size = sizeof(line) - 1;
  }
  if (size > 0 && line[size - 1] != '\n') {
    if (size + 1 >= sizeof(line)) {
      --size;
    }
    line[size++] = '\n';
  }
  WriteAll(sFile, line, static_cast< ssize_t >(size));
  if (sShared >= 0) {
    WriteAll(sShared, line, static_cast< ssize_t >(size));
  }
  if (deviceLost) {
    PortWatchdog::LogcatDump("device lost", false);
  }
#else
  (void)tag;
  (void)text;
#endif
}

#if !defined(_WIN32)
void WriteRaw(const char* data, size_t size) {
#if defined(__ANDROID__)
  // stderr only reaches the file through a thread, which a crash stops.
  if (sFile >= 0) {
    WriteAll(sFile, data, static_cast< ssize_t >(size));
    if (sShared >= 0) {
      WriteAll(sShared, data, static_cast< ssize_t >(size));
    }
    return;
  }
#endif
  WriteAll(STDERR_FILENO, data, static_cast< ssize_t >(size));
}
#endif

bool Start() {
  if (sActive) {
    return true;
  }
  std::string path = Path();
  if (path.empty()) {
    return false;
  }
  std::error_code ec;
  // UTF-8, as Path() is: a narrow path is the ANSI code page on Windows.
  std::filesystem::path file(std::u8string(path.begin(), path.end()));
  // On a first start the user folder may not exist yet, and that run is the one
  // most likely to need a log.
  std::filesystem::create_directories(file.parent_path(), ec);
  sInstanceLock = TakeInstanceLock(file);
  if (!sInstanceLock) {
    // A second live instance on the same user folder must not rotate the first
    // one's log away: it writes to a log named after its pid instead.
    std::filesystem::path other = file;
    other.replace_extension("." + std::to_string(
#if defined(_WIN32)
                                           GetCurrentProcessId()
#else
                                           getpid()
#endif
                                               ) + ".log");
    file = other;
    const std::u8string text = other.u8string();
    path.assign(text.begin(), text.end());
  }
  if (std::filesystem::exists(file, ec)) {
    std::filesystem::path old = file;
    old.replace_extension(".old.log");
    std::filesystem::rename(file, old, ec);
  }
  std::fflush(stdout);
  std::fflush(stderr);
  char started[64] = "";
  const std::time_t now = std::time(nullptr);
  if (const std::tm* local = std::localtime(&now)) {
    std::strftime(started, sizeof(started), "%Y-%m-%d %H:%M:%S", local);
  }
#if defined(_WIN32)
  // As on Linux, a process of its own copies a pipe into the file, so it can take
  // the user's name out and still drain what was written before a crash. It is
  // this program again, started with --log-copy (RunCopy).
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  const HANDLE out = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (out == INVALID_HANDLE_VALUE) {
    return false;
  }
  char header[128];
  const int headerSize = std::snprintf(header, sizeof(header), "metroid_prime_port log, started %s\n", started);
  DWORD wrote = 0;
  WriteFile(out, header, static_cast< DWORD >(headerSize), &wrote, nullptr);
  HANDLE readEnd = nullptr;
  HANDLE writeEnd = nullptr;
  if (!CreatePipe(&readEnd, &writeEnd, &inherit, 1 << 16)) {
    CloseHandle(out);
    return false;
  }
  SetHandleInformation(writeEnd, HANDLE_FLAG_INHERIT, 0); // else the copier never sees the end
  // Where stderr went before (a console, or a file a script redirected it to),
  // so the output still shows up there as well as in the log.
  HANDLE terminal = nullptr;
  const HANDLE oldError = GetStdHandle(STD_ERROR_HANDLE);
  if (oldError != nullptr && oldError != INVALID_HANDLE_VALUE &&
      !DuplicateHandle(GetCurrentProcess(), oldError, GetCurrentProcess(), &terminal, 0, TRUE,
                       DUPLICATE_SAME_ACCESS)) {
    terminal = nullptr;
  }
  wchar_t exe[4096];
  const DWORD exeSize = GetModuleFileNameW(nullptr, exe, 4096);
  std::wstring command = L"\"" + std::wstring(exe, exeSize) + L"\" --log-copy " +
                         std::to_wstring(reinterpret_cast< uintptr_t >(readEnd)) + L" " +
                         std::to_wstring(reinterpret_cast< uintptr_t >(out)) + L" " +
                         std::to_wstring(reinterpret_cast< uintptr_t >(terminal));
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  const bool launched = exeSize > 0 && exeSize < 4096 &&
                       CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                      DETACHED_PROCESS, nullptr, nullptr, &startup, &process);
  CloseHandle(readEnd);
  CloseHandle(out);
  if (terminal != nullptr) {
    CloseHandle(terminal);
  }
  if (!launched) {
    CloseHandle(writeEnd);
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  const int pipeFd = _open_osfhandle(reinterpret_cast< intptr_t >(writeEnd), _O_WRONLY | _O_BINARY);
  if (pipeFd < 0) {
    CloseHandle(writeEnd);
    return false;
  }
  // A GUI program may start without a console, with no descriptor behind the
  // streams to redirect; give them one first.
  for (FILE* stream : {stdout, stderr}) {
    if (_fileno(stream) < 0) {
      FILE* reopened = nullptr;
      freopen_s(&reopened, "NUL", "w", stream);
    }
    _dup2(pipeFd, _fileno(stream));
  }
  _close(pipeFd);
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  // Code that writes to the standard handles rather than the C streams.
  SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast< HANDLE >(_get_osfhandle(_fileno(stdout))));
  SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast< HANDLE >(_get_osfhandle(_fileno(stderr))));
#elif defined(__ANDROID__)
  const int out = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
  if (out < 0) {
    return false;
  }
  char header[128];
  const int headerSize = std::snprintf(header, sizeof(header), "metroid_prime_port log, started %s\n", started);
  WriteAll(out, header, headerSize);
  sFile = out;
  if (const std::string shared = SharedPath(); !shared.empty()) {
    sShared = OpenShared(shared, sSharedOpened);
    if (sShared >= 0) {
      WriteAll(sShared, header, headerSize);
    }
  }
  SDL_GetLogOutputFunction(&sSdlDefault, &sSdlDefaultData);
  SDL_SetLogOutputFunction(SdlLog, nullptr);
  int fds[2];
  if (pipe(fds) == 0) {
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::thread(CopyStdio, fds[0]).detach();
  }
#else
  const int out = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (out < 0) {
    return false;
  }
  char header[128];
  const int headerSize = std::snprintf(header, sizeof(header), "metroid_prime_port log, started %s\n", started);
  WriteAll(out, header, headerSize);
  int fds[2];
  bool terminalSocket = false;
  const int terminal = OpenTerminal(terminalSocket);
  if (terminal < 0 || pipe(fds) != 0) {
    close(out);
    if (terminal >= 0) {
      close(terminal);
    }
    return false;
  }
  sRules = MakeRules();
  long maxFd = sysconf(_SC_OPEN_MAX);
  maxFd = maxFd < 256 ? 256 : maxFd > 65536 ? 65536 : maxFd;
  const pid_t child = fork();
  if (child == 0) {
    close(fds[1]);
    Copy(fds[0], terminal, terminalSocket, out, static_cast< int >(maxFd));
  }
  close(fds[0]);
  close(out);
  close(terminal);
  if (child < 0) {
    close(fds[1]);
    return false;
  }
  dup2(fds[1], STDOUT_FILENO);
  dup2(fds[1], STDERR_FILENO);
  close(fds[1]);
  // Should the copier die anyway, a print must fail with EPIPE, not kill the game.
  signal(SIGPIPE, SIG_IGN);
  // stdout to a pipe is fully buffered, and abort() flushes nothing: one line at a
  // time keeps the last messages before a crash.
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
#endif
  sActive = true;
  std::fprintf(stderr, "port: writing the log to %s\n", path.c_str());
#if defined(__ANDROID__)
  if (const std::string shared = SharedPath(); !shared.empty()) {
    if (sShared >= 0) {
      std::fprintf(stderr, "port: and a copy to %s\n", sSharedOpened.c_str());
    } else {
      std::fprintf(stderr, "port: cannot write a copy to %s\n", shared.c_str());
    }
  }
#endif
  return true;
}

} // namespace PortLogFile
