#include "port_ws.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <climits>
#include <cstring>
#include <mutex>
#include <random>
#include <string_view>
#include <utility>

#ifdef _WIN32
// winsock2.h drags in windows.h, whose min/max macros would break every
// std::min and std::max below. NOMINMAX has to be defined before it is read.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
// Before OpenSSL, whose headers undo wincrypt's X509_NAME and friends.
#include <wincrypt.h>
#else
#include <cerrno>
#include <fcntl.h>
// Bionic declares IPPROTO_TCP here rather than in <netdb.h>, where glibc and
// Winsock get it, so getaddrinfo's ai_protocol below needs this on Android.
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef MP_HAVE_OPENSSL
#include <filesystem>
#include <fstream>
#include <iterator>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#if !defined(_WIN32)
#include <csignal>
#include <ctime>
#include <pthread.h>
#endif
#endif

namespace PortWs {
namespace {

using Clock = std::chrono::steady_clock;

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;

bool EnsureWinsock() {
  static std::once_flag once;
  static int result = WSASYSNOTREADY;
  std::call_once(once, [] {
    WSADATA data{};
    result = WSAStartup(MAKEWORD(2, 2), &data);
  });
  return result == 0;
}

int SocketError() { return WSAGetLastError(); }
bool IsInterrupted(int error) { return error == WSAEINTR; }
bool IsWouldBlock(int error) {
  return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEALREADY;
}
void CloseNative(NativeSocket socket) {
  if (socket != kInvalidSocket)
    closesocket(socket);
}
bool SetNonBlocking(NativeSocket socket) {
  u_long enabled = 1;
  return ioctlsocket(socket, FIONBIO, &enabled) == 0;
}
int StoreSocket(NativeSocket socket) {
  if (socket == kInvalidSocket || socket > static_cast<NativeSocket>(INT_MAX))
    return -1;
  return static_cast<int>(socket);
}
NativeSocket NativeFromStored(int socket) {
  return static_cast<NativeSocket>(static_cast<unsigned int>(socket));
}
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
bool EnsureWinsock() { return true; }
int SocketError() { return errno; }
bool IsInterrupted(int error) { return error == EINTR; }
bool IsWouldBlock(int error) {
  return error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS || error == EALREADY;
}
void CloseNative(NativeSocket socket) {
  if (socket != kInvalidSocket)
    close(socket);
}
bool SetNonBlocking(NativeSocket socket) {
  const int flags = fcntl(socket, F_GETFL, 0);
  return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}
int StoreSocket(NativeSocket socket) { return socket; }
NativeSocket NativeFromStored(int socket) { return socket; }
#endif

std::string SystemError(int error) {
#ifdef _WIN32
  return "socket error " + std::to_string(error);
#else
  return std::strerror(error);
#endif
}

// A return value of 1 means ready, 0 means timed out, and -1 means error. A
// timeout of 0 or less waits indefinitely. An error or hangup on the socket
// counts as ready, so the caller's next call reports it.
int WaitReadyOnce(int storedSocket, bool readable, int timeoutMs) {
  const NativeSocket socket = NativeFromStored(storedSocket);
#ifdef _WIN32
  // Winsock's fd_set is a list of handles, not a bitmap, so select has no
  // descriptor limit here; WSAPoll is avoided because before Windows 10 2004
  // it never reported a failed connect. A failed connect is signalled in the
  // except set, not the write set, so that is watched too.
  fd_set descriptors;
  FD_ZERO(&descriptors);
  FD_SET(socket, &descriptors);
  fd_set failures;
  FD_ZERO(&failures);
  FD_SET(socket, &failures);
  timeval timeout{};
  timeval* timeoutPointer = nullptr;
  if (timeoutMs > 0) {
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
    timeoutPointer = &timeout;
  }
  const int result = select(0, readable ? &descriptors : nullptr,
                            readable ? nullptr : &descriptors, readable ? nullptr : &failures,
                            timeoutPointer);
#else
  // poll rather than select: FD_SET on a descriptor at or above FD_SETSIZE
  // (1024) writes past the fd_set, and nothing stops a process with many files
  // or sockets open from getting such a descriptor for this one.
  pollfd descriptor{};
  descriptor.fd = socket;
  descriptor.events = readable ? POLLIN : POLLOUT;
  const int result = poll(&descriptor, 1, timeoutMs > 0 ? timeoutMs : -1);
#endif
  if (result > 0)
    return 1;
  if (result == 0)
    return 0;
  return -1;
}

// WaitReadyOnce, but with a `cancel` flag the wait is cut into slices and
// ends as if timed out once the flag is set, so a stopping owner does not sit
// out a long timeout. The socket is checked before the flag, so a close frame
// still goes out when the socket is writable.
int WaitReady(int storedSocket, bool readable, int timeoutMs,
              const std::atomic<bool>* cancel) {
  if (cancel == nullptr)
    return WaitReadyOnce(storedSocket, readable, timeoutMs);
  constexpr int kSliceMs = 100;
  const bool infinite = timeoutMs <= 0;
  int left = timeoutMs;
  for (;;) {
    const int slice = infinite ? kSliceMs : std::min(left, kSliceMs);
    const int result = WaitReadyOnce(storedSocket, readable, slice);
    if (result != 0)
      return result;
    if (cancel->load(std::memory_order_acquire))
      return 0;
    if (!infinite) {
      left -= slice;
      if (left <= 0)
        return 0;
    }
  }
}

int RemainingMs(const Clock::time_point& deadline, bool infinite) {
  if (infinite)
    return 0;
  const auto remaining = deadline - Clock::now();
  if (remaining <= Clock::duration::zero())
    return -1;
  auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
  if (std::chrono::milliseconds(milliseconds) < remaining)
    ++milliseconds;
  return static_cast<int>(std::min<int64_t>(milliseconds, INT_MAX));
}

bool SendAll(int storedSocket, const std::string& data, int timeoutMs, std::string& error,
             const std::atomic<bool>* cancel) {
  const bool infinite = timeoutMs <= 0;
  const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 0));
  size_t sent = 0;
  while (sent < data.size()) {
    const int remaining = RemainingMs(deadline, infinite);
    if (remaining < 0) {
      error = "send timed out";
      return false;
    }
    const int ready = WaitReady(storedSocket, false, remaining, cancel);
    if (ready == 0) {
      error = "send timed out";
      return false;
    }
    if (ready < 0) {
      const int socketError = SocketError();
      if (IsInterrupted(socketError))
        continue;
      error = "send wait failed: " + SystemError(socketError);
      return false;
    }
    const size_t chunk = std::min<size_t>(data.size() - sent, static_cast<size_t>(INT_MAX));
#ifdef _WIN32
    const int count = ::send(NativeFromStored(storedSocket), data.data() + sent,
                             static_cast<int>(chunk), 0);
#else
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    const ssize_t count = ::send(NativeFromStored(storedSocket), data.data() + sent, chunk, flags);
#endif
    if (count > 0) {
      sent += static_cast<size_t>(count);
      continue;
    }
    if (count == 0) {
      error = "socket closed during send";
      return false;
    }
    const int socketError = SocketError();
    if (IsInterrupted(socketError) || IsWouldBlock(socketError))
      continue;
    error = "send failed: " + SystemError(socketError);
    return false;
  }
  return true;
}

bool RandomSeed(uint32_t& seed) {
  try {
    std::random_device random;
    seed = 0;
    for (unsigned i = 0; i < 4; ++i)
      seed |= static_cast<uint32_t>(random() & 0xffu) << (8 * i);
    return true;
  } catch (...) {
    return false;
  }
}

std::string Lower(std::string value) {
  for (char& c : value)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}

std::string_view Trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
    value.remove_prefix(1);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
    value.remove_suffix(1);
  return value;
}

bool HasToken(std::string value, const std::string& wanted) {
  size_t start = 0;
  while (start <= value.size()) {
    const size_t comma = value.find(',', start);
    const size_t end = comma == std::string::npos ? value.size() : comma;
    if (Lower(std::string(Trim(std::string_view(value).substr(start, end - start)))) == wanted)
      return true;
    if (comma == std::string::npos)
      break;
    start = comma + 1;
  }
  return false;
}

// A DEFLATE decoder in the style of zlib's puff: canonical Huffman codes read
// a bit at a time. Archipelago messages are at most a few MB, so speed is not
// a concern, and the port's zlib (the game's 1.1.3) is not linked into the
// WebSocket tests.
struct BitReader {
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t pos = 0;
  uint32_t bitBuffer = 0;
  int bitCount = 0;

  // Up to 16 bits, least significant first.
  bool Bits(int need, uint32_t& value) {
    uint32_t buffer = bitBuffer;
    while (bitCount < need) {
      if (pos == size)
        return false;
      buffer |= static_cast<uint32_t>(data[pos++]) << bitCount;
      bitCount += 8;
    }
    value = buffer & ((1u << need) - 1);
    bitBuffer = buffer >> need;
    bitCount -= need;
    return true;
  }
  void AlignToByte() {
    bitBuffer = 0;
    bitCount = 0;
  }
};

constexpr int kMaxCodeBits = 15;

struct Huffman {
  uint16_t count[kMaxCodeBits + 1];
  uint16_t symbol[288];
};

// False for an over-subscribed code. Incomplete codes are allowed (a single
// distance code is common); decoding an unused code then fails.
bool BuildHuffman(Huffman& huffman, const uint8_t* lengths, int symbols) {
  std::fill(std::begin(huffman.count), std::end(huffman.count), 0);
  for (int i = 0; i < symbols; ++i)
    ++huffman.count[lengths[i]];
  if (huffman.count[0] == symbols)
    return true;
  int left = 1;
  for (int length = 1; length <= kMaxCodeBits; ++length) {
    left = (left << 1) - huffman.count[length];
    if (left < 0)
      return false;
  }
  uint16_t offsets[kMaxCodeBits + 1];
  offsets[1] = 0;
  for (int length = 1; length < kMaxCodeBits; ++length)
    offsets[length + 1] = offsets[length] + huffman.count[length];
  for (int i = 0; i < symbols; ++i) {
    if (lengths[i] != 0)
      huffman.symbol[offsets[lengths[i]]++] = static_cast<uint16_t>(i);
  }
  return true;
}

int DecodeSymbol(BitReader& in, const Huffman& huffman) {
  int code = 0;
  int first = 0;
  int index = 0;
  for (int length = 1; length <= kMaxCodeBits; ++length) {
    uint32_t bit = 0;
    if (!in.Bits(1, bit))
      return -1;
    code |= static_cast<int>(bit);
    const int count = huffman.count[length];
    if (code - count < first)
      return huffman.symbol[index + (code - first)];
    index += count;
    first = (first + count) << 1;
    code <<= 1;
  }
  return -1;
}

// Decodes one Huffman-coded block's data, appending to `out`, whose bytes
// before `start` are the window that distances may reach back into.
bool InflateCodes(BitReader& in, const Huffman& literals, const Huffman& distances, std::string& out,
                  size_t start, size_t maxSize) {
  static constexpr uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                               31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static constexpr uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                               2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static constexpr uint16_t kDistanceBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                                 33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                                 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
  static constexpr uint8_t kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  for (;;) {
    int symbol = DecodeSymbol(in, literals);
    if (symbol < 0)
      return false;
    if (symbol < 256) {
      if (out.size() - start >= maxSize)
        return false;
      out.push_back(static_cast<char>(symbol));
      continue;
    }
    if (symbol == 256)
      return true;
    symbol -= 257;
    if (symbol >= 29)
      return false;
    uint32_t extra = 0;
    if (!in.Bits(kLengthExtra[symbol], extra))
      return false;
    const size_t length = kLengthBase[symbol] + extra;
    symbol = DecodeSymbol(in, distances);
    if (symbol < 0 || symbol >= 30 || !in.Bits(kDistanceExtra[symbol], extra))
      return false;
    const size_t distance = kDistanceBase[symbol] + extra;
    if (distance > out.size() || length > maxSize - (out.size() - start))
      return false;
    // The copy may overlap what it produces, so it goes byte by byte.
    const size_t from = out.size() - distance;
    for (size_t i = 0; i < length; ++i)
      out.push_back(out[from + i]);
  }
}

bool InflateStored(BitReader& in, std::string& out, size_t start, size_t maxSize) {
  in.AlignToByte();
  if (in.size - in.pos < 4)
    return false;
  const uint8_t* header = in.data + in.pos;
  const uint32_t length = header[0] | (header[1] << 8);
  const uint32_t inverse = header[2] | (header[3] << 8);
  in.pos += 4;
  if (length != (~inverse & 0xffff) || in.size - in.pos < length || length > maxSize - (out.size() - start))
    return false;
  out.append(reinterpret_cast<const char*>(in.data + in.pos), length);
  in.pos += length;
  return true;
}

bool InflateFixed(BitReader& in, std::string& out, size_t start, size_t maxSize) {
  static Huffman literals;
  static Huffman distances;
  static std::once_flag once;
  std::call_once(once, [] {
    uint8_t lengths[288];
    std::fill(lengths, lengths + 144, 8);
    std::fill(lengths + 144, lengths + 256, 9);
    std::fill(lengths + 256, lengths + 280, 7);
    std::fill(lengths + 280, lengths + 288, 8);
    BuildHuffman(literals, lengths, 288);
    std::fill(lengths, lengths + 30, 5);
    BuildHuffman(distances, lengths, 30);
  });
  return InflateCodes(in, literals, distances, out, start, maxSize);
}

bool InflateDynamic(BitReader& in, std::string& out, size_t start, size_t maxSize) {
  static constexpr uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
  uint32_t literalCount = 0, distanceCount = 0, codeCount = 0;
  if (!in.Bits(5, literalCount) || !in.Bits(5, distanceCount) || !in.Bits(4, codeCount))
    return false;
  literalCount += 257;
  distanceCount += 1;
  codeCount += 4;
  if (literalCount > 286 || distanceCount > 30)
    return false;
  uint8_t lengths[286 + 30] = {};
  for (uint32_t i = 0; i < codeCount; ++i) {
    uint32_t length = 0;
    if (!in.Bits(3, length))
      return false;
    lengths[kOrder[i]] = static_cast<uint8_t>(length);
  }
  Huffman lengthCode;
  if (!BuildHuffman(lengthCode, lengths, 19))
    return false;
  std::fill(std::begin(lengths), std::end(lengths), 0);
  const uint32_t total = literalCount + distanceCount;
  for (uint32_t index = 0; index < total;) {
    const int symbol = DecodeSymbol(in, lengthCode);
    if (symbol < 0)
      return false;
    if (symbol < 16) {
      lengths[index++] = static_cast<uint8_t>(symbol);
      continue;
    }
    uint8_t value = 0;
    uint32_t repeat = 0;
    if (symbol == 16) {
      if (index == 0 || !in.Bits(2, repeat))
        return false;
      value = lengths[index - 1];
      repeat += 3;
    } else if (symbol == 17) {
      if (!in.Bits(3, repeat))
        return false;
      repeat += 3;
    } else {
      if (!in.Bits(7, repeat))
        return false;
      repeat += 11;
    }
    if (repeat > total - index)
      return false;
    std::fill(lengths + index, lengths + index + repeat, value);
    index += repeat;
  }
  if (lengths[256] == 0)
    return false;
  Huffman literals;
  Huffman distances;
  if (!BuildHuffman(literals, lengths, static_cast<int>(literalCount)) ||
      !BuildHuffman(distances, lengths + literalCount, static_cast<int>(distanceCount)))
    return false;
  return InflateCodes(in, literals, distances, out, start, maxSize);
}

bool ValidUrlCharacter(const std::string& text, bool allowPathSpace) {
  for (unsigned char c : text) {
    if (c <= 0x20 || c == 0x7f || (!allowPathSpace && std::isspace(c)))
      return false;
  }
  return true;
}

#ifdef MP_HAVE_OPENSSL
#if !defined(_WIN32) && !defined(SO_NOSIGPIPE)
// OpenSSL writes to the socket with write(), which raises SIGPIPE when the
// server has gone away. Block it on this thread around each TLS call and
// swallow one that arrives, so a dead server is an error instead of killing
// the game. (Windows has no SIGPIPE; Apple sockets get SO_NOSIGPIPE.)
class SigpipeGuard {
public:
  SigpipeGuard() {
    sigemptyset(&mPipe);
    sigaddset(&mPipe, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &mPipe, &mPrevious);
    sigset_t pending;
    sigemptyset(&pending);
    mWasPending = sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1;
  }
  ~SigpipeGuard() {
    const int savedErrno = errno;
    sigset_t pending;
    sigemptyset(&pending);
    if (!mWasPending && sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1) {
      const timespec zero{};
      sigtimedwait(&mPipe, nullptr, &zero);
    }
    pthread_sigmask(SIG_SETMASK, &mPrevious, nullptr);
    errno = savedErrno;
  }
  SigpipeGuard(const SigpipeGuard&) = delete;
  SigpipeGuard& operator=(const SigpipeGuard&) = delete;

private:
  sigset_t mPipe;
  sigset_t mPrevious;
  bool mWasPending = false;
};
#else
struct SigpipeGuard {
  SigpipeGuard() {}
};
#endif

// Takes the oldest queued OpenSSL error as text and clears the queue.
std::string TlsQueueText() {
  const unsigned long code = ERR_get_error();
  ERR_clear_error();
  if (code == 0)
    return "unknown TLS error";
  if (const char* reason = ERR_reason_error_string(code))
    return reason;
  char text[256];
  ERR_error_string_n(code, text, sizeof(text));
  return text;
}

// Where the system trust store is when TlsOptions names none. Android's OpenSSL
// default (SSL_CTX_set_default_verify_paths) points at a compiled-in directory
// that does not exist there, and succeeds anyway having loaded nothing, so its
// CA directories are named instead. Conscrypt's APEX copy comes first: from API
// 34 it is the authoritative store, and it can drop a CA that /system still
// carries.
std::vector<std::string> DefaultCaDirs() {
#ifdef __ANDROID__
  return {"/apex/com.android.conscrypt/cacerts", "/system/etc/security/cacerts"};
#else
  return {};
#endif
}

#ifdef _WIN32
// A static OpenSSL has no trust store on Windows (its default paths point into
// the build machine), so the roots come from the system's ROOT store.
size_t LoadWindowsRoots(X509_STORE* store) {
  HCERTSTORE system = CertOpenSystemStoreW(0, L"ROOT");
  if (system == nullptr)
    return 0;
  size_t loaded = 0;
  PCCERT_CONTEXT cert = nullptr;
  while ((cert = CertEnumCertificatesInStore(system, cert)) != nullptr) {
    const unsigned char* der = cert->pbCertEncoded;
    X509* x509 = d2i_X509(nullptr, &der, static_cast<long>(cert->cbCertEncoded));
    if (x509 != nullptr) {
      if (X509_STORE_add_cert(store, x509) == 1)
        ++loaded;
      X509_free(x509);
    }
  }
  CertCloseStore(system, 0);
  ERR_clear_error(); // duplicates are reported as errors
  return loaded;
}

// Whether Windows itself trusts the chain the server sent, for a TLS server
// named `host`. Windows fetches a root it doesn't have yet (Automatic Root
// Certificates Update) only while CryptoAPI builds a chain, and browsers no
// longer do that, so the local ROOT store LoadWindowsRoots reads can lack a
// root as common as ISRG Root X1 (issue #51).
bool WindowsTrustsChain(X509_STORE_CTX* ctx, const char* host) {
  X509* leaf = X509_STORE_CTX_get0_cert(ctx);
  if (leaf == nullptr || host == nullptr || *host == '\0')
    return false;
  if (X509_check_host(leaf, host, 0, 0, nullptr) != 1)
    return false;
  auto toContext = [](X509* cert) -> PCCERT_CONTEXT {
    unsigned char* der = nullptr;
    const int size = i2d_X509(cert, &der);
    if (size <= 0)
      return nullptr;
    PCCERT_CONTEXT context = CertCreateCertificateContext(X509_ASN_ENCODING, der, static_cast<DWORD>(size));
    OPENSSL_free(der);
    return context;
  };
  PCCERT_CONTEXT leafContext = toContext(leaf);
  if (leafContext == nullptr)
    return false;
  // The server's intermediates, which CryptoAPI only knows from this store.
  HCERTSTORE sent = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr);
  if (STACK_OF(X509)* chain = X509_STORE_CTX_get0_untrusted(ctx); chain != nullptr && sent != nullptr) {
    for (int i = 0; i < sk_X509_num(chain); ++i) {
      if (PCCERT_CONTEXT context = toContext(sk_X509_value(chain, i))) {
        CertAddCertificateContextToStore(sent, context, CERT_STORE_ADD_USE_EXISTING, nullptr);
        CertFreeCertificateContext(context);
      }
    }
  }
  char serverAuth[] = szOID_PKIX_KP_SERVER_AUTH;
  LPSTR usages[] = {serverAuth};
  CERT_CHAIN_PARA chainPara{};
  chainPara.cbSize = sizeof(chainPara);
  chainPara.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
  chainPara.RequestedUsage.Usage.cUsageIdentifier = 1;
  chainPara.RequestedUsage.Usage.rgpszUsageIdentifier = usages;
  PCCERT_CHAIN_CONTEXT chainContext = nullptr;
  bool trusted = false;
  if (CertGetCertificateChain(nullptr, leafContext, nullptr, sent, &chainPara, 0, nullptr, &chainContext)) {
    // The host name was checked above, so the SSL policy checks the chain only.
    SSL_EXTRA_CERT_CHAIN_POLICY_PARA sslPara{};
    sslPara.cbSize = sizeof(sslPara);
    sslPara.dwAuthType = AUTHTYPE_SERVER;
    CERT_CHAIN_POLICY_PARA policyPara{};
    policyPara.cbSize = sizeof(policyPara);
    policyPara.pvExtraPolicyPara = &sslPara;
    CERT_CHAIN_POLICY_STATUS status{};
    status.cbSize = sizeof(status);
    trusted = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chainContext, &policyPara, &status) &&
              status.dwError == 0;
    CertFreeCertificateChain(chainContext);
  }
  if (sent != nullptr)
    CertCloseStore(sent, 0);
  CertFreeCertificateContext(leafContext);
  return trusted;
}

// OpenSSL's verification against the loaded ROOT store, and where that fails,
// Windows' own.
int VerifyWithWindowsFallback(X509_STORE_CTX* ctx, void*) {
  if (X509_verify_cert(ctx) == 1)
    return 1;
  SSL* ssl = static_cast<SSL*>(X509_STORE_CTX_get_ex_data(ctx, SSL_get_ex_data_X509_STORE_CTX_idx()));
  const char* host = ssl != nullptr ? SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name) : nullptr;
  if (!WindowsTrustsChain(ctx, host))
    return 0;
  X509_STORE_CTX_set_error(ctx, X509_V_OK);
  ERR_clear_error();
  return 1;
}
#endif

// Adds every PEM certificate in the files of `dir` to `store` and returns how
// many were added. Files are read whole and in any order, so their names do
// not matter: Android names its CA files by the old subject hash, which
// OpenSSL's hashed-directory lookup would never find. A file with no
// certificate in it is skipped, not fatal. `note` says what was found, for the
// error when no directory yields anything.
size_t LoadCaDir(X509_STORE* store, const std::string& dir, std::string& note) {
  // A CA file is a few kilobytes; anything past this is not one.
  constexpr std::uintmax_t kMaxCaFileSize = 1u << 20;
  std::error_code error;
  std::filesystem::directory_iterator entries(std::filesystem::path(dir), error);
  if (error) {
    note = "cannot open: " + error.message();
    return 0;
  }
  size_t files = 0;
  size_t loaded = 0;
  for (const std::filesystem::directory_entry& entry : entries) {
    std::error_code entryError;
    if (!entry.is_regular_file(entryError) || entry.file_size(entryError) > kMaxCaFileSize || entryError)
      continue;
    ++files;
    std::ifstream file(entry.path(), std::ios::binary);
    const std::string pem{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (bio == nullptr)
      continue;
    while (X509* certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) {
      if (X509_STORE_add_cert(store, certificate) == 1)
        ++loaded;
      X509_free(certificate);
    }
    BIO_free(bio);
    // The read that ends each file fails with "no start line"; that is not an
    // error worth keeping.
    ERR_clear_error();
  }
  note = std::to_string(files) + (files == 1 ? " file" : " files");
  return loaded;
}

// Describes a fatal SSL_get_error result. `closed` reports that the peer
// simply went away (clean close_notify or a bare TCP close).
std::string TlsFailureText(int sslError, bool& closed) {
  closed = false;
  if (sslError == SSL_ERROR_ZERO_RETURN) {
    closed = true;
    ERR_clear_error();
    return "connection closed";
  }
  if (sslError == SSL_ERROR_SYSCALL) {
    const int socketError = SocketError();
    if (ERR_peek_error() == 0 && socketError == 0) {
      closed = true;
      return "connection closed";
    }
    if (ERR_peek_error() == 0)
      return SystemError(socketError);
  }
#ifdef SSL_R_UNEXPECTED_EOF_WHILE_READING
  // OpenSSL 3 reports a server that closes without close_notify this way.
  if (sslError == SSL_ERROR_SSL && ERR_GET_REASON(ERR_peek_error()) == SSL_R_UNEXPECTED_EOF_WHILE_READING) {
    closed = true;
    ERR_clear_error();
    return "connection closed";
  }
#endif
  return TlsQueueText();
}

// SNI carries DNS names only (RFC 6066); IP literals are still checked by
// SSL_set1_host against the certificate's IP addresses.
bool IsIpLiteral(const std::string& host) {
  addrinfo hints{};
  hints.ai_flags = AI_NUMERICHOST;
  addrinfo* result = nullptr;
  const bool numeric = getaddrinfo(host.c_str(), nullptr, &hints, &result) == 0;
  if (result != nullptr)
    freeaddrinfo(result);
  return numeric;
}

bool SendAllTls(SSL* ssl, int storedSocket, const std::string& data, int timeoutMs, std::string& error,
                bool& fatal, const std::atomic<bool>* cancel) {
  const bool infinite = timeoutMs <= 0;
  const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 0));
  size_t sent = 0;
  while (sent < data.size()) {
    // A retry after WANT_READ/WANT_WRITE must repeat the same buffer and
    // length, which it does because `sent` has not moved.
    const int chunk = static_cast<int>(std::min<size_t>(data.size() - sent, static_cast<size_t>(INT_MAX)));
    int count = 0;
    int sslError = SSL_ERROR_NONE;
    {
      SigpipeGuard guard;
      ERR_clear_error();
      count = SSL_write(ssl, data.data() + sent, chunk);
      if (count <= 0)
        sslError = SSL_get_error(ssl, count);
    }
    if (count > 0) {
      sent += static_cast<size_t>(count);
      continue;
    }
    if (sslError != SSL_ERROR_WANT_READ && sslError != SSL_ERROR_WANT_WRITE) {
      bool closed = false;
      const std::string detail = TlsFailureText(sslError, closed);
      fatal = true;
      error = closed ? "socket closed during send" : "send failed: " + detail;
      return false;
    }
    const int remaining = RemainingMs(deadline, infinite);
    if (remaining < 0) {
      error = "send timed out";
      return false;
    }
    const int ready = WaitReady(storedSocket, sslError == SSL_ERROR_WANT_READ, remaining, cancel);
    if (ready == 0) {
      error = "send timed out";
      return false;
    }
    if (ready < 0) {
      const int socketError = SocketError();
      if (IsInterrupted(socketError))
        continue;
      error = "send wait failed: " + SystemError(socketError);
      return false;
    }
  }
  return true;
}
#endif

} // namespace

bool Inflater::InflateMessage(const std::string& in, std::string& out, size_t maxSize) {
  // RFC 7692 7.2.2: the sender strips the empty stored block that ends a sync
  // flush, so it goes back on before decoding.
  std::string input = in;
  input.append("\x00\x00\xff\xff", 4);
  std::string work = mKeepWindow ? std::move(mWindow) : std::string();
  mWindow.clear();
  const size_t start = work.size();
  BitReader reader;
  reader.data = reinterpret_cast<const uint8_t*>(input.data());
  reader.size = input.size();
  for (;;) {
    uint32_t last = 0;
    uint32_t type = 0;
    if (!reader.Bits(1, last) || !reader.Bits(2, type))
      return false;
    bool ok = false;
    if (type == 0)
      ok = InflateStored(reader, work, start, maxSize);
    else if (type == 1)
      ok = InflateFixed(reader, work, start, maxSize);
    else if (type == 2)
      ok = InflateDynamic(reader, work, start, maxSize);
    if (!ok)
      return false;
    // The appended empty stored block ends the message unless a final block
    // came first.
    if (last != 0 || reader.pos == reader.size)
      break;
  }
  out.assign(work, start, std::string::npos);
  if (mKeepWindow) {
    constexpr size_t kWindowSize = 32768;
    mWindow = work.size() > kWindowSize ? work.substr(work.size() - kWindowSize) : std::move(work);
  }
  return true;
}

void Inflater::SetKeepWindow(bool keep) {
  mKeepWindow = keep;
  if (!keep)
    mWindow.clear();
}

void Inflater::Reset() {
  mWindow.clear();
  mKeepWindow = true;
}

bool ParseDeflateResponse(const std::string& header, bool& deflate, bool& noContextTakeover) {
  deflate = false;
  noContextTakeover = false;
  std::string_view rest = header;
  while (!rest.empty()) {
    const size_t comma = rest.find(',');
    const std::string_view extension = Trim(rest.substr(0, comma));
    rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
    if (extension.empty())
      continue;
    // Only one extension was offered, so it may be accepted only once.
    if (deflate)
      return false;
    std::string_view params = extension;
    const size_t semicolon = params.find(';');
    if (Lower(std::string(Trim(params.substr(0, semicolon)))) != "permessage-deflate")
      return false;
    deflate = true;
    params = semicolon == std::string_view::npos ? std::string_view() : params.substr(semicolon + 1);
    while (!params.empty()) {
      const size_t next = params.find(';');
      const std::string_view param = Trim(params.substr(0, next));
      params = next == std::string_view::npos ? std::string_view() : params.substr(next + 1);
      const size_t equals = param.find('=');
      const std::string name = Lower(std::string(Trim(param.substr(0, equals))));
      std::string_view value = equals == std::string_view::npos ? std::string_view() : Trim(param.substr(equals + 1));
      if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        value = value.substr(1, value.size() - 2);
      if (name == "server_no_context_takeover" && equals == std::string_view::npos) {
        noContextTakeover = true;
      } else if (name == "client_no_context_takeover" && equals == std::string_view::npos) {
        // Client messages go out uncompressed, so this changes nothing.
      } else if (name == "server_max_window_bits" || name == "client_max_window_bits") {
        // A smaller server window only shortens back-references; the client
        // one again doesn't apply to uncompressed sends.
        unsigned bits = 0;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), bits);
        if (value.empty() || result.ec != std::errc() || result.ptr != value.data() + value.size() || bits < 8 ||
            bits > 15)
          return false;
      } else {
        return false;
      }
    }
  }
  return true;
}

bool TlsAvailable() {
#ifdef MP_HAVE_OPENSSL
  return true;
#else
  return false;
#endif
}

bool ParseUrl(const std::string& url, std::string& host, uint16_t& port, std::string& path) {
  std::string parsedHost;
  uint16_t parsedPort = 0;
  std::string parsedPath;
  bool secure = false;
  if (!ParseUrl(url, parsedHost, parsedPort, parsedPath, secure) || secure)
    return false;
  host = std::move(parsedHost);
  port = parsedPort;
  path = std::move(parsedPath);
  return true;
}

bool ParseUrl(const std::string& url, std::string& host, uint16_t& port, std::string& path,
              bool& secure) {
  constexpr std::string_view plainScheme = "ws://";
  constexpr std::string_view secureScheme = "wss://";
  bool parsedSecure = false;
  size_t authorityStart = 0;
  if (url.compare(0, plainScheme.size(), plainScheme) == 0) {
    authorityStart = plainScheme.size();
  } else if (url.compare(0, secureScheme.size(), secureScheme) == 0) {
    authorityStart = secureScheme.size();
    parsedSecure = true;
  } else {
    return false;
  }
  const size_t pathStart = url.find('/', authorityStart);
  const size_t authorityEnd = pathStart == std::string::npos ? url.size() : pathStart;
  const std::string authority = url.substr(authorityStart, authorityEnd - authorityStart);
  if (authority.empty() || authority.find('@') != std::string::npos ||
      !ValidUrlCharacter(authority, false))
    return false;

  std::string parsedHost;
  uint16_t parsedPort = parsedSecure ? 443 : 80;
  std::string_view portText;
  if (authority.front() == '[') {
    const size_t close = authority.find(']');
    if (close == std::string::npos || close == 1)
      return false;
    parsedHost = authority.substr(1, close - 1);
    const std::string_view suffix(authority.data() + close + 1, authority.size() - close - 1);
    if (!suffix.empty()) {
      if (suffix.front() != ':' || suffix.size() == 1)
        return false;
      portText = suffix.substr(1);
    }
  } else {
    if (authority.find('[') != std::string::npos || authority.find(']') != std::string::npos)
      return false;
    const size_t colon = authority.find(':');
    if (colon == std::string::npos) {
      parsedHost = authority;
    } else {
      if (authority.find(':', colon + 1) != std::string::npos || colon == 0 || colon + 1 == authority.size())
        return false;
      parsedHost = authority.substr(0, colon);
      portText = std::string_view(authority).substr(colon + 1);
    }
  }
  if (parsedHost.empty() || parsedHost.find_first_of("/?#\\") != std::string::npos)
    return false;
  if (!portText.empty()) {
    unsigned parsed = 0;
    const auto result = std::from_chars(portText.data(), portText.data() + portText.size(), parsed);
    if (result.ec != std::errc() || result.ptr != portText.data() + portText.size() || parsed == 0 ||
        parsed > 65535)
      return false;
    parsedPort = static_cast<uint16_t>(parsed);
  } else if ((!authority.empty() && authority.back() == ':') ||
             (authority.front() == '[' && authority.back() == ':' )) {
    return false;
  }

  std::string parsedPath = pathStart == std::string::npos ? "/" : url.substr(pathStart);
  if (parsedPath.find('#') != std::string::npos || !ValidUrlCharacter(parsedPath, true))
    return false;
  host = std::move(parsedHost);
  port = parsedPort;
  path = std::move(parsedPath);
  secure = parsedSecure;
  return true;
}

std::string Base64Encode(const void* data, size_t size) {
  static constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const auto* bytes = static_cast<const uint8_t*>(data);
  std::string result;
  result.reserve(((size + 2) / 3) * 4);
  for (size_t i = 0; i < size; i += 3) {
    const size_t remaining = size - i;
    const uint32_t value = (static_cast<uint32_t>(bytes[i]) << 16) |
                           (remaining > 1 ? static_cast<uint32_t>(bytes[i + 1]) << 8 : 0) |
                           (remaining > 2 ? static_cast<uint32_t>(bytes[i + 2]) : 0);
    result.push_back(alphabet[(value >> 18) & 0x3f]);
    result.push_back(alphabet[(value >> 12) & 0x3f]);
    result.push_back(remaining > 1 ? alphabet[(value >> 6) & 0x3f] : '=');
    result.push_back(remaining > 2 ? alphabet[value & 0x3f] : '=');
  }
  return result;
}

void Sha1(const void* data, size_t size, uint8_t out[20]) {
  std::vector<uint8_t> message;
  const auto* bytes = static_cast<const uint8_t*>(data);
  if (size != 0)
    message.assign(bytes, bytes + size);
  const uint64_t bitLength = static_cast<uint64_t>(size) * 8u;
  message.push_back(0x80);
  while ((message.size() & 63u) != 56u)
    message.push_back(0);
  for (int i = 7; i >= 0; --i)
    message.push_back(static_cast<uint8_t>(bitLength >> (i * 8)));

  uint32_t h0 = 0x67452301;
  uint32_t h1 = 0xefcdab89;
  uint32_t h2 = 0x98badcfe;
  uint32_t h3 = 0x10325476;
  uint32_t h4 = 0xc3d2e1f0;
  for (size_t offset = 0; offset < message.size(); offset += 64) {
    uint32_t words[80]{};
    for (size_t i = 0; i < 16; ++i) {
      const size_t index = offset + i * 4;
      words[i] = (static_cast<uint32_t>(message[index]) << 24) |
                 (static_cast<uint32_t>(message[index + 1]) << 16) |
                 (static_cast<uint32_t>(message[index + 2]) << 8) |
                 static_cast<uint32_t>(message[index + 3]);
    }
    for (size_t i = 16; i < 80; ++i) {
      const uint32_t value = words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16];
      words[i] = (value << 1) | (value >> 31);
    }
    uint32_t a = h0;
    uint32_t b = h1;
    uint32_t c = h2;
    uint32_t d = h3;
    uint32_t e = h4;
    for (uint32_t i = 0; i < 80; ++i) {
      uint32_t function;
      uint32_t constant;
      if (i < 20) {
        function = (b & c) | ((~b) & d);
        constant = 0x5a827999;
      } else if (i < 40) {
        function = b ^ c ^ d;
        constant = 0x6ed9eba1;
      } else if (i < 60) {
        function = (b & c) | (b & d) | (c & d);
        constant = 0x8f1bbcdc;
      } else {
        function = b ^ c ^ d;
        constant = 0xca62c1d6;
      }
      const uint32_t rotatedA = (a << 5) | (a >> 27);
      const uint32_t temporary = rotatedA + function + e + constant + words[i];
      e = d;
      d = c;
      c = (b << 30) | (b >> 2);
      b = a;
      a = temporary;
    }
    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;
  }
  const uint32_t digest[5] = {h0, h1, h2, h3, h4};
  for (size_t i = 0; i < 5; ++i) {
    out[i * 4] = static_cast<uint8_t>(digest[i] >> 24);
    out[i * 4 + 1] = static_cast<uint8_t>(digest[i] >> 16);
    out[i * 4 + 2] = static_cast<uint8_t>(digest[i] >> 8);
    out[i * 4 + 3] = static_cast<uint8_t>(digest[i]);
  }
}

std::string AcceptKey(const std::string& clientKey) {
  const std::string input = clientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  uint8_t digest[20];
  Sha1(input.data(), input.size(), digest);
  return Base64Encode(digest, sizeof(digest));
}

std::string EncodeFrame(uint8_t opcode, const std::string& payload, uint32_t maskSeed) {
  std::string frame;
  const uint64_t length = payload.size();
  frame.reserve(payload.size() + 14);
  frame.push_back(static_cast<char>(0x80 | (opcode & 0x0f)));
  if (length < 126) {
    frame.push_back(static_cast<char>(0x80 | length));
  } else if (length <= 0xffff) {
    frame.push_back(static_cast<char>(0x80 | 126));
    frame.push_back(static_cast<char>((length >> 8) & 0xff));
    frame.push_back(static_cast<char>(length & 0xff));
  } else {
    frame.push_back(static_cast<char>(0x80 | 127));
    for (int i = 7; i >= 0; --i)
      frame.push_back(static_cast<char>((length >> (i * 8)) & 0xff));
  }
  // The mask key is the four little-endian bytes of maskSeed; payload byte i
  // is XORed with key[i mod 4].
  const uint8_t mask[4] = {static_cast<uint8_t>(maskSeed), static_cast<uint8_t>(maskSeed >> 8),
                           static_cast<uint8_t>(maskSeed >> 16), static_cast<uint8_t>(maskSeed >> 24)};
  for (uint8_t byte : mask)
    frame.push_back(static_cast<char>(byte));
  for (size_t i = 0; i < payload.size(); ++i)
    frame.push_back(static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask[i & 3]));
  return frame;
}

size_t FrameDecoder::Feed(const char* data, size_t size, std::vector<Frame>& out) {
  const size_t initialCount = out.size();
  if (mFailed)
    return 0;
  if (size != 0 && data == nullptr) {
    mFailed = true;
    return 0;
  }
  if (size != 0)
    mBuffer.append(data, size);

  size_t cursor = 0;
  while (mBuffer.size() - cursor >= 2) {
    const uint8_t first = static_cast<uint8_t>(mBuffer[cursor]);
    const uint8_t second = static_cast<uint8_t>(mBuffer[cursor + 1]);
    const bool final = (first & 0x80) != 0;
    const uint8_t opcode = first & 0x0f;
    const bool masked = (second & 0x80) != 0;
    // RSV1 marks a compressed message and belongs only on its first frame.
    const bool compressed = (first & 0x40) != 0;
    if ((first & 0x30) != 0 || (compressed && (!mDeflate || (opcode != 0x1 && opcode != 0x2)))) {
      mFailed = true;
      break;
    }
    const bool control = opcode >= 0x8;
    if (control) {
      if ((opcode != 0x8 && opcode != 0x9 && opcode != 0xa) || !final || (second & 0x7f) > 125) {
        mFailed = true;
        break;
      }
    } else if (opcode != 0x0 && opcode != 0x1 && opcode != 0x2) {
      mFailed = true;
      break;
    }

    uint64_t length = second & 0x7f;
    size_t headerSize = 2;
    if (length == 126) {
      if (mBuffer.size() - cursor < 4)
        break;
      length = (static_cast<uint8_t>(mBuffer[cursor + 2]) << 8) |
               static_cast<uint8_t>(mBuffer[cursor + 3]);
      headerSize += 2;
      if (length < 126) {
        mFailed = true;
        break;
      }
    } else if (length == 127) {
      if (mBuffer.size() - cursor < 10)
        break;
      if ((static_cast<uint8_t>(mBuffer[cursor + 2]) & 0x80) != 0) {
        mFailed = true;
        break;
      }
      length = 0;
      for (size_t i = 0; i < 8; ++i)
        length = (length << 8) | static_cast<uint8_t>(mBuffer[cursor + 2 + i]);
      headerSize += 8;
      if (length <= 0xffff) {
        mFailed = true;
        break;
      }
    }
    if (control && length > 125) {
      mFailed = true;
      break;
    }
    if (length > kMaxMessageSize) {
      mFailed = true;
      break;
    }
    if (masked)
      headerSize += 4;
    if (mBuffer.size() - cursor < headerSize || length > mBuffer.size() - cursor - headerSize)
      break;
    if (opcode == 0x0) {
      if (!mInMessage || length > kMaxMessageSize - mMessage.size()) {
        mFailed = true;
        break;
      }
    } else if (!control) {
      if (mInMessage || length > kMaxMessageSize) {
        mFailed = true;
        break;
      }
    }

    const size_t payloadStart = cursor + headerSize;
    const size_t maskStart = masked ? payloadStart - 4 : 0;
    std::string payload;
    payload.reserve(static_cast<size_t>(length));
    for (size_t i = 0; i < static_cast<size_t>(length); ++i) {
      uint8_t byte = static_cast<uint8_t>(mBuffer[payloadStart + i]);
      if (masked)
        byte ^= static_cast<uint8_t>(mBuffer[maskStart + (i & 3)]);
      payload.push_back(static_cast<char>(byte));
    }
    cursor = payloadStart + static_cast<size_t>(length);

    if (control) {
      out.push_back(Frame{opcode, std::move(payload)});
      continue;
    }
    if (opcode == 0) {
      mMessage.append(payload);
      if (final) {
        const bool wasCompressed = mMessageCompressed;
        const uint8_t messageOpcode = mMessageOpcode;
        std::string message = std::move(mMessage);
        mMessage.clear();
        mMessageOpcode = 0;
        mMessageCompressed = false;
        mInMessage = false;
        if (!PushMessage(messageOpcode, std::move(message), wasCompressed, out))
          break;
      }
      continue;
    }
    if (final) {
      if (!PushMessage(opcode, std::move(payload), compressed, out))
        break;
    } else {
      mMessage = std::move(payload);
      mMessageOpcode = opcode;
      mMessageCompressed = compressed;
      mInMessage = true;
    }
  }
  if (cursor != 0)
    mBuffer.erase(0, cursor);
  return out.size() - initialCount;
}

bool FrameDecoder::PushMessage(uint8_t opcode, std::string payload, bool compressed, std::vector<Frame>& out) {
  if (compressed) {
    std::string inflated;
    if (!mInflater.InflateMessage(payload, inflated, kMaxMessageSize)) {
      mFailed = true;
      return false;
    }
    payload = std::move(inflated);
  }
  out.push_back(Frame{opcode, std::move(payload)});
  return true;
}

void FrameDecoder::EnableDeflate(bool serverNoContextTakeover) {
  mDeflate = true;
  mInflater.Reset();
  mInflater.SetKeepWindow(!serverNoContextTakeover);
}

void FrameDecoder::Reset() {
  mBuffer.clear();
  mMessage.clear();
  mMessageOpcode = 0;
  mMessageCompressed = false;
  mInMessage = false;
  mFailed = false;
  mDeflate = false;
  mInflater.Reset();
}

Client::Client() = default;

Client::~Client() { Close(); }

// Resolves and connects `host`, then runs the TLS handshake when `secure`.
// On failure mError says why; the caller drops the connection.
bool Client::OpenTransport(const std::string& host, uint16_t port, bool secure, const TlsOptions& tls,
                           Clock::time_point deadline, bool infinite) {
  auto TransportFail = [this](const std::string& reason) {
    mError = reason;
    return false;
  };
#ifndef MP_HAVE_OPENSSL
  (void)secure;
  (void)tls;
#endif
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* addresses = nullptr;
  const std::string service = std::to_string(port);
  const int resolveResult = getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
  if (resolveResult != 0)
    return TransportFail("getaddrinfo failed: " + std::to_string(resolveResult));

  std::string connectError = "connect failed";
  for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
    const NativeSocket candidate = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (candidate == kInvalidSocket) {
      connectError = "socket failed: " + SystemError(SocketError());
      continue;
    }
    if (!SetNonBlocking(candidate)) {
      connectError = "could not set nonblocking socket: " + SystemError(SocketError());
      CloseNative(candidate);
      continue;
    }
    int result = ::connect(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen));
    bool connected = result == 0;
    if (!connected) {
      const int socketError = SocketError();
      if (IsWouldBlock(socketError)) {
        const int remaining = RemainingMs(deadline, infinite);
        if (remaining < 0) {
          connectError = "connect timed out";
        } else {
          const int ready = WaitReady(StoreSocket(candidate), false, remaining, mCancel);
          if (ready > 0) {
            int pendingError = 0;
#ifdef _WIN32
            int optionLength = sizeof(pendingError);
#else
            socklen_t optionLength = sizeof(pendingError);
#endif
            if (getsockopt(candidate, SOL_SOCKET, SO_ERROR,
                           reinterpret_cast<char*>(&pendingError), &optionLength) == 0 && pendingError == 0)
              connected = true;
            else
              connectError = "connect failed: " + SystemError(pendingError == 0 ? SocketError() : pendingError);
          } else if (ready == 0) {
            connectError = "connect timed out";
          } else {
            connectError = "connect wait failed: " + SystemError(SocketError());
          }
        }
      } else {
        connectError = "connect failed: " + SystemError(socketError);
      }
    }
    if (connected) {
      const int stored = StoreSocket(candidate);
      if (stored >= 0) {
        mSocket = stored;
        break;
      }
      connectError = "socket handle cannot be represented by this client";
    }
    CloseNative(candidate);
  }
  freeaddrinfo(addresses);
  if (mSocket < 0)
    return TransportFail(connectError);

#ifdef MP_HAVE_OPENSSL
  if (secure) {
#ifdef SO_NOSIGPIPE
    int noSigpipe = 1;
    setsockopt(NativeFromStored(mSocket), SOL_SOCKET, SO_NOSIGPIPE, &noSigpipe, sizeof(noSigpipe));
#endif
    ERR_clear_error();
    mSslContext = SSL_CTX_new(TLS_client_method());
    if (mSslContext == nullptr)
      return TransportFail("could not create TLS context: " + TlsQueueText());
    SSL_CTX_set_min_proto_version(mSslContext, TLS1_2_VERSION);
    SSL_CTX_set_verify(mSslContext, SSL_VERIFY_PEER, nullptr);
    const std::vector<std::string> caDirs =
        tls.caFile.empty() && tls.caDirs.empty() ? DefaultCaDirs() : tls.caDirs;
    if (!tls.caFile.empty()) {
      if (SSL_CTX_load_verify_locations(mSslContext, tls.caFile.c_str(), nullptr) != 1)
        return TransportFail("could not load TLS CA file " + tls.caFile + ": " + TlsQueueText());
    } else if (!caDirs.empty()) {
      // The first directory with a certificate in it wins. An empty trust
      // store would fail every server with an obscure verification error, so
      // it is an error of its own that says where it looked.
      size_t loaded = 0;
      std::string tried;
      for (const std::string& dir : caDirs) {
        std::string note;
        loaded = LoadCaDir(SSL_CTX_get_cert_store(mSslContext), dir, note);
        if (loaded > 0)
          break;
        tried += (tried.empty() ? "" : ", ") + dir + " (" + note + ")";
      }
      if (loaded == 0)
        return TransportFail("no TLS root certificates: loaded 0 from " + tried);
#ifdef _WIN32
    } else {
      // An empty ROOT store is not fatal: Windows' own check can still fetch
      // the root the server needs.
      LoadWindowsRoots(SSL_CTX_get_cert_store(mSslContext));
      SSL_CTX_set_cert_verify_callback(mSslContext, VerifyWithWindowsFallback, nullptr);
    }
#elif defined(__APPLE__)
    } else if (SSL_CTX_load_verify_locations(mSslContext, "/etc/ssl/cert.pem", nullptr) != 1) {
      // A static OpenSSL's default paths point into the build machine; macOS ships its
      // roots as a PEM bundle at this path.
      return TransportFail("could not load the macOS TLS trust store /etc/ssl/cert.pem: " + TlsQueueText());
#endif
#ifndef _WIN32
    } else if (SSL_CTX_set_default_verify_paths(mSslContext) != 1) {
      return TransportFail("could not load the system TLS trust store: " + TlsQueueText());
    }
#endif
    mSsl = SSL_new(mSslContext);
    if (mSsl == nullptr)
      return TransportFail("could not create TLS session: " + TlsQueueText());
    if (SSL_set_fd(mSsl, mSocket) != 1)
      return TransportFail("could not attach TLS to the socket: " + TlsQueueText());
    // The certificate must name this host; a valid certificate for any other
    // name fails verification.
    if (SSL_set1_host(mSsl, host.c_str()) != 1)
      return TransportFail("could not set the TLS host name: " + TlsQueueText());
    if (!IsIpLiteral(host) && SSL_set_tlsext_host_name(mSsl, host.c_str()) != 1)
      return TransportFail("could not set the TLS server name: " + TlsQueueText());

    for (;;) {
      int result = 0;
      int sslError = SSL_ERROR_NONE;
      {
        SigpipeGuard guard;
        ERR_clear_error();
        result = SSL_connect(mSsl);
        if (result != 1)
          sslError = SSL_get_error(mSsl, result);
      }
      if (result == 1)
        break;
      if (sslError != SSL_ERROR_WANT_READ && sslError != SSL_ERROR_WANT_WRITE) {
        mTlsFailed = true;
        const long verifyResult = SSL_get_verify_result(mSsl);
        if (verifyResult != X509_V_OK) {
          ERR_clear_error();
          return TransportFail(std::string("TLS certificate verification failed: ") +
                      X509_verify_cert_error_string(verifyResult));
        }
        bool closed = false;
        const std::string detail = TlsFailureText(sslError, closed);
        return TransportFail(closed ? "server closed during TLS handshake" : "TLS handshake failed: " + detail);
      }
      const int remaining = RemainingMs(deadline, infinite);
      if (remaining < 0)
        return TransportFail("TLS handshake timed out");
      const int ready = WaitReady(mSocket, sslError == SSL_ERROR_WANT_READ, remaining, mCancel);
      if (ready == 0)
        return TransportFail("TLS handshake timed out");
      if (ready < 0) {
        const int socketError = SocketError();
        if (IsInterrupted(socketError))
          continue;
        return TransportFail("TLS handshake wait failed: " + SystemError(socketError));
      }
    }
  }
#endif
  return true;
}

bool Client::Connect(const std::string& host, uint16_t port, const std::string& path, int timeoutMs,
                     bool secure, const TlsOptions& tls) {
  DropConnection();
  mReceiveBuffer.clear();
  mDecoder.Reset();
  mPendingFrames.clear();
  mError.clear();
  mTimeoutMs = timeoutMs;
  auto fail = [this](const std::string& reason) {
    mError = reason;
    DropConnection();
    mReceiveBuffer.clear();
    mDecoder.Reset();
    mPendingFrames.clear();
    return false;
  };

  if (!EnsureWinsock())
    return fail("WSAStartup failed");
  if (host.empty() || host.find_first_of("\r\n\t /\\") != std::string::npos || port == 0 ||
      path.empty() || path.front() != '/' || path.find_first_of("\r\n") != std::string::npos)
    return fail("invalid WebSocket endpoint");
#ifndef MP_HAVE_OPENSSL
  // Never downgrade a wss:// server to plaintext.
  if (secure)
    return fail("wss:// is not supported: this build has no TLS (built without OpenSSL)");
  (void)tls;
#endif

  const bool infinite = timeoutMs <= 0;
  const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 0));
  if (!OpenTransport(host, port, secure, tls, deadline, infinite))
    return fail(mError);

  uint8_t randomKey[16];
  try {
    std::random_device random;
    for (uint8_t& byte : randomKey)
      byte = static_cast<uint8_t>(random() & 0xffu);
  } catch (...) {
    return fail("could not generate WebSocket key");
  }
  const std::string clientKey = Base64Encode(randomKey, sizeof(randomKey));
  std::string hostHeader = host;
  if (host.find(':') != std::string::npos && (host.empty() || host.front() != '['))
    hostHeader = "[" + host + "]";
  if (port != (secure ? 443 : 80))
    hostHeader += ":" + std::to_string(port);
  const std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + hostHeader +
                              "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " +
                              clientKey +
                              "\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Extensions: permessage-deflate\r\n\r\n";
  std::string ioError;
  const int remainingForSend = RemainingMs(deadline, infinite);
  if (remainingForSend < 0 || !SendBytes(request, remainingForSend, ioError))
    return fail(remainingForSend < 0 ? "handshake timed out" : ioError);

  size_t headerEnd = std::string::npos;
  while (headerEnd == std::string::npos) {
    if (mReceiveBuffer.size() > 16 * 1024)
      return fail("WebSocket response headers too large");
    const int remaining = RemainingMs(deadline, infinite);
    if (remaining < 0)
      return fail("handshake timed out");
    const int ready = WaitIo(remaining);
    if (ready == 0)
      return fail("handshake timed out");
    if (ready < 0) {
      const int socketError = SocketError();
      if (IsInterrupted(socketError))
        continue;
      return fail("handshake read wait failed: " + SystemError(socketError));
    }
    std::string bytes;
    bool closed = false;
    std::string readError;
    if (!ReadRaw(bytes, closed, readError))
      return fail(closed ? "server closed during handshake" : "handshake read failed: " + readError);
    mReceiveBuffer.append(bytes);
    headerEnd = mReceiveBuffer.find("\r\n\r\n");
    if (headerEnd == std::string::npos && mReceiveBuffer.size() > 16 * 1024)
      return fail("WebSocket response headers too large");
  }
  if (headerEnd + 4 > 16 * 1024)
    return fail("WebSocket response headers too large");

  const std::string responseHeaders = mReceiveBuffer.substr(0, headerEnd);
  mReceiveBuffer.erase(0, headerEnd + 4); // Preserve any first frame bytes received with the headers.
  const size_t firstLineEnd = responseHeaders.find("\r\n");
  const std::string_view statusLine(responseHeaders.data(),
                                    firstLineEnd == std::string::npos ? responseHeaders.size() : firstLineEnd);
  if (statusLine.size() < 12 || statusLine.substr(0, 9) != "HTTP/1.1 " ||
      statusLine.substr(9, 3) != "101" || (statusLine.size() > 12 && statusLine[12] != ' '))
    return fail("WebSocket upgrade did not return HTTP 101");

  std::string accept;
  std::string upgrade;
  std::string connection;
  std::string extensions;
  size_t lineStart = firstLineEnd == std::string::npos ? responseHeaders.size() : firstLineEnd + 2;
  while (lineStart < responseHeaders.size()) {
    const size_t lineEnd = responseHeaders.find("\r\n", lineStart);
    const size_t end = lineEnd == std::string::npos ? responseHeaders.size() : lineEnd;
    const std::string_view line(responseHeaders.data() + lineStart, end - lineStart);
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos)
      return fail("malformed WebSocket response header");
    const std::string name = Lower(std::string(Trim(line.substr(0, colon))));
    const std::string value(Trim(line.substr(colon + 1)));
    if (name == "sec-websocket-accept") {
      if (!accept.empty())
        return fail("duplicate Sec-WebSocket-Accept header");
      accept = value;
    } else if (name == "upgrade") {
      upgrade = value;
    } else if (name == "connection") {
      connection = value;
    } else if (name == "sec-websocket-extensions") {
      extensions += (extensions.empty() ? "" : ", ") + value;
    }
    lineStart = lineEnd == std::string::npos ? responseHeaders.size() : lineEnd + 2;
  }
  if (accept != AcceptKey(clientKey))
    return fail("invalid Sec-WebSocket-Accept header");
  if (Lower(upgrade) != "websocket" || !HasToken(connection, "upgrade"))
    return fail("invalid WebSocket upgrade headers");
  bool deflate = false;
  bool noContextTakeover = false;
  if (!ParseDeflateResponse(extensions, deflate, noContextTakeover))
    return fail("unsupported WebSocket extension: " + extensions);
  if (deflate)
    mDecoder.EnableDeflate(noContextTakeover);
  mCompressed = deflate;
  mError.clear();
  return true;
}

// A whole HTTP/1.1 GET on its own connection, closed afterwards. The body is
// returned for any status, de-chunked; content codings are not asked for.
bool Client::HttpGet(const std::string& host, uint16_t port, const std::string& path, int timeoutMs, bool secure,
                     const std::string& extraHeaders, int& status, std::string& body, size_t maxBody,
                     const TlsOptions& tls) {
  DropConnection();
  mReceiveBuffer.clear();
  mError.clear();
  status = 0;
  body.clear();
  auto fail = [this](const std::string& reason) {
    mError = reason;
    DropConnection();
    mReceiveBuffer.clear();
    return false;
  };
  if (!EnsureWinsock())
    return fail("WSAStartup failed");
  if (host.empty() || host.find_first_of("\r\n\t /\\") != std::string::npos || port == 0 ||
      path.empty() || path.front() != '/' || path.find_first_of("\r\n ") != std::string::npos)
    return fail("invalid HTTP endpoint");
#ifndef MP_HAVE_OPENSSL
  if (secure)
    return fail("https:// is not supported: this build has no TLS (built without OpenSSL)");
#endif
  const bool infinite = timeoutMs <= 0;
  const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 0));
  if (!OpenTransport(host, port, secure, tls, deadline, infinite))
    return fail(mError);

  std::string hostHeader = host;
  if (host.find(':') != std::string::npos && host.front() != '[')
    hostHeader = "[" + host + "]";
  if (port != (secure ? 443 : 80))
    hostHeader += ":" + std::to_string(port);
  const std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + hostHeader +
                              "\r\nConnection: close\r\nAccept-Encoding: identity\r\n" + extraHeaders + "\r\n";
  std::string ioError;
  const int remainingForSend = RemainingMs(deadline, infinite);
  if (remainingForSend < 0 || !SendBytes(request, remainingForSend, ioError))
    return fail(remainingForSend < 0 ? "request timed out" : ioError);

  // Read to the end: the server closes after the response.
  std::string response;
  for (;;) {
    if (response.size() > maxBody + 64 * 1024)
      return fail("HTTP response too large");
    const int remaining = RemainingMs(deadline, infinite);
    if (remaining < 0)
      return fail("response timed out");
    const int ready = WaitIo(remaining);
    if (ready == 0)
      return fail("response timed out");
    if (ready < 0) {
      const int socketError = SocketError();
      if (IsInterrupted(socketError))
        continue;
      return fail("response read wait failed: " + SystemError(socketError));
    }
    std::string bytes;
    bool closed = false;
    std::string readError;
    if (!ReadRaw(bytes, closed, readError)) {
      if (!closed)
        return fail("response read failed: " + readError);
      break;
    }
    response.append(bytes);
  }
  DropConnection();

  const size_t headerEnd = response.find("\r\n\r\n");
  if (headerEnd == std::string::npos)
    return fail("truncated HTTP response");
  const std::string headers = response.substr(0, headerEnd);
  if (headers.size() < 12 || headers.compare(0, 5, "HTTP/") != 0)
    return fail("malformed HTTP status line");
  const size_t codeAt = headers.find(' ');
  if (codeAt == std::string::npos || codeAt + 4 > headers.size())
    return fail("malformed HTTP status line");
  for (size_t i = codeAt + 1; i < codeAt + 4; ++i) {
    if (headers[i] < '0' || headers[i] > '9')
      return fail("malformed HTTP status line");
    status = status * 10 + (headers[i] - '0');
  }
  bool chunked = false;
  size_t lineStart = headers.find("\r\n");
  while (lineStart != std::string::npos && lineStart + 2 < headers.size()) {
    lineStart += 2;
    const size_t lineEnd = headers.find("\r\n", lineStart);
    const std::string_view line(headers.data() + lineStart,
                                (lineEnd == std::string::npos ? headers.size() : lineEnd) - lineStart);
    const size_t colon = line.find(':');
    if (colon != std::string_view::npos && Lower(std::string(Trim(line.substr(0, colon)))) == "transfer-encoding")
      chunked = HasToken(std::string(line.substr(colon + 1)), "chunked");
    lineStart = lineEnd;
  }
  std::string raw = response.substr(headerEnd + 4);
  if (!chunked) {
    body = std::move(raw);
  } else {
    size_t at = 0;
    for (;;) {
      const size_t sizeEnd = raw.find("\r\n", at);
      if (sizeEnd == std::string::npos)
        return fail("truncated chunked body");
      size_t size = 0;
      size_t digits = 0;
      for (size_t i = at; i < sizeEnd; ++i, ++digits) {
        const char c = raw[i];
        int value = -1;
        if (c >= '0' && c <= '9')
          value = c - '0';
        else if (c >= 'a' && c <= 'f')
          value = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
          value = c - 'A' + 10;
        if (value < 0)
          break; // a chunk extension
        if (size > (maxBody >> 4))
          return fail("HTTP response too large");
        size = size * 16 + static_cast<size_t>(value);
      }
      if (digits == 0)
        return fail("malformed chunk size");
      at = sizeEnd + 2;
      if (size == 0)
        break;
      if (raw.size() - at < size)
        return fail("truncated chunked body");
      body.append(raw, at, size);
      at += size + 2;
      if (body.size() > maxBody)
        return fail("HTTP response too large");
    }
  }
  if (body.size() > maxBody)
    return fail("HTTP response too large");
  return true;
}

void Client::Close() {
  if (mSocket >= 0) {
    uint32_t seed = 0;
    if (RandomSeed(seed))
      SendRaw(EncodeFrame(0x8, std::string(), seed));
    else
      mError = "could not generate close-frame mask";
    ShutdownTls();
  }
  DropConnection();
  mReceiveBuffer.clear();
  mDecoder.Reset();
  mPendingFrames.clear();
}

bool Client::SendText(const std::string& message) {
  if (mSocket < 0) {
    mError = "not connected";
    return false;
  }
  uint32_t seed = 0;
  if (!RandomSeed(seed)) {
    mError = "could not generate frame mask";
    return false;
  }
  return SendRaw(EncodeFrame(0x1, message, seed));
}

bool Client::ReceiveText(std::string& message, int timeoutMs) {
  if (mSocket < 0) {
    mError = "not connected";
    return false;
  }
  const bool infinite = timeoutMs <= 0;
  const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 0));
  for (;;) {
    // Frames decoded earlier wait here in arrival order, so a read holding
    // several messages hands them out one per call.
    while (!mPendingFrames.empty()) {
      Frame frame = std::move(mPendingFrames.front());
      mPendingFrames.pop_front();
      if (frame.opcode == 0x1) {
        message = std::move(frame.payload);
        return true;
      }
      if (frame.opcode == 0x8) {
        uint32_t seed = 0;
        if (RandomSeed(seed))
          SendRaw(EncodeFrame(0x8, frame.payload, seed));
        ShutdownTls();
        DropConnection();
        mReceiveBuffer.clear();
        mDecoder.Reset();
        mPendingFrames.clear();
        mError = "server closed WebSocket";
        return false;
      }
      if (frame.opcode == 0x9) {
        uint32_t seed = 0;
        if (!RandomSeed(seed) || !SendRaw(EncodeFrame(0xa, frame.payload, seed))) {
          DropConnection();
          mPendingFrames.clear();
          return false;
        }
      } else if (frame.opcode != 0xa) {
        mError = "unsupported WebSocket data opcode";
        DropConnection();
        mReceiveBuffer.clear();
        mPendingFrames.clear();
        return false;
      }
    }
    if (!mReceiveBuffer.empty()) {
      // The whole read goes to the decoder at once. Feeding it a byte at a
      // time cost one decoder pass per byte, and a DataPackage message runs
      // to megabytes.
      std::vector<Frame> frames;
      mDecoder.Feed(mReceiveBuffer.data(), mReceiveBuffer.size(), frames);
      mReceiveBuffer.clear();
      if (mDecoder.Failed()) {
        mError = "invalid WebSocket frame";
        DropConnection();
        mPendingFrames.clear();
        return false;
      }
      for (Frame& frame : frames)
        mPendingFrames.push_back(std::move(frame));
      continue;
    }

    const int remaining = RemainingMs(deadline, infinite);
    if (remaining < 0) {
      mError = "receive timed out";
      return false;
    }
    const int ready = WaitIo(remaining);
    if (ready == 0) {
      mError = "receive timed out";
      return false;
    }
    if (ready < 0) {
      const int socketError = SocketError();
      if (IsInterrupted(socketError))
        continue;
      mError = "receive wait failed: " + SystemError(socketError);
      return false;
    }
    std::string bytes;
    bool closed = false;
    std::string readError;
    if (!ReadRaw(bytes, closed, readError)) {
      mError = closed ? "server closed the connection" : "receive failed: " + readError;
      if (closed)
        ShutdownTls();
      DropConnection();
      return false;
    }
    if (!bytes.empty())
      mReceiveBuffer = std::move(bytes);
  }
}

bool Client::WaitReadable(int timeoutMs) {
  if (mSocket < 0) {
    mError = "not connected";
    return false;
  }
  const int ready = WaitIo(timeoutMs);
  if (ready > 0)
    return true;
  if (ready == 0)
    mError = "receive timed out";
  else
    mError = "receive wait failed: " + SystemError(SocketError());
  return false;
}

bool Client::SendRaw(const std::string& data) {
  if (mSocket < 0) {
    mError = "not connected";
    return false;
  }
  std::string error;
  if (!SendBytes(data, mTimeoutMs, error)) {
    mError = std::move(error);
    return false;
  }
  return true;
}

bool Client::SendBytes(const std::string& data, int timeoutMs, std::string& error) {
#ifdef MP_HAVE_OPENSSL
  if (mSsl != nullptr) {
    bool fatal = false;
    const bool sent = SendAllTls(mSsl, mSocket, data, timeoutMs, error, fatal, mCancel);
    if (fatal)
      mTlsFailed = true;
    return sent;
  }
#endif
  return SendAll(mSocket, data, timeoutMs, error, mCancel);
}

bool Client::ReadRaw(std::string& out, bool& closed, std::string& error) {
  closed = false;
  char buffer[4096];
#ifdef MP_HAVE_OPENSSL
  if (mSsl != nullptr) {
    int count = 0;
    int sslError = SSL_ERROR_NONE;
    {
      // Reads can write too (key updates), so they need the guard as well.
      SigpipeGuard guard;
      ERR_clear_error();
      count = SSL_read(mSsl, buffer, sizeof(buffer));
      if (count <= 0)
        sslError = SSL_get_error(mSsl, count);
    }
    mTlsReadWantsWrite = sslError == SSL_ERROR_WANT_WRITE;
    if (count > 0) {
      out.assign(buffer, static_cast<size_t>(count));
      return true;
    }
    if (sslError == SSL_ERROR_WANT_READ || sslError == SSL_ERROR_WANT_WRITE)
      return true;
    error = TlsFailureText(sslError, closed);
    // Only a clean close_notify leaves the session fit for our own shutdown.
    if (sslError != SSL_ERROR_ZERO_RETURN)
      mTlsFailed = true;
    return false;
  }
#endif
#ifdef _WIN32
  const int count = ::recv(NativeFromStored(mSocket), buffer, sizeof(buffer), 0);
#else
  const ssize_t count = ::recv(NativeFromStored(mSocket), buffer, sizeof(buffer), 0);
#endif
  if (count > 0) {
    out.assign(buffer, static_cast<size_t>(count));
    return true;
  }
  if (count == 0) {
    closed = true;
    return false;
  }
  const int socketError = SocketError();
  if (IsInterrupted(socketError) || IsWouldBlock(socketError))
    return true;
  error = SystemError(socketError);
  return false;
}

int Client::WaitIo(int timeoutMs) {
#ifdef MP_HAVE_OPENSSL
  if (mSsl != nullptr) {
    // Decrypted bytes, or a whole record OpenSSL has already pulled off the
    // socket, would never make select() fire.
    if (SSL_has_pending(mSsl) == 1)
      return 1;
    return WaitReady(mSocket, !mTlsReadWantsWrite, timeoutMs, mCancel);
  }
#endif
  return WaitReady(mSocket, true, timeoutMs, mCancel);
}

void Client::ShutdownTls() {
#ifdef MP_HAVE_OPENSSL
  if (mSsl != nullptr && !mTlsFailed) {
    SigpipeGuard guard;
    ERR_clear_error();
    // Sends close_notify without waiting for the server's; the socket closes
    // right after.
    SSL_shutdown(mSsl);
    ERR_clear_error();
  }
#endif
}

void Client::DropConnection() {
#ifdef MP_HAVE_OPENSSL
  SSL_free(mSsl);
  mSsl = nullptr;
  SSL_CTX_free(mSslContext);
  mSslContext = nullptr;
#endif
  mTlsFailed = false;
  mTlsReadWantsWrite = false;
  mCompressed = false;
  if (mSocket >= 0) {
    CloseNative(NativeFromStored(mSocket));
    mSocket = -1;
  }
}

} // namespace PortWs
