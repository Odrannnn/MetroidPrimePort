# tools/

Scripts for building, packaging and developing the port. Each file's own
header has the full usage. "Used by" names what runs it automatically;
everything else is run by hand. The original decompilation's DOL build
tooling is in `decomp/`, not here.

## Build, test and packaging

| Script | What it does | Used by |
|---|---|---|
| `embed_resources.py` | Writes the C++ source that embeds `assets/` and other files into the executable | CMake build |
| `check_settings_index.py` | Checks the F1 settings search index against the widgets it names | CTest `port_settings_index` |
| `pipeline_seed.py` | Records and checks the shipped initial pipeline cache (`assets/initial_pipeline_cache.db`) | CTest `port_pipeline_seed` |
| `ap_fake_server.py` | Dependency-free Archipelago test server, plain or TLS | CTest `port_ws_tests`; by hand for AP testing |
| `make_ap_fixture.py` | Generates the spec-shaped AP `Locations.py` fixture in `ap-world-fixture/` from a port dump | CTest `port_ap_fixture` |
| `make_ap_config.py` | Joins an Archipelago world's `Locations.py` with a port location dump into `archipelago.json` | CTest `port_ap_fixture`; by hand |
| `gen_skippable_cutscenes.py` | Generates `platform/port_skip_cutscenes_data.inc` from randomprime's skippable-cutscene patches | CTest `port_skip_rev2` imports it; by hand to regenerate |
| `collect_licenses.sh` | Collects every licence and notice a shipped build must carry into one directory | CI (all release packages), `make_appimage.sh` |
| `make_appimage.sh` | Packages the executable and texture replacements as an AppImage | CI `linux-release.yml` |
| `build_ffmpeg_min.sh` | Builds the minimal ffmpeg the Windows and macOS packages carry for the Remastered movie import | CI `windows.yml`, `macos.yml` |
| `make_flatpak.sh` | Builds a Flatpak bundle from `flatpak/` | by hand |
| `android_apk.sh` | Builds and signs the Android APK through Gradle | by hand |
| `make_android_keystore.sh` | Creates the project's Android release signing key and `android/keystore.properties` | by hand, once |
| `android_probe.sh` | Cross-configures and builds the port for Android with a local NDK (see `docs/ANDROID.md`) | by hand |

## Code and asset generators

Their output is committed, so a build does not need them or their inputs.

| Script | What it does |
|---|---|
| `gen_ap_locations.py` | `platform/port_ap_locations.inc`: the built-in Archipelago location table |
| `gen_ap_logic.py` | `platform/port_ap_logic_data.inc`: the AP tracker's logic, from the PopTracker pack |
| `gen_ap_pickup_patches.py` | `platform/port_ap_pickups_data.inc`: randomprime's per-pickup room patches |
| `gen_ap_world.py` | `platform/port_ap_world_data.inc`: rooms and elevators an AP seed's options name |
| `gen_custom_pickup_assets.py` | `platform/port_custom_assets_data.inc`: randomprime's custom pickup assets |
| `gen_map_pickups.py` | `platform/port_map_pickups.inc`: every pickup's position, for the map's pickup dots (needs a disc) |
| `gen_save_layouts.py` | `platform/save_layouts/*.inc`: per-world save layout tables for one disc |
| `gen_icons.py` | Rasterises `packaging/varia-bolt.svg` into every platform's icon |
| `make_prompt_glyphs.py` | Per-device button-prompt texture replacements, from `prompt_icons/` |
| `extract_textures.py` | Extracts every TXTR from a disc under Aurora's replacement names |

## Developer and debugging tools

| Script | What it does |
|---|---|
| `mprig.py` | Isolated headless runs of the port (Xvfb + game + debug console); see `docs/DEBUGGING.md` |
| `mpsoak.py` | Pre-release crash soak over the mprig rig (Remastered/retail x validation x MSAA); see `docs/DEBUGGING.md` |
| `mpcon.py` | Client for the debug command console (`MP_CONSOLE`) |
| `rando_seed.py` | Creates item-randomizer seed JSON from location-dump logs |
| `pbr_shots.py` | A/B contact sheets of models under the PBR reflection probe (smoke builds) |
| `anuv_check.py` | Checks the C++ ANUV flattening (`port_remastered_anuv_tool`) against a reference evaluator |
| `card_inspect.py` | Reports existence, data and CRC state of every `.gci` save on an Aurora memory card |
| `disc_diff.py` | Inventories a disc's resources and diffs two inventories (e.g. two revisions) |
| `upstream_screen.py` | Screens an upstream PrimeDecomp delta for changes that alter behaviour, ignoring matching churn |

## Manual checks outside CTest

These need retail data, external tools or a running game, so CTest does not
register them. Run them by hand after changing the code they cover. The
Remastered checks compare against retrotool's output under `build/mpr*/`
(see each script's header), and their C++ helper tools have no CMake target:
compile `tests/port_remastered_<x>_tool.cpp` with
`platform/port_remastered_<x>.cpp` and `-Iplatform/include`, e.g.
`g++ -std=c++20 -O2 -Iplatform/include tests/port_remastered_cmdl_tool.cpp platform/port_remastered_cmdl.cpp -o /tmp/rmcmdl_tool`
(the image tool also needs the astcenc headers and library from `extern/`).

| Script | Needs | Run |
|---|---|---|
| `tests/game_options_persistence_rig.py` | A disc image, Xvfb and a built port (the `mprig.py` setup) | `python3 tests/game_options_persistence_rig.py --disc <iso> --build <build dir>` |
| `tests/port_remastered_cmdl_check.py` | Extracted Remastered models and retrotool | `python3 tests/port_remastered_cmdl_check.py --build /tmp/rmcmdl_tool --models 48` |
| `tests/port_remastered_txtr_check.py` | Extracted Remastered textures and retrotool | `python3 tests/port_remastered_txtr_check.py --tool /tmp/rmtxtr_tool` |
| `tests/port_remastered_image_check.py` | Pillow, numpy and your own test images | `python3 tests/port_remastered_image_check.py <tool> <image>...` |
