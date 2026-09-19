cmake_minimum_required(VERSION 3.25)
project(metroid_prime_port LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_COLOR_DIAGNOSTICS ON)

# Aurora provides the GameCube/Wii compatibility layer (MIT): GX, OS, PAD, DVD,
# CARD, VI, SI, THP, mtx, plus the SDL3/WebGPU application layer.
add_subdirectory(extern/aurora EXCLUDE_FROM_ALL)
if(TARGET aurora_mtx)
    target_compile_definitions(aurora_mtx PRIVATE MTX_USE_PS=1)
endif()

# MusyX is the game's audio engine; the vendored fork is portable.
add_subdirectory(extern/musyx EXCLUDE_FROM_ALL)

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
add_library(mp_game STATIC ${MP_GAME_SOURCES})
target_include_directories(mp_game PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/build/GM8E01_00/include
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/aurora/include
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/include)
target_link_libraries(mp_game PUBLIC
    aurora::core aurora::gx aurora::gd aurora::si aurora::vi aurora::pad
    aurora::mtx aurora::os aurora::dvd aurora::thp aurora::card
    musyx)
# The decompiled code is written for MWCC; modern-compiler warnings are noise
# while we work through real errors. compat.h restores SDK macros Aurora omits.
if(MSVC)
    target_compile_options(mp_game PRIVATE /w
        /FI${CMAKE_CURRENT_SOURCE_DIR}/platform/compat.h)
else()
    target_compile_options(mp_game PRIVATE -w
        -Wno-narrowing -Wno-c++11-narrowing
        -include ${CMAKE_CURRENT_SOURCE_DIR}/platform/compat.h)
endif()

# Port entry point and platform shims (Aurora owns the real main; see
# aurora/main.h).
add_executable(metroid_prime_port
    platform/main.cpp
    platform/debug_ui.cpp
    platform/shims.cpp
    platform/sdk_stubs.cpp
    platform/ai_dma.cpp)
# aurora_core (webgpu/gpu.cpp) and aurora_gx reference each other; group them so
# the linker rescans and resolves the cycle.
if(MSVC)
    # COFF has no --start-group; a second mp_game pass lets lld resolve
    # intra-archive references (e.g. CProjectileWeapon from CBloodFlower).
    target_link_libraries(metroid_prime_port PRIVATE
        mp_game aurora::main aurora::core aurora::gx mp_game)
    target_link_options(metroid_prime_port PRIVATE /INCREMENTAL:NO)
else()
    target_link_libraries(metroid_prime_port PRIVATE
        mp_game
        aurora::main
        "$<LINK_GROUP:RESCAN,aurora::core,aurora::gx>")
endif()

# Aurora imports this machine-independent pipeline description cache and
# compiles its entries in the background before first use.
add_custom_command(TARGET metroid_prime_port POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/initial_pipeline_cache.db
        $<TARGET_FILE_DIR:metroid_prime_port>/initial_pipeline_cache.db)
