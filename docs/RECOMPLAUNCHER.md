# RecompLauncher

Desktop release archives carry a project-owned `recomplauncher.json` beside
the executable. RecompLauncher reads this before its catalog/built-in profile.
The Windows manifest lives at the repository root; `tools/write_recomplauncher.py`
adapts its executable, data paths and V-Sync default for Linux and macOS.

## Install and play

- Windows: install the Windows zip.
- Linux: choose the Linux tarball, or the **AppImage tarball**, in the launcher's
  build picker. Keep the AppImage and `recomplauncher.json` together when
  unpacking it yourself.
- macOS: install the macOS zip. The manifest stays outside the signed app bundle.

Select your own supported GameCube or Trilogy disc image on the setup screen.
The launcher supplies it through `MP_DISC` and remembers it in `disc_path`, so
opening the port directly afterwards uses the same image. It does not copy or
ship any game assets.

The manifest exposes fullscreen, Original experience, render scale, aspect ratio,
MSAA, V-Sync and both frame rate controls. Original experience disables the
graphics and frame-rate options it overrides, preserving their saved values.
The custom frame cap is available only with Original experience and the original
60 fps cap off. Turning the original cap on or off preserves the custom cap.
Unlimited sets only `fps_cap=0`; V-Sync can still limit the presented frame rate.

Saved caps outside the presets and fractional render scales show as
**Custom (in-game)** instead of pretending to match a preset. This choice does
not change the saved value; set an arbitrary value in the game's F1 menu. The
launcher uses the first choice whose keys all match, so the empty `set` fallback
must stay last. With dynamic resolution enabled in-game, render scale sets its
maximum rather than a fixed resolution. Other settings remain in the F1 menu.

## Data and backups

The settings paths follow `platform/include/port_paths.h`:

- Windows and unpacked Linux builds use `user/` beside the executable. Existing
  loose settings are migrated there by the port. An existing per-user install
  stays in its old folder until the player moves it.
- AppImages use the directory containing the AppImage, with the same legacy
  per-user fallback.
- macOS uses `~/Library/Application Support/Metroid Prime/`, outside the bundle.
- Windows legacy data is in `%APPDATA%/Metroid Prime/`; Linux legacy data is in
  `$XDG_DATA_HOME/Metroid Prime/` (normally `~/.local/share/Metroid Prime/`).

The launcher can open the data folder and find logs there. Save backups list the
regional memory-card folders, raw memory cards, save states and Archipelago game
state. Large save states may exceed the launcher's 64 MB per-file backup limit.
Caches, imported assets and connection credentials are not listed for backup.
Custom `MP_USER_PATH` overrides and read-only archive installations are outside
this manifest's path selection; use the game's own settings in those cases.

## Integration limits

This supplies the developer guide's launch and settings integration (level 2).
The port reads the disc directly and needs no mandatory extraction/setup step,
so there is no artificial `prepare` command. No verified-dump badge is claimed:
the manifest has no unverified CRC list. RecompLauncher's documented `game.check`
formats describe ISO 9660 and Xbox discs, not GameCube/Wii filesystems. The port
continues to check supported game IDs, revisions and readability itself.

Each AppImage embeds its generated manifest at `/recomplauncher.json` in the
AppImage filesystem. The AppImage tarball carries an identical adjacent copy
for current RecompLauncher versions, which only copy a standalone AppImage and
do not read its embedded manifest. Standalone installs therefore still use
launcher-side fallback data until embedded-profile reading is implemented
in RecompLauncher. Use the AppImage tarball for the project-owned profile today.
Embedding the file here does not itself add that launcher capability.

## Maintain and check

When settings keys or defaults change, update the root manifest along with the
port. Packaging must never include a player's `port_settings.ini`.

```sh
python3 -I tests/port_recomplauncher.py
python3 tools/write_recomplauncher.py --platform linux build/recomplauncher.json
python3 tools/write_recomplauncher.py --platform linux --appimage-arch x86_64 build/appimage-profile/recomplauncher.json
```

In RecompLauncher's Settings > Developers, enable Developer tools and add a
packaged build folder. Check the manifest, choose a supported disc, change a
setting, and confirm the port starts with it and that save backups find the
memory card. Repeat on Windows, Linux (tarball and AppImage tarball), and macOS
before claiming runtime validation.

See the [RecompLauncher developer guide](https://github.com/elripalda/RecompLauncher/blob/main/docs/DEVELOPERS.md)
and [schema](https://github.com/elripalda/RecompLauncher/blob/main/docs/recomplauncher.schema.json).
