cmake_minimum_required(VERSION 3.25)
project(metroid_prime_port LANGUAGES C CXX)

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
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
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
endif()

if(MP_ANDROID_NOD_STUB)
    if(NOT ANDROID)
        message(FATAL_ERROR "MP_ANDROID_NOD_STUB is only valid for Android builds")
    endif()
    add_library(mp_android_nod_stub STATIC platform/android_probe/nod_stub.c)
    target_include_directories(mp_android_nod_stub PUBLIC platform/android_probe/include)
    add_library(nod::nod ALIAS mp_android_nod_stub)
endif()

# Aurora provides the GameCube/Wii compatibility layer (MIT): GX, OS, PAD, DVD,
# CARD, VI, SI, THP, mtx, plus the SDL3/WebGPU application layer.
add_subdirectory(extern/aurora EXCLUDE_FROM_ALL)
if(TARGET aurora_mtx)
    target_compile_definitions(aurora_mtx PRIVATE MTX_USE_PS=1)
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
# The port has an explicit source manifest, including registration-only and
# shim-dependent translation units. Link those objects directly on both ELF and
# COFF rather than relying on archive extraction or suppressing duplicate symbols.
add_library(mp_game OBJECT ${MP_GAME_SOURCES})
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
    platform/disc.cpp
    platform/shims.cpp
    platform/sdk_stubs.cpp
    platform/port_embedded.cpp
    platform/port_textures.cpp
    platform/port_prompts.cpp
    platform/port_controls.cpp
    platform/port_randomizer.cpp
    platform/port_ap_metroidprime.cpp
    platform/port_ap_protocol.cpp
    platform/port_ap_logic.cpp
    platform/port_ap_world.cpp
    platform/port_rando_gen.cpp
    platform/port_apclient.cpp
    platform/port_ap_solo.cpp
    platform/port_rando_seed.cpp
    platform/port_json.cpp
    platform/port_gpu_driver.cpp
    platform/port_discord.cpp
    platform/port_update_check.cpp
    platform/port_livesplit.cpp
    platform/port_tracker.cpp
    platform/port_map_pickups.cpp
    platform/port_viewmodel.cpp
    platform/port_freecam.cpp
    platform/port_console.cpp
    platform/port_log_file.cpp
    platform/port_crash.cpp
    platform/port_watchdog.cpp
    platform/port_savestate.cpp
    platform/port_savestate_format.cpp
    platform/port_skip_cutscenes.cpp
    platform/port_skip_cutscenes_ops.cpp
    platform/port_gci.cpp
    platform/port_data_folder.cpp
    platform/port_mods.cpp
    platform/port_mods_pak.cpp
    platform/port_pal_languages.cpp
    platform/port_pal_languages_import.cpp
    platform/port_actor_collision_bounds.cpp
    platform/port_importers.cpp
    platform/port_importers_lines.cpp
    platform/port_remastered_nsp.cpp
    platform/port_remastered_nso.cpp
    platform/port_remastered_pak.cpp
    platform/port_remastered_txtr.cpp
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
    platform/port_room_env_file.cpp
    platform/port_room_geo.cpp
    platform/port_room_geo_file.cpp
    platform/port_room_liquid.cpp
    platform/port_collision_view.cpp
    platform/port_remastered_cmdl.cpp
    platform/port_remastered_anuv.cpp
    platform/port_remastered_image.cpp
    platform/port_remastered_table.cpp
    platform/port_remastered_convert.cpp
    platform/port_remastered_ball_light.cpp
    platform/port_remastered_effect.cpp
    platform/port_vfx_particles.cpp
    platform/port_fx_debug.cpp
    platform/port_remastered_effect_convert.cpp
    platform/port_remastered_effect_import.cpp
    platform/port_remastered_report.cpp
    platform/port_remastered_import.cpp
    platform/port_remastered_jpeg.cpp
    platform/port_gallery.cpp
    platform/port_remastered_room.cpp
    platform/port_remastered_anim.cpp
    platform/port_remastered_text.cpp
    platform/port_custom_res.cpp
    platform/port_hints.cpp
    platform/port_ws.cpp
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
# are the decomp style); platform sources themselves have none. MSVC gets nothing
# here (its own flags live elsewhere).
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(${MP_PORT_SOURCES} platform/smoke.cpp PROPERTIES
        COMPILE_OPTIONS "-Wall;-Wextra;-Wno-multichar;-Wno-unused-parameter;-Wno-ignored-qualifiers")
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
else()
    add_executable(metroid_prime_port ${MP_PORT_SOURCES})
endif()
# disc.cpp and port_pal_languages.cpp inflate compressed images and fonts. Aurora
# provides ZLIB::ZLIB everywhere (its zlib-ng on Windows).
target_link_libraries(metroid_prime_port PRIVATE ZLIB::ZLIB)
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
        platform/port_remastered_convert.cpp platform/port_remastered_cmdl.cpp platform/port_remastered_anuv.cpp
        platform/port_remastered_report.cpp)
    target_include_directories(remastered_effect_tool PRIVATE platform/include)
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
else()
    message(FATAL_ERROR "This port currently requires an ELF linker with RESCAN support or an MSVC-compatible COFF linker")
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
install(TARGETS metroid_prime_port RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
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

if(MP_BUILD_TESTS)
    enable_testing()
    add_test(NAME port_version COMMAND metroid_prime_port --version)
    set_tests_properties(port_version PROPERTIES TIMEOUT 15 LABELS port
        PASS_REGULAR_EXPRESSION "Metroid Prime native port")
    add_executable(port_buffer_tests
        tests/port_buffers.cpp
        tests/port_forward_owner.cpp
        src/Kyoto/Streams/CInputStream.cpp
        src/Kyoto/Streams/CMemoryInStream.cpp
        src/Kyoto/Streams/COutputStream.cpp
        src/Kyoto/Streams/CMemoryStreamOut.cpp)
    target_include_directories(port_buffer_tests PRIVATE include extern/aurora/include)
    target_compile_definitions(port_buffer_tests PRIVATE TARGET_PC VERSION=0)
    add_executable(port_ai_tests tests/port_ai.cpp platform/ai_dma.cpp)
    target_include_directories(port_ai_tests PRIVATE platform/include extern/aurora/include)
    target_compile_definitions(port_ai_tests PRIVATE TARGET_PC)
    target_link_libraries(port_ai_tests PRIVATE ${AURORA_SDL3_TARGET})
    add_executable(port_randomizer_tests
        tests/port_randomizer.cpp
        platform/port_randomizer.cpp)
    target_include_directories(port_randomizer_tests PRIVATE
        platform/include extern/aurora/include include)
    target_compile_definitions(port_randomizer_tests PRIVATE TARGET_PC VERSION=0)
    target_link_libraries(port_randomizer_tests PRIVATE ${AURORA_SDL3_TARGET})
    add_executable(port_apclient_tests
        tests/port_apclient.cpp
        platform/port_ap_metroidprime.cpp
        platform/port_ap_protocol.cpp
        platform/port_ap_logic.cpp
        platform/port_ap_world.cpp
        platform/port_json.cpp
        platform/port_randomizer.cpp)
    target_include_directories(port_apclient_tests PRIVATE
        platform/include extern/aurora/include include)
    target_compile_definitions(port_apclient_tests PRIVATE TARGET_PC VERSION=0)
    target_link_libraries(port_apclient_tests PRIVATE ${AURORA_SDL3_TARGET})
    add_executable(port_ap_solo_tests
        tests/port_ap_solo.cpp
        platform/port_ap_solo.cpp
        platform/port_rando_seed.cpp
        platform/port_ap_metroidprime.cpp
        platform/port_ap_protocol.cpp
        platform/port_ap_logic.cpp
        platform/port_ap_world.cpp
        platform/port_json.cpp
        platform/port_randomizer.cpp)
    target_include_directories(port_ap_solo_tests PRIVATE
        platform/include extern/aurora/include include)
    target_compile_definitions(port_ap_solo_tests PRIVATE TARGET_PC VERSION=0)
    target_link_libraries(port_ap_solo_tests PRIVATE ${AURORA_SDL3_TARGET})
    add_executable(port_ap_logic_tests
        tests/port_ap_logic.cpp
        platform/port_ap_logic.cpp
        platform/port_ap_metroidprime.cpp)
    target_include_directories(port_ap_logic_tests PRIVATE
        platform/include extern/aurora/include include)
    target_compile_definitions(port_ap_logic_tests PRIVATE TARGET_PC VERSION=0)
    add_executable(port_rando_gen_tests
        tests/port_rando_gen.cpp
        platform/port_rando_gen.cpp
        platform/port_ap_metroidprime.cpp
        platform/port_ap_logic.cpp
        platform/port_ap_world.cpp
        platform/port_json.cpp)
    target_include_directories(port_rando_gen_tests PRIVATE
        platform/include extern/aurora/include include)
    target_compile_definitions(port_rando_gen_tests PRIVATE TARGET_PC VERSION=0)
    add_executable(port_musyx_tests tests/port_musyx.c)
    target_link_libraries(port_musyx_tests PRIVATE musyx)
    # A break must retire a looping voice in the PC mixer, as it does on the DSP.
    add_executable(port_musyx_charge_loop_tests tests/port_musyx_charge_loop.c)
    target_link_libraries(port_musyx_charge_loop_tests PRIVATE musyx)
    add_executable(port_disc_tests tests/port_disc.cpp platform/disc.cpp)
    target_include_directories(port_disc_tests PRIVATE platform/include extern/aurora/include)
    target_compile_definitions(port_disc_tests PRIVATE TARGET_PC)
    target_link_libraries(port_disc_tests PRIVATE ZLIB::ZLIB)
    add_executable(port_mouse_tests tests/port_mouse.cpp)
    target_include_directories(port_mouse_tests PRIVATE platform/include)
    # Drives a real SDL virtual gamepad through the touch overlay's own code.
    # That code was Android-only and untested, so a wrong button mapping could
    # not fail anywhere except on a phone.
    add_executable(port_touch_pad_tests tests/touch_pad.cpp platform/touch_pad.cpp)
    target_include_directories(port_touch_pad_tests PRIVATE
        platform/include extern/aurora/include)
    target_compile_definitions(port_touch_pad_tests PRIVATE TARGET_PC)
    target_link_libraries(port_touch_pad_tests PRIVATE ${AURORA_SDL3_TARGET})
    add_executable(port_json_tests tests/port_json.cpp platform/port_json.cpp)
    target_include_directories(port_json_tests PRIVATE platform/include)
    target_compile_definitions(port_json_tests PRIVATE TARGET_PC)
    add_executable(port_ws_tests tests/port_ws.cpp platform/port_ws.cpp)
    target_include_directories(port_ws_tests PRIVATE platform/include)
    target_compile_definitions(port_ws_tests PRIVATE TARGET_PC)
    if(WIN32)
        target_link_libraries(port_ws_tests PRIVATE ws2_32)
    endif()
    mp_websocket_tls(port_ws_tests)
    # The TLS end-to-end test runs tools/ap_fake_server.py from the source tree.
    target_compile_definitions(port_ws_tests PRIVATE
        "MP_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
    add_executable(port_pal_languages_tests tests/port_pal_languages.cpp platform/port_pal_languages.cpp
        platform/port_mods_pak.cpp)
    target_include_directories(port_pal_languages_tests PRIVATE platform/include)
    target_link_libraries(port_pal_languages_tests PRIVATE ZLIB::ZLIB)
    add_executable(port_mods_tests tests/port_mods.cpp platform/port_mods_pak.cpp)
    target_include_directories(port_mods_tests PRIVATE platform/include)
    # The collision-box reader indexes PAK tables and inflates them; a test
    # drives it over synthetic PAKs instead of a disc, so the DVD calls the
    # production source is built from are stubbed out.
    add_executable(port_actor_collision_bounds_tests tests/port_actor_collision_bounds.cpp
        platform/port_actor_collision_bounds.cpp platform/port_mods_pak.cpp platform/port_ws.cpp)
    target_include_directories(port_actor_collision_bounds_tests PRIVATE platform/include)
    target_compile_definitions(port_actor_collision_bounds_tests PRIVATE PORT_ACTOR_COLLISION_BOUNDS_NO_DVD)
    if(WIN32)
        target_link_libraries(port_actor_collision_bounds_tests PRIVATE ws2_32)
    endif()
    add_executable(port_room_env_tests tests/port_room_env.cpp platform/port_room_env_file.cpp)
    target_include_directories(port_room_env_tests PRIVATE platform/include)
    add_executable(port_room_geo_tests tests/port_room_geo.cpp platform/port_room_geo_file.cpp)
    target_include_directories(port_room_geo_tests PRIVATE platform/include)
    add_executable(port_hd_font_tests tests/port_hd_font.cpp platform/port_hd_font_file.cpp platform/port_remastered_font.cpp platform/port_font_accent_file.cpp)
    target_include_directories(port_hd_font_tests PRIVATE platform/include)
    add_executable(port_remastered_nso_tests tests/port_remastered_nso.cpp platform/port_remastered_nso.cpp)
    target_include_directories(port_remastered_nso_tests PRIVATE platform/include)
    add_executable(port_remastered_text_tests tests/port_remastered_text.cpp platform/port_remastered_text.cpp)
    target_include_directories(port_remastered_text_tests PRIVATE platform/include)
    add_executable(port_remastered_effect_tests tests/port_remastered_effect.cpp platform/port_remastered_effect.cpp)
    target_include_directories(port_remastered_effect_tests PRIVATE platform/include)
    add_executable(port_remastered_effect_convert_tests tests/port_remastered_effect_convert.cpp platform/port_remastered_effect_convert.cpp platform/port_remastered_effect.cpp)
    target_include_directories(port_remastered_effect_convert_tests PRIVATE platform/include)
    add_executable(port_remastered_effect_import_tests tests/port_remastered_effect_import.cpp platform/port_remastered_effect_import.cpp platform/port_remastered_effect_convert.cpp platform/port_remastered_effect.cpp platform/port_remastered_image.cpp)
    target_include_directories(port_remastered_effect_import_tests PRIVATE platform/include)
    target_link_libraries(port_remastered_effect_import_tests PRIVATE astcenc)
    add_executable(port_hud_bars_tests tests/port_hud_bars.cpp platform/port_hud_bars_file.cpp)
    target_include_directories(port_hud_bars_tests PRIVATE platform/include)
    add_executable(port_remastered_hud_tests tests/port_remastered_hud.cpp platform/port_remastered_hud.cpp platform/port_hud_bars_file.cpp platform/port_remastered_image.cpp platform/port_remastered_pak.cpp)
    target_include_directories(port_remastered_hud_tests PRIVATE platform/include)
    target_link_libraries(port_remastered_hud_tests PRIVATE astcenc)
    add_executable(port_remastered_movie_tests tests/port_remastered_movie.cpp platform/port_remastered_movie.cpp platform/port_remastered_jpeg.cpp)
    target_include_directories(port_remastered_movie_tests PRIVATE platform/include)
    add_executable(port_gallery_tests tests/port_gallery.cpp platform/port_gallery.cpp platform/port_remastered_jpeg.cpp)
    target_include_directories(port_gallery_tests PRIVATE platform/include)
    target_link_libraries(port_gallery_tests PRIVATE aurora::thp)
    add_executable(port_remastered_uv_tests tests/port_remastered_uv_tests.cpp platform/port_remastered_convert.cpp platform/port_remastered_anuv.cpp platform/port_remastered_image.cpp platform/port_remastered_pak.cpp)
    target_include_directories(port_remastered_uv_tests PRIVATE platform/include)
    target_link_libraries(port_remastered_uv_tests PRIVATE astcenc)
    add_executable(port_remastered_report_tests tests/port_remastered_report_tests.cpp platform/port_remastered_report.cpp)
    target_include_directories(port_remastered_report_tests PRIVATE platform/include)
    add_executable(port_remastered_anuv_tests tests/port_remastered_anuv_tests.cpp platform/port_remastered_anuv.cpp)
    target_include_directories(port_remastered_anuv_tests PRIVATE platform/include)
    add_executable(port_remastered_lits_tests tests/port_remastered_lits_tests.cpp platform/port_remastered_convert.cpp platform/port_remastered_anuv.cpp platform/port_remastered_image.cpp platform/port_remastered_pak.cpp)
    target_include_directories(port_remastered_lits_tests PRIVATE platform/include)
    target_link_libraries(port_remastered_lits_tests PRIVATE astcenc)
    add_executable(port_remastered_blend_tests tests/port_remastered_blend_tests.cpp platform/port_remastered_convert.cpp platform/port_remastered_anuv.cpp platform/port_remastered_image.cpp platform/port_remastered_pak.cpp)
    target_include_directories(port_remastered_blend_tests PRIVATE platform/include)
    target_link_libraries(port_remastered_blend_tests PRIVATE astcenc)
    add_executable(port_remastered_astc_tests tests/port_remastered_astc.cpp platform/port_remastered_image.cpp)
    target_include_directories(port_remastered_astc_tests PRIVATE platform/include)
    target_link_libraries(port_remastered_astc_tests PRIVATE astcenc)
    add_executable(port_remastered_image_tests tests/port_remastered_image_tests.cpp platform/port_remastered_image.cpp)
    target_include_directories(port_remastered_image_tests PRIVATE platform/include)
    target_link_libraries(port_remastered_image_tests PRIVATE astcenc)
    add_executable(port_importers_tests tests/port_importers.cpp platform/port_importers_lines.cpp)
    target_include_directories(port_importers_tests PRIVATE platform/include)
    add_executable(port_custom_res_tests tests/port_custom_res.cpp platform/port_custom_res.cpp)
    target_include_directories(port_custom_res_tests PRIVATE platform/include)
    add_executable(port_env_tests tests/port_env.cpp)
    target_include_directories(port_env_tests PRIVATE platform/include)
    add_executable(port_hold_toggle_tests tests/port_hold_toggle.cpp)
    target_include_directories(port_hold_toggle_tests PRIVATE platform/include)
    add_executable(port_tick_pair_tests tests/port_tick_pair.cpp)
    target_include_directories(port_tick_pair_tests PRIVATE platform/include)
    add_executable(port_input_map_tests tests/port_input_map.cpp)
    target_include_directories(port_input_map_tests PRIVATE platform/include)
    add_test(NAME port_input_map_tests COMMAND port_input_map_tests)
    set_tests_properties(port_input_map_tests PROPERTIES TIMEOUT 15 LABELS port)
    aurora_copy_runtime_dlls(port_input_map_tests)
    add_executable(port_log_redact_tests tests/port_log_redact.cpp)
    target_include_directories(port_log_redact_tests PRIVATE platform/include)
    add_test(NAME port_log_redact_tests COMMAND port_log_redact_tests)
    set_tests_properties(port_log_redact_tests PROPERTIES TIMEOUT 15 LABELS port)
    aurora_copy_runtime_dlls(port_log_redact_tests)
    add_executable(port_data_folder_tests tests/port_data_folder.cpp platform/port_data_folder.cpp)
    target_include_directories(port_data_folder_tests PRIVATE platform/include)
    target_link_libraries(port_data_folder_tests PRIVATE ${AURORA_SDL3_TARGET})
    add_test(NAME port_data_folder_tests COMMAND port_data_folder_tests "${CMAKE_CURRENT_BINARY_DIR}/data-folder-test")
    set_tests_properties(port_data_folder_tests PROPERTIES TIMEOUT 15 LABELS port)
    aurora_copy_runtime_dlls(port_data_folder_tests)
    add_executable(port_discord_tests tests/port_discord.cpp platform/port_json.cpp)
    target_include_directories(port_discord_tests PRIVATE platform/include)
    target_compile_definitions(port_discord_tests PRIVATE TARGET_PC)
    add_executable(port_update_check_tests tests/port_update_check.cpp platform/port_update_check.cpp
        platform/port_ws.cpp platform/port_json.cpp)
    target_include_directories(port_update_check_tests PRIVATE platform/include)
    target_compile_definitions(port_update_check_tests PRIVATE TARGET_PC)
    if(WIN32)
        target_link_libraries(port_update_check_tests PRIVATE ws2_32)
    endif()
    mp_websocket_tls(port_update_check_tests)
    add_executable(port_livesplit_tests tests/port_livesplit.cpp)
    target_include_directories(port_livesplit_tests PRIVATE platform/include)
    add_executable(port_savestate_tests tests/port_savestate.cpp platform/port_savestate_format.cpp)
    target_include_directories(port_savestate_tests PRIVATE platform/include)
    add_executable(port_timing_tests tests/port_timing.cpp)
    target_include_directories(port_timing_tests PRIVATE platform/include)
    add_executable(port_audio_math_tests tests/port_audio_math.c)
    target_include_directories(port_audio_math_tests PRIVATE extern/musyx/include)
    add_executable(port_card_tests tests/port_card.cpp)
    target_link_libraries(port_card_tests PRIVATE aurora::card)
    add_test(NAME port_card_tests COMMAND port_card_tests "${CMAKE_CURRENT_BINARY_DIR}/card-test-data")
    set_tests_properties(port_card_tests PROPERTIES TIMEOUT 15 LABELS port)
    aurora_copy_runtime_dlls(port_card_tests)
    add_executable(port_gci_tests tests/port_gci.cpp platform/port_gci.cpp)
    target_include_directories(port_gci_tests PRIVATE platform/include)
    target_link_libraries(port_gci_tests PRIVATE aurora::card)
    if(WIN32)
        target_link_libraries(port_gci_tests PRIVATE shell32 ole32 advapi32)
    endif()
    add_test(NAME port_gci_tests COMMAND port_gci_tests)
    set_tests_properties(port_gci_tests PROPERTIES TIMEOUT 15 LABELS port)
    aurora_copy_runtime_dlls(port_gci_tests)
    foreach(test IN ITEMS port_remastered_report_tests port_buffer_tests port_ai_tests port_randomizer_tests port_apclient_tests port_ap_solo_tests port_ap_logic_tests port_rando_gen_tests port_musyx_tests port_musyx_charge_loop_tests port_disc_tests port_mouse_tests port_touch_pad_tests port_json_tests port_ws_tests port_mods_tests port_pal_languages_tests port_actor_collision_bounds_tests port_room_env_tests port_room_geo_tests port_hd_font_tests port_hud_bars_tests port_remastered_astc_tests port_remastered_hud_tests port_remastered_movie_tests port_gallery_tests port_remastered_nso_tests port_remastered_text_tests port_remastered_uv_tests port_remastered_anuv_tests port_remastered_lits_tests port_remastered_blend_tests port_remastered_image_tests port_remastered_effect_tests port_remastered_effect_convert_tests port_remastered_effect_import_tests port_importers_tests port_custom_res_tests port_hold_toggle_tests port_env_tests port_tick_pair_tests port_livesplit_tests port_discord_tests port_update_check_tests port_savestate_tests port_timing_tests port_audio_math_tests)
        add_test(NAME ${test} COMMAND ${test})
        set_tests_properties(${test} PROPERTIES TIMEOUT 15 LABELS port)
        aurora_copy_runtime_dlls(${test})
    endforeach()
    # port_ws_tests starts an openssl-generated CA and a python TLS server, which
    # is not instant on a loaded runner; 15 seconds is enough to time it out and
    # report nothing about which part was slow.
    set_tests_properties(port_ws_tests PROPERTIES TIMEOUT 120)
    # 200+ seeds, each a few hundred logic sweeps.
    set_tests_properties(port_rando_gen_tests PROPERTIES TIMEOUT 120)
    # The tools' own checks, so a change to the location join or the fixture
    # generator fails here rather than in whoever runs the tools next. Both are
    # dependency-free Python and skip themselves when python3 is absent.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME port_ap_fixture
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/port_ap_fixture.py
                    ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(port_ap_fixture PROPERTIES TIMEOUT 120 LABELS port)
        # The shipped pipeline seed: every row at the current config versions (else aurora
        # ignores it) and retail only (tools/pipeline_seed.py explains re-recording it).
        add_test(NAME port_pipeline_seed
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/pipeline_seed.py check)
        set_tests_properties(port_pipeline_seed PROPERTIES TIMEOUT 60 LABELS port)
    endif()
endif()
