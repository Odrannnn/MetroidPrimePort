# Metroid Prime — native port

A native build of **Metroid Prime** (GameCube, USA v1.00 `GM8E01_00` or PAL `GM8P01`) for Linux,
Windows and Android. The game is compiled natively from the
[PrimeDecomp](https://github.com/PrimeDecomp/prime) decompiled source and renders
through Aurora (SDL3 and WebGPU), with no emulator involved.

**No game content is included.** You need your own copy of the game: the port
reads it from a disc image and never ships any of its assets. This repository
ships no disc image, and neither does any package or release built from it.

## What it adds over the console release

- Widescreen rendering — 4:3, 16:9 or following the window — with the HUD spread
  to the edges and the menus, pause and map screens aspect-corrected
- An uncapped presentation rate over a fixed-rate simulation, so physics and
  animation stay console-accurate at any frame rate
- Graphics options: first-person FOV, MSAA and anisotropic filtering, HUD scale,
  and toggles to hide the helmet and the visor effects
- Mouse aim and twin-stick aiming, with sensitivity, inversion and crosshair size
- A Controls page for rebinding keyboard, mouse and controller, with a second key
  per action, conflict warnings, controller presets (GameCube, Modern,
  Southpaw), deadzones and gyro aim
- Optional gameplay tweaks, all off by default: Fast Morph (quick morph and
  unmorph that keep momentum), Spring Ball (optionally on a gyro flick), Toggle
  Lock-On and Sticky Charge
- Skippable cutscenes: Start skips cutscenes the console release forces you to
  watch
- Save states (eight slots plus undo, F5/F9) and a reveal-whole-map option with
  an item, scan and room tracker
- An on-screen in-game timer and a LiveSplit Server client that starts, splits
  on upgrades and stops at the credits
- Hard mode, the Fusion Suit and the image galleries can be unlocked without
  beating the game
- Memory card import and export as `.gci` files or raw card images, including
  straight from and to Dolphin
- A mods folder that replaces loose disc files or single resources inside PAKs (or adds new ones),
  user texture packs, HD texture replacements, and in-game button prompts that
  follow the input bound to each action
- Discord Rich Presence on desktop (bring your own Discord application id)
- The common options sit in pause > Options beside the game's own; the F1
  overlay holds everything, in pages for Game, Controls, Video, Remastered,
  Mods, Archipelago, Tracker, Save states, System and Debug (cheats sit behind
  a "Show cheats" box)
- A touch overlay on Android, with a virtual controller for the sticks, triggers,
  shoulders and face buttons

## Randomizer and Archipelago

Both are supported, on all three platforms.

- **Randomizer** — item placement is driven by a seed file. `tools/rando_seed.py`
  generates one and `docs/RANDOMIZER.md` covers the format.
- **Archipelago multiworld** — the Metroid Prime AP world's item and location
  tables are built in. Open F1 > Archipelago, enter the server (`host:port`), your
  slot name and the password if any, and connect; no seed file is needed. The
  seed's options are applied and a new game starts at the Landing Site with
  the intro skipped. Each seed and slot gets its own memory card, checks made
  offline are sent on the next connect, Recent games resumes an earlier seed,
  and the Chat sub-tab shows the server log and sends messages such as `!hint`.
  DeathLink is supported.

  `wss://` needs no extra setup on any platform: OpenSSL is vendored and linked
  statically on Android, and the trust store is enumerated by hand, so the
  system CA bundle is not required. See `docs/ARCHIPELAGO.md`.

Cutscenes are always skippable in randomizer and Archipelago games.

## Downloads

Releases are published on the
[GitHub releases page](https://github.com/Odrannnn/MetroidPrimePort/releases).
Every build needs your own disc image; none of them contains one.

| Platform | Artefact | Notes |
| --- | --- | --- |
| Linux | AppImage | Self-contained, no install. `chmod +x` and run. |
| Linux | Flatpak | `io.github.odrannnn.metroidprimeport` |
| Linux | `.tar.gz` | The bare binary, its textures and the licences |
| Windows | `.zip` | Unzip anywhere; needs a Visual C++ redistributable |
| Android | `.apk` | Sideload; enable install from unknown sources |

Android APKs are signed with a project release key. An app signed with one key
cannot be updated by one signed with another, so keep the key — see
`docs/RELEASING.md`.

## Building

Needs CMake 3.25+, Ninja and a C++20 toolchain. Aurora and MusyX are vendored, so
a normal clone is enough; Aurora fetches its pinned dependencies on the first
configure.

```sh
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DAURORA_ENABLE_TESTS=OFF -DMP_BUILD_TESTS=ON
cmake --build build/native -j 4
ctest --test-dir build/native -L port --output-on-failure
```

On Windows, run from an MSVC developer shell and add
`-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl`. CI builds and tests
Linux (GCC 14 and Clang 18) and Windows (clang-cl); see
`.github/workflows/`.

### Android

```sh
tools/make_android_keystore.sh          # once; writes android/keystore.properties
tools/android_apk.sh :app:assembleRelease
```

Both files it writes name a private key and are deliberately untracked. The
build needs a Rust toolchain on `PATH`, because the disc layer is `nod`
cross-compiled for `aarch64-linux-android`. Without one the build still
succeeds but links a stub that cannot open a disc — see
`docs/ANDROID_BUILD_PROBE.md`.

## Running

```sh
./build/native/metroid_prime_port "/path/to/Metroid Prime (USA) (v1.00).iso"
```

The disc can also be set with `MP_DISC`, kept beside the executable, or picked
through a file dialog the first time you launch without one — the choice is
remembered in the settings. The image must be `GM8E01`, disc 0, revision 0 (USA 1.00), or
`GM8P01` (PAL, which also brings its French, German, Spanish and Italian text;
a USA player can add those from a PAL image in F1 > Game > Language). If a
remembered disc stops opening — the file moved, or on Android the system revoked
its access grant — the port asks again rather than exiting.

On Android the first pick is copied into app storage, so later launches need no
permission and there is nothing to grant again.

`--version` prints the source revision without starting graphics; include it in
bug reports. The F1 overlay holds the rest of the settings, and the same flags
are available as `MP_*` environment variables.

## Packaging

`tools/make_appimage.sh` builds a self-contained AppImage and
`tools/make_flatpak.sh` a Flatpak. Neither bundles the disc. See
`docs/NATIVE_PORT.md` for what each one expects from the host.

## Credits and licensing

This is a fork of the [PrimeDecomp/prime](https://github.com/PrimeDecomp/prime)
decompilation, which the game logic is built from. Aurora (`extern/aurora`) and
MusyX (`extern/musyx`) are MIT-licensed vendored snapshots, and the button
prompt icons come from Kenney's Input Prompts pack (CC0), vendored under
`tools/prompt_icons`. Their licences are kept alongside them and are copied into
release packages.

`LICENSE` covers this project's own work — the platform layer, the build and
packaging scripts, and the Android and Flatpak integration. It does **not** cover
the game code under `src/` and `include/`, which comes from the decompilation
and remains Nintendo's. See `NOTICE`.

Metroid Prime is a trademark of Nintendo. This project is unaffiliated with
Nintendo and Retro Studios, and ships no game data.

`docs/NATIVE_PORT.md` is the detailed reference: build options, every setting
and environment variable, the smoke-test hooks, and the platform notes.
