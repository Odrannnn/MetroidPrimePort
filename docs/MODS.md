# Mods and texture packs

How to add mods, texture packs and the built-in imports to the port. Mod authors: the file
formats and the technical details are in [`docs/MOD_FORMATS.md`](MOD_FORMATS.md) (in the source
repository).

- [Community mods](#community-mods)
- [Installing mods](#installing-mods)
- [Built-in mods](#built-in-mods): [Full Flaahgra theme](#full-flaahgra-theme-ntsc-discs),
  [Metroid Prime Remastered](#metroid-prime-remastered),
  [Metroid Dread Samus models](#metroid-dread-samus-models)
- [Texture packs](#texture-packs)
- [Custom importers](#custom-importers)
- [Environment variables](#environment-variables)

## Community mods

Made and maintained by others, not part of the port; report their problems in
their own repositories.

- [Russian translation](https://github.com/454135431515/metroid-prime-ru)
  (USA 1.00): all game text, Cyrillic fonts and redrawn texture labels, built
  from your own disc into a `mods/russian` folder. With the Remastered import
  present it also writes a Cyrillic HD font.

## Installing mods

A mod replaces disc data without touching the disc image. Mods live in the `mods` folder of the
user folder, created on first start:

| Install | User folder |
| --- | --- |
| Portable (writable folder) | `user/` beside the executable |
| Linux, read-only install | `~/.local/share/Metroid Prime/` |
| Flatpak | `~/.var/app/io.github.odrannnn.metroidprimeport/data/Metroid Prime/` |
| Windows, read-only install | `%APPDATA%\Metroid Prime\` |
| Android | the data folder (see below) |

`MP_MODS` points the port at another mods folder.

- **One folder per mod.** Mods are applied in name order, so a later name wins. Folders starting
  with `.` are skipped.
- **Readmes are fine.** Text and image files (`.txt`, `.md`, `.json`, `.png`, ...) are ignored
  silently, so a mod can carry its readme.
- **Turning mods on and off:** F1 > Mods has a Load mods switch (setting `mods`) and a checkbox
  per mod (`mods_disabled`, `/`-separated names). Both take effect on the next start.
- **Checking what loaded:** the log lists each mod's counts and any problems (`mods: ...`), and
  F1 > Mods shows the same.
- **Android:** `mods/` is in the data folder. Move that to shared storage (F1 > System > Data
  folder) to add mods with a file manager. The built-in Remastered import is not one of these
  folders: it stays in app storage ([below](#where-the-import-is-stored)).

Mods load at startup, after the disc is opened (the game caches PAK tables when it boots), and
everything reads through Aurora's DVD overlays, so the game code is unchanged.

### Reloading without a restart

F1 > Mods > Reload mods (console: `mods reload`) reads the mods folder again while the game runs.

Room geometry buffers are reserved at startup on every platform, so a geometry-bearing mod installed
or enabled later loads with a mod reload. The reserve costs no memory until room geometry uses it;
on Android the larger frame buffers are taken only when the first such mod loads. Only when
`MP_FRAME_BUFFERS` or `MP_STORAGE_BUFFERS` pins the buffers too small does the **entire mod** stay
unloaded (including its models, textures and other replacements): the Mods page then says **Restart
the application to apply**, and its enabled switch is kept for the next start.

Except for geometry mods awaiting that startup allocation, added, removed, edited, enabled and
disabled mods take effect on reload, PAK resources and native `.dds` textures alike.

- The game is rebuilt where Samus stands, exactly as a save state load does, with every PAK
  reopened so that all it draws is loaded from the new files.
- Room state is kept when `savestate_room_state` is on; otherwise enemies, doors and puzzles come
  back as after a memory card load.
- A room whose script the reload changed gets no saved objects from the state it captured (the
  same rule as a save state load: see the note under Save states in `docs/SETTINGS.md`); it comes
  back as after a memory card load.
- It runs at the next game tick, so unpause first. Pressed in the menus, it happens when a game
  starts.

Change the folder first, then reload: until then the game still reads the old files, so don't
delete a loaded mod and keep playing.

## Built-in mods

### Full Flaahgra theme (NTSC discs)

The NTSC GameCube discs (USA 1.00, 1.01 and 1.02) ship the Flaahgra boss theme
(`Audio/rui_flaaghraL.dsp` and `R.dsp`) cut to its first 36.2 s (1,159,268 samples), so the
fight loops that short piece. The PAL and Japanese GameCube discs and Metroid Prime Trilogy
(`MP1/Audio/` in its data partition) have the whole 114.7 s track in the same format.

The port cannot ship it, but it can copy it from your own image:

- **F1 > Settings > Audio > Flaahgra boss theme > "Full Flaahgra theme..."** (USA discs only; a
  PAL disc already has it) takes such an image and copies those two files into the mod
  `flaahgra-full-theme` of the mods folder, where the loader serves them in place of the disc's.
  Nothing else is read from the image. Remove deletes the mod.
- **From a terminal:** `metroid_prime_port --import-flaahgra-music <image>` imports without
  starting the game.
- **Adding a PAL disc's languages** (F1 > Game > Language) does the same import from that image,
  so one PAL import brings both.

Each file is checked first (DSP ADPCM, 32 kHz, looping, a whole file, longer than the NTSC copy,
the two channels the same length) and written to a temporary name, then renamed.

It is an ordinary mod: Reload mods (F1 > Mods) or a restart makes the game play it, and turning
mods off (or `MP_MODS` pointing elsewhere) turns it off. On Android the picked file is opened
through the system (no copy of the disc); this path is untested on a device.

### Metroid Prime Remastered

> **Very experimental and currently unsupported.** The import, the room environments and above
> all room geometry are work in progress: expect wrong or missing models, lighting that is off,
> crashes and heavy memory use, and a re-import after most updates. Bug reports about a game
> running with this mod are not handled for now. To get the retail game back, switch it off in
> F1 > Mods > Built-in mods, or remove `remastered-models` from the import's folder (`mods/` on
> desktop, the app's own storage on Android).

The port can build a model mod from your own copy of Metroid Prime Remastered; nothing of it
ships.

#### What you need

- The game's `.nsp`, or a gamecard dump (`.xci`). Compressed `.nsz`/`.xcz` must be decompressed
  first.
- Your Switch's key file: `prod.keys`, not `title.keys`. An `.xci`, or an `.nsp` converted from
  one, needs the key file's `key_area_key_application_XX` keys.
- For the menu movies: **ffmpeg**. The port uses `MP_FFMPEG` if set, else an `ffmpeg` next to the
  game's executable, else the one on the path. The Windows package has one next to the
  executable; on Linux install it from your distribution. Android needs nothing (see
  [What it brings](#what-it-brings)).

#### Importing

F1 > Mods > Built-in mods has a single "Your Nintendo Switch's prod.keys" row above the sections,
since both built-in imports read one key file (`~/.switch/prod.keys` is filled in when it
exists). Prime Remastered > Import then asks only for the image.

- The panel remembers both files once they are picked or used, as `remastered_nsp` and
  `remastered_keys` in the settings file (on Android, the picked documents).
- The import converts in the background while the game runs, on all but two cores. The bar
  counts its steps.
- When it ends, the mod is moved into place in the background ("Moving the mod into place") and
  the mods reload. "Load it again" in the same panel reloads them by hand.
- A cancelled or interrupted import leaves nothing behind; an install a quit interrupted applies
  at the next start.
- When it ends with models that failed, the panel says how many, and "Copy the notes" puts the
  notes on the clipboard for a report.
- "What to import" leaves parts out of the next import (room geometry, particle effects, HUD,
  text, movies, gallery), kept as `remastered_skip` in the settings file. The `MP_REMASTERED_*`
  [switches](#import-switches) can still leave out a part the panel keeps.
- "Parts to use", under the section's switch, picks which parts of the installed import load
  (room geometry and lighting, particle effects, models, HUD, font, text, map, movies); the
  disc's own stands in for each part left out, and nothing is imported again. It applies at the
  next mod reload. Converted textures always load: they have ids of their own, used only by the
  models that name them. Kept as `remastered_hide` in the settings file (the parts left out,
  comma-separated: `geometry`, `effects`, `models`, `hud`, `font`, `text`, `map`,
  `movies`); `MP_REMASTERED_HIDE` overrides it for one run. With room geometry left out the
  import loads without the restart room geometry otherwise needs on a phone started without it.

From a terminal, `metroid_prime_port --import-remastered <image.nsp|image.xci> [key file]` does
the same on every core without starting the game, installs at once and prints the folder it
installed into. The disc comes from `MP_DISC`, the remembered path, or a copy beside the
executable. `--import-remastered-movies <nsp> [keys]` adds only the movies to the mod already
there.

On Android the panel's buttons open the system's file picker: "Pick the keys..." in the shared
keys row, and one for the `.nsp`/`.xci` in the section's import. Neither file is copied: the
import reads them where they are. It runs on two threads there to keep its memory down. Not yet
run on a device.

Measured on the development machine: 342 models and 275 room environments, 44 seconds on 16
threads, 2.3 GB of memory at the peak, 1.1 GB on disk (half of it the room environments). With
room geometry the mod grows from 1 GB to 6.5 GB.

#### What it brings

Each part can be left out with "What to import" or its switch.

- **Models:** a CMDL (plus CSKR for skinned models) over each retail model, with its PBR maps as
  native textures.
- **Room environments:** reflection probes and baked ambient light for every area (`roomenv`
  folder). A world that can't be read is reported and skipped; the models are installed all the
  same.
- **Room geometry** (experimental): every model a room places, with textures capped at 1024 px,
  in the `roomgeo` folder. The full set is about 8,500 models and brings the mod from 1 GB to
  6.5 GB, and the game has to be restarted to draw it. A first import checks the free space for
  that up front and refuses with how much it needs; a later one links most of its files from the
  earlier import instead.
- **Liquid surfaces** (with room geometry): each water, poison or lava volume's own surface
  model, with that room's colour, opacity, wave directions and flow.
- **Particle effects** (experimental; `docs/REMASTERED_EFFECTS.md`).
- **Reworded English text:** scan entries, logbook, pickups (120 strings in 100 tables on the US
  disc), in the `text` folder. Strings that name a button of Remastered's controls or use one of
  its icons keep the disc's text, as do strings with characters outside ASCII.
- **Languages:** see [Languages](#languages) below.
- **Typeface:** Remastered's font as `font/deface.sdfont`, a sharp HD font for the HUD, menus,
  scans and logbook. `MP_HD_FONT=0` turns it off.
- **HUD:** fourteen frames (combat, scan, thermal, X-ray, ball, base, map, pause, pause
  instructions, generic menu, message, quit, flat scan and helmet) in the `hud` folder, with
  the map screen's layout.
- **Map:** the save, missile and elevator station icons, the door arrows between floors, and the
  rooms Remastered reshaped, in `map/`. Experimental: the map's compass is left out, as the disc
  does not have one.
- **Menu movies:** the title, the file select and its transitions, and four of the attract movies
  (the other six, and the credits and ending movies, stay the disc's), in the `Video` folder.
  By default 1600x900 at 30 frames a second, about 550 MB for the sixteen. A movie of any size is
  fitted to the view with its shape kept, so in 4:3 these have bars above and below. Without
  ffmpeg the import finishes without the movies and says so: install ffmpeg and import again (the
  unchanged stages are reused), or run `--import-remastered-movies`. On Android the system's own
  decoder (MediaCodec) reads the movies; nothing has to be installed.
- **Gallery:** the 120 full-size concept art pictures of `UI_FrontEnd` as JPEGs in `gallery/`
  (about 0.3 MB each, 36 MB in all). F1 > Mods > Built-in mods > Prime Remastered > Gallery opens
  them in a viewer window.

#### Languages

Remastered's French, Spanish, German, Italian and Dutch text comes along too (about 1,900 strings
each), named by Remastered's language codes (`EUFR`, `EUSP`, `EUGE`, `EUIT`, `EUDU`).

- Pick the language in F1 > Game > Language (ini key `text_language`, or `MP_LANGUAGE=EUFR` for
  one run). It changes while the game runs (console: `language EUGE`, `language en`): text already
  on screen switches the next time its menu or screen opens.
- A string Remastered doesn't translate, or one whose buttons don't line up with the disc's, keeps
  the disc's wording: a PAL disc's own translation where it has one, else English.
- A PAL disc's French, German, Spanish and Italian work without the mod too.
- Without the mod a USA disc stays in English unless a PAL image's languages are added: F1 > Game
  > Language > "Add languages from a PAL disc..." (or `--import-pal-languages <image>`) reads the
  PAL image once and copies its French, German, Spanish and Italian text tables and fonts to
  `<user>/languages/`. The game keeps running from the USA disc, and the PAL image isn't needed
  afterwards. "Remove" deletes the copy. The same pass adds the PAL disc's
  [full Flaahgra theme](#full-flaahgra-theme-ntsc-discs); if that part fails the languages are
  still added and the message says why.
- The USA disc's fonts are ASCII only, so each accented letter is drawn as its base letter,
  unless the PAL fonts were added (they take effect after a restart) or the mod's typeface
  (`font/deface.sdfont`) is loaded; either one draws the real letter.

#### Performance mode

"Performance mode" in the same section (setting `remastered_performance`, on by default) trades
exactness for speed. On a Radeon 780M at 1080p it saves about 2.4 to 3.3 ms a frame (0.8 ms more
in rooms with glass); the picture differs by at most 2/255 per channel. Off, the frame is exactly
as before.

- Glass refraction reads a screen copy taken every second frame.
- The exposure measure starts from a quarter-size image instead of a half-size one.
- Volumetric fog is blended onto the scene instead of copying the scene and rewriting every
  pixel (not with MSAA).
- In first person with the combat visor, the arm cannon's depth is laid before the world, so the
  world behind it is never shaded (the exposure measure leaves that area out; frames that copy
  the screen while drawing the world, such as glass, turn this off for a few frames).

Console `roomgeo perf [mask]` turns on single shortcuts for comparison.

#### Where the import is stored

- **Desktop:** `mods/remastered-models`, a mod folder like any other; `MP_MODS` and
  `MP_USER_PATH` move it with the rest.
- **Android:** the app's own internal storage, `<internal storage>/remastered-imports`. It is not
  the mods folder and is not moved by `MP_MODS`, `MP_USER_PATH` or the F1 data folder setting
  (System > Data folder). An APK update keeps the import; an uninstall or Android's "clear data"
  removes it (the uninstall dialog's "keep app data" keeps it), so it has to be imported again.
  When the app's internal storage cannot be reached at all the import is refused with a message:
  it is never written to the shared mods folder instead.

**Re-import needed:** after an update that changes the importer, the log says so, a toast shows
once per launch, and the Mods and import panels say "re-import needed". Import again to pick up
the change.

The Android storage rules and the importer's internals are in
[`MOD_FORMATS.md`](MOD_FORMATS.md#remastered-import-internals).

#### Import switches

Environment variables read by the import (a part is imported only when both the panel and the
switch want it). The `--import-*` commands have no GPU and write BC textures unless told
otherwise.

| Variable | Default | Effect |
| --- | --- | --- |
| `MP_REMASTERED_GEOMETRY` | `all` | Room geometry: `all`, a comma-separated list of room names, or `none`. Import-time only. |
| `MP_REMASTERED_EFFECTS` | on | `1` or `0` (also true/on/yes): the experimental particle effect step. |
| `MP_REMASTERED_TEXT` | on | `0` leaves the reworded text out. |
| `MP_REMASTERED_HUD` | on | `0` leaves the HUD out, and with it the map icons and the `.MAPA` rooms. |
| `MP_REMASTERED_MOVIES` | `1600x900@30` | Another size and rate (`1280x720@30`), or `0` (`false`, `off`, `no`) to leave them out. |
| `MP_REMASTERED_GALLERY` | on | `0` leaves the gallery out. |
| `MP_REMASTERED_TEXTURE_FORMAT` | by GPU | `bc` or `astc` forces the texture format (for testing). |
| `MP_FFMPEG` | — | The ffmpeg program to decode the movies with. |

### Metroid Dread Samus models

The other model import, under F1 > Mods > Built-in mods > Metroid Dread: Samus's Power, Varia,
Gravity and Phazon Suit bodies and Morph Balls from your own Metroid Dread `.nsp`/`.xci` and
`prod.keys`, in place of Prime's, in cutscenes, the Morph Ball transitions and the third-person
camera.

- It is a mod of its own, `dread-samus`, so it has a switch of its own and turns off without
  touching the Remastered models. Its section sits beside Prime Remastered, not under it.
- It shares the `prod.keys` row above both sections and the import worker with the Remastered
  import (only one of the two runs at a time).
- From a terminal: `--import-dread-samus`, with the same arguments as `--import-remastered`.
- Phazon takes Dread's Metroid Suit; Fusion keeps its models. It needs the base Dread release
  only.

See [`docs/DREAD_SAMUS.md`](DREAD_SAMUS.md) for the import and its limits.

## Texture packs

Texture packs replace the game's textures with Aurora-format images named
`tex1_<w>x<h>_<texhash>[_<tluthash>]_<fmt>.dds` or `.png`.

- **Built-in folder:** the executable no longer carries a texture pack. `MP_TEXTURES` points at
  a folder of replacements, and a build that keeps `textures/` next to the executable reads that
  one (`<executable dir>/textures`); both are the layer under your pack, and whatever else a build
  carries inside the executable is only the button-prompt icons.
- **Your own pack:** put it in `user_textures` in the [user folder](#installing-mods), or wherever
  `MP_USER_TEXTURES` points, so updates never touch it. It is layered over the folder above, with
  the same folder rules. The overlay's Mods page has a Reload button.
- **Android:** Mods > Texture pack > Choose texture pack folder opens the system folder picker
  and copies the folder's `.png`/`.dds` files into the data folder (pick it again after changing
  it; Remove deletes the copy). The copy lands in `user_textures.new` and is swapped in on the
  next frame, or at the next start if the app closed first.

Priorities: built-in folder 0, user pack 1, binding icons 2, so a remapped action still shows
its binding.

### Button prompts

In-game button prompts are ordinary textures, so packs can replace them per device. A pack folder
may hold subfolders `xbox`, `playstation`, `switch`, `gamecube`, `standard` and `keyboard`.

- The set follows the input the player last used: a pad's type; keyboard for a keyboard or mouse,
  or when no pad is connected; the Android touch overlay's layout. It is swapped automatically.
- Only deliberate input switches it: a key press, a mouse move or click, a pad button, or a stick
  or trigger pushed past half way. The touch overlay's virtual pad does not count.
- F1 Controls > Options "Button glyphs" (`prompt_glyphs=`) pins one set (Xbox, PlayStation,
  Nintendo Switch, GameCube or keyboard & mouse), and `MP_TEXTURE_DEVICE` forces any name.
- A folder with no subfolders is a single device-agnostic pack. A folder that has device
  subfolders but not the one selected loads nothing rather than mixing packs.
- Prompts follow your bindings: rebinding a key, mouse or pad button changes the prompt.

`MP_DUMP_TEXTURES=1` writes every source texture to `<cachePath>/texture_dumps` as DDS, for
authoring replacements. Making prompt art: see
[Authoring button prompts](MOD_FORMATS.md#authoring-button-prompts).

## Custom importers

An importer is a program of your own that builds a mod from files you have (a model pack, another
release of the game); the port ships none. Desktop only.

- Put an executable file (Windows: `.exe`, `.bat`, `.cmd`) in `importers` in the pref folder,
  beside `mods`.
- F1 > Mods gets a Run button for it, with an optional argument field, the tail of its output
  while it runs, and Cancel.
- `metroid_prime_port --import` lists them and `--import <name> [argument]` runs one in the
  terminal without starting the game, returning its exit code.

What the importer gets:

- the argument as `argv[1]`, the mods folder to write into as `MP_MODS_DIR`, and the importers
  folder as its working directory;
- its stdout and stderr are shown a line at a time (a carriage return rewrites the line, so
  progress bars work);
- exit code 0 means the mod is in place, and Reload mods (or a restart) loads it;
- Cancel sends a terminate request to the importer alone, so a script should pass it on to its
  children.

## Environment variables

| Variable | Default | Effect |
| --- | --- | --- |
| `MP_MODS` | `<user>/mods` | The mods folder. |
| `MP_USER_PATH` | — | The user folder (moves the mods folder and a desktop Remastered import with it). |
| `MP_TEXTURES` | built-in folder | Folder of texture replacements overriding `<executable dir>/textures`. |
| `MP_USER_TEXTURES` | `<user>/user_textures` | Your own texture pack. |
| `MP_TEXTURE_DEVICE` | last input | Forces the button-prompt set by folder name. |
| `MP_DUMP_TEXTURES` | off | `1` dumps every source texture to `<cachePath>/texture_dumps`. |
| `MP_TEXTURE_MAX_DIM` | 0 (1024 on Android) | Caps the size a native `.dds` texture is loaded at, in px; 0 = no limit. |
| `MP_TEXTURE_CACHE_MB` | 4096 (1024 on Android) | Memory the loaded `.dds` textures may keep cached. |
| `MP_MEM1_MB` | 24 | Emulated MEM1 arena and game heap in MB (capped at 1024), for heavy mods. |
| `MP_HD_FONT` | on | `0` turns HD `.sdfont` fonts off. |
| `MP_ROOM_LIQUID` | on | `0` draws the retail liquid planes instead of a mod's. |
| `MP_ROOM_GEO_RESIDENT` | on | Frame buffer sizing for room-geometry mods (see `MOD_FORMATS.md`). |
| `MP_FRAME_BUFFERS` | — | Frame buffer size override. |
| `MP_REMASTERED_BACKLIGHT` | on | `0` turns Remastered's character backlight off. |
| `MP_REMASTERED_EFFECT_ACTORS` | on | `0` ignores `.roomfx` files. |
| `MP_LANGUAGE` | `text_language` | Text language for one run (`EUFR`, ...). |
| `MP_DISC` | remembered | The disc image the terminal imports use. |
| `MP_SMOKE_BIND_A` | — | Test-only rebinding of the A action (smoke-driver builds). |

The Remastered import's own switches are in [Import switches](#import-switches).

---

Making mods: [`docs/MOD_FORMATS.md`](MOD_FORMATS.md) describes every file a mod can hold, with
the binary layouts in `platform/include/port_*.h`.
