# Debugging the port headless: `tools/mprig.py`

One CLI replaces the per-task shell rigs: it starts its own Xvfb and game with the debug
console, drives it, screenshots, diffs and tears down. Parallel sessions never collide
(displays from `:200`, console ports from 5100, allocated under a lock). Every command exits
non-zero with a one-line `mprig: <reason>` on stderr when it fails. State is in `build/rig/`
(git-ignored): `build/rig/<name>/{run.json,game.log,user/,shots/}`.

The Xvfb/RADV environment (`MESA_VK_WSI_DEBUG=sw`, `SDL_VIDEODRIVER=x11`, dummy audio,
`MP_FAST_BOOT`, `MP_CONSOLE`, ...) is set automatically. **Never capture on `:0`**: GPU-heavy
runs on the real display have taken the desktop session down.

## Setup

Defaults come from `build/rig.ini` (`[rig]`: `disc`, `user`, `build`; local only), overridable
by env `MPRIG_DISC` / `MPRIG_USER` / `MPRIG_BUILD` (disc also `$MP_DISC`) and by flags.
`mprig.py doctor` checks it all (tools, binary, disc, user dir, Remastered `.import-version`
vs the current `kImportVersion`, `/tmp` space, stale runs).

## Quick start

    M="python3 tools/mprig.py"
    $M start t1 --room 83F6FF6F:D5CDB809    # prints: t1 :200 5100 <log>
    $M cmd t1 status 'roomenv info'          # one console command per argument
    $M shot t1 /tmp/a.png 'view albedo'      # run commands, screenshot -> PNG
    $M stop --all                            # always finish with this

`start --room` returns in about 7-8 s, once the player is in control: cutscenes are skipped
(even unseen ones) and the fade-in after them has run. Pass `--cutscenes` to see them. Use your own run names so sessions stay apart.
`--room` takes a room name too (`--room "landing site"`; `rooms [filter]` lists them).

**Settled** = `status --json` says first person, no cinematic and `fade` false (the in-game fade-in
filter, `CInGameGuiManager::StartFadeIn`, is done), on two polls at least 12 frames apart. `start --room`,
a `warp` sent through `cmd`/`shot`, and `shot` wait for it (not on `--cutscenes` runs).

## Subcommands

- `start <name> [--build B] [--room MLVL[:MREA]|NAME] [--at VIEW] [--env K=V]... [--mods DIR|none] [--settings FILE|none] [--saves] [--size WxH] [--wait S] [--cutscenes] [--gdb] [--replace]`:
  `--build` is a dir under `build/` or a path (default `port-gcc`). The user dir is fresh: only
  `port_settings.ini` and `imgui.ini` are copied (`--saves` adds `USA/`, `savestates/`). Mods
  default to the real user's (read-only); `--mods none` is an empty folder. On a startup crash
  it prints the log tail and the symbolized crash, and leaves nothing running. By default it
  sets `MP_SKIP_CUTSCENES=1` and `skippable_cutscenes=1` in the run's settings (that also applies
  randomprime's room patches, as the F1 setting does), and with `--room` it waits for
  settled (see above; a build without `status --json` falls back to `first person 1, cinematic 0`
  plus `wait 90`). `--cutscenes` turns all of that off.
- `cmd <name> <cmd>... | -f script.txt`: replies printed, exit 1 if any command failed, 2 if the game is gone.
- `shot <name> <out.png> [cmd...] [--crop x,y,w,h] [--settle FRAMES] [--force] [--at VIEW]`: waits up to 20 s
  for settled first (`--force` captures at once; if it never settles it captures and warns on stderr).
  `cmd`/`shot` command lists wait for settled after each `warp` (up to 60 s). A held game (`hold 1`) cannot
  answer `status`, so the check is skipped there.
- `shots <name> <outdir> <room|MLVL:MREA|@view>... [--cmds "c1;c2"] [--at VIEW] [start options]`: one game,
  many rooms. Boots into the first, then per room `warp`, wait settled, run `--cmds`, capture
  `<outdir>/<NN>-<slug>.png`, and print the wall time; stops the run at the end, also on an error.
  `--at VIEW` (or `@view`) shoots a saved viewpoint (warping to its room, restoring its pose).
- `rooms [filter]`: room name and MLVL:MREA, read at runtime from `kRooms[]` in `platform/port_ap_world_data.inc`.
  Names match case-insensitively: exact, then unique prefix, then unique substring; an ambiguous one lists candidates.
- `view save <run> <name> | ls | rm <name>`: viewpoints in `build/rig-views.json`: room, player pos and yaw, and the
  freecam pose when freecam is on. `--at <name>` on `start`/`shot`/`shots` warps if the room differs, restores
  the pose (`tp`, `face`, `freecam pos|look`) and waits settled. Pitch of the player's view is not saved.
- `ab <name> <prefix> --a "cmd;cmd" --b "cmd;cmd" [--settle 30] [--no-hold]`: writes
  `<prefix>-{a,b,diff,ab}.png` and the diff line. Sends `hold 1` first (ticks frozen) so only
  the toggle differs; run `cmd <name> 'hold 0'` afterwards to resume.
- `diff a.png b.png [--out heat.png] [--fail-above MAD]`: `mad= psnr= changed=% bbox=x,y,w,h luma_a= luma_b=`; exit 3 above the threshold.
- `pick <name> <x> <y> [--shot out.png]`: which draw is at a window pixel (top-left origin): owner, CMDL, material, record, shader hash. `--shot` writes a normal frame with a crosshair on the pixel.
- `sheet out.png img... [--cols N] [--labels a,b] [--width 480]`
- `film <name> <out.png> [--frames 0,4,8,16,32] [--pre "cmd;cmd"] [--cols N] [--width W]`: runs `--pre`, then `hold 1`, steps to each listed tick count and shoots; writes a labelled filmstrip plus `<out>-f<N>.png` and prints the diff of each frame vs the previous one. Spawn the effect in `--pre` (tick commands such as `fx` do not run while held). Resumes with `hold 0`.
- `fxbisect <name> <root PART> --shot-cmds "cmd;cmd" --bad-region x,y,w,h [--settle N] [--out-dir D]`: runs the shot commands, reads the effect's `fx tree`, then mutes each distinct asset in turn (re-running the shot commands) and ranks them by how much the region changes against the unmuted reference (an unmuted repeat gives the noise floor, subtracted). Writes shots and `sheet.png` under `build/rig/<name>/fxbisect/`.
- `log <name> [-n 40] [--grep RE] [--raw]`: tail of `game.log`, dropping `MP frame|prompt |mpstream` lines (constant `NOISE`).
- `crash [<name>|<logfile>] [--binary PATH]`: symbolizes the last `port: crashed:` block with one `addr2line` call (`#N func file:line`); warns if the log's `port: build` differs from `<binary> --version`. For Android pass the matching `libmain.so` with `--binary`.
- `ls` (alive/dead, display, port, build, room, uptime; reaps Xvfb of dead runs), `stop <name>|--all` (sends `quit`, then SIGTERM/SIGKILL on the recorded pids only; reports a crash found in the log), `clean [--keep 5]` (deletes dead run dirs under `build/rig/` only).

## Recipes

**A/B a render toggle**

    $M start r --room <MLVL:MREA>
    $M ab r /tmp/bloom --a 'roomenv bloom off' --b 'roomenv bloom on'
    $M sheet /tmp/s.png /tmp/bloom-a.png /tmp/bloom-b.png --labels off,on

Read `-ab.png` and the diff line; `changed=0.00%` means the toggle did nothing in this view.
Move the camera first with `freecam pos|look` or `warp` for a view where it matters.

**Bisect a crash**

    $M start c --build old-build --env MP_CRASH_TEST=segv   # forced crash, to check the symbols
    $M start c --room <id> --gdb --replace                  # real backtraces land in game.log
    $M crash c

Run the same `--room` against two builds (`--build port-gcc` vs `--build wt-x`) and compare.
Symbols only resolve against the binary that crashed: check the build warning.

**Check a Remastered room**: `$M start r --room <MLVL:MREA>`, then
`$M cmd r roomgeo 'roomenv info'` and `$M shot r /tmp/n.png 'view normal'` (also `albedo`, `rough`,
`metal`, `ao`, `glow`; `view off` resets). `doctor` first: a stale `.import-version` means
the install needs a re-import. Use `--mods DIR` to test a mod build without touching the real one.

**Many rooms in one game**

    $M shots r /tmp/rooms "landing site" gully "main plaza" --cmds 'roomenv bloom off'
    $M sheet /tmp/rooms.png /tmp/rooms/*.png --cols 3
    $M view save r mine; $M shot r /tmp/m.png --at mine   # a saved viewpoint, in any run

**Two builds side by side**: start `a --build port-gcc` and `b --build <other>` with the same
`--room`, then `shot` both with the same commands and `diff` / `sheet` them.

## Shaders

From "this pixel looks wrong" to the material, its PBR record and the generated WGSL:

    $M start s --room <MLVL:MREA> --env MP_WGSL_DUMP=/tmp/wgsl      # or `shader dump <dir>` later
    $M pick s 640 360 --shot /tmp/pick.png    # draw at that pixel: owner, CMDL, material, record, shader hash
    $M cmd s 'view drawid' 'drawlog dump /tmp/draws.tsv'            # flat colour per draw; the last frame's draws
    $M cmd s 'roomgeo mats <cmdl>'            # that model's materials (any CMDL drawn since `drawlog on`)

`view drawid` draws every model surface as a flat 24-bit serial (R low byte, G middle, B high, black =
untagged); blending, fog and post passes are off in it. `pick` does that for one frame, reads the
pixel from a screenshot and puts the view back. The hash names `<hash>.wgsl` in the dump dir;
`index.tsv` lists each module's ShaderConfig (a `drawId=1` module is the drawid variant of a normal one,
so dump with the view off for the shader that really draws).

To iterate without a rebuild: copy the `.wgsl` to an override dir, edit the body (the bindings and
the pipeline layout must stay), then

    $M cmd s 'shader override /tmp/ovr' 'shader reload' 'wait 5'; $M shot s /tmp/o.png

(or start with `--env MP_WGSL_OVERRIDE=/tmp/ovr`). The log shows `wgsl override <hash>`; a file that
does not compile logs the error and the generated source is used. Pipelines rebuild as they are drawn,
so wait a few frames before the shot (a shot right after `shader reload` can miss draws; reload twice
if one is stale). Hashes change with the ShaderConfig, so an override dies when a setting that
changes the module is toggled. Clear it with `shader override off`.

### On an Android device

The rig doesn't run there, but the console does. The app reads the system properties
`debug.mport.env` and `debug.mport.env2` at startup as space-separated `K=V` pairs, so env
vars can be set without a rebuild:

    adb shell setprop debug.mport.env "'MP_CONSOLE=4777 MP_OPENGLES=0 MP_BOOT_WORLD=<MLVL>:<MREA>'"
    adb shell am force-stop org.metroidprime.port; adb shell monkey -p org.metroidprime.port 1
    adb forward tcp:4777 tcp:4777; python3 tools/mpcon.py --port 4777 'shader dump <dir>' ...

Paths for `shader dump` / `shader override` / `drawlog dump` go under
`/sdcard/Android/data/org.metroidprime.port/files`. `pick` doesn't work (it looks for a local
screenshot): use `drawlog` plus `adb exec-out screencap -p`. `MP_OPENGLES=0/1` forces the backend
for one run. `MP_DAWN_ENABLE` / `MP_DAWN_DISABLE` take comma-separated Dawn toggle names. `MP_PIPELINE_THREADS=<n>` sets
how many threads compile pipelines (default half the cores, at most 8; GL always 1). Clear the
properties (`setprop debug.mport.env ''`) when you're done.

## Particles

Console `fx` (also in `docs/NATIVE_PORT.md`) shows the live generators of the particle engine
(PART, SWHC, ELSC; each registers itself, always compiled in, no cost while unused):

    fx list [filter]       root generators: #id, kind, asset, pos, live/max particles, frame/life,
                           emitting|idle, finished, children, vfx[...] = native VFX props
                           (VMAT VTMT*n VPMT*n VSMT*n SSZE ITEN VORNn VMSH XFMDn IRND PMDV), or retail-draw
    fx tree <#id>          that generator and its children, recursively
    fx stats               live gens by kind, vfx quads/triangles/draws and CPU ms (update, render) last frame;
                           the first call starts the timers, so ms show from the next frame
    fx mute <asset>|clear|list, fx solo <asset|#id>   muted assets still update and live, but do not draw
                           (the mute is by asset, so every instance; `#id` expands to the whole tree)
    fx timescale <s>       particle time x s (0 freezes particles only; children are not scaled twice)
    fx <PART> [dist] [scale] [loop]   spawn in front of the game camera, prints `generator #id`

`fx list/tree/stats/mute/solo/timescale` run even while `hold 1`; spawning does not (a tick command).

**Spawn and filmstrip an effect**

    $M start r --room 83F6FF6F:D5CDB809 --mods <dir with the import>
    $M cmd r 'wait 300'                       # the first capture after boot shows the intro cutscene
    $M film r /tmp/fx.png --pre 'fx C0E95E90 5' --frames 0,4,8,16,32,60

The whole view moves a little (gun sway), so read the strip, not just the diffs. `fx ... loop` keeps a
short effect in view across a `step`-less run.

**Find which child draws the white quad**

    $M cmd r 'fx C0E95E90 5 loop' 'wait 4' 'fx list C0E95E90'     # -> #id
    $M cmd r 'fx tree #163'                                          # assets of the children
    $M cmd r 'fx mute 5B9BD0F4' 'shot'                               # one at a time, or let fxbisect do it
    $M fxbisect r C0E95E90 --shot-cmds 'fx C0E95E90 5;hold 1;step 8' --bad-region 560,330,160,160

`fxbisect` prints the ranking and `sheet.png` (reference + the five biggest). Use a tight region around the
quad: the shots are not pixel-stable (the noise floor is printed), so a culprit is only credible when it
clearly beats the floor and its mute shot shows the quad gone.

**Solo an effect in a room**

    $M cmd r 'fx list' 'fx solo #137' 'fx mute list'    # only that generator's assets draw
    $M shot r /tmp/solo.png ; $M cmd r 'fx mute clear'

## Converter reports

Every Remastered import writes `<mod>/reports/` (next to `.import-version`): sorted, tab-separated,
deterministic (no timestamps), so two imports diff cleanly.

- `materials.tsv`: one row per converted output material. Columns: `cmdl` (output model id),
  `mat` (its material index), `source` (Remastered model uuid), `srcmat`, `shader` (id8),
  `role` (the shader's lists, e.g. `inverse-exposure+gun-body`), `flags` (hex), `tag`
  (PBR4..7 / WRAP / TEV), `kind` (PBR shader class, 0 standard), `mode` (1 unlit, 2 glow mask,
  4 vertex tint, 8 colour-unlit), `path` (`pbr`|`tev`), `pathReason`, `kindReason`, `emissive`,
  `backlight`, `strength`, `p0`..`p3`, `cube` (reflection cube id).
- `effects.tsv`: one row per Remastered effect considered (a GENP standing for several PARTs has
  a row each). Columns: `genp`, `retail` (the PART it stands for, `-` if none), `result`
  (`imported`|`failed`|`unpaired`|`no-disc-part`), `method` (`carried-over`, the pairing section
  of `kMatchedEffects`: `name`, `room-placement`, `chpr-set`, `chpr-event`, `loose-events`,
  `script-slot`, `event-bones-per-character`, `event-frames-per-character`, `by-hand`; `none`
  when unpaired), `reason` (for a failure), `kinds` (files written), `dropped` (retail properties
  left out), `droppedList` (`FOURCC: why;...`), `approximatedList`.
- `summary.txt`: counts per tag, kind, path, reason and role, and per result, method and failure.
- `reports/parts/` holds each stage's own rows (models, roommodels, effects). They are what a
  reused stage links in, so a no-change re-import keeps the reports complete.

**Why is this material kind X?** `grep -P '^<cmdl>\t' reports/materials.tsv`, read `role`,
`kindReason`, `pathReason`. For one model offline: `remastered_effect_tool mat <romfs> <uuid|name>
[index]` (standalone conversion, so no TEV-path rows).

**Why is this effect unpaired / missing?** `grep <genp> reports/effects.tsv`: `unpaired`/`none`
means no `kMatchedEffects` row and no carried-over id; `failed` has the reason (`parse:`, `no
texture:`...). `remastered_effect_tool explain <romfs> <retailDir> <GENP id>` runs just that
effect and prints the pairing, dropped and approximated lists. `pdump` shows a retail PART's
properties, `pdiff a b` only those that differ (e.g. the disc's against the converted one).

**What changed between two imports?** `diff -u old/reports/materials.tsv new/reports/materials.tsv`
(likewise `effects.tsv`, `summary.txt`).

## GPU self-test

For "the world is black on this GPU" reports (issues #7, #8): renders 10 known patterns into 32x32 offscreen
targets through the same GX -> generated WGSL -> pipeline -> bind-group path the game uses, reads them back and
compares them (RGB, +-2 per channel). Cases: direct, 8-bit and 16-bit indexed vertices, konst/register TEV
colour, a 3-stage TEV combine, RGBA8 / IA8 / I8 / CMPR textures, alpha blend and alpha-compare, depth in both
draw orders, EFB copy sampled back.
A silent warm-up pass runs first so the async pipeline compiles finish, then the logged pass (a few frames later).

    F1 > Video > Compatibility > "GPU self-test"      # or: console `gpuselftest`, or MP_GPU_SELFTEST=1 (once, ~120 frames in)
    grep 'gpu selftest' game.log

    gpu selftest: backend Vulkan, adapter "...", clamped storage loads active|off
    gpu selftest: tev-3-stage: PASS (0.2 ms)
    gpu selftest: indexed16-pos: FAIL (pixel 4,4 got 000000FF want 00FF00FF, 144/1024 pixels wrong) (0.3 ms)
    gpu selftest: 9/10 passed (35 ms)

A FAIL names the feature. Results land a frame or two after the request. Game rendering is unaffected: the
run uses its own targets and the game's GX state is reset afterwards. Code: `extern/aurora/lib/gfx/selftest.cpp`.

## Limits

- Boot is `MP_BOOT_WORLD` only (works on any build); `MP_SMOKE_*` needs `build/smoke-gcc`
  (`--build smoke-gcc --env MP_SMOKE_WORLD=...`).
- `ab` freezes ticks, so animated effects stay put; use `--no-hold` to compare live frames
  (expect motion noise in `diff`).
- `warp` through `cmd`/`shot`/`shots` waits for settled, but a room's streamed models and textures can still be
  arriving; add `wait N` for those. `warp` to another area of the same world works (`shots` does it).
  Measured (2026-10-05, Xvfb): start in Landing Site ~7-8 s; per `shots` room ~2 s same world or a cached world, ~10 s for a Frigate room.
- `fade` covers only the in-game fade-in filter. The water multiply filter and script fades are not counted.

## See also

`tools/mpcon.py` (the plain console client, also usable against a real game),
`docs/NATIVE_PORT.md` (full console command list, env variables),
`build/mpr/re.sh` + `build/mpr/TOOLS.md` (local Remastered reverse-engineering toolkit).
- `fx` mute hides draws only; muted generators keep emitting and updating (so their children still count in
  `fx stats` particles). Owner objects are not shown, and CRSC (crossfade/swoosh descriptions that are not
  generators) are not listed. Roots are the generators nobody else lists as a child.
