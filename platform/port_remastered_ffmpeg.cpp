// The part of the movie import that runs ffmpeg (see port_remastered_movie.h).

#include "port_remastered_movie.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>

#include <SDL3/SDL.h>

namespace PortRemastered {
namespace {

SDL_Process* Start(const std::vector<std::string>& arguments, bool piped) {
  std::vector<const char*> args;
  for (const std::string& argument : arguments) {
    args.push_back(argument.c_str());
  }
  args.push_back(nullptr);
  const SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, const_cast<char**>(args.data()));
  SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
  SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
                        piped ? SDL_PROCESS_STDIO_APP : SDL_PROCESS_STDIO_NULL);
  // ffmpeg's complaints go to the port's own log; a probe's are of no interest.
  SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER,
                        piped ? SDL_PROCESS_STDIO_INHERITED : SDL_PROCESS_STDIO_NULL);
  // Without this, Windows opens a console window for every run (aurora's SDL patch).
  SDL_SetBooleanProperty(props, "SDL.process.create.windows.no_window", true);
  SDL_Process* process = SDL_CreateProcessWithProperties(props);
  SDL_DestroyProperties(props);
  return process;
}

bool Runs(const std::string& program) {
  SDL_Process* process = Start({program, "-version"}, false);
  if (process == nullptr) {
    return false;
  }
  int code = -1;
  const bool exited = SDL_WaitProcess(process, true, &code);
  SDL_DestroyProcess(process);
  return exited && code == 0;
}

} // namespace

std::string FindFfmpeg() {
  if (const char* env = std::getenv("MP_FFMPEG"); env != nullptr && env[0] != '\0') {
    return Runs(env) ? std::string(env) : std::string();
  }
#ifdef _WIN32
  const char* name = "ffmpeg.exe";
#else
  const char* name = "ffmpeg";
#endif
  // One shipped with the port comes before whatever the system has.
  if (const char* base = SDL_GetBasePath(); base != nullptr) {
    std::string beside = std::string(base) + name;
#ifdef __APPLE__
    // In an app bundle the base path is Contents/Resources/, and programs live
    // in Contents/MacOS/ beside the port.
    if (std::string_view(base).ends_with("/Contents/Resources/"))
      beside = std::string(base) + "../MacOS/" + name;
#endif
    std::error_code ec;
    if (std::filesystem::exists(std::filesystem::path(std::u8string(beside.begin(), beside.end())), ec) &&
        Runs(beside)) {
      return beside;
    }
  }
  return Runs(name) ? std::string(name) : std::string();
}

bool ConvertMovie(const std::string& ffmpeg, const std::string& mp4, const std::string& thp, const MovieFormat& format,
                  const std::function<bool()>& cancelled, int& frames, std::string& error) {
  // The console's decoder takes baseline JPEG, 4:2:0, with the standard Huffman
  // tables, and the game converts the picture as BT.601 over the full range.
  const std::string filter = "fps=" + std::to_string(format.fps) + ",scale=" + std::to_string(format.width) + ":" +
                             std::to_string(format.height) + ":flags=lanczos:out_color_matrix=bt601:out_range=pc";
  SDL_Process* process =
      Start({ffmpeg, "-v", "error", "-nostdin", "-i", mp4, "-an", "-vf", filter, "-c:v", "mjpeg", "-pix_fmt",
             "yuvj420p", "-huffman", "default", "-q:v", "3", "-f", "image2pipe", "-"},
            true);
  if (process == nullptr) {
    error = std::string("could not start ffmpeg: ") + SDL_GetError();
    return false;
  }
  const std::filesystem::path path(std::u8string(thp.begin(), thp.end()));
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  bool ok = bool(out);
  if (!ok) {
    error = "cannot write the movie";
  }
  ThpWriter writer(out, format.width, format.height, float(format.fps));
  JpegSplitter splitter;
  const JpegSplitter::Sink sink = [&](const std::vector<uint8_t>& jpeg) {
    writer.Add(jpeg);
    return bool(out);
  };
  SDL_IOStream* output = SDL_GetProcessOutput(process);
  std::vector<uint8_t> buffer(1 << 16);
  bool stopped = false;
  while (ok && output != nullptr) {
    if (cancelled && cancelled()) {
      stopped = true;
      break;
    }
    const size_t got = SDL_ReadIO(output, buffer.data(), buffer.size());
    if (got == 0) {
      if (SDL_GetIOStatus(output) == SDL_IO_STATUS_NOT_READY) {
        SDL_Delay(1);
        continue;
      }
      break;
    }
    if (!splitter.Feed(buffer.data(), got, sink)) {
      ok = false;
      error = out ? "ffmpeg's output is not a stream of JPEGs" : "cannot write the movie";
    }
  }
  if (!ok || stopped) {
    SDL_KillProcess(process, true);
  }
  int code = -1;
  SDL_WaitProcess(process, true, &code);
  SDL_DestroyProcess(process);
  if (stopped) {
    ok = false;
    error = "cancelled";
  } else if (ok && code != 0) {
    ok = false;
    error = "ffmpeg failed (exit code " + std::to_string(code) + ")";
  } else if (ok && (!splitter.Idle() || !writer.Finish())) {
    ok = false;
    error = writer.Frames() == 0 ? "ffmpeg decoded no frames" : "the movie was cut short";
  }
  frames = writer.Frames();
  out.close();
  if (!ok) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
  return ok;
}

} // namespace PortRemastered
