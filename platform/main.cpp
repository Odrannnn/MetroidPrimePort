// Port entry point. Aurora owns the real process entry (aurora_main) and the
// window/GPU/input/audio backend; this file initializes it, mounts the user's
// disc, and hands control to the game.
//
// Disc path resolution: first non-flag argv, else $MP_DISC, else the default
// path used during development.

#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <aurora/event.h>
#include <aurora/main.h>
#include <dolphin/gx.h>
#include <aurora/texture.hpp>
#include <dolphin/vi.h>

#include "port_debug.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" int metroid_main(int argc, char** argv);

namespace {
const char* ResolveDiscPath(int argc, char** argv) {
    if (argc > 1 && argv[1][0] != '-') {
        return argv[1];
    }
    if (const char* env = std::getenv("MP_DISC")) {
        return env;
    }
    return nullptr;
}
} // namespace

int main(int argc, char** argv) {
    // A 16:9 window when widescreen is requested; the game's render mode is
    // widened to match. Values are the default window size only.
    const bool widescreen = PortDebug::AspectMode() != PortDebug::kAspect_4_3;
    const AuroraConfig config = {
        .appName = "Metroid Prime",
        .userPath = nullptr,
        .cachePath = nullptr,
        .resourcesPath = nullptr,
        .desiredBackend = BACKEND_AUTO,
        .vsync = false,
        // Keep the internal framebuffer at the game's logical size so its two
        // framebuffer allocations fit in MEM1; Aurora upscales to the window.
        .windowWidth = static_cast<uint32_t>(widescreen ? 854 : 640),
        .windowHeight = 480,
        .mem1Size = MEM1_DEFAULT_SIZE,
        .mem2Size = ARAM_DEFAULT_SIZE,
    };

    aurora_initialize(argc, argv, &config);
    VISetFrameBufferScale(1.f);
    // Fit the internal EFB to the game's render-mode aspect rather than the
    // window aspect, so fixed 4:3/16:9 modes are never stretched when the window
    // shape differs; the present letterboxes instead.
    AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);

    // Optional HD texture replacements, in Aurora's naming convention
    // (tex1_<w>x<h>_<texhash>[_<tluthash>]_<format>.dds/.png). Loaded once and
    // kept alive for the process. Dolphin pack names are not supported yet.
    static aurora::texture::ReplacementGroup sTextureReplacements;
    if (const char* textures = std::getenv("MP_TEXTURES")) {
        sTextureReplacements = aurora::texture::load_replacement_directory(textures);
        std::fprintf(stderr, "metroid_prime_port: loaded %zu texture replacements from %s\n",
                     sTextureReplacements.registrations.size(), textures);
    }

    const char* discPath = ResolveDiscPath(argc, argv);
    if (discPath == nullptr) {
        std::fprintf(stderr,
                     "metroid_prime_port: no disc image given.\n"
                     "  usage: %s <path to Metroid Prime (USA) (v1.00).iso>\n"
                     "  or set MP_DISC to the image path.\n",
                     argv[0]);
        aurora_shutdown();
        return 1;
    }
    if (!aurora_dvd_open(discPath)) {
        std::fprintf(stderr, "metroid_prime_port: failed to open disc image: %s\n", discPath);
        aurora_shutdown();
        return 1;
    }
    std::printf("metroid_prime_port: disc mounted: %s\n", discPath);

    // Prime the window/event state so the game's first aurora_begin_frame can
    // succeed (the game submits GX during early init, before its main loop).
    aurora_update();

    const int result = metroid_main(argc, argv);

    aurora_dvd_close();
    aurora_shutdown();
    return result;
}
