cmake_minimum_required(VERSION 3.25)
project(metroid_prime_port LANGUAGES C CXX)
if(IOS)
    # platform/port_ios.mm. Enabled here: enabled where the target is made, it
    # comes after the subdirectories and leaves CMake without its compile rules.
    enable_language(OBJCXX)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_COLOR_DIAGNOSTICS ON)

# Build speed, used only when the tools are installed. ccache makes recompiling
# unchanged sources nearly free (stashing or setting edits aside rewrites mtimes)
# and lets a fresh build directory fill from the cache: base_dir rewrites paths
# under the source tree to relative ones and hash_dir=false drops the build
# directory from the hash, so sibling build/<name> directories share entries.
# The sloppiness lets SDL3's precompiled-header sources be cached at all.
# mold cuts the link of the 230 MB RelWithDebInfo binary.
option(MP_USE_CCACHE "Compile through ccache when it is installed" ON)
option(MP_USE_MOLD "Link with mold when it is installed (Linux hosts)" ON)
if(MP_USE_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER AND NOT MSVC)
    find_program(MP_CCACHE ccache)
    if(MP_CCACHE)
        set(_mp_ccache "${MP_CCACHE}" "base_dir=${CMAKE_SOURCE_DIR}" "hash_dir=false"
            "sloppiness=pch_defines,time_macros")
        set(CMAKE_C_COMPILER_LAUNCHER ${_mp_ccache})
        set(CMAKE_CXX_COMPILER_LAUNCHER ${_mp_ccache})
    endif()
endif()
if(MP_USE_MOLD AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_LINKER_TYPE)
    find_program(MP_MOLD mold)
    if(MP_MOLD)
        set(CMAKE_LINKER_TYPE MOLD)
    endif()
endif()

include(CTest)
option(MP_BUILD_TESTS "Build native-port regression tests" ON)
if(ANDROID)
    set(_mp_embed_default OFF)
else()
    set(_mp_embed_default ON)
endif()
option(MP_EMBED_RESOURCES "Build the controller glyph textures and the pipeline cache seed into the executable (desktop)" ${_mp_embed_default})
option(MP_ENABLE_SMOKE_DRIVER "Enable the opt-in real-disc lifecycle test driver" OFF)
option(MP_ANDROID_NOD_STUB "Use a non-functional nod stub for Android build probes" OFF)
# A system Abseil, fmt, SQLite, libpng or freetype ties the binary to the build
# host's copies; Abseil's sonames change with every release, so a distro update
# stops it starting (issue #27). Bundled, they are built from source and linked
# in statically. SDL3, OpenSSL, zlib and zstd stay as they are: their sonames are
# stable, and a static OpenSSL would look for CA certificates where the build
# host's distro keeps them.
if((CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID) OR APPLE)
    set(_mp_bundled_default ON)
else()
    set(_mp_bundled_default OFF)
endif()
option(MP_BUNDLED_DEPS "Build Abseil, fmt, SQLite, libpng and freetype from source and link them statically, ignoring system copies" ${_mp_bundled_default})
set(AURORA_BUNDLED_DEPS ${MP_BUNDLED_DEPS})

# The decompiled game/engine sources and the port code around them are written
# for a *signed* plain `char`: the PowerPC/MWCC original and every host the port
# has been verified on (x86_64, Windows) agree on that. ARM defaults `char` to
# unsigned, which silently changes sentinels such as CDSPStreamManager's -1
# companion index into 255: the stream header read then takes the companion path
# with a 255 index, discards the stream, and all streamed audio (music) goes
# quiet while MusyX sound effects keep working. Force the verified semantics on
# every target that compiles game-facing code. MSVC/clang-cl already default to
# signed char. platform/compat.h asserts the invariant for the game sources.
function(mp_signed_char target)
    if(NOT MSVC)
        target_compile_options(${target} PRIVATE -fsigned-char)
    endif()
endfunction()

# MSVC picks a member-function pointer's size per translation unit, from
# whether the class is complete where the pointer type is first needed.
# CAiStateFunc/CAiTriggerFunc point into CAi, which some headers only
# forward-declare, so CAiState had a different layout in CStateMachine.cpp
# than in CPatterned.cpp, and the first AI think jumped to garbage (issue #4).
# /vmg uses the general representation everywhere, as Itanium's fixed one does.
if(MSVC)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/vmg>)
    # cl.exe reports __cplusplus as 199711L without this whatever the standard,
    # so the SDK's AUTO fell back to __typeof__, which it lacks (issue #170).
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>)
endif()

if(MP_ANDROID_NOD_STUB)
    if(NOT ANDROID)
        message(FATAL_ERROR "MP_ANDROID_NOD_STUB is only valid for Android builds")
    endif()
    add_library(mp_android_nod_stub STATIC platform/android_probe/nod_stub.c)
    target_include_directories(mp_android_nod_stub PUBLIC platform/android_probe/include)
    add_library(nod::nod ALIAS mp_android_nod_stub)
endif()

# Apple's libc++ still marks std::jthread/std::stop_token (used by aurora's
# threads) experimental; the flag unlocks them and links libc++experimental.
if(APPLE)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-fexperimental-library>)
    # The iOS Simulator SDK ships no libc++experimental; jthread and stop_token
    # are header-only, so its build compiles with the flag and links without it.
    if(NOT CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
        add_link_options(-fexperimental-library)
    endif()
endif()

# Aurora provides the GameCube/Wii compatibility layer (MIT): GX, OS, PAD, DVD,
# CARD, VI, SI, THP, mtx, plus the SDL3/WebGPU application layer.
add_subdirectory(extern/aurora EXCLUDE_FROM_ALL)
if(TARGET aurora_mtx)
    target_compile_definitions(aurora_mtx PRIVATE MTX_USE_PS=1)
endif()
# EXCLUDE_FROM_ALL also keeps Aurora's tests out of the default build, but ctest still runs them,
# so a stale binary from an older build passed or failed in place of the current code.
if(AURORA_ENABLE_TESTS)
    get_property(aurora_test_targets DIRECTORY extern/aurora/tests PROPERTY BUILDSYSTEM_TARGETS)
    add_custom_target(aurora_tests ALL)
    add_dependencies(aurora_tests ${aurora_test_targets})
endif()

# MusyX is the game's audio engine; the vendored fork is portable.
add_subdirectory(extern/musyx EXCLUDE_FROM_ALL)
mp_signed_char(musyx)

# ARM's astc-encoder (Apache-2.0): the Remastered importer's ASTC texture writer.
add_subdirectory(extern/astcenc EXCLUDE_FROM_ALL)

# Decompiled game + engine sources (see files.cmake).
include(files.cmake)
# Not portable: raw PowerPC asm (NES emulator wrapper), and decomp-only
# type-matching scaffolding.
list(REMOVE_ITEM MP_GAME_SOURCES
    src/NESemu/modwrapper.cpp
    src/MetroidPrime/TypesMatch.cpp)
# The in-game NES emulator embeds hand-written PowerPC asm; exclude it for now
# and stub its entry points when wiring gameplay.
list(FILTER MP_GAME_SOURCES EXCLUDE REGEX "^src/NESemu/")
# The game entry is included now that it is adapted to Aurora (its `main` is
# renamed to `metroid_main` and driven by platform/main.cpp).
list(APPEND MP_GAME_SOURCES src/MetroidPrime/main.cpp)
# Port code moved out of decomp files that must still compile exactly as the game
# sources do (same flags, definitions and forced include as the file it came from).
list(APPEND MP_GAME_SOURCES
    platform/port_remastered_movers.cpp
    platform/port_raster_font.cpp
    platform/port_morph_ball.cpp
    platform/port_automapper.cpp
    platform/port_particle_vfx_read.cpp
    platform/port_element_gen.cpp
    platform/port_entity_schema.cpp
    src/MetroidPrime/CPortNestedLights.cpp)
# The port has an explicit source manifest, including registration-only and
# shim-dependent translation units. Link those objects directly on both ELF and
# COFF rather than relying on archive extraction or suppressing duplicate symbols.
add_library(mp_game OBJECT ${MP_GAME_SOURCES})
# The entity schema table holds a few thousand accessor lambdas, each its own COMDAT
# section on COFF: past the 65k sections an object holds without /bigobj.
if(MSVC)
    set_source_files_properties(platform/port_entity_schema.cpp PROPERTIES COMPILE_OPTIONS /bigobj)
endif()
mp_signed_char(mp_game)
target_include_directories(mp_game PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/aurora/include
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/include)
target_link_libraries(mp_game PUBLIC
    aurora::core aurora::gx aurora::gd aurora::si aurora::vi aurora::pad
    aurora::mtx aurora::os aurora::dvd aurora::thp aurora::card
    musyx astcenc)
# Keep useful native diagnostics; suppress only known decomp syntax warnings.
# compat.h restores SDK macros Aurora omits.
if(MSVC)
    target_compile_options(mp_game PRIVATE /W3
        /FI${CMAKE_CURRENT_SOURCE_DIR}/platform/compat.h)
    # C++20 makes cl.exe /permissive-, which rejects a string literal bound to
    # char* (C2440, CZipSupport.cpp); the decomp does that, as -Wno-write-strings
    # allows elsewhere (issue #170). clang-cl only warns.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(mp_game PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/Zc:strictStrings->)
    endif()
else()
    target_compile_options(mp_game PRIVATE
        -Wformat -Werror=return-type -Wno-narrowing -Wno-multichar -Wno-write-strings
        -include ${CMAKE_CURRENT_SOURCE_DIR}/platform/compat.h)
endif()

# Port entry point and platform shims (Aurora owns the real main; see
# aurora/main.h). Android loads the target through SDL's Java activity.
set(MP_PORT_SOURCES
    platform/main.cpp
    platform/debug_ui.cpp
    platform/debug_ui/android_jni.cpp
    platform/debug_ui/archipelago.cpp
    platform/debug_ui/audio.cpp
    platform/debug_ui/cheats.cpp
    platform/debug_ui/controls.cpp
    platform/debug_ui/debug.cpp
    platform/debug_ui/file_dialogs.cpp
    platform/debug_ui/gallery.cpp
    platform/debug_ui/game_sections.cpp
    platform/debug_ui/input.cpp
    platform/debug_ui/mods.cpp
    platform/debug_ui/options.cpp
    platform/debug_ui/overlays.cpp
    platform/debug_ui/performance.cpp
    platform/debug_ui/randomizer.cpp
    platform/debug_ui/remastered.cpp
    platform/debug_ui/runtime.cpp
    platform/debug_ui/savestates.cpp
    platform/debug_ui/search.cpp
    platform/debug_ui/settings.cpp
    platform/debug_ui/tabs.cpp
    platform/debug_ui/touch_overlay.cpp
    platform/debug_ui/tracker.cpp
    platform/debug_ui/ui_common.cpp
    platform/debug_ui/video.cpp
    platform/debug_ui/window.cpp
    platform/port_input_settings.cpp
    platform/port_gyro_motion.cpp
    platform/disc.cpp
    platform/shims.cpp
    platform/sdk_stubs.cpp
    platform/port_embedded.cpp
    platform/port_textures.cpp
    platform/port_prompts.cpp
    platform/port_controls.cpp
    platform/input/port_input_devices.cpp
    platform/input/port_input_engine.cpp
    platform/input/port_input_bindings.cpp
    platform/input/port_input_remap_ui.cpp
    platform/port_randomizer.cpp
    platform/port_ap_metroidprime.cpp
    platform/port_ap_protocol.cpp
    platform/port_ap_logic.cpp
    platform/port_ap_world.cpp
    platform/port_rando_gen.cpp
    platform/port_apclient.cpp
    platform/port_apclient_worker.cpp
    platform/port_ap_solo.cpp
    platform/port_rando_seed.cpp
    platform/port_json.cpp
    platform/port_gpu_driver.cpp
    platform/port_discord.cpp
    platform/port_update_check.cpp
    platform/port_livesplit.cpp
    platform/port_tracker.cpp
    platform/port_beam_doors.cpp
    platform/port_beam_doors_side.cpp
    platform/port_map_pickups.cpp
    platform/port_map_marks.cpp
    platform/port_map_side.cpp
    platform/port_map_side_paths.cpp
    platform/port_map_pickups_index.cpp
    platform/port_viewmodel.cpp
    platform/port_actor_shadow.cpp
    platform/port_freecam.cpp
    platform/port_third_person.cpp
    platform/port_third_person_body.cpp
    platform/port_world_holders.cpp
    platform/port_photo.cpp
    platform/port_console.cpp
    platform/port_log_file.cpp
    platform/port_crash.cpp
    platform/port_memory_log.cpp
    platform/port_watchdog.cpp
    platform/port_savestate.cpp
    platform/port_savestate_format.cpp
    platform/port_dynamic_res.cpp
    platform/port_savestate_room.cpp
    platform/port_savestate_boss.cpp
    platform/port_savestate_fields.cpp
    platform/port_statedump.cpp
    platform/port_boss_flaahgra.cpp
    platform/port_boss_prime.cpp
    platform/port_boss_ridley.cpp
    platform/port_boss_thardus.cpp
    platform/port_skip_cutscenes.cpp
    platform/port_revision_scripts.cpp
    platform/port_skip_streams.cpp
    platform/port_skip_cutscenes_ops.cpp
    platform/port_gci.cpp
    platform/port_save_convert.cpp
    platform/port_data_folder.cpp
    platform/port_mods.cpp
    platform/port_mods_pak.cpp
    platform/port_pal_languages.cpp
    platform/port_pal_languages_import.cpp
    platform/port_flaahgra_music.cpp
    platform/port_flaahgra_music_import.cpp
    platform/port_actor_collision_bounds.cpp
    platform/port_importers.cpp
    platform/port_importers_lines.cpp
    platform/port_remastered_nsp.cpp
    platform/port_remastered_nso.cpp
    platform/port_remastered_pak.cpp
    platform/port_remastered_txtr.cpp
    platform/port_remastered_tegra_swizzle.cpp
    platform/port_remastered_bcdec.cpp
    platform/port_remastered_astc_decode.cpp
    platform/port_hd_font.cpp
    platform/port_hd_font_file.cpp
    platform/port_font_accent_file.cpp
    platform/port_hud_bars.cpp
    platform/port_hud_bars_file.cpp
    platform/port_speedrun_timer.cpp
    platform/port_remastered_font.cpp
    platform/port_remastered_hud.cpp
    platform/port_remastered_map.cpp
    platform/port_remastered_movie.cpp
    platform/port_room_env.cpp
    platform/port_clustered_lights.cpp
    platform/port_room_env_script.cpp
    platform/port_room_env_load.cpp
    platform/port_room_env_file.cpp
    platform/port_room_env_tiles.cpp
    platform/port_envfx_assets.cpp
    platform/port_envfx_file.cpp
    platform/port_shell.cpp
    platform/port_shell_assets.cpp
    platform/port_shell_file.cpp
    platform/port_room_geo.cpp
    platform/port_room_geo_file.cpp
    platform/port_room_liquid.cpp
    platform/port_room_effects.cpp
    platform/port_queen_barrier.cpp
    platform/port_queen_barrier_validate.cpp
    platform/port_room_effects_file.cpp
    platform/port_retail_placement.cpp
    platform/port_retail_placement_file.cpp
    platform/port_collision_view.cpp
    platform/port_pointer.cpp
    platform/port_pointer_menu.cpp
    platform/port_remastered_cmdl.cpp
    platform/port_remastered_anuv.cpp
    platform/port_remastered_image.cpp
    platform/port_remastered_table.cpp
    platform/port_remastered_convert.cpp
    platform/port_remastered_convert_retail.cpp
    platform/port_remastered_convert_material.cpp
    platform/port_remastered_convert_pbr.cpp
    platform/port_remastered_convert_skin.cpp
    platform/port_remastered_ball_light.cpp
    platform/port_remastered_effect.cpp
    platform/port_vfx_particles.cpp
    platform/port_fx_debug.cpp
    platform/port_remastered_effect_convert.cpp
    platform/port_remastered_effect_import.cpp
    platform/port_remastered_report.cpp
    platform/port_remastered_import.cpp
    platform/port_storage_report.cpp
    platform/port_remastered_import_retail.cpp
    platform/port_remastered_import_remastered.cpp
    platform/port_dread_samus_import.cpp
    platform/port_dread_samus.cpp
    platform/port_dread_pkg.cpp
    platform/port_dread_bcmdl.cpp
    platform/port_dread_bsmat.cpp
    platform/port_dread_bctex.cpp
    platform/port_dread_reskin.cpp
    platform/port_dread_material.cpp
    platform/port_remastered_jpeg.cpp
    platform/port_gallery.cpp
    platform/port_remastered_room.cpp
    platform/port_remastered_room_file.cpp
    platform/port_remastered_room_retail.cpp
    platform/port_remastered_room_scripts.cpp
    platform/port_remastered_room_effects.cpp
    platform/port_remastered_anim.cpp
    platform/port_remastered_text.cpp
    platform/port_custom_res.cpp
    platform/port_ball_looks.cpp
    platform/port_ball_looks_disc.cpp
    platform/port_disc_reader.cpp
    platform/port_suit_colours.cpp
    platform/port_suit_colours_disc.cpp
    platform/port_hints.cpp
    platform/port_ws.cpp
    platform/port_disc_verify.cpp
    platform/port_disc_replace.cpp
    platform/port_disc_verify_core.cpp
    platform/touch_pad.cpp
    platform/glibc_compat.c
    platform/ai_dma.cpp)
# The movie import's decoder: a separate ffmpeg program on the desktop. Android
# has none to start, so its own decoder (MediaCodec) and the port's JPEG encoder
# (compiled everywhere: the gallery uses it too) do the same job there.
if(ANDROID)
    list(APPEND MP_PORT_SOURCES platform/port_remastered_mediacodec.cpp)
else()
    list(APPEND MP_PORT_SOURCES platform/port_remastered_ffmpeg.cpp)
endif()
# The port's own sources build warning-clean: -Wall -Wextra catch mistakes in
# port code. Decompiled game/engine code (src/, include/) and vendored code
# (extern/) are excluded. -Wno-multichar: the code deliberately uses 'TXTR'-style
# FourCC constants. -Wno-unused-parameter and -Wno-ignored-qualifiers: every hit
# is inside decompiled headers (include/) that platform sources include and that
# must not be edited (named-but-unused params and const-qualified return types
# are the decomp style); platform sources themselves have none. clang-cl reads
# -Wall as -Weverything (540k warnings per Windows CI run), so it gets /W4, its
# spelling of -Wall -Wextra. cl.exe gets nothing here (its own flags live elsewhere).
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
    set_source_files_properties(${MP_PORT_SOURCES} platform/smoke.cpp PROPERTIES
        COMPILE_OPTIONS "-Wall;-Wextra;-Wno-multichar;-Wno-unused-parameter;-Wno-ignored-qualifiers")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set_source_files_properties(${MP_PORT_SOURCES} platform/smoke.cpp PROPERTIES
        COMPILE_OPTIONS "/W4;-Wno-multichar;-Wno-unused-parameter;-Wno-ignored-qualifiers")
endif()
if(ANDROID)
    add_library(metroid_prime_port SHARED ${MP_PORT_SOURCES})
    target_link_libraries(metroid_prime_port PRIVATE mediandk)
    target_link_options(metroid_prime_port PRIVATE "-Wl,-u,SDL_main")
    # Custom Vulkan drivers (Turnip) through libadrenotools (port_gpu_driver.cpp). Its
    # hook libraries and the shim Dawn loads as libvulkan.so are separate shared
    # libraries, loaded by path: android/app/build.gradle lists them as targets.
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        add_subdirectory(extern/adrenotools EXCLUDE_FROM_ALL)
        add_library(mport_vkshim SHARED platform/android/vkshim.c)
        set_target_properties(mport_vkshim PROPERTIES C_VISIBILITY_PRESET hidden)
        target_link_libraries(metroid_prime_port PRIVATE adrenotools z)
    endif()
elseif(IOS)
    # An iOS app is a flat bundle: the executable, Info.plist and the icons side
    # by side. Nothing signs it here; tools/ios_package.sh finishes the bundle
    # (icons, an ad hoc signature) and players re-sign the .ipa with their own
    # Apple ID when they sideload it.
    add_executable(metroid_prime_port MACOSX_BUNDLE ${MP_PORT_SOURCES} platform/port_ios.mm)
    set_source_files_properties(platform/port_ios.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    target_link_libraries(metroid_prime_port PRIVATE "-framework UIKit" "-framework UniformTypeIdentifiers")
    file(STRINGS android/app/build.gradle _mp_version_line REGEX "versionName \"[^\"]+\"")
    string(REGEX REPLACE ".*versionName \"([^\"]+)\".*" "\\1" MP_IOS_VERSION "${_mp_version_line}")
    set_target_properties(metroid_prime_port PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/ios/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER io.github.odrannnn.metroidprimeport
        MACOSX_BUNDLE_BUNDLE_NAME "Metroid Prime"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${MP_IOS_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${MP_IOS_VERSION}")
else()
    add_executable(metroid_prime_port ${MP_PORT_SOURCES})
endif()
# Touch settings and the virtual pad on phones and tablets. Android draws the
# controls in Java (TouchControlsView); iOS draws them in C++ (touch_overlay.cpp).
# The desktop switch builds the C++ overlay on a host build so its drawing and
# event handling can be tried without a device; the console's `finger` command
# sends the touches.
option(MP_TOUCH_OVERLAY_DESKTOP "Draw the touch controls on a desktop build (testing)" OFF)
if(ANDROID OR IOS)
    target_compile_definitions(metroid_prime_port PRIVATE PORT_TOUCH_CONTROLS)
endif()
if(IOS)
    target_compile_definitions(metroid_prime_port PRIVATE PORT_TOUCH_OVERLAY)
endif()
if(MP_TOUCH_OVERLAY_DESKTOP AND NOT ANDROID)
    target_compile_definitions(metroid_prime_port PRIVATE PORT_TOUCH_CONTROLS PORT_TOUCH_OVERLAY)
endif()
# disc.cpp and port_pal_languages.cpp inflate compressed images and fonts. Aurora
# provides ZLIB::ZLIB everywhere (its zlib-ng on Windows).
target_link_libraries(metroid_prime_port PRIVATE ZLIB::ZLIB)
target_link_libraries(metroid_prime_port PRIVATE port_remastered_ground_contacts)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR ANDROID)
    # port_crash.cpp logs each C++ throw's type and stack.
    target_link_options(metroid_prime_port PRIVATE "-Wl,--wrap=__cxa_throw")
    target_compile_definitions(metroid_prime_port PRIVATE MP_WRAP_CXA_THROW)
endif()
# Developer tool, not a test (it needs game data): dumps a CHPR's animations for
# diffing against build/mpr/anim/chpr_anim.py.
if(NOT ANDROID)
    add_executable(port_remastered_anim_tool
        tests/port_remastered_anim_tool.cpp platform/port_remastered_anim.cpp)
    target_include_directories(port_remastered_anim_tool PRIVATE platform/include)
    set_target_properties(port_remastered_anim_tool PROPERTIES EXCLUDE_FROM_ALL ON)
endif()
if(NOT ANDROID)
    # Developer tool: the Remastered effect reader/converter/importer on real paks, plus
    # pdump/pdiff/explain/mat (docs/DEBUGGING.md "Converter reports"). Needs libzstd.
    add_executable(remastered_effect_tool
        tests/port_remastered_effect_tool.cpp platform/port_remastered_effect.cpp
        platform/port_remastered_effect_convert.cpp platform/port_remastered_effect_import.cpp
        platform/port_remastered_image.cpp platform/port_remastered_txtr.cpp platform/port_remastered_pak.cpp
        platform/port_remastered_tegra_swizzle.cpp platform/port_remastered_bcdec.cpp
        platform/port_remastered_astc_decode.cpp
        platform/port_remastered_convert.cpp platform/port_remastered_cmdl.cpp platform/port_remastered_anuv.cpp
        platform/port_remastered_convert_retail.cpp platform/port_remastered_convert_material.cpp
        platform/port_remastered_convert_pbr.cpp platform/port_remastered_convert_skin.cpp
        platform/port_remastered_report.cpp)
    target_include_directories(remastered_effect_tool PRIVATE platform/include include extern/aurora/include)
    target_compile_definitions(remastered_effect_tool PRIVATE TARGET_PC VERSION=0)
    target_link_libraries(remastered_effect_tool PRIVATE astcenc ${CMAKE_THREAD_LIBS_INIT})
    set_target_properties(remastered_effect_tool PROPERTIES EXCLUDE_FROM_ALL ON)
endif()
if(NOT ANDROID)
    # Developer tool too: flattens real models' ANUV programs (tools/anuv_check.py).
    add_executable(port_remastered_anuv_tool
        tests/port_remastered_anuv_tool.cpp platform/port_remastered_anuv.cpp platform/port_remastered_cmdl.cpp)
    target_include_directories(port_remastered_anuv_tool PRIVATE platform/include)
    set_target_properties(port_remastered_anuv_tool PROPERTIES EXCLUDE_FROM_ALL ON)
endif()
if(WIN32)
    # A GUI program, so Windows opens no console window next to the game
    # (SDL_main supplies WinMain). PortLogFile::AttachParentConsole prints to
    # the terminal it was started from, if any.
    set_target_properties(metroid_prime_port PROPERTIES WIN32_EXECUTABLE ON)
    # shell32/ole32/advapi32: finding Dolphin's folder (port_gci.cpp).
    # dbghelp: the crash report's stack and dump (port_crash.cpp).
    target_link_libraries(metroid_prime_port PRIVATE ws2_32 shell32 ole32 advapi32 dbghelp)
    # UTF-8 as the ANSI code page, so the UTF-8 paths SDL returns work with the
    # narrow file APIs; see the manifest. CMake merges it into the linker's own.
    target_sources(metroid_prime_port PRIVATE packaging/metroid_prime_port.manifest)
    # The icon Explorer and the window show.
    target_sources(metroid_prime_port PRIVATE packaging/metroid_prime_port.rc)
endif()
# OpenSSL gives the Archipelago WebSocket client wss://. On the desktop it is
# optional: without it the port still builds and a wss:// server fails with a
# clear error. On Android it is built from source (cmake/AndroidOpenSSL.cmake),
# because the NDK has no TLS library and a find_package that quietly finds
# nothing is how an APK ends up refusing every wss:// server. Leaving it out
# there has to be asked for.
option(MP_ALLOW_NO_TLS "Android: build without OpenSSL, so wss:// is refused" OFF)
if(ANDROID)
    if(NOT MP_ALLOW_NO_TLS)
        include(cmake/AndroidOpenSSL.cmake)
    endif()
else()
    find_package(OpenSSL)
endif()
function(mp_websocket_tls target)
    if(OpenSSL_FOUND)
        target_compile_definitions(${target} PRIVATE MP_HAVE_OPENSSL)
        target_link_libraries(${target} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        if(WIN32)
            target_link_libraries(${target} PRIVATE crypt32)
        endif()
        if(MP_OPENSSL_BUILD_TARGET)
            add_dependencies(${target} ${MP_OPENSSL_BUILD_TARGET})
        endif()
    endif()
endfunction()
if(OpenSSL_FOUND)
    message(STATUS "OpenSSL ${OPENSSL_VERSION} found: wss:// enabled")
else()
    message(STATUS "OpenSSL not found: wss:// disabled")
endif()
mp_websocket_tls(metroid_prime_port)
if(ANDROID AND OpenSSL_FOUND)
    # The OpenSSL linked in is private to the game library. Exporting its
    # symbols would let them interpose on, or be interposed by, another copy in
    # the process. Only its archives are named: excluding ALL would also hide
    # the JNI entry points in the static SDL archive, which Java looks up by name.
    target_link_options(metroid_prime_port PRIVATE "-Wl,--exclude-libs,libssl.a:libcrypto.a")
endif()
mp_signed_char(metroid_prime_port)
add_custom_target(port_build_info
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DOUTPUT_SOURCE=${CMAKE_CURRENT_BINARY_DIR}/generated/port_build_info.cpp"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildRevision.cmake"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/generated/port_build_info.cpp"
    VERBATIM)
add_dependencies(metroid_prime_port port_build_info)
target_sources(metroid_prime_port PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/port_build_info.cpp")
if(MP_EMBED_RESOURCES)
    # The glyph textures and the pipeline cache seed live inside the executable, so
    # a packaged folder needs no loose copies, and a new executable dropped into an
    # old install ignores the stale files left there. Regenerate the textures with
    # tools/make_prompt_glyphs.py.
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    file(GLOB_RECURSE MP_EMBED_INPUTS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/textures/*")
    set(MP_EMBED_SEED "${CMAKE_CURRENT_SOURCE_DIR}/assets/initial_pipeline_cache.db")
    set(MP_EMBED_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/port_embedded_data.cpp")
    add_custom_command(
        OUTPUT "${MP_EMBED_SOURCE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_resources.py"
            "${MP_EMBED_SOURCE}"
            --dir "${CMAKE_CURRENT_SOURCE_DIR}/textures" textures
            --file "${MP_EMBED_SEED}" initial_pipeline_cache.db
        DEPENDS ${MP_EMBED_INPUTS} "${MP_EMBED_SEED}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_resources.py"
        COMMENT "Embedding textures and the pipeline cache seed"
        VERBATIM)
    target_sources(metroid_prime_port PRIVATE "${MP_EMBED_SOURCE}")
    target_compile_definitions(metroid_prime_port PRIVATE MP_EMBED_RESOURCES)
else()
    # Keep the HD texture replacements next to the executable, which is where the
    # port looks for them by default, so a built binary picks them up without a
    # manual copy. Regenerate them with tools/make_prompt_glyphs.py. The generated
    # sets are cleared first: copy_directory only adds, and an icon the generator
    # no longer writes would otherwise keep replacing a texture it no longer claims
    # (two retired legend hashes kept drawing L/R over the map's [M] and [S] icons).
    set(PORT_TEXTURE_SETS bindings keyboard playstation switch xbox)
    list(TRANSFORM PORT_TEXTURE_SETS PREPEND "$<TARGET_FILE_DIR:metroid_prime_port>/textures/")
    add_custom_command(TARGET metroid_prime_port POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${PORT_TEXTURE_SETS}
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/textures"
            "$<TARGET_FILE_DIR:metroid_prime_port>/textures"
        COMMENT "Copying texture replacements next to the executable")
endif()
if(MP_ENABLE_SMOKE_DRIVER)
    target_sources(metroid_prime_port PRIVATE platform/smoke.cpp)
    target_compile_definitions(mp_game PRIVATE MP_ENABLE_SMOKE_DRIVER)
endif()
# aurora_core (webgpu/gpu.cpp) and aurora_gx reference each other; group them so
# the linker rescans and resolves the cycle.
if(MSVC)
    target_link_libraries(metroid_prime_port PRIVATE
        mp_game
        aurora::main aurora::core aurora::gx)
    target_link_options(metroid_prime_port PRIVATE /INCREMENTAL:NO)
elseif(CMAKE_LINK_GROUP_USING_RESCAN_SUPPORTED OR CMAKE_CXX_LINK_GROUP_USING_RESCAN_SUPPORTED)
    target_link_libraries(metroid_prime_port PRIVATE
        mp_game
        aurora::main
        "$<LINK_GROUP:RESCAN,aurora::core,aurora::gx>")
elseif(APPLE)
    # ld64 searches every archive again until nothing new resolves, so the
    # cycle needs no group.
    target_link_libraries(metroid_prime_port PRIVATE
        mp_game
        aurora::main aurora::core aurora::gx)
else()
    message(FATAL_ERROR "This port currently requires an ELF linker with RESCAN support, Apple ld or an MSVC-compatible COFF linker")
endif()

include(extern/aurora/cmake/AuroraCopyRuntimeDLLs.cmake)
aurora_copy_runtime_dlls(metroid_prime_port)

# Aurora imports this machine-independent pipeline description cache and
# compiles its entries in the background before first use.
# Embedded builds carry it inside the executable instead.
if(NOT MP_EMBED_RESOURCES)
    add_custom_command(TARGET metroid_prime_port POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/initial_pipeline_cache.db
            $<TARGET_FILE_DIR:metroid_prime_port>/initial_pipeline_cache.db)
endif()

# Install rules. Without these a `cmake --install` installs nothing at all, which
# is not a cosmetic problem: flatpak-builder's only install step is the build
# system's own, so the Flatpak manifest built an application with no executable
# in it, and a package manager installing this project would get an empty tree.
#
# The data files go next to the executable because that is where the port looks
# for them (unless MP_EMBED_RESOURCES builds them in): DefaultTexturesPath() in platform/main.cpp resolves
# <SDL_GetBasePath()> + "textures", and Aurora's pipeline cache is opened by name
# beside the binary. They are installed to bin/ for that reason, not by accident.
include(GNUInstallDirs)
install(TARGETS metroid_prime_port
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    BUNDLE DESTINATION .)
if(NOT MP_EMBED_RESOURCES)
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/textures/
        DESTINATION ${CMAKE_INSTALL_BINDIR}/textures)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/assets/initial_pipeline_cache.db
        DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/packaging/io.github.odrannnn.metroidprimeport.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
# The application icon. The desktop entry names it, and AppStream needs one too:
# without it, appstreamcli compose - which is the last step of a Flatpak build -
# fails the whole package with icon-not-found. Installed here rather than in a
# packaging script so the AppImage, the Flatpak and a package-manager install all
# pick it up from the same rule.
# Installed under the app id, which is the name the desktop entry and the
# metainfo's <icon> both refer to, whatever the file in packaging/ is called.
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/packaging/varia-bolt.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
    RENAME io.github.odrannnn.metroidprimeport.svg)
# The AppStream description, which the desktop entry cannot carry. Flatpak
# software centres and app stores show this rather than the desktop file, and
# Flathub will not accept a component without it. Installed rather than left for
# the manifest so the AppImage and a package-manager install carry it too.
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/packaging/io.github.odrannnn.metroidprimeport.metainfo.xml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
              ${CMAKE_CURRENT_SOURCE_DIR}/NOTICE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/metroid-prime-port)

# Source-derived Remastered box/triangle contact geometry (0xc0a180) and its
# adapter. Linked into the game by the Remastered mover; the tests use it
# standalone. The exact IEEE semantics (-ffp-contract=off) are part of the contract.
add_library(port_remastered_ground_contacts STATIC
    platform/port_remastered_ground_contacts.cpp
    platform/port_remastered_ground_world.cpp
    platform/port_collision_failsafe.cpp)
target_include_directories(port_remastered_ground_contacts PUBLIC platform/include)
set_target_properties(port_remastered_ground_contacts PROPERTIES CXX_STANDARD 17)
if(MSVC)
    target_compile_options(port_remastered_ground_contacts PRIVATE /fp:strict)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(port_remastered_ground_contacts PRIVATE -fno-fast-math -ffp-contract=off)
endif()

# NCollidablePairs::Sphere_Sphere_Swept 0xc012d0: the sphere-vs-sphere swept query
# over the shared contact/failsafe helpers. Geometry only; the pair table, materials
# and the mover's policy stay outside. Same exact-IEEE contract as the library above,
# which it links for Source::NormalizeWithMagnitude and RaySphere.
add_library(port_remastered_sphere_sweep STATIC
    platform/port_remastered_sphere_sweep.cpp)
target_link_libraries(port_remastered_sphere_sweep PUBLIC port_remastered_ground_contacts)
target_include_directories(port_remastered_sphere_sweep PUBLIC platform/include)
set_target_properties(port_remastered_sphere_sweep PROPERTIES CXX_STANDARD 17)
if(MSVC)
    target_compile_options(port_remastered_sphere_sweep PRIVATE /fp:strict)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(port_remastered_sphere_sweep PRIVATE -fno-fast-math -ffp-contract=off)
endif()

# NCollisionRayCast::Capsule 0xbf6f10 with CMath::SolveQuadratic 0x699b0 and
# NCollisionRayCast::InfiniteCylinder 0xbf73c0: the source ray-vs-capsule query the
# Remastered ball mover sweeps. Geometry only - no caller, materials or contacts; it links
# the contact library for the shared 0x6b030/0x6b0b0/0x65f70 helpers. Same exact-IEEE
# contract as the libraries above.
add_library(port_remastered_capsule_ray STATIC
    platform/port_remastered_capsule_ray.cpp)
target_link_libraries(port_remastered_capsule_ray PUBLIC port_remastered_ground_contacts)
target_include_directories(port_remastered_capsule_ray PUBLIC platform/include)
set_target_properties(port_remastered_capsule_ray PROPERTIES CXX_STANDARD 17)
if(MSVC)
    target_compile_options(port_remastered_capsule_ray PRIVATE /fp:strict)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(port_remastered_capsule_ray PRIVATE -fno-fast-math -ffp-contract=off)
endif()

if(MP_BUILD_TESTS)
    enable_testing()
    include(cmake/PortTests.cmake)
    add_test(NAME port_version COMMAND metroid_prime_port --version)
    set_tests_properties(port_version PROPERTIES TIMEOUT 15 LABELS port
        PASS_REGULAR_EXPRESSION "Metroid Prime native port")
    # The Remastered model converter and the readers it sits on, which several tests
    # build against. An object library, so each test links every object as it did when
    # it listed these sources itself.
    add_library(mp_test_remastered_convert OBJECT
        platform/port_remastered_convert.cpp
        platform/port_remastered_convert_retail.cpp
        platform/port_remastered_convert_material.cpp
        platform/port_remastered_convert_pbr.cpp
        platform/port_remastered_convert_skin.cpp
        platform/port_remastered_anuv.cpp
        platform/port_remastered_image.cpp
        platform/port_remastered_pak.cpp)
    target_include_directories(mp_test_remastered_convert PUBLIC platform/include)
    target_link_libraries(mp_test_remastered_convert PUBLIC astcenc)
    mp_port_test(port_buffer_tests
        SOURCES tests/port_buffers.cpp tests/port_forward_owner.cpp
            src/Kyoto/Streams/CInputStream.cpp src/Kyoto/Streams/CMemoryInStream.cpp
            src/Kyoto/Streams/COutputStream.cpp src/Kyoto/Streams/CMemoryStreamOut.cpp
        INCLUDES include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0)
    mp_port_test(port_ai_tests
        SOURCES tests/port_ai.cpp platform/ai_dma.cpp
        INCLUDES platform/include extern/aurora/include
        DEFINITIONS TARGET_PC
        LIBRARIES ${AURORA_SDL3_TARGET})
    mp_port_test(port_map_pickups_tests
        SOURCES tests/port_map_pickups.cpp platform/port_map_pickups_index.cpp
        INCLUDES platform/include extern/aurora/include
        DEFINITIONS TARGET_PC)
    mp_port_test(port_map_marks_tests SOURCES tests/port_map_marks.cpp INCLUDES platform/include)
    mp_port_test(port_map_side_tests
        SOURCES tests/port_map_side.cpp platform/port_map_side.cpp
        INCLUDES platform/include extern/aurora/include
        DEFINITIONS TARGET_PC)
    mp_port_test(port_randomizer_tests
        SOURCES tests/port_randomizer.cpp platform/port_randomizer.cpp platform/port_json.cpp
        INCLUDES platform/include extern/aurora/include include
        DEFINITIONS TARGET_PC VERSION=0
        LIBRARIES ${AURORA_SDL3_TARGET})
    mp_port_test(port_apclient_tests
        SOURCES tests/port_apclient.cpp
            platform/port_ap_metroidprime.cpp platform/port_ap_protocol.cpp
            platform/port_ap_logic.cpp platform/port_ap_world.cpp platform/port_json.cpp
            platform/port_randomizer.cpp
        INCLUDES platform/include extern/aurora/include include
        DEFINITIONS TARGET_PC VERSION=0
        LIBRARIES ${AURORA_SDL3_TARGET})
    mp_port_test(port_beam_doors_tests
        SOURCES tests/port_beam_doors.cpp platform/port_beam_doors_side.cpp platform/port_map_side.cpp
            platform/port_ap_world.cpp platform/port_json.cpp platform/port_randomizer.cpp
            platform/port_ap_logic.cpp platform/port_ap_metroidprime.cpp
        INCLUDES platform/include extern/aurora/include include
        DEFINITIONS TARGET_PC VERSION=0
        LIBRARIES ${AURORA_SDL3_TARGET})
    mp_port_test(port_ap_solo_tests
        SOURCES tests/port_ap_solo.cpp
            platform/port_ap_solo.cpp platform/port_rando_seed.cpp
            platform/port_ap_metroidprime.cpp platform/port_ap_protocol.cpp
            platform/port_ap_logic.cpp platform/port_ap_world.cpp platform/port_json.cpp
            platform/port_randomizer.cpp
        INCLUDES platform/include extern/aurora/include include
        DEFINITIONS TARGET_PC VERSION=0
        LIBRARIES ${AURORA_SDL3_TARGET})
    mp_port_test(port_ap_logic_tests
        SOURCES tests/port_ap_logic.cpp platform/port_ap_logic.cpp platform/port_ap_metroidprime.cpp
        INCLUDES platform/include extern/aurora/include include
        DEFINITIONS TARGET_PC VERSION=0)
    # 200+ seeds, each a few hundred logic sweeps.
    mp_port_test(port_rando_gen_tests
        SOURCES tests/port_rando_gen.cpp
            platform/port_rando_gen.cpp platform/port_ap_metroidprime.cpp
            platform/port_ap_logic.cpp platform/port_ap_world.cpp platform/port_json.cpp
        INCLUDES platform/include extern/aurora/include include
        DEFINITIONS TARGET_PC VERSION=0
        TIMEOUT 120)
    mp_port_test(port_musyx_tests SOURCES tests/port_musyx.c LIBRARIES musyx)
    # A break must retire a looping voice in the PC mixer, as it does on the DSP.
    mp_port_test(port_musyx_charge_loop_tests SOURCES tests/port_musyx_charge_loop.c LIBRARIES musyx)
    mp_port_test(port_skip_streams_tests
        SOURCES tests/port_skip_streams.cpp
            platform/port_skip_streams.cpp platform/port_skip_cutscenes_ops.cpp
        INCLUDES platform/include platform)
    mp_port_test(port_revision_scripts_tests
        SOURCES tests/port_revision_scripts.cpp
            platform/port_revision_scripts.cpp platform/port_skip_cutscenes_ops.cpp
        INCLUDES platform/include platform)
    mp_port_test(port_disc_tests
        SOURCES tests/port_disc.cpp platform/disc.cpp
        INCLUDES platform/include extern/aurora/include
        DEFINITIONS TARGET_PC
        LIBRARIES ZLIB::ZLIB)
    mp_port_test(port_disc_verify_tests
        SOURCES tests/port_disc_verify.cpp platform/port_disc_verify_core.cpp
        INCLUDES platform/include platform
        LIBRARIES ZLIB::ZLIB)
    mp_port_test(port_mouse_tests SOURCES tests/port_mouse.cpp INCLUDES platform/include)
    # Drives a real SDL virtual gamepad through the touch overlay's own code.
    # That code was Android-only and untested, so a wrong button mapping could
    # not fail anywhere except on a phone.
    mp_port_test(port_touch_pad_tests
        SOURCES tests/touch_pad.cpp platform/touch_pad.cpp
        INCLUDES platform/include extern/aurora/include
        DEFINITIONS TARGET_PC
        LIBRARIES ${AURORA_SDL3_TARGET})
    mp_port_test(port_json_tests
        SOURCES tests/port_json.cpp platform/port_json.cpp
        INCLUDES platform/include
        DEFINITIONS TARGET_PC)
    # The `status --json` physics components: a non-finite value must be JSON null,
    # checked by parsing the production helper's output with the strict reader above.
    # The generated entity field schema (tools/gen_entity_schema.py): it compiles the
    # accessors against every entity header as mp_game does, forced include and all.
    mp_port_test(port_entity_schema_tests
        SOURCES tests/port_entity_schema.cpp platform/port_entity_schema.cpp
        INCLUDES platform/include include extern/aurora/include extern/musyx/include
        DEFINITIONS TARGET_PC AURORA AURORA_ENABLE_GX MUSY_TARGET=MUSY_TARGET_PC)
    mp_signed_char(port_entity_schema_tests)
    if(MSVC)
        target_compile_options(port_entity_schema_tests PRIVATE /FI${CMAKE_CURRENT_SOURCE_DIR}/platform/compat.h)
    else()
        target_compile_options(port_entity_schema_tests PRIVATE -Wno-narrowing -Wno-multichar
            -Wno-write-strings -include ${CMAKE_CURRENT_SOURCE_DIR}/platform/compat.h)
    endif()
    mp_port_test(port_console_physics_tests
        SOURCES tests/port_console_physics.cpp platform/port_json.cpp
        INCLUDES platform/include
        DEFINITIONS TARGET_PC)
    # The TLS end-to-end test runs tools/ap_fake_server.py from the source tree. It
    # starts an openssl-generated CA and a python TLS server, which is not instant on a
    # loaded runner; 15 seconds is enough to time it out and report nothing about which
    # part was slow.
    mp_port_test(port_ws_tests
        SOURCES tests/port_ws.cpp platform/port_ws.cpp
        INCLUDES platform/include
        DEFINITIONS TARGET_PC "MP_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\""
        TIMEOUT 120)
    if(WIN32)
        target_link_libraries(port_ws_tests PRIVATE ws2_32)
    endif()
    mp_websocket_tls(port_ws_tests)
    mp_port_test(port_pal_languages_tests
        SOURCES tests/port_pal_languages.cpp platform/port_pal_languages.cpp
            platform/port_mods_pak.cpp
        INCLUDES platform/include
        LIBRARIES ZLIB::ZLIB)
    mp_port_test(port_flaahgra_music_tests
        SOURCES tests/port_flaahgra_music.cpp platform/port_flaahgra_music.cpp
        INCLUDES platform/include)
    mp_port_test(port_mods_tests
        SOURCES tests/port_mods.cpp platform/port_mods_pak.cpp
        INCLUDES platform/include)
    # The collision-box reader indexes PAK tables and inflates them; a test
    # drives it over synthetic PAKs instead of a disc, so the DVD calls the
    # production source is built from are stubbed out.
    mp_port_test(port_actor_collision_bounds_tests
        SOURCES tests/port_actor_collision_bounds.cpp
            platform/port_actor_collision_bounds.cpp platform/port_mods_pak.cpp platform/port_ws.cpp
        INCLUDES platform/include
        DEFINITIONS PORT_ACTOR_COLLISION_BOUNDS_NO_DVD)
    if(WIN32)
        target_link_libraries(port_actor_collision_bounds_tests PRIVATE ws2_32)
    endif()
    mp_port_test(port_room_env_tests
        SOURCES tests/port_room_env.cpp
            platform/port_room_env_file.cpp platform/port_room_env_tiles.cpp
        INCLUDES platform/include)
    mp_port_test(port_room_env_tiles_tests
        SOURCES tests/port_room_env_tiles.cpp platform/port_room_env_tiles.cpp
        INCLUDES platform/include)
    mp_port_test(port_room_geo_tests
        SOURCES tests/port_room_geo.cpp platform/port_room_geo_file.cpp
        INCLUDES platform/include)
    mp_port_test(port_room_effects_tests
        SOURCES tests/port_room_effects.cpp platform/port_room_effects_file.cpp
        INCLUDES platform/include)
    mp_port_test(port_retail_placement_tests
        SOURCES tests/port_retail_placement_tests.cpp platform/port_retail_placement_file.cpp
        INCLUDES platform/include)
    mp_port_test(port_hd_font_tests
        SOURCES tests/port_hd_font.cpp platform/port_hd_font_file.cpp
            platform/port_remastered_font.cpp platform/port_font_accent_file.cpp
        INCLUDES platform/include)
    mp_port_test(port_remastered_nso_tests
        SOURCES tests/port_remastered_nso.cpp platform/port_remastered_nso.cpp
        INCLUDES platform/include)
    mp_port_test(port_remastered_text_tests
        SOURCES tests/port_remastered_text.cpp platform/port_remastered_text.cpp
        INCLUDES platform/include)
    mp_port_test(port_remastered_effect_tests
        SOURCES tests/port_remastered_effect.cpp platform/port_remastered_effect.cpp
        INCLUDES platform/include)
    # CVectorElement.cpp references particle globals and streams these tests never
    # reach; GC_SECTIONS has the linker drop them.
    mp_port_test(port_remastered_effect_convert_tests
        SOURCES tests/port_remastered_effect_convert.cpp
            platform/port_remastered_effect_convert.cpp platform/port_remastered_effect.cpp
            src/Kyoto/Particles/CVectorElement.cpp src/Kyoto/Math/CQuaternion.cpp
            src/Kyoto/Math/CVector3f.cpp src/Kyoto/Math/CMatrix3f.cpp
        INCLUDES platform/include include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        GC_SECTIONS)
    # Element batch 2 (NEGT, PLNV counts, CRCV, SPEV, ELPS): converter output.
    mp_port_test(port_elem2_convert_tests
        SOURCES tests/port_elem2_convert.cpp
            platform/port_remastered_effect_convert.cpp platform/port_remastered_effect.cpp
            src/Kyoto/Particles/CVectorElement.cpp src/Kyoto/Math/CQuaternion.cpp
            src/Kyoto/Math/CVector3f.cpp src/Kyoto/Math/CMatrix3f.cpp
        INCLUDES platform/include include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        GC_SECTIONS)
    # Element batch 3 (T2S_, SCLE, NFOM, DIVD, swoosh SUST): converter output.
    mp_port_test(port_elem3_convert_tests
        SOURCES tests/port_elem3_convert.cpp
            platform/port_remastered_effect_convert.cpp platform/port_remastered_effect.cpp
            src/Kyoto/Particles/CVectorElement.cpp src/Kyoto/Particles/CRealElement.cpp
            src/Kyoto/Particles/CIntElement.cpp src/Kyoto/Math/CQuaternion.cpp
            src/Kyoto/Math/CVector3f.cpp src/Kyoto/Math/CMatrix3f.cpp
        INCLUDES platform/include include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        GC_SECTIONS)
    mp_port_test(port_remastered_effect_convert_vars_tests
        SOURCES tests/port_remastered_effect_convert_vars.cpp
            platform/port_remastered_effect_convert.cpp platform/port_remastered_effect.cpp
            src/Kyoto/Particles/CVectorElement.cpp src/Kyoto/Math/CQuaternion.cpp
            src/Kyoto/Math/CVector3f.cpp src/Kyoto/Math/CMatrix3f.cpp
        INCLUDES platform/include include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        GC_SECTIONS)
    # ADV9 / IAV0..3 / PAP0 / PAP9 converter output (PPVR).
    mp_port_test(port_remastered_effect_convert_pap_tests
        SOURCES tests/port_remastered_effect_convert_pap.cpp
            platform/port_remastered_effect_convert.cpp platform/port_remastered_effect.cpp
            src/Kyoto/Particles/CVectorElement.cpp src/Kyoto/Math/CQuaternion.cpp
            src/Kyoto/Math/CVector3f.cpp src/Kyoto/Math/CMatrix3f.cpp
        INCLUDES platform/include include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        GC_SECTIONS)

    mp_port_test(port_electric2_bolt_tests
        SOURCES tests/port_electric2_bolt.cpp
        INCLUDES platform/include)
    # Remastered's clustered light gather on the CPU: the grid, the 80-byte records and the mask.
    mp_port_test(port_clustered_lights_tests
        SOURCES tests/port_clustered_lights.cpp platform/port_clustered_lights.cpp
        INCLUDES platform/include)
    # Curl noise math, the VECF field and PCRM; the token half (CPortCurlNoiseField.cpp) is
    # left out and GC_SECTIONS drops what refers to it.
    mp_port_test(port_curl_noise_tests
        SOURCES tests/port_curl_noise.cpp src/Kyoto/Particles/CPortCurlNoise.cpp
            src/Kyoto/Streams/CInputStream.cpp src/Kyoto/Streams/CMemoryInStream.cpp
            src/Kyoto/Math/CVector3f.cpp
        INCLUDES include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        GC_SECTIONS)
    mp_port_test(port_curl_noise_octave_tests
        SOURCES tests/port_curl_noise_octave.cpp src/Kyoto/Particles/CPortCurlNoise.cpp
            src/Kyoto/Streams/CInputStream.cpp src/Kyoto/Streams/CMemoryInStream.cpp
            src/Kyoto/Math/CVector3f.cpp
        INCLUDES include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        GC_SECTIONS)
    mp_port_test(port_remastered_effect_convert_crlnoct_tests
        SOURCES tests/port_remastered_effect_convert_crlnoct.cpp
            platform/port_remastered_effect_convert.cpp platform/port_remastered_effect.cpp
            src/Kyoto/Particles/CVectorElement.cpp src/Kyoto/Math/CQuaternion.cpp
            src/Kyoto/Math/CVector3f.cpp src/Kyoto/Math/CMatrix3f.cpp
        INCLUDES platform/include include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        GC_SECTIONS)
    # ABS_, COSE, RSGN and PIRS: header-only elements (CPortParticleVars.hpp).
    mp_port_test(port_scalar_elements_tests
        SOURCES tests/port_scalar_elements.cpp src/Kyoto/Particles/CParticleGlobals.cpp src/Kyoto/CRandom16.cpp src/Kyoto/Math/CVector3f.cpp
        INCLUDES include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        GC_SECTIONS)
    # ADV9, IAV0..3 and the constant-after-start rule (CPortPartVars).
    mp_port_test(port_part_vars_tests
        SOURCES tests/port_part_vars.cpp src/Kyoto/Particles/CParticleGlobals.cpp src/Kyoto/CRandom16.cpp src/Kyoto/Math/CVector3f.cpp
        INCLUDES include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        GC_SECTIONS)
    # MPAC, MPCN and ANCM (Remastered's vector elements).
    mp_port_test(port_vector_elements_tests
        SOURCES tests/port_vector_elements.cpp src/Kyoto/Particles/CParticleGlobals.cpp src/Kyoto/CRandom16.cpp
            src/Kyoto/Particles/CVectorElement.cpp src/Kyoto/Math/CQuaternion.cpp
            src/Kyoto/Math/CVector3f.cpp src/Kyoto/Math/CMatrix3f.cpp
        INCLUDES include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        GC_SECTIONS)
    # PNCV and ROTV outside a component tree (Remastered's CVENormalizedCompensatedVelocity and
    # CVERotateVector).
    mp_port_test(port_rotv_pncv_tests
        SOURCES tests/port_rotv_pncv.cpp src/Kyoto/Particles/CParticleGlobals.cpp src/Kyoto/CRandom16.cpp
            src/Kyoto/Math/CTransform4f.cpp
            src/Kyoto/Particles/CVectorElement.cpp src/Kyoto/Math/CQuaternion.cpp
            src/Kyoto/Math/CVector3f.cpp src/Kyoto/Math/CMatrix3f.cpp
        INCLUDES include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        GC_SECTIONS)
    mp_port_test(port_remastered_effect_convert_rotv_tests
        SOURCES tests/port_remastered_effect_convert_rotv.cpp
            platform/port_remastered_effect_convert.cpp platform/port_remastered_effect.cpp
            src/Kyoto/Particles/CVectorElement.cpp src/Kyoto/Math/CQuaternion.cpp
            src/Kyoto/Math/CVector3f.cpp src/Kyoto/Math/CMatrix3f.cpp
        INCLUDES platform/include include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        GC_SECTIONS)
    # KEYF (Remastered's CREKeyframeInput): header-only, and its converter output.
    mp_port_test(port_keyf_tests
        SOURCES tests/port_keyf.cpp
        INCLUDES include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        GC_SECTIONS)
    mp_port_test(port_remastered_effect_convert_keyf_tests
        SOURCES tests/port_remastered_effect_convert_keyf.cpp
            platform/port_remastered_effect_convert.cpp platform/port_remastered_effect.cpp
            src/Kyoto/Particles/CVectorElement.cpp src/Kyoto/Math/CQuaternion.cpp
            src/Kyoto/Math/CVector3f.cpp src/Kyoto/Math/CMatrix3f.cpp
        INCLUDES platform/include include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        GC_SECTIONS)
    # SLTW (Remastered's lifetime tween on the system slot): the element and its converter output.
    mp_port_test(port_system_tween_tests
        SOURCES tests/port_system_tween.cpp src/Kyoto/Particles/CParticleGlobals.cpp src/Kyoto/CRandom16.cpp
            src/Kyoto/Math/CVector3f.cpp
        INCLUDES include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        GC_SECTIONS)
    mp_port_test(port_remastered_effect_convert_sltw_tests
        SOURCES tests/port_remastered_effect_convert_sltw.cpp
            platform/port_remastered_effect_convert.cpp platform/port_remastered_effect.cpp
            src/Kyoto/Particles/CVectorElement.cpp src/Kyoto/Math/CQuaternion.cpp
            src/Kyoto/Math/CVector3f.cpp src/Kyoto/Math/CMatrix3f.cpp
        INCLUDES platform/include include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        GC_SECTIONS)
    # PLNE and PLNV (Remastered's plane emitters), their spread and PLNV's shuffler.
    mp_port_test(port_plane_emitters_tests
        SOURCES tests/port_plane_emitters.cpp src/Kyoto/Particles/CParticleGlobals.cpp src/Kyoto/CRandom16.cpp
            src/Kyoto/Particles/CEmitterElement.cpp
            src/Kyoto/Math/CVector3f.cpp src/Kyoto/Math/CMatrix3f.cpp
        INCLUDES include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        GC_SECTIONS)
    # SPEV and ELPS (Remastered's sphere with variance and ellipsoid, the port-only PSPV and PELP).
    mp_port_test(port_sphere_emitters_tests
        SOURCES tests/port_sphere_emitters.cpp src/Kyoto/Particles/CParticleGlobals.cpp src/Kyoto/CRandom16.cpp
            src/Kyoto/Particles/CEmitterElement.cpp
            src/Kyoto/Math/CVector3f.cpp src/Kyoto/Math/CMatrix3f.cpp
        INCLUDES include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        GC_SECTIONS)
    # ASPV and LINV (Remastered's angle sphere and line with variance, the port-only PASV and PLWV).
    mp_port_test(port_variance_emitters_tests
        SOURCES tests/port_variance_emitters.cpp src/Kyoto/Particles/CParticleGlobals.cpp src/Kyoto/CRandom16.cpp
            src/Kyoto/Particles/CEmitterElement.cpp src/Kyoto/Particles/CVectorElement.cpp
            src/Kyoto/Math/CQuaternion.cpp src/Kyoto/Math/CVector3f.cpp src/Kyoto/Math/CMatrix3f.cpp
        INCLUDES include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        GC_SECTIONS)
    # DBIS (Remastered's particle depth bias, the port-only PDBS): the projection m22 offset.
    mp_port_test(port_particle_depth_bias_tests
        SOURCES tests/port_particle_depth_bias.cpp
        INCLUDES include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        GC_SECTIONS)
    # CRCV (Remastered's circle with variance, the port-only PCRC).
    mp_port_test(port_circle_variance_tests
        SOURCES tests/port_circle_variance.cpp src/Kyoto/Particles/CParticleGlobals.cpp src/Kyoto/CRandom16.cpp
            src/Kyoto/Particles/CVectorElement.cpp src/Kyoto/Math/CQuaternion.cpp
            src/Kyoto/Math/CVector3f.cpp src/Kyoto/Math/CMatrix3f.cpp
        INCLUDES include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        GC_SECTIONS)
    mp_port_test(port_ice_rotation_element_tests
        SOURCES tests/port_ice_rotation_element.cpp
            src/Kyoto/Particles/CVectorElement.cpp src/Kyoto/Math/CQuaternion.cpp
            src/Kyoto/Math/CVector3f.cpp src/Kyoto/Math/CMatrix3f.cpp
        INCLUDES include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        GC_SECTIONS)
    mp_port_test(port_remastered_anim_tests
        SOURCES tests/port_remastered_anim_tests.cpp platform/port_remastered_anim.cpp
        INCLUDES platform/include)
    mp_port_test(port_nested_light_tests
        SOURCES tests/port_nested_light_math.cpp
            src/Kyoto/Graphics/CLight.cpp src/Kyoto/Graphics/CColor.cpp
            src/Kyoto/Math/RMathUtils.cpp src/Kyoto/Math/CVector3f.cpp
            src/Kyoto/Streams/CInputStream.cpp src/Kyoto/Streams/COutputStream.cpp
        INCLUDES include extern/aurora/include platform/include
        DEFINITIONS TARGET_PC VERSION=0)
    mp_port_test(port_remastered_effect_import_tests
        SOURCES tests/port_remastered_effect_import.cpp
            platform/port_remastered_effect_import.cpp platform/port_remastered_effect_convert.cpp
            platform/port_remastered_effect.cpp platform/port_remastered_image.cpp
        INCLUDES platform/include include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        LIBRARIES astcenc)
    mp_port_test(port_remastered_import_tests
        SOURCES tests/port_remastered_import.cpp platform/port_remastered_effect.cpp
        INCLUDES platform/include)
    mp_port_test(port_queen_barrier_validate_tests
        SOURCES tests/port_queen_barrier_validate.cpp
            platform/port_queen_barrier_validate.cpp
            platform/port_remastered_effect_convert.cpp platform/port_remastered_effect.cpp
        INCLUDES platform/include include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0)
    # Where the built-in Remastered import lives and how a scan picks it up
    # (port_remastered_storage.h): policy plus the folder walk, so it gets a scratch folder of its own.
    mp_port_test(port_remastered_storage_tests
        SOURCES tests/port_remastered_storage.cpp
        INCLUDES platform/include
        ARGS "${CMAKE_CURRENT_BINARY_DIR}/remastered-storage-test")
    mp_port_test(port_envfx_file_tests
        SOURCES tests/port_envfx_file.cpp platform/port_envfx_file.cpp
        INCLUDES platform/include)
    mp_port_test(port_pause_lighting_tests
        SOURCES tests/port_pause_lighting.cpp
        INCLUDES platform/include)
    mp_port_test(port_shell_file_tests
        SOURCES tests/port_shell_file.cpp platform/port_shell_file.cpp
        INCLUDES platform/include)
    mp_port_test(port_hud_bars_tests
        SOURCES tests/port_hud_bars.cpp platform/port_hud_bars_file.cpp
        INCLUDES platform/include)
    mp_port_test(port_remastered_hud_tests
        SOURCES tests/port_remastered_hud.cpp platform/port_remastered_hud.cpp
            platform/port_hud_bars_file.cpp platform/port_remastered_image.cpp
            platform/port_remastered_pak.cpp
        INCLUDES platform/include
        LIBRARIES astcenc)
    mp_port_test(port_remastered_movie_tests
        SOURCES tests/port_remastered_movie.cpp platform/port_remastered_movie.cpp
            platform/port_remastered_jpeg.cpp
        INCLUDES platform/include)
    mp_port_test(port_gallery_tests
        SOURCES tests/port_gallery.cpp platform/port_gallery.cpp platform/port_remastered_jpeg.cpp
        INCLUDES platform/include
        LIBRARIES aurora::thp)
    mp_port_test(port_remastered_uv_tests
        SOURCES tests/port_remastered_uv_tests.cpp
        LIBRARIES mp_test_remastered_convert)
    mp_port_test(port_remastered_source_file_tests
        SOURCES tests/port_remastered_source_file.cpp
            platform/port_remastered_nsp.cpp platform/port_remastered_nso.cpp
        INCLUDES platform/include
        LIBRARIES ${CMAKE_THREAD_LIBS_INIT})
    if(OpenSSL_FOUND)
        # A synthetic encrypted image with made-up keys (no console keys needed).
        mp_port_test(port_remastered_nsp_tests
            SOURCES tests/port_remastered_nsp.cpp
                platform/port_remastered_nsp.cpp platform/port_remastered_nso.cpp
            INCLUDES platform/include
            TIMEOUT 30)
        mp_websocket_tls(port_remastered_nsp_tests)
    endif()
    mp_port_test(port_remastered_parsers_tests
        SOURCES tests/port_remastered_parsers.cpp
            platform/port_remastered_pak.cpp platform/port_remastered_txtr.cpp
            platform/port_remastered_tegra_swizzle.cpp platform/port_remastered_bcdec.cpp
            platform/port_remastered_astc_decode.cpp
        INCLUDES platform/include)
    mp_port_test(port_shot_aim_tests SOURCES tests/port_shot_aim.cpp INCLUDES platform/include)
    mp_port_test(port_remastered_report_tests
        SOURCES tests/port_remastered_report_tests.cpp platform/port_remastered_report.cpp
        INCLUDES platform/include)
    mp_port_test(port_remastered_anuv_tests
        SOURCES tests/port_remastered_anuv_tests.cpp platform/port_remastered_anuv.cpp
        INCLUDES platform/include)
    mp_port_test(port_remastered_lits_tests
        SOURCES tests/port_remastered_lits_tests.cpp
        LIBRARIES mp_test_remastered_convert)
    mp_port_test(port_remastered_orient_normal_tests
        SOURCES tests/port_remastered_orient_normal_tests.cpp
        LIBRARIES mp_test_remastered_convert)
    mp_port_test(port_remastered_ice_tests
        SOURCES tests/port_remastered_ice_tests.cpp
        LIBRARIES mp_test_remastered_convert)
    mp_port_test(port_remastered_driver_tests
        SOURCES tests/port_remastered_driver_tests.cpp
            platform/port_remastered_room_file.cpp platform/port_remastered_room_retail.cpp
            platform/port_remastered_room_scripts.cpp
            platform/port_remastered_room_effects.cpp platform/port_room_effects_file.cpp
            platform/port_remastered_anim.cpp platform/port_remastered_cmdl.cpp
            platform/port_remastered_txtr.cpp platform/port_room_env_file.cpp
            platform/port_remastered_tegra_swizzle.cpp platform/port_remastered_bcdec.cpp
            platform/port_remastered_astc_decode.cpp
            platform/port_room_env_tiles.cpp platform/port_room_geo_file.cpp
        INCLUDES platform/include include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        LIBRARIES mp_test_remastered_convert)
    # The world master's script resolver and the rooms' effect actors, over synthetic ROOM files.
    mp_port_test(port_remastered_room_scripts_tests
        SOURCES tests/port_remastered_room_scripts.cpp
            platform/port_remastered_room_file.cpp platform/port_remastered_room_retail.cpp
            platform/port_remastered_room_scripts.cpp
            platform/port_remastered_room_effects.cpp platform/port_room_effects_file.cpp
            platform/port_remastered_anim.cpp platform/port_remastered_cmdl.cpp
            platform/port_remastered_txtr.cpp platform/port_room_env_file.cpp
            platform/port_remastered_tegra_swizzle.cpp platform/port_remastered_bcdec.cpp
            platform/port_remastered_astc_decode.cpp
            platform/port_room_env_tiles.cpp platform/port_room_geo_file.cpp
        INCLUDES platform/include platform include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        LIBRARIES mp_test_remastered_convert)
    mp_port_test(port_remastered_blend_tests
        SOURCES tests/port_remastered_blend_tests.cpp
        LIBRARIES mp_test_remastered_convert)
    mp_port_test(port_inventory_visor_tests
        SOURCES tests/port_inventory_visor_tests.cpp
        INCLUDES platform/include include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        LIBRARIES mp_test_remastered_convert)
    mp_port_test(port_remastered_astc_tests
        SOURCES tests/port_remastered_astc.cpp platform/port_remastered_image.cpp
        INCLUDES platform/include
        LIBRARIES astcenc)
    mp_port_test(port_remastered_image_tests
        SOURCES tests/port_remastered_image_tests.cpp platform/port_remastered_image.cpp
        INCLUDES platform/include
        LIBRARIES astcenc)
    mp_port_test(port_importers_tests
        SOURCES tests/port_importers.cpp platform/port_importers_lines.cpp
        INCLUDES platform/include)
    mp_port_test(port_ball_looks_tests
        SOURCES tests/port_ball_looks.cpp platform/port_ball_looks.cpp
            platform/port_remastered_image.cpp
        INCLUDES platform/include
        LIBRARIES astcenc)
    mp_port_test(port_suit_colours_tests
        SOURCES tests/port_suit_colours.cpp platform/port_suit_colours.cpp
        INCLUDES platform/include
        LIBRARIES astcenc)
    mp_port_test(port_custom_res_tests
        SOURCES tests/port_custom_res.cpp platform/port_custom_res.cpp
        INCLUDES platform/include)
    mp_port_test(port_env_tests SOURCES tests/port_env.cpp INCLUDES platform/include)
    mp_port_test(port_bytes_hash_tests SOURCES tests/port_bytes_hash.cpp INCLUDES platform/include)
    mp_port_test(port_air_mover_contact_tests
        SOURCES tests/port_air_mover_contact.cpp
            src/Collision/CMaterialFilter.cpp platform/port_remastered_ground_contacts.cpp
            platform/port_remastered_ground_world.cpp
        INCLUDES platform/include include extern/sdk/include)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(port_air_mover_contact_tests PRIVATE -ffp-contract=off -fno-fast-math)
    endif()
    mp_port_test(port_octree_frontonly_tests
        SOURCES tests/port_octree_frontonly.cpp
            src/WorldFormat/CAreaOctTree_Tests.cpp src/WorldFormat/CAreaOctTree.cpp
            src/WorldFormat/CCollisionSurface.cpp src/Collision/CMaterialFilter.cpp
            src/Kyoto/Math/CAABox.cpp src/Kyoto/Math/CVector3i.cpp src/Kyoto/Math/CVector3f.cpp
            src/Kyoto/Streams/CInputStream.cpp src/Kyoto/Streams/CMemoryInStream.cpp
            src/Kyoto/Streams/COutputStream.cpp src/Kyoto/Math/CVector2f.cpp
            src/Kyoto/Math/CTransform4f.cpp src/Kyoto/Math/CUnitVector3f.cpp src/Kyoto/Math/CPlane.cpp
            src/Kyoto/Math/RMathUtils.cpp
        INCLUDES include extern/aurora/include extern/sdk/include
        DEFINITIONS TARGET_PC VERSION=0)
    # The exact-IEEE Remastered collision kernels, under the same flags as their libraries.
    # The box-actor landing query is header-only over the 0xbf61b4 impacts and links the
    # same contact library.
    foreach(t port_remastered_ground_contacts port_collision_failsafe port_remastered_box_landing)
        mp_port_test(${t}_tests
            SOURCES tests/${t}.cpp
            LIBRARIES port_remastered_ground_contacts
            CXX_STANDARD 17 EXACT_IEEE)
    endforeach()
    # The sphere-vs-sphere swept query: the hand cases of its spec plus the shared
    # NormalizeWithMagnitude helper.
    mp_port_test(port_remastered_sphere_sweep_tests
        SOURCES tests/port_remastered_sphere_sweep.cpp
        LIBRARIES port_remastered_sphere_sweep
        CXX_STANDARD 17 EXACT_IEEE)
    # The ray-vs-capsule kernel: the capsule spec's hand cases, its threshold and sentinel
    # behaviour, and the shared quadratic/cylinder/reciprocal prerequisites.
    mp_port_test(port_remastered_capsule_ray_tests
        SOURCES tests/port_remastered_capsule_ray.cpp
        LIBRARIES port_remastered_capsule_ray
        CXX_STANDARD 17 EXACT_IEEE)
    # The Remastered grounded mover's pure policy (header-only): the entry, response,
    # snap and retained-object-state decisions.
    mp_port_test(port_remastered_ground_policy_tests
        SOURCES tests/port_remastered_ground_policy.cpp
        INCLUDES platform/include
        EXACT_IEEE)
    # Moving platforms under the Remastered grounded mover (issue #117 slice 1): the tree
    # walk, contact order, rider choice and decay, carry, admission and exactly-once carry.
    mp_port_test(port_ground_rider_tests
        SOURCES tests/port_ground_rider.cpp
            src/Collision/CMaterialFilter.cpp platform/port_remastered_ground_contacts.cpp
            platform/port_remastered_ground_world.cpp platform/port_collision_failsafe.cpp
        INCLUDES platform/include include extern/sdk/include
        EXACT_IEEE)
    # Remastered MP1 collision material translation and filter policy (header-only).
    mp_port_test(port_remastered_collision_material_tests
        SOURCES tests/port_remastered_collision_material.cpp
        INCLUDES platform/include
        LIBRARIES port_remastered_ground_contacts
        EXACT_IEEE)
    mp_port_test(port_hdr_scene_policy_tests
        SOURCES tests/port_hdr_scene_policy.cpp
        INCLUDES include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0)
    mp_port_test(port_actor_shadow_tests SOURCES tests/port_actor_shadow.cpp INCLUDES platform/include)
    mp_port_test(port_visor_mesh_tests SOURCES tests/port_visor_mesh.cpp INCLUDES platform/include)
    mp_port_test(port_hold_toggle_tests SOURCES tests/port_hold_toggle.cpp INCLUDES platform/include)
    mp_port_test(port_pointer_tests SOURCES tests/port_pointer.cpp INCLUDES platform/include)
    mp_port_test(port_input_engine_tests
        SOURCES tests/port_input_engine.cpp platform/input/port_input_engine.cpp
        INCLUDES platform/include)
    mp_port_test(port_input_bindings_tests
        SOURCES tests/port_input_bindings.cpp
            platform/input/port_input_bindings.cpp platform/input/port_input_engine.cpp
        INCLUDES platform/include)
    mp_port_test(port_tick_pair_tests SOURCES tests/port_tick_pair.cpp INCLUDES platform/include)
    mp_port_test(port_dynamic_res_tests
        SOURCES tests/port_dynamic_res.cpp platform/port_dynamic_res.cpp
        INCLUDES platform/include)
    mp_port_test(port_presentation_rules_tests
        SOURCES tests/port_presentation_rules.cpp
        INCLUDES platform/include)
    mp_port_test(port_input_map_tests SOURCES tests/port_input_map.cpp INCLUDES platform/include)
    mp_port_test(port_input_settings_tests
        SOURCES tests/port_input_settings.cpp platform/port_input_settings.cpp
        INCLUDES platform/include)
    mp_port_test(port_gyro_motion_tests
        SOURCES tests/port_gyro_motion.cpp platform/port_gyro_motion.cpp
        INCLUDES platform/include)
    # The Windows renderer choice (issue #135): the names the settings file and
    # MP_GRAPHICS_BACKEND carry, the per-run override that outranks the file, and the
    # status the Video page draws (platform/include/port_graphics_backend.h). The
    # settings key's own read/write is covered by port_settings_roundtrip.
    mp_port_test(port_graphics_backend_tests
        SOURCES tests/port_graphics_backend.cpp
        INCLUDES platform/include extern/aurora/include)
    mp_port_test(port_log_redact_tests SOURCES tests/port_log_redact.cpp INCLUDES platform/include)
    mp_port_test(port_data_folder_tests
        SOURCES tests/port_data_folder.cpp platform/port_data_folder.cpp
        INCLUDES platform/include
        LIBRARIES ${AURORA_SDL3_TARGET}
        ARGS "${CMAKE_CURRENT_BINARY_DIR}/data-folder-test")
    mp_port_test(port_config_reset_tests
        SOURCES tests/port_config_reset.cpp
        INCLUDES platform/include
        ARGS "${CMAKE_CURRENT_BINARY_DIR}/config-reset-test")
    mp_port_test(port_discord_tests
        SOURCES tests/port_discord.cpp platform/port_json.cpp
        INCLUDES platform/include
        DEFINITIONS TARGET_PC)
    mp_port_test(port_update_check_tests
        SOURCES tests/port_update_check.cpp platform/port_update_check.cpp
            platform/port_ws.cpp platform/port_json.cpp
        INCLUDES platform/include
        DEFINITIONS TARGET_PC)
    if(WIN32)
        target_link_libraries(port_update_check_tests PRIVATE ws2_32)
    endif()
    mp_websocket_tls(port_update_check_tests)
    mp_port_test(port_livesplit_tests SOURCES tests/port_livesplit.cpp INCLUDES platform/include)
    mp_port_test(port_savestate_tests
        SOURCES tests/port_savestate.cpp platform/port_savestate_format.cpp
        INCLUDES platform/include)
    mp_port_test(port_timing_tests SOURCES tests/port_timing.cpp INCLUDES platform/include)
    mp_port_test(port_audio_math_tests
        SOURCES tests/port_audio_math.c
        INCLUDES extern/musyx/include)
    mp_port_test(port_card_tests
        SOURCES tests/port_card.cpp
        LIBRARIES aurora::card
        ARGS "${CMAKE_CURRENT_BINARY_DIR}/card-test-data")
    mp_port_test(port_gci_tests
        SOURCES tests/port_gci.cpp platform/port_gci.cpp platform/port_save_convert.cpp
        INCLUDES platform/include
        LIBRARIES aurora::card)
    if(WIN32)
        target_link_libraries(port_gci_tests PRIVATE shell32 ole32 advapi32)
    endif()
    # Save converter. Fixtures (real saves and scripts/wii2gci.py) are local
    # data: point PORT_SAVE_FIXTURES at them, or the fixture checks are skipped.
    set(PORT_SAVE_FIXTURES "${CMAKE_SOURCE_DIR}/../wii-save" CACHE PATH "Save converter fixtures (saves/, scripts/)")
    mp_port_test(port_save_convert_tests
        SOURCES tests/port_save_convert.cpp platform/port_save_convert.cpp
        INCLUDES platform/include
        ARGS "${PORT_SAVE_FIXTURES}"
        TIMEOUT 120)
    mp_port_test(port_charge_locator_tests
        SOURCES tests/port_charge_locator.cpp
        INCLUDES include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0)
    mp_port_test(port_power_charge_release_tests
        SOURCES tests/port_power_charge_release.cpp
        INCLUDES platform/include
        DEFINITIONS TARGET_PC VERSION=0)
    mp_port_test(port_particle_math_tests
        SOURCES tests/port_particle_math.cpp src/Kyoto/Particles/CParticleGlobals.cpp src/Kyoto/CRandom16.cpp
            src/Kyoto/Math/CVector3f.cpp src/Kyoto/Math/CTransform4f.cpp
        INCLUDES include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        GC_SECTIONS)
    # The model (mesh) opacity selection of Remastered's OpacityFresnel recipes: header-only
    # (platform/include/port_vfx_opacity.h), so it needs no sources of its own.
    mp_port_test(port_vfx_opacity_tests
        SOURCES tests/port_vfx_opacity.cpp
        INCLUDES platform/include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0)
    # VFX_BarrierShield's fragment maths: a scalar golden reference for the breakup, the motion, the
    # .45 sawtooth boundary and the chromatic scene samples (header-only: the maths is the shader's).
    mp_port_test(port_vfx_barrier_tests
        SOURCES tests/port_vfx_barrier.cpp
        INCLUDES platform/include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0)
    mp_port_test(port_vfx_material_tests
        SOURCES tests/port_vfx_material.cpp
            src/Kyoto/Particles/CPortVfxMat.cpp platform/port_vfx_particles.cpp
            src/Kyoto/Particles/CGenDescription.cpp src/Kyoto/Particles/CColorElement.cpp
            src/Kyoto/Graphics/DolphinCColor.cpp
            src/Kyoto/Streams/CInputStream.cpp src/Kyoto/Streams/CMemoryInStream.cpp
            src/Kyoto/Math/CVector3f.cpp
        INCLUDES platform/include include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        GC_SECTIONS)
    mp_port_test(port_swoosh_ring_tests SOURCES tests/port_swoosh_ring.cpp INCLUDES platform/include)
    # Remastered's visor goo splat manager maths (include/MetroidPrime/PortVisorGoo.hpp).
    mp_port_test(port_visor_goo_tests
        SOURCES tests/port_visor_goo_tests.cpp src/Kyoto/CRandom16.cpp src/Kyoto/Math/CVector3f.cpp
        INCLUDES include extern/aurora/include
        DEFINITIONS TARGET_PC VERSION=0
        GC_SECTIONS)
    # --- Dread readers A (PKG, BCTEX, title check) ---
    # CRC-64, .pkg table, .bctex (gzip, block-linear, BC1/3/5) and the opened image's title id,
    # over synthetic files; real-data checks are opt-in (PORT_DREAD_ROMFS, PORT_DREAD_NSP).
    mp_port_test(port_dread_readers_a_tests
        SOURCES tests/port_dread_readers_a.cpp
            platform/port_dread_pkg.cpp platform/port_dread_bctex.cpp
            platform/port_remastered_tegra_swizzle.cpp platform/port_remastered_bcdec.cpp
            platform/port_remastered_nsp.cpp platform/port_remastered_nso.cpp
        INCLUDES platform/include tests
        LIBRARIES ZLIB::ZLIB ${CMAKE_THREAD_LIBS_INIT}
        TIMEOUT 120)
    mp_websocket_tls(port_dread_readers_a_tests)
    # --- end Dread readers A ---
    # --- Dread readers B (BCMDL, BSMAT) ---
    mp_port_test(port_dread_readers_b_tests
        SOURCES tests/port_dread_readers_b.cpp platform/port_dread_bcmdl.cpp
            platform/port_dread_bsmat.cpp
        INCLUDES platform/include
        LIBRARIES ZLIB::ZLIB)
    # --- end Dread readers B ---
    # --- Dread re-skin (step 4: Dread Samus onto Prime's skeletons) ---
    # CINF parse, frame/scale fit, repose and weight mapping over synthetic stick figures;
    # the real-data run is opt-in (PORT_DREAD_SAMUS_PKG_DIR + PORT_PRIME_SAMUS_DIR).
    mp_port_test(port_dread_reskin_tests
        SOURCES tests/port_dread_reskin.cpp platform/port_dread_reskin.cpp platform/port_dread_bcmdl.cpp
        INCLUDES platform/include
        LIBRARIES ZLIB::ZLIB)
    # --- end Dread re-skin ---
    # --- Dread suit material (step 5: mp_samuspbr -> synthetic Remastered PBR material) ---
    # Channel remap, emissive and ids over synthetic fixtures, then through the converter's
    # ReadMaterial; the real-data run is opt-in (PORT_DREAD_SAMUS_PKG_DIR).
    mp_port_test(port_dread_material_tests
        SOURCES tests/port_dread_material.cpp platform/port_dread_material.cpp platform/port_dread_bctex.cpp
            platform/port_dread_bsmat.cpp platform/port_remastered_tegra_swizzle.cpp
            platform/port_remastered_bcdec.cpp
        INCLUDES platform platform/include
        LIBRARIES mp_test_remastered_convert ZLIB::ZLIB ${CMAKE_THREAD_LIBS_INIT}
        TIMEOUT 120)
    # --- end Dread suit material ---
    # Third-person camera: eye placement and wall pull-in maths (header-only, no game state).
    mp_port_test(port_third_person_tests
        SOURCES tests/port_third_person.cpp
        INCLUDES platform/include)
    mp_port_test(port_third_person_body_tests
        SOURCES tests/port_third_person_body.cpp
        INCLUDES platform/include)
    # The order a mod reload runs in: the port's own model holders go before the PAKs reopen
    # (port_world_holders.h), or the next world takes this one's models from the pool.
    mp_port_test(port_world_holders_tests
        SOURCES tests/port_world_holders.cpp platform/port_world_holders.cpp
        INCLUDES platform/include)
    # --- Dread Samus import (step 6: the dread-samus mod) ---
    # The converter's explicit-weights path, the synthetic Model of a re-skinned body, the retail
    # skin reader and the mod order rule over synthetic fixtures; the whole import on real data is
    # opt-in (PORT_DREAD_ROMFS + PORT_PRIME_SAMUS_DIR).
    mp_port_test(port_dread_samus_tests
        SOURCES tests/port_dread_samus.cpp platform/port_dread_samus.cpp platform/port_dread_material.cpp
            platform/port_dread_bctex.cpp platform/port_dread_bsmat.cpp platform/port_dread_bcmdl.cpp
            platform/port_dread_pkg.cpp platform/port_dread_reskin.cpp
            platform/port_remastered_tegra_swizzle.cpp platform/port_remastered_bcdec.cpp
            platform/port_remastered_cmdl.cpp platform/port_ball_looks.cpp
        INCLUDES platform platform/include
        LIBRARIES mp_test_remastered_convert ZLIB::ZLIB ${CMAKE_THREAD_LIBS_INIT}
        TIMEOUT 600)
    # --- end Dread Samus import ---
    # Every port_settings.ini key reads back as written (platform/debug_ui/settings.cpp,
    # kSettingKeys); the game binary checks itself and exits before starting.
    add_test(NAME port_settings_roundtrip COMMAND metroid_prime_port --settings-selftest)
    set_tests_properties(port_settings_roundtrip PROPERTIES TIMEOUT 30 LABELS port
        ENVIRONMENT "MP_USER_PATH=${CMAKE_CURRENT_BINARY_DIR}/settings-selftest/")
    # The tools' own checks, so a change to the location join or the fixture
    # generator fails here rather than in whoever runs the tools next. Both are
    # dependency-free Python and skip themselves when python3 is absent.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME port_android_save_dialog_title
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/port_android_save_dialog_title.py
                    ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(port_android_save_dialog_title PROPERTIES TIMEOUT 15 LABELS port)
        add_test(NAME port_ap_fixture
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/port_ap_fixture.py
                    ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(port_ap_fixture PROPERTIES TIMEOUT 120 LABELS port)
        # The rev2 skip tables; PORT_SKIP_DISCS ("100=iso;102=iso;...", local data) adds
        # the all-disc replay.
        set(PORT_SKIP_DISCS "" CACHE STRING "Disc images for the skip table replay, version=path;...")
        add_test(NAME port_skip_rev2
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/port_skip_rev2.py
                    ${CMAKE_CURRENT_SOURCE_DIR} ${PORT_SKIP_DISCS})
        set_tests_properties(port_skip_rev2 PROPERTIES TIMEOUT 900 LABELS port)
        # The shipped pipeline seed: every row at the current config versions (else aurora
        # ignores it) and retail only (tools/pipeline_seed.py explains re-recording it).
        add_test(NAME port_pipeline_seed
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/pipeline_seed.py check)
        set_tests_properties(port_pipeline_seed PROPERTIES TIMEOUT 60 LABELS port)
        # The F1 settings search index against the widgets it names, both ways.
        add_test(NAME port_settings_index
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_settings_index.py
                    ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(port_settings_index PROPERTIES TIMEOUT 15 LABELS port)
        # The committed entity schema against the headers; skips (77) without clang-19.
        add_test(NAME port_entity_schema_current
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_entity_schema.py --check)
        set_tests_properties(port_entity_schema_current PROPERTIES TIMEOUT 300 LABELS port
            SKIP_RETURN_CODE 77)
        add_test(NAME port_air_mover_wiring
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/port_air_mover_wiring.py
                    ${CMAKE_CURRENT_SOURCE_DIR}/platform/port_remastered_movers.cpp)
        set_tests_properties(port_air_mover_wiring PROPERTIES TIMEOUT 15 LABELS port)
        # The door payload reaches its encoder and decoder from the capture
        # and the restore (issue #131).
        add_test(NAME port_door_save_wiring
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/port_door_save_wiring.py
                    ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(port_door_save_wiring PROPERTIES TIMEOUT 15 LABELS port)
        # A state saved in a boss fight restores the fight: delivered activation,
        # regenerated generator copies and the replayed script message journal.
        add_test(NAME port_boss_save_wiring
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/port_boss_save_wiring.py
                    ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(port_boss_save_wiring PROPERTIES TIMEOUT 15 LABELS port)
        # A totem scan, not the text refresh at room load, asks the AP server for
        # the artifact hint (issue #170).
        add_test(NAME port_ap_scan_hint_wiring
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/port_ap_scan_hint_wiring.py
                    ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(port_ap_scan_hint_wiring PROPERTIES TIMEOUT 15 LABELS port)
        # No raw string literal piece grows past MSVC's 16 KB limit (issue #170).
        add_test(NAME port_msvc_string_limit
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/port_msvc_string_limit.py
                    ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(port_msvc_string_limit PROPERTIES TIMEOUT 30 LABELS port)
        # The map side file reaches its staging, its flush and its load from the
        # card driver, and the save state carries the same data in its blob.
        add_test(NAME port_map_side_wiring
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/port_map_side_wiring.py
                    ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(port_map_side_wiring PROPERTIES TIMEOUT 15 LABELS port)
        # The Extra map UI setting gates the legend entries and the Mark room
        # prompt alone, is off by default, and is persisted.
        add_test(NAME port_map_extra_ui_wiring
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/port_map_extra_ui_wiring.py
                    ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(port_map_extra_ui_wiring PROPERTIES TIMEOUT 15 LABELS port)
        # F1's Mods page holds the Remastered and Dread sections: one page, the old
        # MP_DEBUG_TAB names opening their section, and the search rows reaching it.
        add_test(NAME port_mods_page_wiring
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/port_mods_page_wiring.py
                    ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(port_mods_page_wiring PROPERTIES TIMEOUT 15 LABELS port)
        # The model-opacity key must be carried by exactly one row of the converter's
        # recipe table (port_vfx_opacity.h): the C++ tests pin that one recipe's word
        # and the selection, not the count.
        add_test(NAME port_vfx_opacity_wiring
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/port_vfx_opacity_wiring.py
                    ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(port_vfx_opacity_wiring PROPERTIES TIMEOUT 15 LABELS port)
        # BarrierShield (bit 29) is wired the same way everywhere: the shader branch, both fog
        # exclusions, both slot reads, the scene gate, the CCH rows and the time, the vertex
        # attribute, the mesh feed and readiness, and the converter's recipe and MFC0 gate.
        add_test(NAME port_vfx_barrier_wiring
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/port_vfx_barrier_wiring.py
                    ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(port_vfx_barrier_wiring PROPERTIES TIMEOUT 15 LABELS port)
        # Every command in the console's table is documented in docs/CONSOLE.md.
        add_test(NAME port_console_docs
            COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/port_console_docs.py
                    ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(port_console_docs PROPERTIES TIMEOUT 15 LABELS port)
    endif()
endif()
