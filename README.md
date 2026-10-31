# Metroid Prime Unbound - native port
[![FOSSA Status](https://app.fossa.com/api/projects/git%2Bgithub.com%2FOdrannnn%2FMetroidPrimePort.svg?type=shield)](https://app.fossa.com/projects/git%2Bgithub.com%2FOdrannnn%2FMetroidPrimePort?ref=badge_shield)


> **Disclaimer:** this is an independent project, not affiliated with or endorsed
> by [PrimeDecomp](https://github.com/PrimeDecomp/prime). It builds on their
> decompilation, but they don't maintain or support this port: please report
> problems here, not to them.

**Looking for the sequel? See the [Metroid Prime 2: Echoes native port](https://github.com/Odrannnn/MetroidPrime2Port).**

**Make sure to check out the F1 (Or start+back on controller) menu for all the additional features and QoL!**

**You can load prime remastered models and textures in the game (check F1 > mods), you must provide your
personal Metroid Prime Remastered dumped image, no assets are provided in this repo**

A native build of **Metroid Prime** for Linux, Windows, Android, macOS and iPhone/iPad, compiled
from the [PrimeDecomp](https://github.com/PrimeDecomp/prime) decompilation and
rendered through Aurora (SDL3/WebGPU). There's no emulator involved. Supported
discs: USA 1.00, 1.01 and 1.02, PAL and Trilogy, both USA and PAL.

**No game content is included.** You need your own copy of the game: the port
reads it from a disc image and never ships any of its assets. This repository
ships no disc image, and neither does any package or release built from it.

## What it adds over the console release

- **Modern controls:** Mouse & Keyboard, Dual stick camera, Gyro controls, Native touch controls for mobile!
- **Remastered assets:**  (Experimental) import models, effects and lighting from your own Metroid Prime
  Remastered dump (F1 > Remastered)
- **Metroid Prime Trilogy:** run Trilogy's Metroid Prime from a Wii disc, with Trilogy's HUD,
  scan visor and radial beam and visor menus
- **Metroid Dread Samus:** import Samus's suits from your own Metroid Dread dump, first-person
  arm cannon included
- **Third-person camera** with an aiming cannon arm, and **suit colour customisation**
- **Widescreen:** with an edge-to-edge HUD, and **uncapped frame rate**
- **Graphics:** FOV, MSAA, anisotropic filtering, resolution scale and dynamic
  resolution, HUD scale, arm cannon offset, toggles to hide the helmet and visor
  effects, and a choice of renderer, OpenGL included
- **Controls:** mouse and twin-stick aim, gyro, lock-on free aim, full rebinding
  and presets, separate Morph Ball binds
- **Optional tweaks:** Fast Morph, Spring Ball, Toggle Lock-On, Sticky Charge... And so many more!
- **Skippable cutscenes**
- **Save states** and **quick resume** after quitting
- **Saves:** import from and export to USA/PAL GameCube, Wii Trilogy and Dolphin
- **Photo mode** with free-cam and high-resolution capture
- **Map and tracker:** reveal the map, plus item, scan and room tracking
- **Dolphin texture packs**, including HD texture replacements
- **Mods:** drop-in mod folders that replace disc data, plus [community mods](docs/MODS.md)
- **Discord Rich Presence** on desktop, and a **touch overlay** with a layout editor on Android and iOS
- The **F1 overlay** has every setting
- And more..!

## Randomizer and Archipelago Multiworld

- **Randomizer** - Generate your own randomizer games
- **Archipelago multiworld** - Generate a game in archipelago with the [Metroid Prime APWorld](https://github.com/UltiNaruto/MetroidAPrime) and host it. Open F1 > Archipelago, enter the server (`host:port`), your
  slot name and the password if any, and connect; no seed file is needed. Each seed and slot gets its own separate save, checks made
  offline are sent on the next connect, chat with command support, DeathLink.
  
## Downloads

Releases are published on the
[GitHub releases page](https://github.com/Odrannnn/MetroidPrimePort/releases).
Every build needs your own disc image; none of them contains one.

**Quick start:** download the build for your platform, start it, and pick your disc image when it
asks. Supported discs are the GameCube USA 1.00, 1.01
and 1.02 (`GM8E01`), PAL (`GM8P01`) and Trilogy (USA `R3ME01` and PAL `R3MP01`), as `.iso`, `.gcm`,
`.rvz`, `.wbfs`, `.ciso` or `.nkit`. 

Press **F1** for every setting.

## Building

CMake 3.25+, Ninja and a C++20 toolchain; a normal clone is enough. The
requirements, the configure, build and test commands for Linux, Windows and
macOS, the CMake options and what CI runs are in
[`docs/NATIVE_PORT.md`](docs/NATIVE_PORT.md#build); the Android build is in
[`docs/ANDROID.md`](docs/ANDROID.md). Before changing the game code, read
[where port code goes](docs/NATIVE_PORT.md#where-port-code-goes).

## Running

```sh
./build/native/metroid_prime_port "/path/to/Metroid Prime (USA) (v1.00).iso"
```

## Troubleshooting

- **Where are the logs?** `metroid_prime_port.log` in the user folder (F1 >
  System shows its path); the previous run's is `metroid_prime_port.old.log`. On
  Android a copy goes to `Documents/MetroidPrime/`; on iOS it is in Files > On
  My iPhone (or iPad) > Metroid Prime. Attach it to a bug report.
- **"unsupported disc"**: the image is not one of the revisions above (see the
  log for the disc id it found). Re-dump it, or check you picked the GameCube disc.
- **Settings in a bad state**: F1 > System > Settings > **Reset configuration...**
  backs up and resets every setting and mapping; saves are not touched.

## Credits

This port would not exist without the work of these projects. Thank you.

**Foundation**

- [PrimeDecomp](https://github.com/PrimeDecomp/prime): the decompilation the game
  code is built from, and the decompiled Dolphin SDK.
- [Aurora](https://github.com/encounter/aurora): the GameCube graphics, input and
  platform layer over SDL3 and WebGPU.
- [MusyX](https://github.com/AxioDL/musyx): the audio mixer.
- [nod](https://github.com/encounter/nod): the disc image reader.

**Randomizer and Archipelago**

- [randomprime](https://github.com/aprilwade/randomprime): the room patches for
  skippable cutscenes and Archipelago pickups, the room and elevator tables, and
  the custom pickup and door assets are generated from it.
- The [Metroid Prime APWorld](https://github.com/UltiNaruto/MetroidAPrime): the
  door table, and the game this client connects to.
- The Metroid Prime AP PopTracker pack: the Archipelago logic rules.
- [Archipelago](https://archipelago.gg): the multiworld itself.

**Remastered import**

- [astc-encoder](https://github.com/ARM-software/astc-encoder): the ASTC texture encoder.
- tegra_swizzle, bcdec_rs and astc-decode, ported to C++ for the texture importer.

**Platform and input**

- [libadrenotools](https://github.com/bylaws/libadrenotools): custom Vulkan drivers on Android.
- [GamepadMotionHelpers](https://github.com/JibbSmart/GamepadMotionHelpers): gyro
  calibration and aiming spaces.
- [Kenney Input Prompts](https://kenney.nl/assets/input-prompts) (CC0): the
  controller button icons.
- [Dolphin](https://dolphin-emu.org): the texture pack and save formats the port reads and writes.

**Libraries**

[SDL3](https://libsdl.org), [Dawn](https://dawn.googlesource.com/dawn),
[Dear ImGui](https://github.com/ocornut/imgui), [RmlUi](https://github.com/mikke89/RmlUi),
[fmt](https://github.com/fmtlib/fmt), [Abseil](https://abseil.io), [FreeType](https://freetype.org),
libpng, zlib-ng, [zstd](https://github.com/facebook/zstd), [xxHash](https://github.com/Cyan4973/xxHash),
[SQLite](https://sqlite.org) and OpenSSL.

## Licensing

Every third-party component keeps its own licence; the full list, with where each
licence text lives, is in [`NOTICE`](NOTICE), and the texts are copied into every
release package.

`LICENSE` covers this project's own work — the platform layer, the build and
packaging scripts, and the Android and Flatpak integration. It does **not** cover
the game code under `src/` and `include/`, which comes from the decompilation
and remains Nintendo's. See `NOTICE`.

Metroid Prime is a trademark of Nintendo. This project is unaffiliated with
Nintendo and Retro Studios, and ships no game data.


[![FOSSA Status](https://app.fossa.com/api/projects/git%2Bgithub.com%2FOdrannnn%2FMetroidPrimePort.svg?type=large)](https://app.fossa.com/projects/git%2Bgithub.com%2FOdrannnn%2FMetroidPrimePort?ref=badge_large)

## Documentation

| Document | What it covers |
|---|---|
| `docs/SETTINGS.md` | Every control, setting and environment variable |
| `docs/NATIVE_PORT.md` | The detailed reference: build options, smoke-test hooks, platform notes |
| `docs/MODS.md` | HD texture packs and the mods folder |
| `docs/MOD_FORMATS.md` | Mod file formats, for mod authors |
| `docs/DREAD_SAMUS.md` | The Metroid Dread Samus model import |
| `docs/RANDOMIZER.md` | The built-in randomizer |
| `docs/ARCHIPELAGO.md` | The Archipelago multiworld client |
| `docs/CONSOLE.md` | The debug console (`MP_CONSOLE`) |
| `docs/DEBUGGING.md` | Headless debugging with `tools/mprig.py` |
| `tools/README.md` | What each script in `tools/` does and what runs it |
| `docs/RELEASING.md` | What a release contains and must carry |
| `docs/ANDROID.md` | The Android port's status and build |
| `docs/IOS.md` | The experimental iPhone/iPad build: sideloading, the disc, where files are |
| `docs/REMASTERED_EFFECTS.md` | Importing Remastered particle effects |
| `docs/FRAME_INTERPOLATION.md`, `docs/HIGH_FPS_AUDIT.md` | High frame rate design notes |
| `decomp/` | The original decompilation's GameCube build and notes (not used by the port) |
| `PORT_NOTES.md` | Open follow-ups and non-obvious facts |

## AI assistance

Parts of this port were written with AI coding assistants, under my oversight.