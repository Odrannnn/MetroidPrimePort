#include "port_ws.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
// The cancel test below: a listener that never answers.
#include <atomic>
#include <chrono>
#include <thread>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#if defined(MP_HAVE_OPENSSL)
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#endif

namespace {

bool sPassed = true;

void Check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "[ws-tests] FAILED: %s\n", message);
    sPassed = false;
  }
}

std::string Hex(const uint8_t* bytes, size_t size) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) {
    result.push_back(digits[bytes[i] >> 4]);
    result.push_back(digits[bytes[i] & 0x0f]);
  }
  return result;
}

std::string ServerFrame(uint8_t opcode, bool final, const std::string& payload) {
  std::string frame;
  frame.push_back(static_cast<char>((final ? 0x80 : 0) | opcode));
  if (payload.size() < 126) {
    frame.push_back(static_cast<char>(payload.size()));
  } else if (payload.size() <= 0xffff) {
    frame.push_back(static_cast<char>(126));
    frame.push_back(static_cast<char>((payload.size() >> 8) & 0xff));
    frame.push_back(static_cast<char>(payload.size() & 0xff));
  } else {
    frame.push_back(static_cast<char>(127));
    const uint64_t size = payload.size();
    for (int i = 7; i >= 0; --i)
      frame.push_back(static_cast<char>((size >> (i * 8)) & 0xff));
  }
  frame.append(payload);
  return frame;
}

std::string FromHex(const char* hex) {
  std::string bytes;
  for (size_t i = 0; hex[i] != '\0' && hex[i + 1] != '\0'; i += 2)
    bytes.push_back(static_cast<char>(std::stoi(std::string(hex + i, 2), nullptr, 16)));
  return bytes;
}

// A server frame with RSV1 set on its first byte.
std::string CompressedFrame(uint8_t opcode, bool final, const std::string& payload) {
  std::string frame = ServerFrame(opcode, final, payload);
  frame[0] = static_cast<char>(frame[0] | 0x40);
  return frame;
}

// Vectors from Python's zlib.compressobj(wbits=-15) with Z_SYNC_FLUSH and the
// 00 00 ff ff tail stripped, as a permessage-deflate server sends them.
const char* kFixedHello = "ca48cdc9c957c840900000"; // "hello hello hello", fixed Huffman
const char* kStored = "000700f8ff73746f7265642100";  // "stored!", level 0
const char* kRoomInfo = "aa564ace4d51b2520acacfcff5cc4bcb57aa0500";
const char* kRoomInfoAgain = "aac6100100"; // the same text, only a back-reference into the window
const char* kDynamic =
    "7cd6318e90501846d1bd505b7081f7805982859ab804a799c2298cddc4bdeb023cd45ff55f029c8fe5c7cfd7e565f9f6ebedfdf7e7"
    "ef5fbf2c9f96f7e565fdf3f1ff210d9b865dc3a16168981a4e0d97869b07fa74de1e8f8fd7c7f3e3fd31402c1013c4061b1b6c7efe"
    "6cb0b1c1c6061b1b6c6cb0b1c1c6061b1bec6cb0b3c1ee97800d7636d8d96067839d0d7636d8d9e06083830d0e3638fc256083830d"
    "0e3638d8e06083830d061b0c36186c30d860f873c806830d061b0c36186c30d960b2c16483c906930da6ff096c30d960b2c16483930d"
    "4e3638d9e06483930d4e3638fd636483930d4e36b8d8e062838b0d2e36b8d8e062838b0d2eeb800d2e36b8d9e066839b0d6e36b8d9"
    "e066839b0d6e36b84da407231949ab95b49a49ab9db41a4aaba5b49a4aabadb41a4bab6b3c91d1351ed0f8a0c607363eb8f1018e0f"
    "727ca0a3ed98f198f598f998fd980199059909990d991199159919991d992199259929992d993199359939993d994199459949994d"
    "995199559959995d996199659969996d997199759979997d998199859989998d999199959999999d99a199a599a999ad99b199b599b"
    "999bd99c199c599c999cd99d199d599d999dd99e199e599e999ed99f199f599f999fd99019a059a099a0d9a119a159a199a1d9a219"
    "a259a29da3f8bfe05";

std::string DynamicText() {
  std::string text;
  for (int i = 0; i < 200; ++i)
    text += "{\"cmd\":\"PrintJSON\",\"n\":" + std::to_string(i) + "}";
  return text;
}

void CheckDeflate() {
  const size_t limit = PortWs::FrameDecoder::kMaxMessageSize;
  {
    PortWs::Inflater inflater;
    std::string out;
    Check(inflater.InflateMessage(FromHex(kFixedHello), out, limit) && out == "hello hello hello",
          "inflate a fixed-Huffman message");
    Check(inflater.InflateMessage(FromHex(kStored), out, limit) && out == "stored!", "inflate a stored block");
    Check(inflater.InflateMessage(FromHex("00"), out, limit) && out.empty(), "inflate an empty message");
  }
  {
    PortWs::Inflater inflater;
    std::string out;
    Check(inflater.InflateMessage(FromHex(kDynamic), out, limit) && out == DynamicText(),
          "inflate a dynamic-Huffman message");
    PortWs::Inflater limited;
    Check(!limited.InflateMessage(FromHex(kDynamic), out, 100), "inflate refuses output past the size limit");
  }
  {
    PortWs::Inflater inflater;
    std::string first;
    std::string second;
    Check(inflater.InflateMessage(FromHex(kRoomInfo), first, limit) &&
              inflater.InflateMessage(FromHex(kRoomInfoAgain), second, limit) && first == "{\"cmd\":\"RoomInfo\"}" &&
              second == first,
          "context takeover: the second message reaches into the first's window");
    PortWs::Inflater noWindow;
    noWindow.SetKeepWindow(false);
    Check(noWindow.InflateMessage(FromHex(kRoomInfo), first, limit) &&
              !noWindow.InflateMessage(FromHex(kRoomInfoAgain), second, limit),
          "without the window a back-reference into the previous message fails");
  }
  {
    PortWs::Inflater inflater;
    std::string out;
    Check(!inflater.InflateMessage(FromHex("ffffffff"), out, limit), "reserved block type fails");
    std::string truncated = FromHex(kDynamic);
    truncated.resize(40);
    Check(!inflater.InflateMessage(truncated, out, limit), "truncated dynamic block fails");
  }

  PortWs::FrameDecoder plain;
  std::vector<PortWs::Frame> frames;
  const std::string hello = CompressedFrame(1, true, FromHex(kFixedHello));
  plain.Feed(hello.data(), hello.size(), frames);
  Check(plain.Failed() && frames.empty(), "RSV1 without negotiated deflate fails");

  PortWs::FrameDecoder decoder;
  decoder.EnableDeflate(false);
  const std::string dynamic = FromHex(kDynamic);
  std::string stream = hello;
  stream += ServerFrame(1, true, "plain");
  stream += CompressedFrame(1, false, dynamic.substr(0, 100));
  stream += ServerFrame(9, true, "p");
  stream += ServerFrame(0, false, dynamic.substr(100, 200));
  stream += ServerFrame(0, true, dynamic.substr(300));
  frames.clear();
  for (size_t i = 0; i < stream.size(); i += 7)
    decoder.Feed(stream.data() + i, std::min<size_t>(7, stream.size() - i), frames);
  Check(!decoder.Failed() && frames.size() == 4 && frames[0].payload == "hello hello hello" &&
            frames[1].payload == "plain" && frames[2].opcode == 9 && frames[3].opcode == 1 &&
            frames[3].payload == DynamicText(),
        "decoder inflates compressed messages, fragmented or not, beside uncompressed ones");

  const std::string badFrames[] = {
      ServerFrame(1, false, "a") + CompressedFrame(0, true, "b"), // RSV1 on a continuation
      CompressedFrame(9, true, ""),                                // RSV1 on a control frame
      std::string("\xa1\x00", 2),                                  // RSV2
      CompressedFrame(1, true, "\xff\xff"),                        // not DEFLATE
  };
  for (const std::string& bad : badFrames) {
    PortWs::FrameDecoder strict;
    strict.EnableDeflate(false);
    std::vector<PortWs::Frame> ignored;
    strict.Feed(bad.data(), bad.size(), ignored);
    Check(strict.Failed(), "misplaced RSV bits and bad compressed data fail the decoder");
  }
  decoder.Reset();
  frames.clear();
  decoder.Feed(hello.data(), hello.size(), frames);
  Check(decoder.Failed(), "Reset turns deflate back off");

  struct ExtensionCase {
    const char* header;
    bool accepted;
    bool deflate;
    bool noContextTakeover;
  };
  const ExtensionCase extensions[] = {
      {"", true, false, false},
      {"permessage-deflate", true, true, false},
      {"PerMessage-Deflate ; server_no_context_takeover", true, true, true},
      {"permessage-deflate; client_no_context_takeover; server_max_window_bits=12", true, true, false},
      {"permessage-deflate; server_max_window_bits=\"15\"", true, true, false},
      {"permessage-deflate; server_max_window_bits=7", false, true, false},
      {"permessage-deflate; server_max_window_bits", false, true, false},
      {"permessage-deflate; unknown_param", false, true, false},
      {"permessage-deflate, permessage-deflate", false, true, false},
      {"x-webkit-deflate-frame", false, false, false},
  };
  for (const ExtensionCase& test : extensions) {
    bool deflate = false;
    bool noContextTakeover = false;
    const bool accepted = PortWs::ParseDeflateResponse(test.header, deflate, noContextTakeover);
    Check(accepted == test.accepted, "ParseDeflateResponse acceptance table");
    if (accepted)
      Check(deflate == test.deflate && noContextTakeover == test.noContextTakeover,
            "ParseDeflateResponse values");
  }
}

void CheckRoundTrip(size_t size) {
  const std::string payload(size, 'x');
  const std::string encoded = PortWs::EncodeFrame(1, payload, 0x12345678);
  PortWs::FrameDecoder decoder;
  std::vector<PortWs::Frame> frames;
  size_t consumed = 0;
  const size_t chunks[] = {1, 2, 7, 3, 19, 5};
  size_t chunkIndex = 0;
  while (consumed < encoded.size()) {
    const size_t count = std::min(chunks[chunkIndex++ % 6], encoded.size() - consumed);
    decoder.Feed(encoded.data() + consumed, count, frames);
    consumed += count;
  }
  Check(!decoder.Failed() && frames.size() == 1 && frames[0].opcode == 1 &&
            frames[0].payload == payload,
        "masked encode/decode round trip at each length encoding boundary");
}

#if defined(MP_HAVE_OPENSSL)
namespace platform {

// Quotes a path for the shell that std::system goes through: single quotes on
// POSIX, double quotes on cmd.exe, which has no single-quote form.
std::string QuotePath(const std::string& path) {
#ifdef _WIN32
  return "\"" + path + "\"";
#else
  return "'" + path + "'";
#endif
}

bool Run(const std::string& command) { return std::system(command.c_str()) == 0; }

bool Have(const std::string& program) {
#ifdef _WIN32
  return Run("where " + program + " >nul 2>&1");
#else
  return Run("command -v " + program + " >/dev/null 2>&1");
#endif
}

// A fresh directory under the system temp root, named after the process so
// parallel runs cannot collide.
std::string MakeTempDir() {
  std::error_code ignored;
  const unsigned long unique = [] {
#ifdef _WIN32
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long>(getpid());
#endif
  }();
  const std::filesystem::path base =
      std::filesystem::temp_directory_path() / ("mp-ws-tls-" + std::to_string(unique));
  std::filesystem::remove_all(base, ignored);
  std::filesystem::create_directories(base);
  return base.string();
}

const char* Quiet() {
#ifdef _WIN32
  return " >nul 2>&1";
#else
  return " >/dev/null 2>&1";
#endif
}

const char* ChangeTo() {
#ifdef _WIN32
  return "cd /d ";
#else
  return "cd ";
#endif
}

// A running fake server, however the platform starts one.
class Server {
public:
  Server(const std::vector<std::string>& arguments, const std::string& logPath) : mLogPath(logPath) {
#ifdef _WIN32
    // CreateProcessA does not interpret shell redirection, so `> file` would be
    // handed to the program as arguments. The log is a real file handle passed
    // through the child's standard handles, which is the fork/dup2 equivalent.
    const HANDLE log = CreateFileA(mLogPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::string command = QuotePath(arguments.front());
    for (size_t i = 1; i < arguments.size(); ++i)
      command += " " + QuotePath(arguments[i]);
    std::vector<char> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back('\0');
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    if (log != INVALID_HANDLE_VALUE) {
      startup.dwFlags = STARTF_USESTDHANDLES;
      startup.hStdInput = log;
      startup.hStdOutput = log;
      startup.hStdError = log;
    }
    PROCESS_INFORMATION process{};
    mStarted = CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, log != INVALID_HANDLE_VALUE,
                              0, nullptr, nullptr, &startup, &process) != 0;
    if (mStarted) {
      mProcess = process.hProcess;
      CloseHandle(process.hThread);
    }
    if (log != INVALID_HANDLE_VALUE)
      CloseHandle(log);
#else
    const pid_t child = fork();
    if (child == 0) {
      const int log = ::open(mLogPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (log >= 0) {
        dup2(log, STDOUT_FILENO);
        dup2(log, STDERR_FILENO);
      }
      std::vector<std::string> storage = arguments;
      std::vector<char*> argv;
      for (std::string& argument : storage)
        argv.push_back(argument.data());
      argv.push_back(nullptr);
      execvp(argv[0], argv.data());
      _exit(127);
    }
    mPid = child;
    mStarted = child > 0;
#endif
  }

  ~Server() { Stop(); }

  bool started() const { return mStarted; }
  const std::string& logPath() const { return mLogPath; }

  void Stop() {
    if (!mStarted)
      return;
    mStarted = false;
#ifdef _WIN32
    TerminateProcess(mProcess, 0);
    WaitForSingleObject(mProcess, 2000);
    CloseHandle(mProcess);
#else
    kill(mPid, SIGTERM);
    int status = 0;
    waitpid(mPid, &status, 0);
#endif
  }

private:
  bool mStarted = false;
  std::string mLogPath;
#ifdef _WIN32
  HANDLE mProcess = nullptr;
#else
  pid_t mPid = -1;
#endif
};

uint16_t FreeLoopbackPort() {
#ifdef _WIN32
  // Winsock is started here and deliberately never cleaned up, matching
  // PortWs::EnsureWinsock: the client connects after this and initialises it
  // through its own once_flag, and a WSACleanup in between would drop the
  // reference count to zero and leave the later socket calls unusable.
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
    return 0;
  const SOCKET probe = ::socket(AF_INET, SOCK_STREAM, 0);
  if (probe == INVALID_SOCKET)
    return 0;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int length = sizeof(address);
  uint16_t port = 0;
  if (::bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
      ::getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length) == 0)
    port = ntohs(address.sin_port);
  ::closesocket(probe);
  return port;
#else
  const int probe = ::socket(AF_INET, SOCK_STREAM, 0);
  if (probe < 0)
    return 0;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t length = sizeof(address);
  uint16_t port = 0;
  if (::bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
      ::getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length) == 0)
    port = ntohs(address.sin_port);
  ::close(probe);
  return port;
#endif
}

} // namespace platform

bool Contains(const std::string& text, const char* wanted) {
  return text.find(wanted) != std::string::npos;
}

// Waits for the server to accept a plain TCP connection. Starting python and
// importing its TLS stack can take several seconds on a loaded CI machine, and
// the TLS connect below used to race it: on Windows the first handshake could
// time out against a server that was not listening yet, while the rejection
// cases a moment later found it up and healthy. A plaintext connect is the
// cheapest way to tell "not started yet" from "started but broken".
bool WaitForListener(uint16_t port, int seconds) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < deadline) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
#ifdef _WIN32
    const SOCKET probe = ::socket(AF_INET, SOCK_STREAM, 0);
    if (probe != INVALID_SOCKET) {
      const bool accepted =
          ::connect(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
      ::closesocket(probe);
      if (accepted)
        return true;
    }
#else
    const int probe = ::socket(AF_INET, SOCK_STREAM, 0);
    if (probe >= 0) {
      const bool accepted =
          ::connect(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
      ::close(probe);
      if (accepted)
        return true;
    }
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

// wss:// against tools/ap_fake_server.py --tls with a throwaway CA: a verified
// round trip, then the rejections that make verification mean something.
void CheckTlsEndToEnd() {
  using namespace platform;
  if (!Have("openssl") || !Have("python3")) {
    std::puts("[ws-tests] tls skipped (no openssl/python3)");
    return;
  }
  std::error_code ignored;
  const std::string dir = MakeTempDir();
  if (dir.empty()) {
    Check(false, "TLS test temp directory");
    return;
  }
  {
    std::ofstream extensions(dir + "/server.ext");
    extensions << "subjectAltName=IP:127.0.0.1\nbasicConstraints=CA:FALSE\n";
  }
  // EC keys keep generation fast. `wrong-ca` never signs anything the server
  // presents.
  const std::string quiet = Quiet();
  const std::string newKey = "openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes ";
  const std::string prefix = ChangeTo() + QuotePath(dir) + " && ";
  const bool generated =
      Run(prefix + newKey + "-x509 -days 1 -subj /CN=mp-test-ca -keyout ca.key -out ca.pem" + quiet) &&
      Run(prefix + newKey +
          "-x509 -days 1 -subj /CN=mp-wrong-ca -keyout wrong-ca.key -out wrong-ca.pem" + quiet) &&
      Run(prefix + newKey + "-subj /CN=127.0.0.1 -keyout server.key -out server.csr" + quiet) &&
      Run(prefix + "openssl x509 -req -days 1 -in server.csr -CA ca.pem -CAkey ca.key " +
          "-CAcreateserial -extfile server.ext -out server.pem" + quiet);
  Check(generated, "openssl CLI generates the test CA and server certificate");
  // Starting four openssl processes and a python server is not instant on a
  // loaded CI machine, and the whole test has a timeout of its own.
  if (!generated) {
    std::filesystem::remove_all(dir, ignored);
    return;
  }
  const uint16_t port = FreeLoopbackPort();
  Check(port != 0, "a free loopback port for the TLS server");
  if (port == 0) {
    std::filesystem::remove_all(dir, ignored);
    return;
  }

  const std::string script = std::string(MP_SOURCE_DIR) + "/tools/ap_fake_server.py";
  const std::string cert = dir + "/server.pem";
  const std::string key = dir + "/server.key";
  const std::string logPath = dir + "/server.log";
  // Bound to all IPv4 interfaces, not just 127.0.0.1, so the mismatch case can
  // connect to another loopback address by number. Resolving a name for that
  // case is what made it flaky: on Windows `localhost` can resolve to ::1,
  // where this IPv4 server is not listening, and the connect times out instead
  // of reaching the certificate check.
  Server server({"python3", script, "--tls", "--cert", cert, "--key", key,
                 "--host", "0.0.0.0", "--port", std::to_string(port)},
                logPath);
  Check(server.started(), "start the TLS fake server");
  if (!server.started()) {
    std::filesystem::remove_all(dir, ignored);
    return;
  }

  PortWs::TlsOptions goodCa;
  goodCa.caFile = dir + "/ca.pem";
  PortWs::Client client;
  bool connected = false;
  // The server needs a moment to start listening; retry until it does.
  if (!WaitForListener(port, 10)) {
    std::ifstream earlyLog(logPath);
    std::fprintf(stderr, "[ws-tests] fake server never listened on %u within 10s; its log was:\n%s\n",
                 port, std::string(std::istreambuf_iterator<char>(earlyLog), {}).c_str());
  }
  // The listener being up does not guarantee the first TLS attempt lands: the
  // server accepts the connect a moment before it can complete the handshake. A
  // short retry covers that without the test outlasting ctest's own timeout -
  // three generous budgets here add up to more than the 15 seconds the target
  // is given, and a slow runner then reports a timeout that says nothing about
  // which budget was exhausted.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
  while (!connected && std::chrono::steady_clock::now() < deadline) {
    connected = client.Connect("127.0.0.1", port, "/", 2000, true, goodCa);
    if (!connected)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (!connected) {
    std::fprintf(stderr, "[ws-tests] wss connect error: %s\n", client.Error());
    std::ifstream earlyLog(logPath);
    std::fprintf(stderr, "[ws-tests] fake server log:\n%s\n",
                 std::string(std::istreambuf_iterator<char>(earlyLog), {}).c_str());
  }
  Check(connected, "wss:// connects and verifies against the test CA");

  std::string roomInfo;
  std::string reply;
  if (connected) {
    Check(client.Compressed(), "the fake server accepts permessage-deflate");
    Check(client.ReceiveText(roomInfo, 3000) && Contains(roomInfo, "\"RoomInfo\""),
          "RoomInfo arrives over TLS");
    Check(client.SendText(R"([{"cmd":"Connect","name":"Player1","password":"","game":"Metroid Prime",)"
                          R"("uuid":"test","version":{"major":0,"minor":6,"build":0,"class":"Version"},)"
                          R"("items_handling":7,"tags":[],"slot_data":false}])"),
          "text frame sends over TLS");
    Check(client.ReceiveText(reply, 3000) && Contains(reply, "\"Connected\""),
          "the server's Connected reply arrives over TLS");
    std::string items;
    Check(client.ReceiveText(items, 3000) && Contains(items, "\"ReceivedItems\""),
          "ReceivedItems follows over TLS");
    // Nothing else is queued now, so the receive timeout must fire rather
    // than block on a TLS read.
    std::string nothing;
    const auto before = std::chrono::steady_clock::now();
    const bool idle = client.ReceiveText(nothing, 300);
    const auto waited = std::chrono::steady_clock::now() - before;
    Check(!idle && client.IsOpen() && Contains(client.Error(), "timed out") &&
              waited < std::chrono::seconds(2),
          "an idle TLS receive times out and keeps the connection");
    client.Close();
    Check(!client.IsOpen(), "Close releases the TLS connection");
  }
  std::printf("[ws-tests] tls round trip: %s\n", reply.empty() ? "(none)" : reply.c_str());

  struct Rejection {
    const char* label;
    const char* host;
    std::string caFile;
    const char* wanted;
  };
  const Rejection rejections[] = {
      {"wrong CA", "127.0.0.1", dir + "/wrong-ca.pem", "certificate verification failed"},
      // 127.0.0.2 is loopback everywhere but is not in the certificate, which
      // covers IP:127.0.0.1 only. A number rather than a name keeps the check
      // from depending on how the machine resolves localhost.
      {"certificate for another address", "127.0.0.2", dir + "/ca.pem",
       "certificate verification failed"},
      {"system trust store", "127.0.0.1", "", "certificate verification failed"},
      {"missing CA file", "127.0.0.1", dir + "/missing.pem", "could not load TLS CA file"},
  };
  for (const Rejection& rejection : rejections) {
#ifdef __APPLE__
    // macOS's lo0 only answers on 127.0.0.1, so 127.0.0.2 just times out.
    if (std::strcmp(rejection.host, "127.0.0.2") == 0)
      continue;
#endif
    PortWs::TlsOptions options;
    options.caFile = rejection.caFile;
    PortWs::Client rejected;
    const bool accepted = rejected.Connect(rejection.host, port, "/", 3000, true, options);
    const std::string error = rejected.Error();
    std::printf("[ws-tests] tls %s rejected: %s\n", rejection.label, error.c_str());
    Check(!accepted && !rejected.IsOpen() && Contains(error, rejection.wanted),
          "TLS rejects an unverifiable server with a readable error");
  }

  // caDirs, laid out the way Android lays out its store: one PEM per file,
  // named <hash>.0, each followed by a text dump of the certificate. The names
  // are deliberately not the subject hash OpenSSL would look up, so these pass
  // only if every file is read. The junk file, the subdirectory and the dump
  // must be skipped rather than fatal.
  namespace fs = std::filesystem;
  auto caDir = [&](const std::string& name, const std::string& pem) {
    const fs::path path = fs::path(dir) / name;
    fs::create_directories(path);
    if (!pem.empty()) {
      std::ifstream source(fs::path(dir) / pem, std::ios::binary);
      std::ofstream target(path / "00000000.0", std::ios::binary);
      target << source.rdbuf() << "Certificate:\n    Data:\n        Version: 3 (0x2)\n";
    }
    return path.string();
  };
  const std::string goodDir = caDir("dir-good", "ca.pem");
  const std::string wrongDir = caDir("dir-wrong", "wrong-ca.pem");
  const std::string emptyDir = caDir("dir-empty", "");
  const std::string junkDir = caDir("dir-junk", "");
  const std::string missingDir = (fs::path(dir) / "dir-missing").string();
  for (const std::string& junkIn : {goodDir, junkDir}) {
    std::ofstream(fs::path(junkIn) / "README") << "not a certificate\n-----BEGIN NOTHING-----\n";
    fs::create_directories(fs::path(junkIn) / "subdir");
  }
  auto connectWith = [&](std::vector<std::string> dirs, const std::string& caFile, std::string& error) {
    PortWs::TlsOptions options;
    options.caFile = caFile;
    options.caDirs = std::move(dirs);
    PortWs::Client tlsClient;
    std::string first;
    const bool ok = tlsClient.Connect("127.0.0.1", port, "/", 3000, true, options) &&
                    tlsClient.ReceiveText(first, 3000) && Contains(first, "\"RoomInfo\"");
    error = tlsClient.Error();
    tlsClient.Close();
    return ok;
  };
  std::string dirError;
  Check(connectWith({goodDir}, "", dirError),
        "caDirs: a <hash>.0 CA beside a junk file and a text dump loads and verifies");
  Check(!connectWith({emptyDir, missingDir}, "", dirError) &&
            Contains(dirError, "no TLS root certificates") && Contains(dirError, emptyDir.c_str()) &&
            Contains(dirError, missingDir.c_str()) && Contains(dirError, "loaded 0") &&
            !Contains(dirError, "certificate verification failed"),
        "caDirs: no certificate anywhere is a loud error naming every directory");
  std::printf("[ws-tests] tls empty caDirs rejected: %s\n", dirError.c_str());
  Check(!connectWith({junkDir}, "", dirError) && Contains(dirError, "no TLS root certificates"),
        "caDirs: a directory of junk holds no certificates");
  Check(!connectWith({wrongDir}, "", dirError) && Contains(dirError, "certificate verification failed"),
        "caDirs: the wrong CA fails verification");
  Check(connectWith({emptyDir, goodDir}, "", dirError),
        "caDirs: an empty directory falls through to the next");
  Check(!connectWith({wrongDir, goodDir}, "", dirError) &&
            Contains(dirError, "certificate verification failed"),
        "caDirs: the first directory with a certificate wins; later ones are not merged");
  Check(!connectWith({goodDir}, dir + "/wrong-ca.pem", dirError) &&
            Contains(dirError, "certificate verification failed"),
        "caFile takes precedence over caDirs");

  // The server is still healthy after turning away bad handshakes.
  PortWs::Client again;
  Check(again.Connect("127.0.0.1", port, "/", 3000, true, goodCa) && again.ReceiveText(roomInfo, 3000),
        "the TLS server still serves a verified client after rejections");
  again.Close();

  server.Stop();
  if (!sPassed) {
    std::ifstream log(logPath);
    std::fprintf(stderr, "[ws-tests] fake server log:\n%s\n",
                 std::string(std::istreambuf_iterator<char>(log), {}).c_str());
  }
  std::filesystem::remove_all(dir, ignored);
}
#endif

} // namespace

int main() {
  uint8_t digest[20];
  PortWs::Sha1("", 0, digest);
  Check(Hex(digest, sizeof(digest)) == "da39a3ee5e6b4b0d3255bfef95601890afd80709",
        "SHA-1 empty-string vector");
  PortWs::Sha1("abc", 3, digest);
  Check(Hex(digest, sizeof(digest)) == "a9993e364706816aba3e25717850c26c9cd0d89d",
        "SHA-1 abc vector");
  const std::string rfc448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  PortWs::Sha1(rfc448.data(), rfc448.size(), digest);
  Check(Hex(digest, sizeof(digest)) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1",
        "SHA-1 RFC 3174 448-bit padding vector");

  Check(PortWs::Base64Encode("", 0).empty(), "base64 empty vector");
  Check(PortWs::Base64Encode("f", 1) == "Zg==" && PortWs::Base64Encode("fo", 2) == "Zm8=" &&
            PortWs::Base64Encode("foo", 3) == "Zm9v" &&
            PortWs::Base64Encode("foob", 4) == "Zm9vYg==" &&
            PortWs::Base64Encode("fooba", 5) == "Zm9vYmE=" &&
            PortWs::Base64Encode("foobar", 6) == "Zm9vYmFy",
        "base64 standard vectors and padding");
  Check(PortWs::AcceptKey("dGhlIHNhbXBsZSBub25jZQ==") ==
            "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
        "RFC 6455 Sec-WebSocket-Accept example");

  struct UrlCase {
    const char* url;
    bool accepted;
    const char* host;
    uint16_t port;
    const char* path;
  };
  const UrlCase urls[] = {
      {"ws://example.com", true, "example.com", 80, "/"},
      {"ws://example.com:8080", true, "example.com", 8080, "/"},
      {"ws://example.com:1234/path?q=1", true, "example.com", 1234, "/path?q=1"},
      {"ws://[::1]:9000/chat", true, "::1", 9000, "/chat"},
      {"wss://example.com", false, "", 0, ""},
      {"http://example.com", false, "", 0, ""},
      {"ws://", false, "", 0, ""},
      {"ws://:80/path", false, "", 0, ""},
      {"ws://example.com:abc", false, "", 0, ""},
      {"ws://example.com:0", false, "", 0, ""},
      {"ws://example.com:65536", false, "", 0, ""},
      {"ws://example.com:", false, "", 0, ""},
      {"ws://user@example.com", false, "", 0, ""},
      {"ws://example.com?x=1", false, "", 0, ""},
      {"ws://example.com/a#frag", false, "", 0, ""},
  };
  for (const UrlCase& test : urls) {
    std::string host = "unchanged";
    uint16_t port = 444;
    std::string path = "unchanged";
    const bool accepted = PortWs::ParseUrl(test.url, host, port, path);
    Check(accepted == test.accepted, "ParseUrl acceptance table");
    if (accepted)
      Check(host == test.host && port == test.port && path == test.path,
            "ParseUrl host, port, and path values");
    // The scheme-reporting overload agrees on every ws:// form.
    if (std::string(test.url).rfind("ws://", 0) == 0) {
      std::string secureHost;
      uint16_t securePort = 0;
      std::string securePath;
      bool secure = true;
      const bool secureAccepted = PortWs::ParseUrl(test.url, secureHost, securePort, securePath, secure);
      Check(secureAccepted == test.accepted && (!secureAccepted ||
                (!secure && secureHost == host && securePort == port && securePath == path)),
            "ParseUrl secure overload matches the ws:// table");
    }
  }

  struct SecureUrlCase {
    const char* url;
    bool accepted;
    bool secure;
    const char* host;
    uint16_t port;
    const char* path;
  };
  const SecureUrlCase secureUrls[] = {
      {"ws://example.com", true, false, "example.com", 80, "/"},
      {"wss://example.com", true, true, "example.com", 443, "/"},
      {"wss://example.com:38281", true, true, "example.com", 38281, "/"},
      {"wss://archipelago.gg:38281/room/abc?x=1", true, true, "archipelago.gg", 38281, "/room/abc?x=1"},
      {"wss://[::1]:9000/chat", true, true, "::1", 9000, "/chat"},
      {"wss://127.0.0.1:443/", true, true, "127.0.0.1", 443, "/"},
      {"wss://", false, false, "", 0, ""},
      {"wss://:443/path", false, false, "", 0, ""},
      {"wss://example.com:", false, false, "", 0, ""},
      {"wss://example.com:0", false, false, "", 0, ""},
      {"wss://example.com:65536", false, false, "", 0, ""},
      {"wss://user@example.com", false, false, "", 0, ""},
      {"wss://example.com/a#frag", false, false, "", 0, ""},
      {"wsss://example.com", false, false, "", 0, ""},
      {"https://example.com", false, false, "", 0, ""},
      {"ftp://example.com", false, false, "", 0, ""},
      {"wss:/example.com", false, false, "", 0, ""},
  };
  for (const SecureUrlCase& test : secureUrls) {
    std::string host = "unchanged";
    uint16_t port = 444;
    std::string path = "unchanged";
    bool secure = !test.secure;
    const bool accepted = PortWs::ParseUrl(test.url, host, port, path, secure);
    Check(accepted == test.accepted, "ParseUrl wss:// acceptance table");
    if (accepted)
      Check(secure == test.secure && host == test.host && port == test.port && path == test.path,
            "ParseUrl wss:// scheme, host, port, and path values");
    else
      Check(host == "unchanged" && port == 444 && path == "unchanged",
            "rejected URLs leave the outputs alone");
  }

  if (!PortWs::TlsAvailable()) {
    PortWs::Client client;
    Check(!client.Connect("127.0.0.1", 9, "/", 500, true) && !client.IsOpen() &&
              std::string(client.Error()).find("no TLS") != std::string::npos,
          "a build without OpenSSL refuses wss:// instead of connecting in plaintext");
  }

  const std::string encodedSmall = PortWs::EncodeFrame(1, "HI", 0x04030201);
  const std::string handComputed("\x81\x82\x01\x02\x03\x04\x49\x4b", 8);
  Check(encodedSmall == handComputed &&
            encodedSmall == PortWs::EncodeFrame(1, "HI", 0x04030201),
        "deterministic masked frame matches hand-computed bytes");

  CheckRoundTrip(0);
  CheckRoundTrip(125);
  CheckRoundTrip(126);
  CheckRoundTrip(65535);
  CheckRoundTrip(65536);

  PortWs::FrameDecoder fragmented;
  std::string fragments;
  fragments += ServerFrame(1, false, "hel");
  fragments += ServerFrame(9, true, "p");
  fragments += ServerFrame(0, true, "lo");
  std::vector<PortWs::Frame> fragmentFrames;
  for (size_t i = 0; i < fragments.size(); ++i)
    fragmented.Feed(fragments.data() + i, 1, fragmentFrames);
  Check(!fragmented.Failed() && fragmentFrames.size() == 2 &&
            fragmentFrames[0].opcode == 9 && fragmentFrames[0].payload == "p" &&
            fragmentFrames[1].opcode == 1 && fragmentFrames[1].payload == "hello",
        "fragment reassembly with interleaved ping frame");

  const std::string maskedServer = PortWs::EncodeFrame(1, "masked", 0xdeadbeef);
  PortWs::FrameDecoder maskedDecoder;
  std::vector<PortWs::Frame> maskedFrames;
  maskedDecoder.Feed(maskedServer.data(), maskedServer.size(), maskedFrames);
  Check(!maskedDecoder.Failed() && maskedFrames.size() == 1 &&
            maskedFrames[0].opcode == 1 && maskedFrames[0].payload == "masked",
        "decoder accepts masked server frames");

  std::string oversized("\x81\x7f", 2);
  const uint64_t overLimit = PortWs::FrameDecoder::kMaxMessageSize + 1;
  for (int i = 7; i >= 0; --i)
    oversized.push_back(static_cast<char>((overLimit >> (i * 8)) & 0xff));
  PortWs::FrameDecoder oversizedDecoder;
  std::vector<PortWs::Frame> oversizedFrames;
  oversizedDecoder.Feed(oversized.data(), oversized.size(), oversizedFrames);
  Check(oversizedDecoder.Failed(), "oversized message latches decoder failure");

  CheckDeflate();

#ifndef _WIN32
  {
    // The kernel completes the TCP handshake from the backlog, but nobody
    // answers the upgrade, so only the cancel flag can end this before the
    // 10 s timeout.
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    const bool listening =
        listener >= 0 && ::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
        ::listen(listener, 1) == 0 &&
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0;
    Check(listening, "a silent loopback listener for the cancel test");
    if (listening) {
      std::atomic<bool> cancel{false};
      PortWs::Client client;
      client.SetCancelFlag(&cancel);
      std::thread canceller([&cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        cancel.store(true, std::memory_order_release);
      });
      const auto start = std::chrono::steady_clock::now();
      const bool connected = client.Connect("127.0.0.1", ntohs(address.sin_port), "/", 10000);
      const auto elapsed = std::chrono::steady_clock::now() - start;
      canceller.join();
      Check(!connected && !client.IsOpen() && elapsed < std::chrono::seconds(2),
            "a set cancel flag ends a stalled handshake well before its timeout");
    }
    if (listener >= 0)
      ::close(listener);
  }
#endif

#if defined(MP_HAVE_OPENSSL)
  CheckTlsEndToEnd();
#endif

  if (!sPassed)
    return 1;
  std::puts("[ws-tests] passed");
  return 0;
}
