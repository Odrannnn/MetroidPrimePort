#include "port_livesplit.h"

#include "port_log.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace PortLiveSplit {
namespace {

#if defined(_WIN32)
using Socket = SOCKET;
const Socket kNoSocket = INVALID_SOCKET;
void CloseSocket(Socket s) { closesocket(s); }
int PollSockets(pollfd* fds, int count, int timeoutMs) { return WSAPoll(fds, count, timeoutMs); }
bool SetNonBlocking(Socket s) {
  u_long enabled = 1;
  return ioctlsocket(s, FIONBIO, &enabled) == 0;
}
bool ConnectPending() {
  const int error = WSAGetLastError();
  return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
}
bool WouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
bool InitSockets() {
  static const bool ok = [] {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return ok;
}
const int kSendFlags = 0;
#else
using Socket = int;
const Socket kNoSocket = -1;
void CloseSocket(Socket s) { close(s); }
int PollSockets(pollfd* fds, int count, int timeoutMs) { return poll(fds, count, timeoutMs); }
bool SetNonBlocking(Socket s) {
  const int flags = fcntl(s, F_GETFL, 0);
  return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
bool ConnectPending() { return errno == EINPROGRESS; }
bool WouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
bool InitSockets() { return true; }
// A closed peer must fail the send, not raise SIGPIPE. macOS has no MSG_NOSIGNAL:
// its sockets take SO_NOSIGPIPE instead (NoSigpipe, after socket()).
#ifdef MSG_NOSIGNAL
const int kSendFlags = MSG_NOSIGNAL;
#else
const int kSendFlags = 0;
#endif
#ifdef SO_NOSIGPIPE
void NoSigpipe(Socket s) {
  const int on = 1;
  setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
}
#else
void NoSigpipe(Socket) {}
#endif
#endif

const int kConnectTimeoutMs = 2000;
const auto kRetryDelay = std::chrono::seconds(3);

// "host", "host:port" or "[v6]:port"; the port defaults to LiveSplit's 16834.
bool ParseAddress(const std::string& address, std::string& host, std::string& port) {
  host = address;
  port = "16834";
  if (!host.empty() && host.front() == '[') {
    const size_t close = host.find(']');
    if (close == std::string::npos)
      return false;
    if (close + 1 < host.size()) {
      if (host[close + 1] != ':')
        return false;
      port = host.substr(close + 2);
    }
    host = host.substr(1, close - 1);
  } else {
    const size_t colon = host.find(':');
    if (colon != std::string::npos && host.find(':', colon + 1) == std::string::npos) {
      port = host.substr(colon + 1);
      host.resize(colon);
    }
  }
  return !host.empty() && !port.empty();
}

Socket Connect(const std::string& address, std::string& error) {
  std::string host, port;
  if (!ParseAddress(address, host, port)) {
    error = "bad address \"" + address + "\"";
    return kNoSocket;
  }
  if (!InitSockets()) {
    error = "WSAStartup failed";
    return kNoSocket;
  }
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* results = nullptr;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &results) != 0 || results == nullptr) {
    error = "cannot resolve " + host;
    return kNoSocket;
  }
  error = "connection refused (is LiveSplit's TCP server started?)";
  Socket s = kNoSocket;
  for (addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
    s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s == kNoSocket)
      continue;
    NoSigpipe(s);
    if (!SetNonBlocking(s)) {
      CloseSocket(s);
      s = kNoSocket;
      continue;
    }
    if (connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0)
      break;
    if (ConnectPending()) {
      pollfd pfd{};
      pfd.fd = s;
      pfd.events = POLLOUT;
      if (PollSockets(&pfd, 1, kConnectTimeoutMs) == 1 && (pfd.revents & POLLOUT) != 0) {
        int soError = 0;
        socklen_t length = sizeof(soError);
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &length) == 0 &&
            soError == 0)
          break;
      } else {
        error = "connection timed out";
      }
    }
    CloseSocket(s);
    s = kNoSocket;
  }
  freeaddrinfo(results);
  if (s != kNoSocket) {
    const int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    error.clear();
  }
  return s;
}

bool SendAll(Socket s, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    const auto n = send(s, data.data() + sent, static_cast<int>(data.size() - sent), kSendFlags);
    if (n > 0) {
      sent += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && WouldBlock()) {
      pollfd pfd{};
      pfd.fd = s;
      pfd.events = POLLOUT;
      if (PollSockets(&pfd, 1, 1000) == 1)
        continue;
    }
    return false;
  }
  return true;
}

// False once the peer has closed or the socket failed. Replies (LiveSplit only
// answers queries, which the port does not send) are read and dropped.
bool StillOpen(Socket s) {
  for (;;) {
    pollfd pfd{};
    pfd.fd = s;
    pfd.events = POLLIN;
    if (PollSockets(&pfd, 1, 0) <= 0)
      return true;
    if ((pfd.revents & (POLLERR | POLLNVAL)) != 0)
      return false;
    char buffer[256];
    const auto n = recv(s, buffer, sizeof(buffer), 0);
    if (n == 0)
      return false;
    if (n < 0)
      return WouldBlock();
  }
}

struct Runtime {
  std::mutex mutex;
  std::condition_variable wake;
  bool enabled = false;
  std::string address;
  unsigned generation = 0; // bumped by every Configure that changes the target
  bool stop = false;
  bool workerDone = false;
  std::deque<std::string> queue;
  std::string error;
  std::thread worker;
  std::atomic<int> status{kStatus_Off};
  std::atomic<bool> splitUpgrades{true};
  Tracker tracker; // game thread only

  void SetStatus(EStatus value, const std::string& message) {
    status.store(value, std::memory_order_release);
    error = message; // under mutex
  }

  void Run() {
    std::unique_lock<std::mutex> lock(mutex);
    while (!stop) {
      if (!enabled) {
        SetStatus(kStatus_Off, "");
        wake.wait(lock, [this] { return stop || enabled; });
        continue;
      }
      const unsigned target = generation;
      const std::string to = address;
      SetStatus(kStatus_Connecting, "");
      lock.unlock();
      std::string failure;
      const Socket s = Connect(to, failure);
      lock.lock();
      if (s == kNoSocket) {
        SetStatus(kStatus_Failed, failure);
        wake.wait_for(lock, kRetryDelay, [&] { return stop || generation != target; });
        continue;
      }
      PortLog::Write("[livesplit] connected to %s\n", to.c_str());
      SetStatus(kStatus_Connected, "");
      queue.clear(); // commands made before the connection are stale
      std::string lost;
      while (!stop && generation == target) {
        wake.wait_for(lock, std::chrono::milliseconds(250),
                      [&] { return stop || generation != target || !queue.empty(); });
        std::string batch;
        for (const std::string& line : queue)
          batch += line + "\r\n";
        queue.clear();
        lock.unlock();
        const bool ok = (batch.empty() || SendAll(s, batch)) && StillOpen(s);
        lock.lock();
        if (!ok) {
          lost = "connection lost";
          break;
        }
      }
      CloseSocket(s);
      PortLog::Write("[livesplit] disconnected from %s\n", to.c_str());
      if (!lost.empty()) {
        SetStatus(kStatus_Failed, lost);
        wake.wait_for(lock, kRetryDelay, [&] { return stop || generation != target; });
      }
    }
    SetStatus(kStatus_Off, "");
    workerDone = true;
    wake.notify_all();
  }

  // Called at exit. A worker stuck resolving a name is left behind rather than
  // holding up the exit (the Runtime is never freed).
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

  void Emit(const std::vector<std::string>& lines) {
    if (lines.empty() || status.load(std::memory_order_acquire) != kStatus_Connected)
      return;
    {
      std::lock_guard<std::mutex> lock(mutex);
      for (const std::string& line : lines)
        queue.push_back(line);
    }
    wake.notify_all();
  }
};

Runtime& GetRuntime() {
  // Leaked on purpose (see Runtime::Shutdown); the stopper runs it with the
  // other static destructors.
  static Runtime* runtime = new Runtime;
  static struct Stopper {
    Runtime* runtime;
    ~Stopper() { runtime->Shutdown(); }
  } stopper{runtime};
  return *runtime;
}

} // namespace

EStatus Status() { return static_cast<EStatus>(GetRuntime().status.load(std::memory_order_acquire)); }

std::string LastError() {
  Runtime& rt = GetRuntime();
  std::lock_guard<std::mutex> lock(rt.mutex);
  return rt.error;
}

void Configure(bool enabled, const std::string& address, bool splitUpgrades) {
  Runtime& rt = GetRuntime();
  rt.splitUpgrades.store(splitUpgrades, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(rt.mutex);
    if (rt.enabled == enabled && rt.address == address)
      return;
    rt.enabled = enabled;
    rt.address = address;
    ++rt.generation;
    if (enabled && !rt.worker.joinable() && !rt.stop)
      rt.worker = std::thread([&rt] { rt.Run(); });
  }
  rt.wake.notify_all();
}

void GameTick(double igt, const int* capacities) {
  Runtime& rt = GetRuntime();
  std::vector<std::string> lines;
  rt.tracker.Tick(igt, capacities, rt.splitUpgrades.load(std::memory_order_relaxed), lines);
  rt.Emit(lines);
}

void GameEnd(double igt) {
  Runtime& rt = GetRuntime();
  std::vector<std::string> lines;
  rt.tracker.EndGame(igt, lines);
  rt.Emit(lines);
}

void GameSessionEnd(double igt) {
  Runtime& rt = GetRuntime();
  std::vector<std::string> lines;
  rt.tracker.EndSession(igt, lines);
  rt.Emit(lines);
}

void SendRaw(const std::string& line) { GetRuntime().Emit({line}); }

} // namespace PortLiveSplit
