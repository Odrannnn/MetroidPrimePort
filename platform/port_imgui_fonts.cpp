// The overlay's font. ImGui's built-in ProggyClean covers only U+0020-00FF, so
// game text the overlay shows (room and world names in the resume prompt,
// Travel, save states) drew every other character as '?': all of a translation
// mod's Cyrillic, and the curly quotes and dashes of the stock strings. A
// system sans that has them is merged in for those ranges; ProggyClean keeps
// the rest, so the overlay looks as it did wherever it already drew right.
#include "port_imgui_fonts.h"

#include "port_log.h"

#include <imgui.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace PortImGuiFonts {
namespace {

namespace fs = std::filesystem;

// ProggyClean's size; the merged glyphs match it.
constexpr float kFontPixels = 13.f;

const ImWchar kRanges[] = {
    0x0100, 0x024F, // Latin Extended-A and -B
    0x0370, 0x03FF, // Greek
    0x0400, 0x052F, // Cyrillic and its supplement
    0x2010, 0x205E, // general punctuation: dashes, curly quotes, ellipsis
    0x20AC, 0x20AC, // euro sign
    0x2122, 0x2122, // trade mark sign
    0,
};

bool IsFile(const fs::path& path) {
  std::error_code error;
  return fs::is_regular_file(path, error);
}

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__ANDROID__)
// Font file names in order of preference, looked for anywhere under the
// font directories: distributions file the same font under different subdirs
// (TTF/, truetype/dejavu/, dejavu/), and a Flatpak sees its runtime's fonts
// and the host's (/run/host/fonts).
std::string FindLinuxFont() {
  const char* const names[] = {
      "DejaVuSans.ttf",
      "NotoSans-Regular.ttf",
      "LiberationSans-Regular.ttf",
      "FreeSans.ttf",
  };
  std::vector<fs::path> roots;
  const auto addDataDirs = [&roots](const char* value) {
    std::string dirs = value != nullptr ? value : "";
    size_t start = 0;
    while (start <= dirs.size()) {
      const size_t end = std::min(dirs.find(':', start), dirs.size());
      if (end > start) {
        roots.emplace_back(fs::path(dirs.substr(start, end - start)) / "fonts");
      }
      start = end + 1;
    }
  };
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    roots.emplace_back(fs::path(home) / ".local/share/fonts");
    roots.emplace_back(fs::path(home) / ".fonts");
  }
  addDataDirs(std::getenv("XDG_DATA_DIRS"));
  roots.emplace_back("/usr/share/fonts");
  roots.emplace_back("/usr/local/share/fonts");
  roots.emplace_back("/run/host/fonts");

  std::string found[std::size(names)];
  for (size_t r = 0; r < roots.size(); ++r) {
    const fs::path& root = roots[r];
    if (std::find(roots.begin(), roots.begin() + r, root) != roots.begin() + r) {
      continue;
    }
    std::error_code error;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error);
    for (; !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
      const std::string name = it->path().filename().string();
      for (size_t i = 0; i < std::size(names); ++i) {
        if (found[i].empty() && name == names[i] && IsFile(it->path())) {
          found[i] = it->path().string();
        }
      }
    }
    if (!found[0].empty()) {
      break;
    }
  }
  for (const std::string& path : found) {
    if (!path.empty()) {
      return path;
    }
  }
  return {};
}
#endif

std::string FindFont() {
#if defined(_WIN32)
  const char* windir = std::getenv("WINDIR");
  const fs::path dir = fs::path(windir != nullptr && *windir != '\0' ? windir : "C:\\Windows") / "Fonts";
  for (const char* name : {"segoeui.ttf", "arial.ttf", "tahoma.ttf"}) {
    if (IsFile(dir / name)) {
      return (dir / name).string();
    }
  }
  return {};
#elif defined(__APPLE__)
  for (const char* path : {"/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf",
                           "/System/Library/Fonts/Supplemental/Verdana.ttf"}) {
    if (IsFile(path)) {
      return path;
    }
  }
  return {};
#elif defined(__ANDROID__)
  for (const char* path : {"/system/fonts/Roboto-Regular.ttf", "/system/fonts/NotoSans-Regular.ttf",
                           "/system/fonts/DroidSans.ttf"}) {
    if (IsFile(path)) {
      return path;
    }
  }
  return {};
#else
  return FindLinuxFont();
#endif
}

} // namespace

void Init(const AuroraWindowSize*) {
  ImFontAtlas* atlas = ImGui::GetIO().Fonts;
  atlas->AddFontDefault();
  const std::string path = FindFont();
  if (path.empty()) {
    PortLog::Write("overlay font: no system font with Cyrillic found; non-Latin text shows as '?'\n");
    return;
  }
  ImFontConfig config;
  config.MergeMode = true;
  config.PixelSnapH = true;
  // A file stb_truetype can't parse fails the whole atlas, which would leave
  // the overlay with no font at all: build now and drop it if so.
  if (atlas->AddFontFromFileTTF(path.c_str(), kFontPixels, &config, kRanges) == nullptr || !atlas->Build()) {
    PortLog::Write("overlay font: could not load %s\n", path.c_str());
    atlas->Clear();
    atlas->AddFontDefault();
    return;
  }
  PortLog::Write("overlay font: extra glyphs from %s\n", path.c_str());
}

} // namespace PortImGuiFonts
