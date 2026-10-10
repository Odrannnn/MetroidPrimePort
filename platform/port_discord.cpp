#include "port_discord.h"

#include "port_json.h"
#include "port_log.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif !defined(__ANDROID__)
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace PortDiscord {
namespace {
#if !defined(_WIN32) && defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0; // macOS: SO_NOSIGPIPE on the socket instead
#endif

const auto kRetryDelay = std::chrono::seconds(5);
// Discord allows about five activity updates per 20 seconds.
const auto kMinSendInterval = std::chrono::seconds(4);
const auto kHandshakeTimeout = std::chrono::seconds(5);

#if defined(__ANDROID__)

struct Pipe {
  bool Open(std::string& error) {
    error = "not available on Android";
    return false;
  }
  bool Write(const std::string&) { return false; }
  bool Read(std::string&, int) { return false; }
  void Close() {}
  long Pid() const { return 0; }
};

#elif defined(_WIN32)

struct Pipe {
  HANDLE handle = INVALID_HANDLE_VALUE;

  bool Open(std::string& error) {
    for (const std::string& path : SocketCandidates([](const char*) -> const char* { return nullptr; })) {
      handle = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0,
                           nullptr);
      if (handle != INVALID_HANDLE_VALUE)
        return true;
    }
    error = "Discord is not running";
    return false;
  }
  bool Write(const std::string& data) {
    DWORD written = 0;
    return WriteFile(handle, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
           written == data.size();
  }
  // Appends whatever arrives within timeoutMs; false once the pipe broke.
  bool Read(std::string& buffer, int timeoutMs) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
      DWORD available = 0;
      if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr))
        return false;
      if (available > 0) {
        std::string chunk(available, '\0');
        DWORD got = 0;
        if (!ReadFile(handle, chunk.data(), available, &got, nullptr))
          return false;
        buffer.append(chunk.data(), got);
        return true;
      }
      if (std::chrono::steady_clock::now() >= until)
        return true;
      Sleep(20);
    }
  }
  void Close() {
    if (handle != INVALID_HANDLE_VALUE)
      CloseHandle(handle);
    handle = INVALID_HANDLE_VALUE;
  }
  long Pid() const { return static_cast<long>(GetCurrentProcessId()); }
};

#else

struct Pipe {
  int fd = -1;

  bool Open(std::string& error) {
    for (const std::string& path : SocketCandidates([](const char* name) { return std::getenv(name); })) {
      sockaddr_un address{};
      if (path.size() >= sizeof(address.sun_path))
        continue;
      address.sun_family = AF_UNIX;
      std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
      fd = socket(AF_UNIX, SOCK_STREAM, 0);
      if (fd < 0)
        break;
#ifdef SO_NOSIGPIPE
      {
        const int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
      }
#endif
      if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0)
          fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        return true;
      }
      close(fd);
      fd = -1;
    }
    error = "Discord is not running";
    return false;
  }
  bool Write(const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
      const ssize_t n = send(fd, data.data() + sent, data.size() - sent, kSendFlags);
      if (n > 0) {
        sent += static_cast<size_t>(n);
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        pollfd pfd{fd, POLLOUT, 0};
        if (poll(&pfd, 1, 1000) == 1)
          continue;
      }
      return false;
    }
    return true;
  }
  bool Read(std::string& buffer, int timeoutMs) {
    pollfd pfd{fd, POLLIN, 0};
    const int ready = poll(&pfd, 1, timeoutMs);
    if (ready <= 0)
      return ready == 0 || errno == EINTR;
    const size_t before = buffer.size();
    for (;;) {
      char chunk[4096];
      const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
      if (n > 0) {
        buffer.append(chunk, static_cast<size_t>(n));
        continue;
      }
      // A peer that closes right after its last frame (a refused handshake's
      // CLOSE) still gets that frame read; the next call sees the end.
      if (n == 0)
        return buffer.size() > before;
      return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
    }
  }
  void Close() {
    if (fd >= 0)
      close(fd);
    fd = -1;
  }
  long Pid() const { return static_cast<long>(getpid()); }
};

#endif

// A DISPATCH ERROR's or CLOSE's message.
std::string MessageOf(const std::string& payload) {
  PortJson::Value value;
  size_t offset = 0;
  const char* reason = nullptr;
  if (!PortJson::Parse(payload, value, offset, &reason))
    return "bad reply from Discord";
  if (const PortJson::Value* data = value.Find("data"))
    if (data->IsObject())
      return data->StringOr("message", "error");
  return value.StringOr("message", "closed by Discord");
}

bool IsEvent(const std::string& payload, const char* event) {
  PortJson::Value value;
  size_t offset = 0;
  const char* reason = nullptr;
  return PortJson::Parse(payload, value, offset, &reason) && value.StringOr("evt") == event;
}

struct Runtime {
  std::mutex mutex;
  std::condition_variable wake;
  bool enabled = false;
  std::string appId;
  unsigned generation = 0; // bumped by every Configure that changes the target
  bool stop = false;
  bool workerDone = false;
  std::string error;
  std::thread worker;
  std::atomic<int> status{kStatus_Off};
  std::atomic<bool> active{false}; // enabled with an id, for the game tick
  Presence want;                   // under mutex
  bool inGame = false;             // game thread only
  int64_t gameStart = 0;           // game thread only

  void SetStatus(EStatus value, const std::string& message) {
    status.store(value, std::memory_order_release);
    error = message; // under mutex
  }

  // Waits for the next frame, answering pings. False on a broken stream.
  bool NextFrame(Pipe& pipe, std::string& buffer, Frame& frame, int timeoutMs) {
    bool bad = false;
    for (;;) {
      if (TakeFrame(buffer, frame, bad)) {
        if (frame.op == kOp_Ping) {
          if (!pipe.Write(EncodeFrame(kOp_Pong, frame.payload)))
            return false;
          continue;
        }
        return true;
      }
      if (bad)
        return false;
      const size_t before = buffer.size();
      if (!pipe.Read(buffer, timeoutMs))
        return false;
      if (buffer.size() == before) {
        frame.op = ~0u; // timed out
        return true;
      }
    }
  }

  // Connects and handshakes; on failure sets `failure`.
  bool Handshake(Pipe& pipe, const std::string& id, std::string& buffer, std::string& failure) {
    if (!pipe.Open(failure))
      return false;
    if (!pipe.Write(EncodeFrame(kOp_Handshake, HandshakePayload(id)))) {
      failure = "handshake failed";
      pipe.Close();
      return false;
    }
    const auto until = std::chrono::steady_clock::now() + kHandshakeTimeout;
    while (std::chrono::steady_clock::now() < until) {
      Frame frame;
      if (!NextFrame(pipe, buffer, frame, 250))
        break;
      if (frame.op == kOp_Close) {
        failure = MessageOf(frame.payload);
        pipe.Close();
        return false;
      }
      if (frame.op == kOp_Frame && IsEvent(frame.payload, "READY"))
        return true;
    }
    failure = "no reply from Discord";
    pipe.Close();
    return false;
  }

  void Run() {
    std::unique_lock<std::mutex> lock(mutex);
    unsigned nonce = 0;
    while (!stop) {
      if (!enabled || appId.empty()) {
        SetStatus(enabled ? kStatus_Failed : kStatus_Off,
                  enabled ? "set the Discord application id" : "");
        const unsigned seen = generation;
        wake.wait(lock, [&] { return stop || generation != seen; });
        continue;
      }
      const unsigned target = generation;
      const std::string id = appId;
      SetStatus(kStatus_Connecting, "");
      lock.unlock();
      Pipe pipe;
      std::string buffer, failure;
      const bool ok = Handshake(pipe, id, buffer, failure);
      lock.lock();
      if (!ok) {
        SetStatus(kStatus_Failed, failure);
        wake.wait_for(lock, kRetryDelay, [&] { return stop || generation != target; });
        continue;
      }
      PortLog::Write("[discord] connected\n");
      SetStatus(kStatus_Connected, "");
      bool sentAny = false;
      Presence sent;
      auto lastSend = std::chrono::steady_clock::now() - kMinSendInterval;
      std::string lost;
      while (!stop && generation == target) {
        const auto now = std::chrono::steady_clock::now();
        std::string frameOut;
        if ((!sentAny || want != sent) && now - lastSend >= kMinSendInterval) {
          sent = want;
          sentAny = true;
          lastSend = now;
          frameOut = EncodeFrame(kOp_Frame, ActivityPayload(pipe.Pid(), sent, std::to_string(++nonce)));
        }
        lock.unlock();
        bool alive = frameOut.empty() || pipe.Write(frameOut);
        Frame frame;
        std::string replyError;
        while (alive) {
          alive = NextFrame(pipe, buffer, frame, 250);
          if (!alive || frame.op == ~0u)
            break;
          if (frame.op == kOp_Close) {
            lost = MessageOf(frame.payload);
            alive = false;
          } else if (frame.op == kOp_Frame && IsEvent(frame.payload, "ERROR")) {
            replyError = MessageOf(frame.payload);
            PortLog::Write("[discord] %s\n", replyError.c_str());
          }
        }
        lock.lock();
        if (!replyError.empty())
          error = replyError;
        if (!alive) {
          if (lost.empty())
            lost = "connection lost";
          break;
        }
      }
      if (lost.empty() && sentAny) {
        // Turned off or retargeted: take the activity down before leaving.
        lock.unlock();
        pipe.Write(EncodeFrame(kOp_Frame, ClearPayload(pipe.Pid(), std::to_string(++nonce))));
        lock.lock();
      }
      pipe.Close();
      PortLog::Write("[discord] disconnected%s%s\n", lost.empty() ? "" : ": ", lost.c_str());
      if (!lost.empty()) {
        SetStatus(kStatus_Failed, lost);
        wake.wait_for(lock, kRetryDelay, [&] { return stop || generation != target; });
      }
    }
    SetStatus(kStatus_Off, "");
    workerDone = true;
    wake.notify_all();
  }

  // Called at exit; see PortLiveSplit's Runtime::Shutdown.
  void Shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stop = true;
    }
    wake.notify_all();
    if (!worker.joinable())
      return;
    bool done;
    {
      std::unique_lock<std::mutex> lock(mutex);
      done = wake.wait_for(lock, std::chrono::seconds(2), [this] { return workerDone; });
    }
    if (done)
      worker.join();
    else
      worker.detach();
  }

  void Want(const Presence& presence) {
    std::lock_guard<std::mutex> lock(mutex);
    want = presence;
  }
};

Runtime& GetRuntime() {
  static Runtime* runtime = [] {
    Runtime* rt = new Runtime;
    rt->want.details = "In the menus";
    return rt;
  }();
  static struct Stopper {
    Runtime* runtime;
    ~Stopper() { runtime->Shutdown(); }
  } stopper{runtime};
  return *runtime;
}

} // namespace

bool Supported() {
#if defined(__ANDROID__)
  return false;
#else
  return true;
#endif
}

EStatus Status() { return static_cast<EStatus>(GetRuntime().status.load(std::memory_order_acquire)); }

std::string LastError() {
  Runtime& rt = GetRuntime();
  std::lock_guard<std::mutex> lock(rt.mutex);
  return rt.error;
}

void Configure(bool enabled, const std::string& appId) {
  if (!Supported())
    enabled = false;
  Runtime& rt = GetRuntime();
  rt.active.store(enabled && !appId.empty(), std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(rt.mutex);
    if (rt.enabled == enabled && rt.appId == appId)
      return;
    rt.enabled = enabled;
    rt.appId = appId;
    ++rt.generation;
    if (enabled && !rt.worker.joinable() && !rt.stop)
      rt.worker = std::thread([&rt] { rt.Run(); });
  }
  rt.wake.notify_all();
}

bool Enabled() { return GetRuntime().active.load(std::memory_order_relaxed); }

void SetMenu() {
  Runtime& rt = GetRuntime();
  rt.inGame = false;
  Presence presence;
  presence.details = "In the menus";
  rt.Want(presence);
}

void SetGame(const GameInfo& info) {
  Runtime& rt = GetRuntime();
  if (!rt.inGame) {
    rt.inGame = true;
    rt.gameStart = static_cast<int64_t>(std::time(nullptr));
  }
  rt.Want(GamePresence(info, rt.gameStart));
}

std::string CurrentText() {
  Runtime& rt = GetRuntime();
  std::lock_guard<std::mutex> lock(rt.mutex);
  return rt.want.state.empty() ? rt.want.details : rt.want.details + " / " + rt.want.state;
}

} // namespace PortDiscord
