// Port entry point. Aurora owns the real process entry (aurora_main) and the
// window/GPU/input/audio backend; this file initializes it, mounts the user's
// disc, and hands control to the game.
//
// Disc path resolution: first non-flag argument, then $MP_DISC.

#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <aurora/gfx.h>
#include <aurora/event.h>
#include <aurora/main.h>
#include <dolphin/gx.h>
#include <dolphin/vi.h>
#include <dolphin/dvd.h>

#include "port_env.h"
#include "port_strings.h"
#include "port_embedded.h"
#include "port_crash.h"
#include "port_disc.h"
#include "port_gci.h"
#include "port_debug.h"
#include "port_graphics_backend.h"
#include "port_paths.h"
#include "port_actor_collision_bounds.h"
#include "port_apclient.h"
#include "port_randomizer.h"
#include "port_textures.h"
#include "port_prompts.h"
#include "port_build_info.h"
#include "port_disc_replace.h"
#include "port_disc_verify.h"
#include "port_log.h"
#include "port_log_file.h"
#include "port_memory_log.h"
#include "port_mods.h"
#include "port_pal_languages.h"
#include "port_flaahgra_music.h"
#include "port_room_geo.h"
#include "port_importers.h"
#include "port_remastered_import.h"
#include "port_storage_report.h"
#include "port_gpu_driver.h"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_platform_defines.h>
#if defined(SDL_PLATFORM_IOS)
#include "port_ios.h"
#endif
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_timer.h>

#if defined(__ANDROID__)
#include <SDL3/SDL_system.h>
#include <android/log.h>
#include <sys/system_properties.h>
#endif
#if (defined(__linux__) && !defined(__ANDROID__)) || defined(__APPLE__)
#include <sys/utsname.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

extern "C" int metroid_main(int argc, char** argv);
extern "C" void AIPortShutdown(void);

namespace {
#if defined(__ANDROID__)
// Aurora logs to stderr, which Android discards. Send it to logcat instead so
// the Vulkan/audio/disc diagnostics are actually reachable on a device.
void AndroidLogCallback(AuroraLogLevel level, const char* module, const char* message,
                        unsigned int len) {
    int priority = ANDROID_LOG_INFO;
    switch (level) {
    case LOG_DEBUG:
        priority = ANDROID_LOG_DEBUG;
        break;
    case LOG_WARNING:
        priority = ANDROID_LOG_WARN;
        break;
    case LOG_ERROR:
        priority = ANDROID_LOG_ERROR;
        break;
    case LOG_FATAL:
        priority = ANDROID_LOG_FATAL;
        break;
    case LOG_INFO:
    default:
        break;
    }
    char line[2048];
    std::snprintf(line, sizeof(line), "[%s] %.*s", module != nullptr ? module : "", static_cast< int >(len),
                  message);
    __android_log_write(priority, "aurora", line);
    PortLogFile::Write("aurora", line);
}
#else
// Aurora's default logger is fmt::println, which throws when stderr's write
// fails (the log copier gone, EPIPE) from inside a noexcept function: the game
// aborted at shutdown. Same format; a failed write just drops the line.
void StderrLogCallback(AuroraLogLevel level, const char* module, const char* message, unsigned int len) {
    const char* name = "unknown";
    switch (level) {
    case LOG_DEBUG:
        name = "debug";
        break;
    case LOG_INFO:
        name = "info";
        break;
    case LOG_WARNING:
        name = "warning";
        break;
    case LOG_ERROR:
        name = "error";
        break;
    case LOG_FATAL:
        name = "fatal";
        break;
    default:
        break;
    }
    std::fprintf(stderr, "[%s] [%s] %.*s\n", name, module != nullptr ? module : "", static_cast< int >(len), message);
}
#endif

std::string LowerExtension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext;
}

bool IsDiscImage(const std::filesystem::path& path) {
    static const char* const kExtensions[] = {".iso", ".gcm", ".rvz", ".wbfs", ".ciso", ".nkit"};
    const std::string ext = LowerExtension(path);
    for (const char* candidate : kExtensions) {
        if (ext == candidate) {
            return true;
        }
    }
    return false;
}

// How well a file next to the executable fits as the disc: plain images (.iso,
// .gcm, NKit's .nkit.iso) start with the disc header, so the wrong game or
// region can be skipped before it fails to boot; the compressed formats keep
// it elsewhere and are taken on trust, after any plain image that matched.
bool IsSupportedId(const char* id6, unsigned diskNumber, unsigned version) {
    return PortDiscReplace::IsSupportedId(id6, diskNumber, version);
}

enum class DiscMatch { No, Maybe, Yes };

DiscMatch MatchDiscImage(const std::filesystem::path& path) {
    if (!IsDiscImage(path)) {
        return DiscMatch::No;
    }
    const std::string ext = LowerExtension(path);
    if (ext != ".iso" && ext != ".gcm") {
        return DiscMatch::Maybe;
    }
    // Through SDL with a UTF-8 name: std::fopen on Windows takes the ANSI code
    // page, which cannot name every folder a user might unpack the game into.
    SDL_IOStream* file = SDL_IOFromFile(PortPaths::detail::ToUtf8(path).c_str(), "rb");
    if (file == nullptr) {
        return DiscMatch::No;
    }
    Uint8 header[8] = {};
    const size_t got = SDL_ReadIO(file, header, sizeof(header));
    SDL_CloseIO(file);
    // Game id, maker, disc number, revision.
    return got == sizeof(header) && IsSupportedId(reinterpret_cast<const char*>(header), header[6], header[7])
               ? DiscMatch::Yes
               : DiscMatch::No;
}

// Looks for a disc image next to the executable (and in its immediate
// subdirectories) so a copied build is self-contained and starts without
// asking. For an AppImage that is the folder the .AppImage file is in. On iOS,
// whose app bundle is signed and read-only, it is the Documents folder the
// Files app shows, so an image copied there starts without asking.
std::string FindDiscNextToExecutable() {
#if defined(__ANDROID__)
    const char* rawBase = SDL_GetBasePath();
    const std::string base = rawBase != nullptr ? rawBase : "";
#elif defined(SDL_PLATFORM_IOS)
    const std::string base = PortIos::DocumentsFolder();
#else
    const std::string base = PortPaths::detail::ExecutableFolder();
#endif
    if (base.empty()) {
        return {};
    }
    namespace fs = std::filesystem;
    // The iterator and the per-entry checks keep separate error codes: an entry
    // whose status cannot be read (a symlink into a directory without access)
    // fails its own check and is skipped, where a shared code would end the
    // whole search before a good image further on.
    // Path conversions can still throw (a name the narrow encoding cannot hold),
    // and this runs before anything that could report it, so nothing escapes.
    try {
        std::error_code ec;
        const fs::path baseDir = PortPaths::detail::FromUtf8(base);
        std::vector<fs::path> dirs{baseDir};
        for (fs::directory_iterator it(baseDir, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code entryEc;
            if (it->is_directory(entryEc)) {
                dirs.push_back(it->path());
            }
        }
        // Directory order is the file system's; sorted, the same image wins on
        // every launch when there are several.
        std::sort(dirs.begin() + 1, dirs.end());
        std::string maybe;
        for (const fs::path& dir : dirs) {
            std::vector<fs::path> files;
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                std::error_code entryEc;
                if (it->is_regular_file(entryEc) && IsDiscImage(it->path())) {
                    files.push_back(it->path());
                }
            }
            std::sort(files.begin(), files.end());
            for (const fs::path& file : files) {
                const DiscMatch match = MatchDiscImage(file);
                if (match == DiscMatch::Yes) {
                    return PortPaths::detail::ToUtf8(file);
                }
                if (match == DiscMatch::Maybe && maybe.empty()) {
                    maybe = PortPaths::detail::ToUtf8(file);
                }
            }
        }
        if (!maybe.empty()) {
            return maybe;
        }
    } catch (const std::exception& e) {
        PortLog::Write("metroid_prime_port: searching for a disc image failed: %s\n", e.what());
    }
    return {};
}

// MP_DISC, or nullptr when it is unset or empty.
const char* DiscFromEnvironment() {
    const char* env = port::EnvString("MP_DISC");
    return env != nullptr && env[0] != '\0' ? env : nullptr;
}

const char* ResolveDiscPath(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-' && argv[i][0] != '\0') {
            return argv[i];
        }
    }
    if (const char* env = DiscFromEnvironment()) {
        return env;
    }
    if (const char* saved = PortDebug::DiscPath(); saved != nullptr) {
#if defined(__ANDROID__)
        if (std::strncmp(saved, "content://", 10) == 0) {
            // A URI from before the copy existed. Prefer the local copy, which
            // needs no permission grant; fall back to the URI if it is not there
            // yet, since the grant may still be live.
            const std::string local = PortPaths::UserFolder() + "disc.iso";
            std::error_code ec;
            static const std::string sLocal =
                !PortPaths::UserFolder().empty() && std::filesystem::exists(local, ec) ? local : std::string();
            if (!sLocal.empty()) {
                return sLocal.c_str();
            }
            return saved;
        }
        // The copy made from a picked file sits in the data folder, which may
        // have moved since (port_data_folder.h): prefer the copy in the folder
        // in use, so deleting the old one loses nothing.
        if (const char* name = std::strrchr(saved, '/'); name != nullptr && std::strcmp(name, "/disc.iso") == 0) {
            const std::string moved = PortPaths::UserFolder() + "disc.iso";
            std::error_code ec;
            static const std::string sMoved =
                !PortPaths::UserFolder().empty() && moved != saved && std::filesystem::exists(moved, ec) ? moved
                                                                                                         : std::string();
            if (!sMoved.empty()) {
                return sMoved.c_str();
            }
        }
#endif
        // The error_code overload: the throwing one would end the program on a
        // remembered path that is merely unreadable, instead of moving on.
        std::error_code ec;
        if (std::filesystem::exists(saved, ec)) {
            return saved;
        }
    }
    static const std::string sFound = FindDiscNextToExecutable();
    if (sFound.empty()) {
        return nullptr;
    }
    PortLog::Write("metroid_prime_port: using the disc image next to the executable: %s\n", sFound.c_str());
    return sFound.c_str();
}

// aurora_dvd_open reports failure for three different reasons - the file would
// not open, the disc parser rejected it, or the data partition was missing -
// and says which of them nowhere. Splitting them here turns an unexplained
// exit into a specific, actionable line.
void ReportDiscOpenFailure(const char* path) {
    PortLog::Write( "metroid_prime_port: failed to open disc image: %s\n", path);
    SDL_ClearError();
    SDL_IOStream* probe = SDL_IOFromFile(path, "rb");
    if (probe == nullptr) {
        PortLog::Write( "  the file itself could not be opened: %s\n", SDL_GetError());
        return;
    }
    const Sint64 size = SDL_GetIOSize(probe);
    Uint8 header[8] = {};
    const size_t got = SDL_ReadIO(probe, header, sizeof(header));
    SDL_CloseIO(probe);
    if (got != sizeof(header)) {
        PortLog::Write( "  the file opened but is only %lld bytes: too short to be a disc\n",
                        static_cast<long long>(size));
        return;
    }
    PortLog::Write( "  the file opened and is %lld bytes, so the disc parser rejected it\n",
                    static_cast<long long>(size));
    PortLog::Write( "  first bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n", header[0], header[1],
                    header[2], header[3], header[4], header[5], header[6], header[7]);
}

// Whether the disc was named on the command line or by MP_DISC, as opposed to
// remembered, found or picked: a wrong one there is the caller's to fix.
bool ResolveDiscFromArgs(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-' && argv[i][0] != '\0') {
            return true;
        }
    }
    return DiscFromEnvironment() != nullptr;
}

// The disc id's game name and maker as one six-character id.
std::array<char, 6> DiscId6(const DVDDiskID& id) {
    std::array<char, 6> id6{};
    std::memcpy(id6.data(), id.gameName, 4);
    std::memcpy(id6.data() + 4, id.company, 2);
    return id6;
}

bool IsSupportedDisc(const DVDDiskID* id) {
    return id != nullptr && IsSupportedId(DiscId6(*id).data(), id->diskNumber, id->gameVersion);
}

// aurora_dvd_open, then for Metroid Prime Trilogy its Metroid Prime: the MP1
// directory as the root (the paks the game opens) and rs5mp1_p.dol as the DOL
// (the embedded font and tables), so the rest reads it like a GameCube disc.
bool OpenDiscImage(const char* path) {
    if (!aurora_dvd_open(path)) {
        return false;
    }
    if (PortDisc::Current() == PortDisc::Version::Trilogy && !aurora_dvd_set_root("MP1", "rs5mp1_p.dol")) {
        PortLog::Write("metroid_prime_port: the Trilogy disc has no MP1 directory or rs5mp1_p.dol\n");
        aurora_dvd_close();
        return false;
    }
    return true;
}

// A compressed image (RVZ, WIA, GCZ) cut short by an interrupted download or
// copy still opens, since the header is at the front, and the game only finds
// out a few frames in, when a read fails (issue #8). Containers store the disc
// in order, so a cut loses the files that end last first: reading the last
// byte of those catches it at the disc check. (Reading every file's last byte
// took 4 s for an LZMA RVZ on a desktop.)
// Returns the first file that can't be read, or an empty string.
std::string FindUnreadableDiscFile() {
    constexpr size_t kFilesToCheck = 8;
    const Uint64 start = SDL_GetTicks();
    const s32 count = aurora_dvd_base_entry_count();
    std::vector<std::pair<int64_t, s32>> ends; // (end offset, entry), the last ones first
    for (s32 entry = 1; entry < count; ++entry) {
        const int64_t offset = aurora_dvd_base_offset(entry);
        if (offset < 0) {
            continue; // a directory
        }
        void* file = aurora_dvd_base_open(entry);
        const int64_t size = file != nullptr ? aurora_dvd_base_seek(file, 0, SEEK_END) : -1;
        aurora_dvd_base_close(file);
        ends.emplace_back(offset + std::max<int64_t>(size, 0), entry);
    }
    std::sort(ends.begin(), ends.end(), std::greater<>());
    ends.resize(std::min(ends.size(), kFilesToCheck));
    for (const auto& [end, entry] : ends) {
        void* file = aurora_dvd_base_open(entry);
        const int64_t size = file != nullptr ? aurora_dvd_base_seek(file, 0, SEEK_END) : -1;
        uint8_t last = 0;
        const bool ok = size == 0 || (size > 0 && aurora_dvd_base_seek(file, size - 1, SEEK_SET) == size - 1 &&
                                      aurora_dvd_base_read(file, &last, 1) == 1);
        aurora_dvd_base_close(file);
        if (!ok) {
            char path[256] = {};
            return DVDConvertEntrynumToPath(entry, path, sizeof(path)) ? std::string(path) : "entry " + std::to_string(entry);
        }
    }
    PortLog::Write("metroid_prime_port: disc image is complete (its last %zu files read in %llu ms)\n", ends.size(),
                   static_cast<unsigned long long>(SDL_GetTicks() - start));
    return {};
}

constexpr const char* kSupportedDisc =
    "Metroid Prime for the GameCube is supported: USA 1.00, 1.01\nand 1.02 (GM8E01), and PAL (GM8P01, experimental).";

const char* SupportedDiscText() {
    return kSupportedDisc;
}

// What the user picked instead, in words: the usual mistakes are another
// region or a later revision of the same game.
std::string DescribeDisc(const char* id6, unsigned version) {
    return PortDiscReplace::DescribeDisc(id6, version);
}

std::string DescribeUnsupportedDisc(const DVDDiskID* id) {
    if (id == nullptr) {
        return "This disc image has no readable game id.";
    }
    return DescribeDisc(DiscId6(*id).data(), id->gameVersion);
}

// Shows a disc message and asks whether to pick a disc image (true) or close
// the game. With offerPick false the box only has Close. Skipped when nothing
// can show it (headless, or the scripted runs that set MP_NO_DISC_DIALOG), as
// the disc dialog is; the answer is then offerPick, which is what happened
// before the box had a choice.
bool AskPickDisc(const char* title, const std::string& message, const char* pickLabel, bool offerPick) {
    if (port::EnvFlag("MP_NO_DISC_DIALOG")) {
        return offerPick;
    }
    int windowCount = 0;
    SDL_Window** windows = SDL_GetWindows(&windowCount);
    SDL_Window* window = windows != nullptr && windowCount > 0 ? windows[0] : nullptr;
    SDL_free(windows);
    if (window == nullptr) {
        return offerPick;
    }
    // Escape (and Back on Android) closes: the box is in the way of nothing else,
    // so leaving it is leaving the game.
    enum { kClose, kPick };
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, kPick, pickLabel},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT | (offerPick ? 0u : SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT), kClose,
         "Close"},
    };
    SDL_MessageBoxData data{};
    data.flags = SDL_MESSAGEBOX_ERROR;
    data.window = window;
    data.title = title;
    data.message = message.c_str();
    data.numbuttons = offerPick ? 2 : 1;
    data.buttons = offerPick ? buttons : buttons + 1;
    int answer = -1;
    if (!SDL_ShowMessageBox(&data, &answer)) {
        PortLog::Write("metroid_prime_port: could not show the disc message: %s\n", SDL_GetError());
        return offerPick;
    }
    return offerPick && answer == kPick;
}

// Says on screen why the disc was refused (without it a refused disc looked
// like a crash on start) and asks whether to pick another or close the game.
bool ShowDiscError(const std::string& problem, const std::string& path, bool askNext) {
    // Short lines: some message boxes do not wrap (SDL's X11 one).
    const std::string message = problem + "\n\n" + path + "\n\n" + SupportedDiscText();
    return AskPickDisc("Metroid Prime: wrong disc image", message, "Pick another disc", askNext);
}

// Drops a refused disc so the next launch does not open it again. On Android
// the copy made from a picked file goes too: ResolveDiscPath prefers it over
// the remembered setting, and it is 1.4 GB of the wrong game.
void ForgetDisc(const std::string& path) {
    bool forget = false;
    if (const char* remembered = PortDebug::DiscPath(); remembered != nullptr && path == remembered) {
        forget = true;
    }
#if defined(__ANDROID__)
    if (!PortPaths::UserFolder().empty() && path == PortPaths::UserFolder() + "disc.iso") {
        std::error_code ec;
        if (std::filesystem::remove(path, ec)) {
            PortLog::Write("metroid_prime_port: deleted the copy of the refused disc\n");
        }
        forget = true;
    }
#endif
    if (forget) {
        PortDebug::SetDiscPath("");
        PortDebug::SaveSettingsNow();
        PortLog::Write("metroid_prime_port: forgot the remembered disc image\n");
    }
}

// A read of the image that fails mid-game (damage the mount check missed)
// ends the session, and the same image would end every later one. So the
// failure is noted in a file naming the image, and the next launch refuses
// that image and asks for another.
std::string DiscReadFailedMarker() {
    return PortPaths::UserFolder().empty() ? std::string() : PortPaths::UserFolder() + "disc-read-failed";
}

std::string s_mountedDisc;

void NoteDiscReadFailure() {
    PortLog::Write("metroid_prime_port: a read of the disc image failed\n");
    const std::string marker = DiscReadFailedMarker();
    if (marker.empty()) {
        return;
    }
    if (FILE* file = std::fopen(marker.c_str(), "wb"); file != nullptr) {
        std::fputs(s_mountedDisc.c_str(), file);
        std::fclose(file);
    }
}

// Whether the last session failed to read this image. Clears the note, and
// has the overlay say why the last session ended whichever image it was.
bool DiscReadFailedLastTime(const std::string& path) {
    const std::string marker = DiscReadFailedMarker();
    if (marker.empty()) {
        return false;
    }
    std::ifstream in(marker, std::ios::binary);
    if (!in) {
        return false;
    }
    const std::string failed = port::ReadAllText(in);
    in.close();
    std::error_code ec;
    std::filesystem::remove(marker, ec);
    PortDebug::NoteDiscReadFailedLastSession();
    return failed == path;
}

#if defined(__ANDROID__)
// Android's picker hands back a content:// URI, not a path. Opening one is
// possible (SDL routes SDL_IOFromFile through ContentResolver) but it depends
// on a permission grant that the system can revoke at any time - and on some
// builds the open fails even on the launch that picked the file. Copying the
// image into app storage once makes the remembered setting a plain file, which
// is readable with no grant at all and survives anything short of an uninstall.
//
// Returns the local copy's path, or an empty string if the copy failed.
std::string CopyDiscFromContentUri(const std::string& uri) {
    // Shared with the in-game "Replace disc image" (port_disc_replace.cpp). A
    // refusal leaves the URI to be checked once mounted, as before.
    std::string target;
    return PortDiscReplace::CopyToUserFolder(uri, &target, nullptr, nullptr, nullptr).empty() ? target : std::string();
}

// A disc picked after Android destroyed the activity behind the picker: the
// pick arrives at the next launch, which MetroidPrimeActivity.onActivityResult
// leaves in pending_disc_uri for us. Without this the next launch asked again,
// and on a phone that destroys the activity each time the picker covers it the
// game never started (#203). Remembers the pick like AskForDiscImage does.
void TakeDiscPickedLastLaunch() {
    const char* dir = SDL_GetAndroidInternalStoragePath();
    if (dir == nullptr) {
        return;
    }
    const std::filesystem::path file = std::filesystem::path(dir) / "pending_disc_uri";
    std::string chosen;
    {
        std::ifstream in(file);
        std::getline(in, chosen);
    }
    std::error_code ec;
    std::filesystem::remove(file, ec);
    if (chosen.empty()) {
        return;
    }
    PortLog::Write("metroid_prime_port: using the disc image picked before the app was restarted\n");
    if (std::strncmp(chosen.c_str(), "content://", 10) == 0) {
        const std::string local = CopyDiscFromContentUri(chosen);
        if (!local.empty()) {
            chosen = local;
        }
    }
    PortDebug::SetDiscPath(chosen.c_str());
    PortDebug::SaveSettingsNow();
    PortLog::Write("metroid_prime_port: disc image set to %s\n", chosen.c_str());
}
#endif  // __ANDROID__

// Asks for the disc image with the platform's file dialog and remembers the
// choice. SDL delivers the result on another thread, so this pumps events until
// it arrives; the callback also fires with an empty list if the dialog fails.
// *cancelled is set when the user closed the dialog without a pick (Back on
// Android), as against a dialog that failed, timed out or was quit.
std::string AskForDiscImage(bool* cancelled = nullptr) {
    static std::atomic< bool > answered{false};
    static std::atomic< bool > dismissed{false};
    static std::string chosen;
    if (cancelled != nullptr) {
        *cancelled = false;
    }
    // Static because the callback cannot capture, but reset on every call: the
    // stale-disc retry asks a second time, and without this it would return the
    // first answer at once without showing a dialog. A first call only returns
    // early (timeout, quit, no window) on the way to exiting, so no callback
    // from it can still be pending here.
    answered.store(false);
    dismissed.store(false);
    chosen.clear();
    // Static: SDL reads the filters until the dialog closes, which can be after
    // a timed-out wait has returned.
    static const SDL_DialogFileFilter filters[] = {
        {"Metroid Prime disc image (iso, gcm, rvz, wbfs, ciso, nkit)", "iso;gcm;rvz;wbfs;ciso;nkit"},
        {"All files", "*"},
    };
    if (port::EnvFlag("MP_NO_DISC_DIALOG")) {
        // For scripted runs with a window, such as the packaged startup check
        // on a build runner, where the dialog would sit open until it times out.
        PortLog::Write( "metroid_prime_port: not asking for a disc image (MP_NO_DISC_DIALOG)\n");
        return {};
    }
    int windowCount = 0;
    SDL_Window** windows = SDL_GetWindows(&windowCount);
    SDL_Window* window = windows != nullptr && windowCount > 0 ? windows[0] : nullptr;
    SDL_free(windows);
    if (window == nullptr) {
        // Headless, as on a build runner: nothing to show a dialog on, so say
        // no disc was given rather than waiting for an answer that cannot come.
        PortLog::Write( "metroid_prime_port: no window to ask for a disc image on\n");
        return {};
    }
    PortLog::Write( "metroid_prime_port: no disc image found; asking for one\n");
    // A titled dialog: an untitled file picker on first launch does not say
    // what it wants. (Android's picker shows no title.)
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, const_cast<SDL_DialogFileFilter*>(filters));
    SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 2);
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, window);
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING,
                          "Select your Metroid Prime disc image (GameCube, USA v1.00, or PAL: experimental)");
    const SDL_DialogFileCallback onAnswer = [](void*, const char* const* files, int) {
        if (files != nullptr && files[0] != nullptr) {
            chosen = files[0];
        }
        // An empty list is a cancel; a null one is SDL's error.
        dismissed.store(files != nullptr && files[0] == nullptr);
        answered.store(true);
    };
#if defined(SDL_PLATFORM_IOS)
    // SDL has no file dialog on iOS.
    PortIos::ShowDiscPicker(window, onAnswer, nullptr);
#else
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFILE, onAnswer, nullptr, props);
#endif
    SDL_DestroyProperties(props);
    // Wait for the answer, but not forever: a dialog that never calls back
    // would otherwise hang a scripted or headless run.
    const Uint64 deadline = SDL_GetTicks() + 5 * 60 * 1000;
    while (!answered.load()) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                PortLog::Write( "metroid_prime_port: disc selection cancelled\n");
                return {};
            }
        }
        if (SDL_GetTicks() > deadline) {
            PortLog::Write( "metroid_prime_port: disc selection timed out\n");
            return {};
        }
        // Android's picker covers the app and destroys its surface. Nothing
        // draws a frame here, so drop the swapchain now or it stays attached to
        // the dead window, which can lose the device (the likely cause of a
        // crash reported on the first launch, the one that asks for the disc).
        aurora_release_lost_surface();
        SDL_Delay(10);
    }
    if (!chosen.empty()) {
#if defined(__ANDROID__)
        // The picker returns a content:// URI. Copy it to a real file so that the
        // remembered setting needs no grant on the next launch.
        if (std::strncmp(chosen.c_str(), "content://", 10) == 0) {
            const std::string local = CopyDiscFromContentUri(chosen);
            if (!local.empty()) {
                chosen = local;
            }
        }
#endif
        PortDebug::SetDiscPath(chosen.c_str());
        // Persist immediately: the settings are otherwise only written from the
        // overlay's draw path, which never runs if the game cannot frame.
        PortDebug::SaveSettingsNow();
        PortLog::Write( "metroid_prime_port: disc image set to %s\n", chosen.c_str());
    } else if (dismissed.load()) {
        PortLog::Write("metroid_prime_port: no disc image picked\n");
        if (cancelled != nullptr) {
            *cancelled = true;
        }
    }
    return chosen;
}

// Asks for the disc image until one is picked or the user chooses to close.
// A picker closed without a pick used to end the game at once, which on Android
// (Back in the picker) looked like a crash.
std::string PickDisc() {
    for (;;) {
        bool cancelled = false;
        std::string disc = AskForDiscImage(&cancelled);
        if (!disc.empty() || !cancelled) {
            return disc;
        }
        std::string message = std::string("No disc image was picked.\n\n") + SupportedDiscText();
#if defined(SDL_PLATFORM_IOS)
        message += "\n\nOr copy the image into On My iPhone (or iPad) > Metroid Prime in the Files app "
                   "and start the game again.";
#endif
        if (!AskPickDisc("Metroid Prime: no disc image", message,
                         "Pick a disc", true)) {
            return {};
        }
    }
}

// Default texture-replacement folder next to the executable.
const char* DefaultTexturesPath() {
    static const std::string sPath = [] {
#if defined(__ANDROID__)
        char* pref = SDL_GetPrefPath(nullptr, "Metroid Prime");
        if (pref == nullptr) {
            return std::string();
        }
        const std::string dir = std::string(pref) + "textures";
        SDL_free(pref);
#else
        const char* base = SDL_GetBasePath();
        if (base == nullptr) {
            return std::string();
        }
        const std::string dir = std::string(base) + "textures";
#endif
        std::error_code ec;
        return std::filesystem::is_directory(dir, ec) ? dir : std::string();
    }();
    return sPath.empty() ? nullptr : sPath.c_str();
}

// The API to ask Aurora for. It is tried first, with Aurora's own order (D3D12,
// Vulkan, D3D11 on Windows; Vulkan, OpenGL ES on Linux) as the fallback, so a driver
// that can't start it still plays.
AuroraBackend DesiredGraphicsBackend() {
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
    // The `graphics_backend` setting (F1 > Video > Compatibility, issue #135), read at
    // window creation, so a change needs a restart. Auto keeps asking for nothing in
    // particular. MP_GRAPHICS_BACKEND=<one of Choices()' names> overrides the setting
    // for one run; an unknown value warns and leaves the saved selection in charge.
    // port_graphics_backend.h owns the mapping and the same resolution the Video page
    // shows, so a request cannot be described one way and started another.
#if !defined(_WIN32)
    // OpenGL ES is not one of that setting's choices on Linux: it keeps its own toggle
    // for GPUs whose Vulkan driver draws wrong (issue #7), and wins over the choice.
    if (port::EnvFlag("MP_OPENGLES", PortDebug::OpenGles())) {
        return BACKEND_OPENGLES;
    }
#endif
    const char* const env = PortGraphicsBackend::EnvRequest();
    const PortGraphicsBackend::Request request = PortGraphicsBackend::Resolve(PortDebug::GraphicsBackend(), env);
    if (request.rejected) {
        // The names this build offers, so the warning can't suggest one it can't start.
        std::string names;
        for (const PortDebug::EGraphicsBackend choice : PortGraphicsBackend::Choices()) {
            names += (names.empty() ? "" : ", ");
            names += PortDebug::GraphicsBackendConfigName(choice);
        }
        PortLog::Write("port: MP_GRAPHICS_BACKEND=%s is not one of %s; using graphics_backend=%s\n",
                       env, names.c_str(), PortDebug::GraphicsBackendConfigName(request.backend));
    }
    return PortGraphicsBackend::ToAurora(request.backend);
#else
    // Elsewhere the API is Dawn's to choose: MP_OPENGLES=0/1 overrides the `opengles`
    // setting for one run (Android's OpenGL ES, issue #7).
    return port::EnvFlag("MP_OPENGLES", PortDebug::OpenGles()) ? BACKEND_OPENGLES : BACKEND_AUTO;
#endif
}
} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    if ((argc == 4 || argc == 5) && std::strcmp(argv[1], "--log-copy") == 0) {
        return PortLogFile::RunCopy(argv[2], argv[3], argc == 5 ? argv[4] : nullptr);
    }
    PortLogFile::AttachParentConsole();
#endif
    if (argc == 2 && std::strcmp(argv[1], "--settings-selftest") == 0) {
        return PortDebug::SettingsSelfTest();
    }
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("Metroid Prime native port %s (%s)\n", MP_BUILD_VERSION, MP_BUILD_REVISION);
        return 0;
    }
#if defined(__ANDROID__)
    // An app gets no environment of its own, so debug runs pass MP_* variables as
    // `adb shell setprop debug.mport.env 'K=V K=V'` (and .env2: a value holds 91 bytes).
    for (const char* prop : {"debug.mport.env", "debug.mport.env2"}) {
        char value[PROP_VALUE_MAX] = {};
        __system_property_get(prop, value);
        std::string_view rest = value;
        while (!rest.empty()) {
            const size_t space = rest.find(' ');
            const std::string pair(rest.substr(0, space));
            rest = space == std::string_view::npos ? std::string_view{} : rest.substr(space + 1);
            if (const size_t eq = pair.find('='); eq != std::string::npos && eq != 0) {
                setenv(pair.substr(0, eq).c_str(), pair.c_str() + eq + 1, 1);
                PortLog::Write("%s: %s\n", prop, pair.c_str());
            }
        }
    }
#else
    // --import [name [argument]]: run a mod importer (port_importers.h) from
    // the terminal, without starting the game.
    if (argc >= 2 && std::strcmp(argv[1], "--import") == 0) {
        return PortImporters::RunFromCommandLine(argc, argv);
    }
    // --import-remastered <image.nsp> [key file]: build the remastered-models
    // mod from the user's own copy, without starting the game. The disc comes
    // from MP_DISC or the path the game remembers.
    // --import-remastered-movies does only the menu movies, into that mod.
    const bool importMovies = argc >= 2 && std::strcmp(argv[1], "--import-remastered-movies") == 0;
    if (importMovies || (argc >= 2 && std::strcmp(argv[1], "--import-remastered") == 0)) {
        if (argc < 3) {
            std::fprintf(stderr, "usage: %s %s <image.nsp> [key file]\n", argv[0], argv[1]);
            return 2;
        }
        PortDebug::LoadDiscPath();
        const char* disc = ResolveDiscPath(1, argv);
        if (disc == nullptr || !OpenDiscImage(disc)) {
            std::fprintf(stderr, "cannot open the Metroid Prime disc image; set MP_DISC\n");
            return 1;
        }
        const DVDDiskID* id = DVDGetCurrentDiskID();
        int result = 1;
        if (!IsSupportedDisc(id)) {
            std::fprintf(stderr, "unsupported disc: %s\n", DescribeUnsupportedDisc(id).c_str());
        } else {
            result = PortRemastered::RunImportFromCommandLine(argv[2], argc >= 4 ? argv[3] : "", importMovies);
        }
        aurora_dvd_close();
        return result;
    }
    // --import-dread-samus <image.nsp|xci> [key file]: Samus's bodies from the user's own Metroid
    // Dread into <mods>/dread-samus (port_dread_samus.h), without starting the game. The disc comes
    // from MP_DISC or the path the game remembers. With MP_DREAD_ROMFS=<extracted RomFS> the image
    // argument is ignored (pass "-").
    if (argc >= 2 && std::strcmp(argv[1], "--import-dread-samus") == 0) {
        if (argc < 3) {
            std::fprintf(stderr, "usage: %s %s <image.nsp|xci> [key file]\n", argv[0], argv[1]);
            return 2;
        }
        PortDebug::LoadDiscPath();
        const char* disc = ResolveDiscPath(1, argv);
        if (disc == nullptr || !OpenDiscImage(disc)) {
            std::fprintf(stderr, "cannot open the Metroid Prime disc image; set MP_DISC\n");
            return 1;
        }
        const DVDDiskID* id = DVDGetCurrentDiskID();
        int result = 1;
        if (!IsSupportedDisc(id)) {
            std::fprintf(stderr, "unsupported disc: %s\n", DescribeUnsupportedDisc(id).c_str());
        } else {
            result = PortRemastered::RunDreadSamusImportFromCommandLine(argv[2], argc >= 4 ? argv[3] : "");
        }
        aurora_dvd_close();
        return result;
    }
    // --import-pal-languages <image>: add a PAL disc's languages to a USA game
    // (port_pal_languages.h), without starting it.
    if (argc >= 2 && std::strcmp(argv[1], "--import-pal-languages") == 0) {
        if (argc != 3) {
            std::fprintf(stderr, "usage: %s %s <PAL disc image>\n", argv[0], argv[1]);
            return 2;
        }
        return PortPalLanguages::RunFromCommandLine(argv[2]);
    }
    // --import-flaahgra-music <image>: add the full Flaahgra theme from a PAL/JP
    // GameCube or Trilogy image to the mods folder (port_flaahgra_music.h).
    if (argc >= 2 && std::strcmp(argv[1], "--import-flaahgra-music") == 0) {
        if (argc != 3) {
            std::fprintf(stderr, "usage: %s %s <PAL/JP GameCube or Trilogy image>\n", argv[0], argv[1]);
            return 2;
        }
        return PortFlaahgraMusic::RunFromCommandLine(argv[2]);
    }
#endif
    // The file log starts first so it holds everything after it, build id included.
    {
        const bool logFile = port::EnvFlag("MP_LOG_FILE", PortDebug::LogFile());
        if (logFile && !PortLogFile::Start()) {
            PortLog::Write("port: cannot write the log to %s\n", PortLogFile::Path().c_str());
        }
    }
    PortCrash::Install();
    PortLog::Write( "metroid_prime_port: version %s, build %s\n", MP_BUILD_VERSION, MP_BUILD_REVISION);
    for (const std::string& line : PortPaths::MigrationLog()) {
        PortLog::Write("port: portable data: %s\n", line.c_str());
    }
    PortRandomizer::EnsureLoaded();
    PortAp::EnsureLoaded();
    // A 16:9 window when widescreen is requested; the game's render mode is
    // widened to match. Values are the default window size only.
    const bool widescreen = PortDebug::AspectMode() != PortDebug::kAspect_4_3;
    // MP_DUMP_TEXTURES=1 writes every source texture to
    // <cachePath>/texture_dumps as DDS, so replacement packs can be authored.
    const bool dumpTextures = port::EnvFlag("MP_DUMP_TEXTURES");
    std::string resourcesPath;
#if defined(__ANDROID__)
    if (char* pref = SDL_GetPrefPath(nullptr, "Metroid Prime")) {
        resourcesPath = pref;
        SDL_free(pref);
    }
#endif
    // MP_MEM1_MB raises the MEM1 arena above its 24 MB default, which grows the
    // CGameAllocator heap, so a very heavy mod model stops running it out of room.
    // Unset, 0 or anything below the default keeps 24 MB; capped at 1 GB so the
    // byte count stays in AuroraConfig's u32. It costs host memory, not GPU memory.
    //
    // A mod with room geometry draws several times what the game's own rooms do, so
    // with one installed the arena starts at kRoomGeoMem1MB and a frame's buffers at
    // kRoomGeoFrameBuffers times their size. MP_FRAME_BUFFERS=<n> sets that scale itself.
    const unsigned long kRoomGeoMem1MB = 256;
#if defined(__ANDROID__) || defined(SDL_PLATFORM_IOS)
    // Every scale step costs about 13 MB in each of the six copies of a frame's
    // buffers; at 12x a tablet's game ran at 1.3 GB and Android killed it for
    // memory. 6x still holds the heaviest room measured (20 MiB of vertices) 1.5x.
    const unsigned long kRoomGeoFrameBuffers = 6;
#else
    const unsigned long kRoomGeoFrameBuffers = 12;
#endif
    const bool roomGeometry = PortMods::HasRoomGeometry();
    uint32_t mem1Size = MEM1_DEFAULT_SIZE;
    {
        const int envMb = port::EnvInt("MP_MEM1_MB", -1);
        const unsigned long mb = envMb >= 0 ? static_cast<unsigned long>(envMb) : roomGeometry ? kRoomGeoMem1MB : 0;
        if (mb > MEM1_DEFAULT_SIZE / (1024 * 1024)) {
            mem1Size = static_cast<uint32_t>(std::min(mb, 1024UL) * 1024 * 1024);
            PortLog::Write("port: MEM1 arena raised to %u MB (%s)\n", mem1Size / (1024 * 1024),
                           envMb >= 0 ? "MP_MEM1_MB" : "room geometry");
        }
    }
    // With room_geo_resident (or MP_ROOM_GEO_RESIDENT=1), a room geometry mod's models stay on
    // the GPU from when they load, in kResidentMiB set aside for them, and a frame only sends
    // what is not kept: the frame's buffers need kResidentFrameBuffers times their size, a
    // margin for anything that falls back to being sent. MP_ROOM_GEO_RESIDENT_MB sets the room.
#if defined(__ANDROID__)
    const unsigned long kResidentMiB = 128;
#else
    const unsigned long kResidentMiB = 256;
#endif
    const unsigned long kResidentFrameBuffers = 2;
    bool resident = roomGeometry && PortDebug::RoomGeoResidentAtStartup();
    resident = roomGeometry && port::EnvFlag("MP_ROOM_GEO_RESIDENT", resident);
    uint32_t residentMiB = 0;
    if (resident) {
        const int envMiB = port::EnvInt("MP_ROOM_GEO_RESIDENT_MB", -1);
        residentMiB = static_cast<uint32_t>(
            envMiB >= 0 ? std::clamp(static_cast<unsigned long>(envMiB), 16UL, 2048UL) : kResidentMiB);
    }
    uint32_t frameBufferScale = !roomGeometry ? 1 : resident ? kResidentFrameBuffers : kRoomGeoFrameBuffers;
    const int envFrameBuffers = port::EnvInt("MP_FRAME_BUFFERS", -1);
    if (envFrameBuffers >= 0) {
        frameBufferScale = static_cast<uint32_t>(std::clamp(static_cast<unsigned long>(envFrameBuffers), 1UL, 16UL));
    }
    // 0 follows frameBufferScale and, in resident mode, grows after an overflow (aurora,
    // up to 6x); MP_STORAGE_BUFFERS pins it.
    uint32_t frameStorageScale = 0;
    const int envStorageBuffers = port::EnvInt("MP_STORAGE_BUFFERS", -1);
    if (envStorageBuffers >= 0) {
        frameStorageScale = static_cast<uint32_t>(std::clamp(static_cast<unsigned long>(envStorageBuffers), 1UL, 16UL));
    }
    if (frameBufferScale > 1 || frameStorageScale > 1) {
        PortLog::Write("port: frame buffers at %ux, storage at %ux (%s)\n", frameBufferScale,
                       frameStorageScale != 0 ? frameStorageScale : frameBufferScale,
                       envFrameBuffers >= 0 || envStorageBuffers >= 0 ? "MP_FRAME_BUFFERS/MP_STORAGE_BUFFERS"
                                                                      : "room geometry");
    }
    // The startup compile of the cached and seeded pipelines costs ~950 MiB of process memory
    // (issue #162): Android, and any device short on RAM, builds them on first use instead. An 8 GB
    // tablet held ~900 MiB of driver pipeline state from them and swapped all session.
    bool pipelineLazy = false;
    {
        char why[96] = "";
        pipelineLazy = PortMemoryLog::LowRamDevice(why, sizeof(why));
#if defined(__ANDROID__)
        if (!pipelineLazy) {
            pipelineLazy = true;
            std::strncat(why, ", Android", sizeof(why) - std::strlen(why) - 1);
        }
#endif
        const char* reason = why;
        if (const char* env = port::EnvString("MP_PIPELINE_LAZY"); env != nullptr && env[0] != '\0') {
            pipelineLazy = port::EnvFlag("MP_PIPELINE_LAZY", pipelineLazy);
            reason = "MP_PIPELINE_LAZY";
        }
        if (pipelineLazy || why[0] != '\0' || reason != why) {
            PortLog::Write("port: pipelines %s at startup (%s)\n", pipelineLazy ? "built on first use" : "precompiled",
                           reason);
        }
    }
    // Built pipelines stay until memory runs short (issue #187), areas left long ago included, so
    // walking back is free. Lazy mode trims on the system's memory warnings (port_memory_log.cpp) and
    // on a MemAvailable check every 5 s: under 1024 MiB available it releases what a minute of
    // frames didn't draw, under 512 MiB all but what the last frames drew (GLOBAL pipelines stay).
    // An 8 GB phone or tablet runs the game with 2-4 GB available; Android's own low-memory
    // threshold (logged with each memory warning) is a few hundred MiB, so 1024 trims well before
    // the system starts killing apps and 512 just above it. Overrides: MP_PIPELINE_TRIM_MIB and
    // MP_PIPELINE_TRIM_SEVERE_MIB (0 turns a level off), MP_PIPELINE_EVICT (a fixed idle sweep).
    uint32_t pipelineEvictFrames = 0;
    if (pipelineLazy) {
        const int trimMib = port::EnvInt("MP_PIPELINE_TRIM_MIB", 1024);
        const int severeMib = port::EnvInt("MP_PIPELINE_TRIM_SEVERE_MIB", 512);
        PortMemoryLog::SetPipelinePressure(trimMib, severeMib);
        if (port::EnvString("MP_PIPELINE_TRIM_MIB") != nullptr ||
            port::EnvString("MP_PIPELINE_TRIM_SEVERE_MIB") != nullptr) {
            PortLog::Write("port: pipeline trim under %d MiB available, all idle under %d MiB "
                           "(MP_PIPELINE_TRIM_MIB/MP_PIPELINE_TRIM_SEVERE_MIB)\n",
                           trimMib, severeMib);
        }
    }
    if (const char* env = port::EnvString("MP_PIPELINE_EVICT"); env != nullptr && env[0] != '\0') {
        pipelineEvictFrames = static_cast<uint32_t>(std::strtoul(env, nullptr, 10));
        PortLog::Write("port: pipeline eviction %u frames (MP_PIPELINE_EVICT)\n", pipelineEvictFrames);
    }
    // PBR pipelines are only drawn with a mod's converted materials (issue #162): without an
    // enabled mod their cached rows are not compiled at startup. MP_PIPELINE_PBR=0|1 overrides.
    const bool pipelineSkipPbr = !port::EnvFlag("MP_PIPELINE_PBR", PortMods::AnyModEnabled());
    // Settings, mods, save states and the shader caches sit in user/ beside the
    // executable when that folder can be written to (port_paths.h).
    const std::string& userFolder = PortPaths::UserFolder();
    const char* cacheEnv = port::EnvString("MP_CACHE_PATH");
#if defined(__ANDROID__)
    // The shader caches stay in app storage when the data moves to shared
    // storage: they are disposable, and SQLite is slow on the shared mount.
    const std::string defaultCache = PortPaths::detail::PrivateFolder();
#elif defined(SDL_PLATFORM_IOS)
    // Kept out of Documents: the Files app needn't list disposable caches.
    const std::string defaultCache = PortPaths::detail::WithSeparator(PortPaths::detail::PrefFolder());
#else
    const std::string& defaultCache = userFolder;
#endif
    const std::string cacheFolder = cacheEnv != nullptr && cacheEnv[0] != '\0' ? cacheEnv : defaultCache;
    PortLog::Write("port: user folder %s%s\n", userFolder.empty() ? "(none)" : userFolder.c_str(),
#if defined(__ANDROID__)
                   PortPaths::IsPortable() ? " (shared storage)" : "");
#else
                   PortPaths::IsPortable() ? " (portable)" : "");
#endif
#if defined(__ANDROID__)
    // Device identifiers for GPU-driver triage (issues #7, #8): the log alone
    // should say which SoC and driver build drew the world.
    {
        std::string deviceInfo;
        for (const char* prop : {"ro.product.manufacturer", "ro.product.model", "ro.product.device",
                                 "ro.soc.manufacturer", "ro.soc.model", "ro.board.platform", "ro.hardware",
                                 "ro.build.version.release", "ro.build.version.sdk", "ro.build.fingerprint",
                                 "ro.hardware.vulkan", "ro.hardware.egl", "ro.gfx.driver.0"}) {
            char value[PROP_VALUE_MAX] = {};
            if (__system_property_get(prop, value) > 0 && value[0] != '\0') {
                deviceInfo += "\n  ";
                deviceInfo += prop;
                deviceInfo += ": ";
                deviceInfo += value;
            }
        }
        PortLog::Write("port: device:%s\n", deviceInfo.empty() ? " (no properties)" : deviceInfo.c_str());
    }
#elif defined(__linux__) || defined(__APPLE__)
    {
        struct utsname uts = {};
        if (uname(&uts) == 0) {
            PortLog::Write("port: host %s %s %s\n", uts.sysname, uts.release, uts.machine);
        }
    }
#endif
    PortMemoryLog::LogStartup();
    const std::span<const uint8_t> embeddedSeed = PortEmbedded::Find("initial_pipeline_cache.db");
#if !defined(__ANDROID__)
    // The window icon, for a bare binary that no desktop entry describes.
    // Android takes its icon from the APK.
    static uint8_t windowIcon[] = {
#include "port_window_icon.inc"
    };
#endif
    AuroraConfig config = {
        .appName = "Metroid Prime",
        .userPath = userFolder.empty() ? nullptr : userFolder.c_str(),
        .cachePath = cacheFolder.empty() ? nullptr : cacheFolder.c_str(),
        .resourcesPath = resourcesPath.empty() ? nullptr : resourcesPath.c_str(),
        // Which API to ask Aurora for: aurora tries it first and falls back to its
        // own order (see DesiredGraphicsBackend).
        .desiredBackend = DesiredGraphicsBackend(),
        .msaa = static_cast<uint32_t>(PortDebug::Msaa()),
        .maxTextureAnisotropy = static_cast<uint16_t>(PortDebug::Anisotropy()),
        .vsync = false,
        .startFullscreen = PortDebug::Fullscreen(),
        .allowJoystickBackgroundEvents = false,
        .pauseOnFocusLost = false,
        .allowTextureDumps = dumpTextures,
        .allowCpuAdapter = false,
        // Let SDL place the window. The default 0,0 is the client area's corner on
        // Windows, which puts the title bar above the top of the screen.
        .windowPosX = -1,
        .windowPosY = -1,
        // 720p by default (the F1 overlay's sidebar needs the height); Aurora
        // shrinks it to fit a smaller desktop.
        .windowWidth = static_cast<uint32_t>(widescreen ? 1280 : 960),
        .windowHeight = 720,
#if !defined(__ANDROID__)
        .iconRGBA8 = windowIcon,
        .iconWidth = 64,
        .iconHeight = 64,
#else
        .iconRGBA8 = nullptr,
        .iconWidth = 0,
        .iconHeight = 0,
#endif
#if defined(__ANDROID__)
        // Android sets its log callback below.
        .logCallback = nullptr,
#else
        .logCallback = StderrLogCallback,
#endif
        .logLevel = LOG_DEBUG,
        .imGuiInitCallback = nullptr,
        .mem1Size = mem1Size,
        .mem2Size = ARAM_DEFAULT_SIZE,
        .frameBufferScale = frameBufferScale,
        .frameStorageScale = frameStorageScale,
        .residentGeometryMiB = residentMiB,
        // An embedded seed wins over a stale initial_pipeline_cache.db beside the executable.
        .pipelineCacheSeedData = embeddedSeed.data(),
        .pipelineCacheSeedSize = embeddedSeed.size(),
        // MP_PIPELINE_SEED=0: start from an empty cache (tools/pipeline_seed.py tour records with it).
        .pipelineCacheNoSeed = !port::EnvFlag("MP_PIPELINE_SEED", true),
        // Low-RAM Android builds pipelines on first use (issue #162); MP_PIPELINE_LAZY=0|1 overrides.
        .pipelineCacheLazy = pipelineLazy,
        // Idle pipelines are released after ~2 min at 60 fps when pipelines are built on first use;
        // MP_PIPELINE_EVICT=0|<frames> overrides (issue #162).
        .pipelineEvictFrames = pipelineEvictFrames,
        .pipelineCacheSkipPbr = pipelineSkipPbr,
        .pipelineCacheSkipPbrReason = "no mod enabled",
        // Set below when a custom Vulkan driver (port_gpu_driver.h) is loaded.
        .vulkanLibraryDir = nullptr,
        // Set below from the Driver workarounds settings.
        .dawnEnableToggles = nullptr,
        .dawnDisableToggles = nullptr,
        // Explicit MP_FRAME_BUFFERS / MP_STORAGE_BUFFERS keep the staging buffers at full size.
        .frameBuffersPinned = envFrameBuffers >= 0 || envStorageBuffers >= 0,
        // Aurora's Null backend draws nothing: the game would play its sound behind
        // a black window (issue #33). Scripted runs, which can't answer a box, keep it.
        .noGraphicsMessage = port::EnvFlag("MP_NO_DISC_DIALOG")
                                 ? nullptr
                                 : "No graphics device could be started, so the game can't show a picture.\n\n"
                                   "Update your graphics driver. In a virtual machine, turn on 3D acceleration "
                                   "or run the game on the host instead.\n\n"
                                   "The log file in the user folder has the details.",
    };

#if defined(__ANDROID__)
    // SDL3 drops touch-derived mouse events by default, and ImGui's SDL3
    // backend only understands mouse events. The touch overlay in Java claims
    // gameplay touches, so whatever reaches SDL here is meant for ImGui.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
    config.logCallback = AndroidLogCallback;
#endif
    // SDL3 reaches for its Wayland backend whenever a Wayland display is
    // reachable, and does so even without WAYLAND_DISPLAY — it falls back to the
    // default socket in XDG_RUNTIME_DIR. Under GNOME that backend never returns
    // from SDL_ShowWindow: it dispatches pending events, libdecor's client-side
    // decoration configure re-enters GTK layout from inside that dispatch, and the
    // process spins at 100% before the first frame is ever presented. Nothing in
    // the port can fix that, so whenever an X display is available ask for the X11
    // backend instead. SDL_VIDEODRIVER still wins, which is also how anyone who
    // wants Wayland opts back in. macOS has neither backend (its SDL build has
    // no x11 driver, and XQuartz sets DISPLAY), so it is left to SDL's default.
#if !defined(_WIN32) && !defined(__ANDROID__) && !defined(__APPLE__)
    {
        const char* requested = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
        const char* x11 = std::getenv("DISPLAY");
        const auto present = [](const char* v) { return v != nullptr && v[0] != '\0'; };
        const bool unnamed = !present(requested);
        if (unnamed && present(x11)) {
            SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
            PortLog::Write(
                         "port: using SDL's x11 video driver on %s; its wayland backend hangs on "
                         "window creation under GNOME (libdecor). Set SDL_VIDEODRIVER to "
                         "override.\n",
                         x11);
        } else if (unnamed) {
            // Nothing to fall back to: SDL will use Wayland, and on GNOME that is
            // the hang above. Say so before the silence, since there is no way to
            // tell from inside the hang.
            std::fputs("port: no DISPLAY set, so SDL will use its wayland backend, which hangs on "
                       "window creation under GNOME (libdecor). Run under an X display, or set "
                       "SDL_VIDEODRIVER yourself.\n",
                       stderr);
        }
    }
#endif
    // A driver can crash outright while Dawn starts on OpenGL ES or OpenGL (Mesa does),
    // and the setting would then crash every launch with no way back to the F1 menu. The
    // marker outlives such a crash, so the next start turns the setting off and uses the
    // automatic choice. Both markers share this: same folder, same lifecycle.
    const std::filesystem::path markerDir = std::filesystem::path(userFolder.empty() ? "." : userFolder);
    const std::filesystem::path glesMarker = markerDir / "opengles_starting";
    const std::filesystem::path glMarker = markerDir / "opengl_starting";
    if (config.desiredBackend == BACKEND_OPENGLES || config.desiredBackend == BACKEND_OPENGL) {
        const bool gles = config.desiredBackend == BACKEND_OPENGLES;
        const std::filesystem::path& marker = gles ? glesMarker : glMarker;
        std::error_code ec;
        if (std::filesystem::exists(marker, ec)) {
            if (gles) {
                PortLog::Write("port: the last start on OpenGL ES did not finish; turning it off\n");
                PortDebug::SetOpenGles(false);
            } else {
                PortLog::Write("port: the last start on OpenGL did not finish; using Auto\n");
                PortDebug::SetGraphicsBackend(PortDebug::kGraphicsBackend_Auto);
            }
            config.desiredBackend = BACKEND_AUTO;
            std::filesystem::remove(marker, ec);
        } else {
            std::ofstream(marker) << "1\n";
        }
    }
    // A custom Vulkan driver (Turnip) can crash while it starts, just like GL above; the
    // same kind of marker sends the next start back to the system driver.
    // MP_GPU_DRIVER=<id> (empty = system) overrides the setting for one run.
    const std::filesystem::path driverMarker =
        std::filesystem::path(userFolder.empty() ? "." : userFolder) / "gpu_driver_starting";
    static std::string vulkanLibraryDir;
    const char* envDriver = port::EnvString("MP_GPU_DRIVER");
    {
        const std::string driver = envDriver != nullptr ? envDriver : PortDebug::GpuDriver();
        std::error_code ec;
        if (!driver.empty() && config.desiredBackend != BACKEND_OPENGLES && PortGpuDriver::Supported()) {
            if (std::filesystem::exists(driverMarker, ec)) {
                PortLog::Write("port: the last start with GPU driver %s crashed; using the system driver\n",
                               driver.c_str());
                PortDebug::SetGpuDriver("");
                PortGpuDriver::SetLoadError("it crashed last time; switched back to the system driver");
                std::filesystem::remove(driverMarker, ec);
            } else {
                std::ofstream(driverMarker) << driver << '\n';
                vulkanLibraryDir = PortGpuDriver::Prepare(driver);
                config.vulkanLibraryDir = vulkanLibraryDir.empty() ? nullptr : vulkanLibraryDir.c_str();
            }
        } else {
            // Left by a crash before the user switched back to System: don't hold it against the next driver.
            std::filesystem::remove(driverMarker, ec);
        }
    }
    PortDebug::ApplyStorageClamp();
    static std::string dawnEnableToggles;
    static std::string dawnDisableToggles;
    PortDebug::BeginDriverWorkarounds(userFolder, dawnEnableToggles, dawnDisableToggles);
    config.dawnEnableToggles = dawnEnableToggles.c_str();
    config.dawnDisableToggles = dawnDisableToggles.c_str();
    aurora_initialize(argc, argv, &config);
    // The settings file, JSON and the logs read and write numbers with the C
    // library, in the "C" format. A toolkit loaded with the window (a GTK
    // decoration plugin, an input method) may call setlocale(LC_ALL, ""), which
    // under a comma-decimal locale would make "1.5" read as 1 and print as "1,5".
    std::setlocale(LC_NUMERIC, "C");
    if (config.desiredBackend == BACKEND_OPENGLES || config.desiredBackend == BACKEND_OPENGL) {
        std::error_code ec;
        std::filesystem::remove(config.desiredBackend == BACKEND_OPENGLES ? glesMarker : glMarker, ec);
    }
    // Aurora went back to the system driver when Vulkan failed with the custom one.
    if (aurora_vulkan_library_failed() && !PortGpuDriver::Active().empty()) {
        PortLog::Write("port: Vulkan failed with GPU driver %s; using the system driver\n",
                       PortGpuDriver::Active().c_str());
        if (envDriver == nullptr) {
            PortDebug::SetGpuDriver("");
        }
        PortGpuDriver::SetLoadError("Vulkan failed to start with it; switched back to the system driver");
    }
    // The driver started: only a crash before this point reverts it.
    {
        std::error_code ec;
        std::filesystem::remove(driverMarker, ec);
    }
    // From what the device gave, which can be less than was asked for.
    if (aurora_get_frame_storage_scale() != (frameStorageScale != 0 ? frameStorageScale : frameBufferScale)) {
        PortLog::Write("port: frame storage at %ux, all this device allows\n", aurora_get_frame_storage_scale());
    }
    if (aurora_get_frame_buffer_scale() != frameBufferScale) {
        PortLog::Write("port: frame buffers at %ux, all this device allows\n", aurora_get_frame_buffer_scale());
    }
    PortRoomGeo::SetBuffersReady(aurora_get_frame_buffer_scale() > 1);
    if (resident) {
        const uint32_t got = aurora_get_resident_geometry_mib();
        PortRoomGeo::SetResident(got != 0);
        PortLog::Write("port: room geometry kept on the GPU in %u MiB\n", got);
    }
    // Apply the persisted render scale. Vsync is applied on the first drawn
    // frame (once the swapchain surface exists) so it uses real capabilities.
    VISetFrameBufferScale(PortDebug::RenderScale());
    // Fit the internal EFB to the game's render-mode aspect rather than the
    // window aspect, so fixed 4:3/16:9 modes are never stretched when the window
    // shape differs; the present letterboxes instead.
    AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);

    // Optional HD texture replacements, in Aurora's naming convention
    // (tex1_<w>x<h>_<texhash>[_<tluthash>]_<format>.dds/.png); a per-device
    // subfolder is selected from the connected controller. Aurora also accepts
    // Dolphin format names such as CMPR and RGBA8.
    // MP_TEXTURES wins. Otherwise a build that carries the set (MP_EMBED_RESOURCES)
    // passes no folder, so PortTextures/PortPrompts use the built-in copy and a stale
    // `textures` folder left in an old install is ignored; any other build reads the
    // folder next to the executable.
    const char* textures = port::EnvString("MP_TEXTURES");
    if (textures == nullptr || textures[0] == '\0') {
        textures = PortEmbedded::Under("textures/").empty() ? DefaultTexturesPath() : nullptr;
    }
    // The user's own pack, over the built-in set. Kept in the user folder, under
    // a name updates never replace (the built-in set is read-only in an AppImage or
    // Flatpak, and re-copied on every Android launch).
    std::string userTextures;
    if (const char* env = port::EnvString("MP_USER_TEXTURES"); env != nullptr && env[0] != '\0') {
        userTextures = env;
    } else if (!PortPaths::UserFolder().empty()) {
        userTextures = PortPaths::UserFolder() + "user_textures";
    }
    PortTextures::Initialize(textures, userTextures.c_str());
    // Binding-aware prompt icons, served from <textures>/bindings.
    PortPrompts::Initialize(textures);

    // Disc image: an explicit argument or MP_DISC, else the path saved on a
    // previous launch, else a copy beside the executable, else ask for one.
    PortDebug::LoadDiscPath();
#if defined(__ANDROID__)
    // Remembered as the disc path, so the lookup below finds it.
    TakeDiscPickedLastLaunch();
#endif
    std::string discImage;
    if (const char* resolved = ResolveDiscPath(argc, argv); resolved != nullptr) {
        discImage = resolved;
    } else {
        discImage = PickDisc();
    }
    if (discImage.empty()) {
        PortLog::Write(
                     "metroid_prime_port: no disc image given.\n"
                     "  usage: %s <path to Metroid Prime (USA v1.00, or PAL: experimental).iso>\n"
                     "  or set MP_DISC, or place the image next to the executable.\n", argv[0]);
        aurora_shutdown();
        return 1;
    }
    const char* discPath = discImage.c_str();

    // A disc that will not open, or is not the one game this runs, is said on
    // screen and forgotten, then asked for again. Exiting instead left the
    // remembered path in place, so every later launch opened the same wrong
    // image and closed without a word (on Android, with no terminal, the app
    // just opened and shut).
    const bool discFromArgs = ResolveDiscFromArgs(argc, argv);
    for (;;) {
        std::string problem;
        if (!OpenDiscImage(discPath)) {
            ReportDiscOpenFailure(discPath);
            problem = "This file could not be read as a GameCube disc image.";
        } else if (!IsSupportedDisc(DVDGetCurrentDiskID())) {
            problem = DescribeUnsupportedDisc(DVDGetCurrentDiskID());
            PortLog::Write("metroid_prime_port: unsupported disc: %s\n", problem.c_str());
            aurora_dvd_close();
        } else if (DiscReadFailedLastTime(discImage)) {
            PortLog::Write("metroid_prime_port: refused the disc image: a read of it failed last session\n");
            problem = "Part of this disc image couldn't be read last time, so it's damaged.\n"
                      "Copy the file again, or check it in Dolphin (Properties > Verify).";
            aurora_dvd_close();
        } else if (const std::string unreadable = FindUnreadableDiscFile(); !unreadable.empty()) {
            PortLog::Write("metroid_prime_port: disc image is incomplete: %s can't be read\n", unreadable.c_str());
            problem = "This disc image is incomplete or damaged: part of it (" + unreadable +
                      ") can't be read.\nIt's probably an interrupted download or copy. Copy the file again.";
            aurora_dvd_close();
        }
        if (problem.empty()) {
            break;
        }
        ForgetDisc(discImage);
        if (!ShowDiscError(problem, discImage, !discFromArgs)) {
            if (!discFromArgs) {
                PortLog::Write("metroid_prime_port: closed at the disc message\n");
            }
            aurora_shutdown();
            return 1;
        }
        PortLog::Write("metroid_prime_port: asking for the disc image again\n");
        discImage = PickDisc();
        if (discImage.empty()) {
            PortLog::Write("metroid_prime_port: no disc image given.\n");
            aurora_shutdown();
            return 1;
        }
        discPath = discImage.c_str();
    }
    PortLog::Write("metroid_prime_port: disc mounted: %s\n", discPath);
    PortDiscVerify::SetImagePath(discImage);
    const PortDisc::Version discVersion = PortDisc::Current();
    PortLog::Write("metroid_prime_port: disc is %s%s\n", PortDisc::Name(discVersion),
                   PortDisc::HasPalData(discVersion) ? " (experimental support)" : "");
    // Saves are the disc's region's (a PAL save's worlds are laid out
    // differently): its game code and Dolphin's card for that region. Trilogy's
    // are PAL-sized but its own, so neither disc loads the other's.
    PortGci::SetGameCode(discVersion == PortDisc::Version::Trilogy ? "R3ME"
                         : discVersion == PortDisc::Version::Pal   ? "GM8P"
                                                                   : "GM8E");
    s_mountedDisc = discImage;
    aurora_dvd_set_read_error_callback(NoteDiscReadFailure);
    // A Remastered import finished in the last session becomes the mod now,
    // before anything has a file of the old one open.
    if (PortRemastered::ApplyPendingImport()) {
        PortLog::Write("metroid_prime_port: installed the imported Remastered models\n");
    }
    PortRemastered::RemoveLegacyImport();
    PortStorageReport::StartLogging();
    PortMods::Initialize();
    // Video-relevant settings in one line, so GPU-driver triage (issues #7,
    // #8) needs nothing but the log.
    {
        bool remastered = false;
        const PortMods::Status& status = PortMods::CurrentStatus();
        if (status.active) {
            for (const PortMods::ModInfo& mod : status.mods) {
                if (mod.enabled && mod.import) {
                    remastered = true;
                    break;
                }
            }
        }
        char dynamicRes[64];
        if (PortDebug::DynamicRes()) {
            std::snprintf(dynamicRes, sizeof(dynamicRes), "on (target %d, min %.2f)", PortDebug::DynamicResTarget(),
                          static_cast<double>(PortDebug::DynamicResMin()));
        } else {
            std::snprintf(dynamicRes, sizeof(dynamicRes), "off");
        }
        char scale[16] = "auto";
        if (PortDebug::RenderScale() > 0.f) {
            std::snprintf(scale, sizeof(scale), "%.2fx", static_cast<double>(PortDebug::RenderScale()));
        }
        PortLog::Write("port: settings: backend %s (in use: %s), msaa %d, anisotropy %d, scale %s, dynamic-res %s, "
                       "smoothing %s (interp %s), vsync %s, frame-cap %d, remastered-models %s\n",
                       PortGraphicsBackend::DisplayName(config.desiredBackend),
                       PortGraphicsBackend::DisplayName(aurora_get_backend()), PortDebug::Msaa(),
                       PortDebug::Anisotropy(), scale, dynamicRes,
                       PortDebug::SmoothFrames() ? "on" : "off", PortDebug::FrameInterpolation() ? "on" : "off",
                       PortDebug::VsyncEnabled() ? "on" : "off", PortDebug::FrameRateCap(),
                       remastered ? "on" : "off");
        PortDebug::LogSettings();
    }
    // The index that gives solid actors their disc model's collision box reads
    // the base disc, so it belongs to the disc's lifecycle rather than to a
    // mods reload: drop anything an earlier disc left open.
    PortActorCollisionBounds::Reset();

    // Prime the window/event state so the game's first aurora_begin_frame can
    // succeed (the game submits GX during early init, before its main loop).
    aurora_update();

    int result = 1;
    try {
        result = metroid_main(argc, argv);
    } catch (const std::exception& error) {
        PortLog::Write( "metroid_prime_port: %s\n", error.what());
    }

    AIPortShutdown();
    // An import still running reads the disc.
    PortRemastered::StopImport();
    PortDiscVerify::Shutdown();
    PortDiscReplace::Shutdown();
    PortActorCollisionBounds::Reset();
    aurora_dvd_close();
    aurora_shutdown();
    return result;
}
