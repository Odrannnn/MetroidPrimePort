# iPhone and iPad

## Status

**Experimental, and untested on real hardware.** The build is made in CI on
GitHub's macOS runners. Each run also starts the app in the iOS Simulator as a
smoke test. Nobody working on the port owns an iPhone or iPad, so the first
reports from testers are what will show whether it plays. Please open an issue
and attach the log (see [Where things are](#where-things-are)).

- iOS / iPadOS 16.0 or newer, 64-bit (arm64).
- The renderer is WebGPU on Metal (Dawn), the same layer as on desktop.
- Touch controls are drawn by the game itself. The cog in the bottom-left
  corner opens the settings overlay; its Controls tab has the touch options.
  A paired controller works too (Bluetooth, MFi or USB).
- Memory use is set the way it is on Android: a phone with 4 GB of RAM or less
  may still run out of memory.

## Getting the app

The `.ipa` is **unsigned**. iOS installs only signed apps, so you sign it
yourself with a free Apple ID. No paid developer account and no Mac are needed.

1. Download `MetroidPrimePort.ipa` from a release. For a newer build, take the
   `metroid_prime_port-ios` artifact of a green
   [ios workflow](../.github/workflows/ios.yml) run.
2. Sign it and install it with a sideloading tool, such as
   [AltStore](https://altstore.io), [SideStore](https://sidestore.io) or
   [Sideloadly](https://sideloadly.io). AltStore and Sideloadly need a Windows
   PC or a Mac each time they install or refresh the app. SideStore needs a
   computer only once, to pair with the device (Linux works too); after that it
   installs and refreshes the app on the device itself.
3. On iOS 16 or later, turn on **Settings > Privacy & Security > Developer
   Mode**, then trust your Apple ID under **Settings > General > VPN & Device
   Management**.

A free Apple ID has limits. A signature lasts **7 days**: refresh the app with
the same tool before then, or it won't open until you do (your data is kept).
You can have at most 3 sideloaded apps at once.

## The disc

You need your own GameCube disc image of Metroid Prime (USA 1.00, 1.01 or
1.02, or PAL). Nothing of the game ships with the app.

On first start a file picker opens. Pick the image (`.iso`, `.gcm`, `.rvz`,
`.wbfs`, `.ciso` or `.nkit`) from Files, iCloud Drive or a USB drive. The
image is copied into the app's own folder, so it stays available offline; you
can delete the original, or the copy from Files to free the space.

You can also copy the image into **Files > On My iPhone (or iPad) > Metroid
Prime** before starting the app. The game finds it there, or in a subfolder
there, without asking.

## Where things are

Everything the game writes is in that same **Files > On My iPhone (or iPad) >
Metroid Prime** folder:

| What | Where |
|---|---|
| Log | `metroid_prime_port.log` (the previous run's is `metroid_prime_port.old.log`) |
| Settings | `port_settings.ini` |
| Memory card saves | `USA/Card A/` (`EUR/Card A/` for PAL) |
| Mods, texture packs | `mods/` (see [`MODS.md`](MODS.md)) |
| Your disc image | wherever you put it there |

The shader caches stay in the app's hidden Library folder; iOS may clear them,
and the game then rebuilds them. Deleting the app deletes this folder too, so
copy your saves elsewhere before reinstalling a build signed with a different
Apple ID.

## Building

CI builds it: `.github/workflows/ios.yml` runs CMake with the iOS toolchain
for `iphoneos` and `iphonesimulator`, then `tools/ios_package.sh` packages the
`.app`, and `tools/ios_sim_smoke.sh` runs the Simulator smoke test. A local
build needs a Mac with Xcode; the configure flags are in the workflow.
