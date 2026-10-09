# Release readiness

What a build of this port contains, what has to travel with it, and what is
still missing before it can be handed to someone else. Every claim here is
checked against the tree; where something is not true yet, it says so.

## What the port is

A native recompilation of **Metroid Prime** (`GM8E01`, USA v1.00) plus a
platform layer. The game code is statically recompiled from the retail
executable, so **no game assets, no disc image, and no extracted data may be
packaged with a build**. The build takes the disc image as a runtime argument
and nothing else: `tools/make_appimage.sh` says so in its own help text, and
the port asks for the image on first launch when it cannot find one.

## Licensing

| Component | Licence | Where it is |
|---|---|---|
| Aurora (windowing, WebGPU/Dawn plumbing) | MIT | `extern/aurora/LICENSE` |
| MusyX (audio mixer) | MIT | `extern/musyx/LICENSE` |
| astc-encoder (ASTC texture writer) | Apache-2.0 | `extern/astcenc/LICENSE.txt` |
| SDL3 | zlib | fetched at configure time, `build/*/_deps/sdl-src/LICENSE.txt` |
| Dear ImGui | MIT | fetched, `imgui-src/LICENSE` |
| fmt | MIT | fetched, `fmt-src/LICENSE` |
| zstd | BSD | fetched, `zstd-src/LICENSE` |
| Button prompt icons | Kenney CC0 | `tools/prompt_icons/`, built into `textures/` |

**The repository's own work is MIT, and the game code is not.** `LICENSE` grants
MIT over the platform layer, the build and packaging scripts, the tests, `tools/`
and `docs/`. `NOTICE` states what that deliberately does not cover: `src/` and
`include/` are a recompilation of the retail game, no permission to redistribute
them is asserted anywhere in this tree, and the grant stops at the repository's
own code rather than sweeping those directories in.

That is a grant over the port, not an answer about the game. Anyone packaging a
release still has to settle the second question — what may be done with a
working copy of decompiled game code — and only the copyright holder can answer
it. The licence here is not evidence that they may.

### Where saves live, and the per-build-directory card

The memory card is placed with `CARDSetBasePath(SDL_GetBasePath())`, so **the card
belongs to the directory the executable is in** — not to `MP_USER_PATH`, which
controls only the dawn cache and the game profile. On desktop that is next to the
binary; on Android it is the app's storage, because the APK is read-only.

The practical consequence is that every build directory has its own card, so a
save written by one build is invisible to another. That is worth knowing when
comparing runs, and worth saying in a support answer, because "my save vanished"
is otherwise inexplicable. It is a deliberate choice — a copied build stays
self-contained — and it is why `MP_USER_PATH` cannot be used to move saves.

### Notices must travel with a build

- **All four do.** Each ships the port's own `LICENSE` and `NOTICE` alongside
  the third-party terms, because a grant nobody can read inside the package is
  not much of a grant.
- **Windows**: `windows.yml` copies the port's grant and notice, Aurora's and
  MusyX's licences, and every `LICENSE*`/`COPYING*`/`NOTICE*` from the fetched
  packages into `dist/licenses/`, preserving the dependency path, and puts
  `docs/NATIVE_PORT.md` in as the `README`. It also packages `textures/` (the
  HD and button-prompt sets), so the artefact zips as-is with no copy from a
  Linux build. SDL3 (built from source), zlib, libpng, nod and OpenSSL are
  linked statically into the exe, so their licences (including `openssl.txt`
  and `nod-*.txt`) are the only trace; only `webgpu_dawn.dll`, `dxil.dll` and
  `dxcompiler.dll` ship as DLLs (Dawn's prebuilt package has no static lib).
  The workflow fails if the exe imports SDL3/zlib/libpng/nod/OpenSSL DLLs.
- **Windows also carries `ffmpeg.exe`**, which the Remastered import runs to
  decode that game's movies (Windows has no ffmpeg of its own, and the port
  links no decoder). It is a separate program under the LGPL 2.1, built in the
  workflow by `tools/build_ffmpeg_min.sh` from the official, unmodified source
  tarball (pinned by hash) with only the H.264 decoder and the JPEG encoder:
  no GPL or non-free parts, no external libraries. `dist/licenses/` gets the
  LGPL text and `ffmpeg.txt`, which names the source URL, its hash and the
  configure line, so anyone can rebuild or replace it. Keep those three
  together with the executable in every Windows package. The Linux packages
  ship no ffmpeg and use the system's.
- **macOS** (experimental): `macos.yml` builds `Metroid Prime Port.app` with
  SDL3, nod, OpenSSL, zstd and xxhash linked in statically (the workflow fails
  if `otool -L` shows anything outside `/usr/lib` and `/System/Library`), puts
  the same LGPL `ffmpeg` as Windows in `Contents/MacOS`, and collects the
  licences into `licenses/` beside the app. Both are ad-hoc signed, not
  notarised.
- **The AppImage and the APK**: `tools/make_appimage.sh` collects them into
  `usr/share/licenses/metroid-prime-port/`. It bundles no shared libraries.
  The APK's `syncLicenseNotices` task gathers the same set into `assets/`,
  verified present in a built package: `port-license.txt`, `port-notice.txt`,
  `aurora.txt`, `musyx.txt`, `sdl-src.txt`, `imgui-src.txt`, `fmt-src.txt` and
  `zstd-src.txt`.
- **The Flatpak** collects them in the manifest's `post-install`: the two
  vendored snapshots out of the tree, and the four fetched packages by glob,
  because the fetched ones only exist once `cmake-ninja` has run.

## Signing identity

**Android release signing is this project's own key.** `tools/make_android_keystore.sh`
creates it and writes `android/keystore.properties`; the file and the `.jks` are
both untracked, and `.gitignore` says so. The build refuses to sign a release
with the shared debug key unless that is asked for explicitly, because the debug
key is not an identity: it is public, it is the same on every machine, and it
says nothing about who built the package.

The refusal matters more than it looks. Android treats a signature change as a
different app, so a debug-signed release has to be **uninstalled** before a
properly signed one will install. Finding that out after shipping is worse than
being told at build time.

Where the four values come from: `android/keystore.properties`, then
`MP_APK_KEYSTORE` / `MP_APK_KEYSTORE_PASSWORD` / `MP_APK_KEY_ALIAS` /
`MP_APK_KEY_PASSWORD` — so a CI secret and a local file take the same path. All
four are required. A partial set is an error naming which are missing, and a
`storeFile` that is not there is an error naming the path, rather than a silent
fall back to the debug key.

`tools/android_apk.sh` opts in on the caller's behalf **only** when no key is
configured, and says so on stderr, because a build that is merely going to be
sideloaded should not need a key to exist first. `--strict-signing` turns that
into a refusal. The rule itself is Gradle's, not the script's: calling Gradle
directly with no key stops the build.

Verified on a release build: the package's signer is
`CN=Metroid Prime Port, OU=Port, O=Metroid Prime Port` (SHA-256 `d8814c79…`),
not the debug key's `CN=Android Debug` (`edd22fdb…`), the arm64 `.so` is 29 MB,
all six third-party notices are in `assets/`, and no `.iso`, `.pak` or `.strg`
is in the package.

The port targets `versionName "0.19.0"` and `versionCode 22`. A version bump
also adds a `<release>` entry (newest first) to
`packaging/io.github.odrannnn.metroidprimeport.metainfo.xml`, which is the
version Flatpak reports; `tools/make_flatpak.sh` stops if the two differ.

## Per-platform packaging

| Platform | Builds from | Produces | State |
|---|---|---|---|
| Linux | `cmake -S . -B build/port-gcc` | executable | works; tests green. `cmake --install` also produces a complete tree, verified by running it |
| Linux | `.github/workflows/linux-release.yml` | AppImage + tarball | manual trigger; builds on AlmaLinux 9 and runs `tools/make_appimage.sh`. Use these for releases, not a desktop build |
| Linux | `tools/make_flatpak.sh` | Flatpak | manifest installs a working tree and collects notices; builds (`tools/make_flatpak.sh` writes `build/flatpak/MetroidPrimePort.flatpak`). The app id is `io.github.odrannnn.metroidprimeport`, with AppStream metainfo shipped |
| Windows | `.github/workflows/windows.yml` | zipped `dist/` | manual trigger only for now; green when run, artifact uploaded, packaged startup checked |
| macOS | `.github/workflows/macos.yml` | ad-hoc signed `.app` (arm64) + `licenses/` | experimental; builds, tests and packaged startup checked in CI, never run on a real Mac |
| Android | `tools/android_apk.sh :app:assembleRelease` | APK | builds, signed with this project's own key; runs on-device (POCO F8 Ultra, 60 FPS), touch/Continue-load still unverified |

The Linux binary is the only one with a test suite attached: 54 `port`-labelled
ctest targets, all run by both CI jobs.

Every build ships `assets/initial_pipeline_cache.db`: the pipeline configs of a tour
of the front end and every room, so a first start compiles the game's shaders in the
background instead of skipping draws as it meets them. It holds retail rows only. When
aurora bumps a pipeline config version, aurora ignores the seed and the
`port_pipeline_seed` test fails. To record it again, run
`python3 tools/pipeline_seed.py tour`, then `merge` (see the script's `--help`).

## Runtime dependencies

A Linux binary only runs where glibc and libstdc++ are at least as new as the
ones it was linked against, so a desktop build is only as portable as the
desktop. The 0.12.0 AppImage needed glibc 2.43 and GLIBCXX_3.4.34 and would not
start on LMDE (issue #3). The release artefacts therefore come from
`.github/workflows/linux-release.yml`, which builds in `almalinux:9` with
gcc-toolset-14 and fails unless the binary needs at most **glibc 2.34** and
**GLIBCXX_3.4.29** (Debian 12, Ubuntu 22.04, RHEL 9 and newer). The AppImage
bundles nothing, so freetype, libpng, zstd and OpenSSL 3 come from the system.
`gh workflow run linux-release.yml --ref port -R Odrannnn/MetroidPrimePort`, then
`gh run download <id> -n metroid_prime_port-linux`.

A Vulkan driver, X11 or Wayland, and DBus for the file dialog come from the
host. A session that reaches `show_window` over SDL's Wayland backend will hang
under GNOME unless the port's own X11 preference applies — see
`docs/NATIVE_PORT.md`.

## What still blocks a real release

1. ~~**Saving writes an empty save, on every platform.**~~ **Not a defect — it was
   the test harness, and saving and reloading both work.** The empty save came from
   a *card repair*, not a save: the in-game save screen found a file it considered
   corrupt, and answering that dialog deletes the file and re-creates it blank, with
   no save data. A save needs a second confirmation, at the screen's `SaveReady`
   state, and the driver was only ever pressing once. With that fixed, a save
   writes real data and the front end loads it — evidence in
   `docs/images/save-main-menu.png` (the main menu showing
   `[Samus A] 00% | Space Pirate Frigate | 00:00 Elapsed`) and
   `docs/images/save-loaded.png` (the loaded game, on the Frigate). The saved
   file's 3004-byte data region has 134 non-zero bytes, a valid CRC and slot 1
   flagged present.

   The one thing still unexplained is **why the in-game screen finds a corrupt
   file at all** on a card that was supposed to be empty. That is not a shipping
   blocker, but it is not understood and is still open.
2. **No answer for the decompiled game source.** The port's own work is MIT
   (`LICENSE`, scoped) and `NOTICE` says so, but what may be done with a working
   copy of decompiled game code is a question for the copyright holder, and no
   part of this tree answers it.
3. ~~**The AppImage only names the shared libraries it copies.**~~ It no longer
   copies any.
4. **Android on-device behaviour is partly verified** — the release build runs on
   a POCO F8 Ultra at a steady 60 FPS (see `docs/ANDROID_BUILD_PROBE.md`). Still
   unverified: loading a save from the title screen's Continue, touch ergonomics
   and tap reliability, and performance on any other device.
5. **~~`wss://` does not work on Android~~ — resolved.** OpenSSL 3.5.8 is built
   from a pinned, hash-checked source for the NDK and linked statically, so
   `MP_HAVE_OPENSSL` is defined in the APK and a `wss://` server connects. On
   Android the port now loads the system trust store itself, because
   `SSL_CTX_set_default_verify_paths` points at a compiled-in `OPENSSLDIR` that
   does not exist there and returns success having loaded nothing; the store is
   enumerated by hand from `/apex/com.android.conscrypt/cacerts`, and zero
   certificates is a loud error naming the directories rather than an obscure
   verification failure later. Both halves are proven on the emulator — see
   `docs/ANDROID_BUILD_PROBE.md`. Costs about 2 MB of APK — the measured numbers
   are in `docs/ANDROID_BUILD_PROBE.md`.
6. **The Flatpak is not on Flathub.** `tools/make_flatpak.sh` builds a bundle
   from the committed `port` branch; the project's own install rules put a
   runnable tree in `/app`, and the notices are collected. Before it could be
   published:
   - **~~No icon is shipped at all~~ — resolved.** `packaging/varia-bolt.svg` is
     the maintainer's own artwork (it has to be *original*: anything
     recognisably Nintendo's is not ours to redistribute), and every package
     carries it. The install rules put the SVG in `share/icons/` under the app
     id for the Flatpak; `tools/gen_icons.py` rasterises it into the committed
     `packaging/metroid_prime_port.png` (AppImage), `metroid_prime_port.ico`
     (linked into the Windows executable through `metroid_prime_port.rc`), the
     Android adaptive icon's foreground, and `platform/port_window_icon.inc`,
     which the desktop builds set as the window icon so the bare binary in the
     tarball has it too. Run the script again after changing the SVG.
   - **No screenshots.** Flathub requires them, and any screenshot of the running
     game shows Nintendo's game, which this package may not redistribute. This
     one cannot be fixed by writing a file.
   - The app id is now `io.github.odrannnn.metroidprimeport`, in the metainfo,
     the desktop entry, the icon name, the manifest, the install rules and the
     packaging script. It is lowercase and passes `appstreamcli validate
     --pedantic` with no complaints, which it did not before: the old
     `org.metroidprime.MetroidPrimePort` drew `cid-contains-uppercase-letter`.
     `io.github.*` is a domain the maintainer controls, which is what Flathub
     verifies an id against — the previous `org.metroidprime` was a placeholder
     nobody owned. Verified by installing to a prefix and checking that the
     binary runs, the metainfo validates from the install tree, and the desktop
     entry's id matches the file it is installed as.
