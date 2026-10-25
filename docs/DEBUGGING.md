# Debugging the port headless: `tools/mprig.py`

One CLI replaces the per-task shell rigs: it starts its own Xvfb and game with the debug
console, drives it, screenshots, diffs and tears down. Parallel sessions never collide
(displays from `:200`, console ports from 5100, allocated under a lock). Every command exits
non-zero with a one-line `mprig: <reason>` on stderr when it fails. State is in `build/rig/`
(git-ignored): `build/rig/<name>/{run.json,game.log,user/,shots/}`.

The Xvfb/RADV environment (`MESA_VK_WSI_DEBUG=sw`, `SDL_VIDEODRIVER=x11`, dummy audio,
`MP_FAST_BOOT`, `MP_CONSOLE`, ...) is set automatically, with `AURORA_PRESENT_EVERY=6`: Aurora renders
every frame but presents one in 6, since an Xvfb present is a CPU copy (`shot` reads the EFB, so it is
unaffected; `--env AURORA_PRESENT_EVERY=1` presents them all). That only pays off uncapped: `start --turbo [ticks]`
(`MP_TURBO`, default 8 ticks per drawn frame, no frame cap) for runs that wait on game time. **Never capture on `:0`**: GPU-heavy
runs on the real display have taken the desktop session down. The one exception is
`start --host-display`, for the OpenGL backends only (Xvfb's llvmpipe can't run Dawn's GL): it
skips Xvfb and opens a window on the user's `$DISPLAY` with the real driver (it picks up the
newest Xwayland auth file under GNOME). Use it only with the user's OK, one run at a time.
To test the GL 3.3 path on a Mesa driver, add `--env MP_GRAPHICS_BACKEND=opengl --env
MESA_GL_VERSION_OVERRIDE=3.3 --env MESA_GLSL_VERSION_OVERRIDE=330 --env
"MESA_EXTENSION_OVERRIDE=+GL_ARB_texture_storage -GL_ARB_compute_shader
-GL_ARB_shader_storage_buffer_object -GL_ARB_shader_image_load_store"` (Mesa hides
texture storage at 3.3, which real 3.3 drivers expose); game.log then reports texture vertex
fetch and no compute shaders.

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
    $M stop --all t                          # always finish with this: your runs named t*

`start --room` returns in about 7-8 s, once the player is in control: cutscenes are skipped
(even unseen ones) and the fade-in after them has run. Pass `--cutscenes` to see them. Use your own run names so sessions stay apart.
`--room` takes a room name too (`--room "landing site"`; `rooms [filter]` lists them).

Runs start in god mode (`MP_GODMODE=1`): the player takes no damage, so a run doesn't die to an
enemy or lava while you read a screenshot. Pass `--mortal` only to test damage or death. (`hurt` bypasses god mode: it hits the player
without taking energy either way.)

Getting somewhere fast (every move is one `cmd` call with many commands, never a walk):

- Never walk with `press`/`stick` to reach a place: those are for testing movement or input.
  Boot into the room (`--room "<name>"`), then `goto <id>` (stands on the floor 6 units from an
  actor, facing it; use the hex editor id from `objs [filter]`, which stays the same across loads,
  unlike `u<uid>`; a `goto` onto a pickup can collect it and pause the game until `press a 1`), `tp x y z` + `face <yaw>`, `look <id>`, or a saved view
  (`--at VIEW`, `view save`). `room <area>` jumps to another area of the world.
- Clear what is in the way rather than playing through it: `kill <id>` (bosses too), `send <id> <msg>`
  (open a door, start a trigger), `give <item> [n]` (missing beam, visor, Morph Ball, missiles).
- Put a whole step in one call: `$M cmd t1 'goto 00080072' 'visor thermal' 'wait 30' status`, or
  `shot` with the commands; several rooms are one `shots` call. Read `status` before a screenshot:
  it is text and costs no image.

**Settled** = `status --json` says first person, no cinematic and `fade` false (the in-game fade-in
filter, `CInGameGuiManager::StartFadeIn`, is done), on two polls at least 12 frames apart. `start --room`,
a `warp` sent through `cmd`/`shot`, and `shot` wait for it (not on `--cutscenes` runs).

## Subcommands

- `start <name> [--build B] [--room MLVL[:MREA]|NAME] [--at VIEW] [--env K=V]... [--mods DIR|none] [--settings FILE|none] [--saves] [--mortal] [--size WxH] [--wait S] [--cutscenes] [--gdb] [--replace]`:
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
- `ls` (alive/dead, display, port, build, room, uptime; reaps Xvfb of dead runs), `stop <name>|--all PREFIX|--everyone` (`--all` stops only live runs whose name starts with PREFIX, so give your runs a topic prefix; `--everyone` also stops other sessions' runs; sends `quit`, then SIGTERM/SIGKILL on the recorded pids only; reports a crash found in the log), `clean [--keep 5]` (deletes dead run dirs under `build/rig/` only).

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

## Save-state round-trip check

`tools/statecheck.py` finds save-state gaps (a boss resuming in the wrong phase, a timer or door that
comes back different) without a player: it dumps every live object (`statedump <file>` in the console,
JSON lines from `platform/port_statedump.cpp`), saves to slot 8, loads it, dumps again and diffs.

    S="python3 tools/statecheck.py --build port-gcc"          # --build etc. go to `mprig start`
    $S --room "artifact temple" --setup 'send 001003D3 SetToZero' --setup 'wait 900'
    $S --run mine --setup 'kill 00100101 1500' --setup 'wait 300'   # a run already in the room
    $S --rooms scenarios.txt --out /tmp/sc                          # one game, many scenarios

A scenario file has one line per scenario, `ROOM [| setup ; setup]` (`#` comments). The game is held
for the round trip so the order is exact: dump A, `state save` (taken at the start of the next tick),
`step 1`, dump A1, `state load`, `step 1` until `[savestate] room state:` is logged, dump B (when that line ends in `neighbour areas still streaming`, B0 is dumped then and B waits for `room state, neighbour areas:`: the areas resident at the save stream back in and take their saved state; what B0 had but B lacks died in that wait, a `gone-after-restore` warning, e.g. debris near the end of its life). Generated copies the save leaves to their generators (sounds, maze walls) are warnings when missing.
Dumps are kept under `--out` (`NN-<room>-{A,A1,B0,B,C}.jsonl`; B0 and C only when taken). Add `--turbo 8` for long setup waits if
your `mprig` has it.

Reading the report: first a per-class table (`!` strict, `~` warning, kind and count), then
details per class. **Strict** fields are `active`, `hp`, `kbres`, `door_open`, `timer`/`timing`,
`counter`, `boss` (hex block, with the differing byte offsets), `ai` (state name), `body`, `loco`, the
player's `morph` and `hp`, and missing/extra objects that have an editor id. **Warnings** are
position/velocity/forward beyond a tolerance (`--tol-pos`, `--tol-vel`), animation, AI state time and
missing/extra objects with no editor id (effects and projectiles are not saved by design). A field that
differs from A but equals A1 (the world one tick later) is "tick drift" and only counted. Exit 0 with no
strict diff, 1 otherwise, 2 if the harness failed. A last section, "schema fields", diffs each object's
`fields` (the bool/int/float/bitfield/math/TUniqueId members of its class chain, from the generated
schema in `docs/SAVESTATE_SCHEMA.md`, that its `kTagFields` block saves; references as `eid:` editor
ids), all strict, grouped by class.field; `--tol-field` is the float tolerance. The members no block
saves (other classes' fields, CActor render state; dumped as `fields_other`) follow as one uncounted
"unsaved schema fields" line, each diff with `--other-fields`: a gap finder, not a failure. Port-added
members (`xPort*`, `xpc_*`) are skipped. `--post-wait N` also dumps C N ticks after B and prints C against A as a second,
uncounted report, for a restore that hands over late (Metroid Prime's exo comes back inactive in B).
The cinematic cameras, waypoints and sounds always show
position warnings: they are moved by scripts the restore does not replay; ignore them.

Enemies (every `CPatterned`) and generated pickups save their schema fields in the room record's `kTagFields` block
(`platform/port_savestate_fields.cpp`; the load log's "N enemy fields set"). For a baseline without it,
start the game with `--start-arg=--env --start-arg=MP_SAVESTATE_FIELDS=0`. Expected leftovers in the
"schema fields" section: `x254_` and other members the constructor never sets, `xPort*`, render and
physics bookkeeping (`x9c_renderBounds`, `xc8_drawnToken`, `xfc_constantForce`), and what the AI state
machine, body controller and animation change on the first tick (no codec for them yet).

## Crash soak

`tools/mpsoak.py` is the pre-release smoke: it drives `mprig` through the shipping matrix -- Remastered
and retail (`--mods none`), Dawn validation and robustness on (`--env
MP_DAWN_DISABLE=skip_validation,disable_robustness`, which makes any WebGPU error fatal in game) and
the release toggles -- at MSAA 1 and 4, switched inside the same game with the console's `msaa N`.
Every run also passes `MP_GX_CHECK_ARRAYS=1`: aurora then walks each indexed draw's vertex indices and
drops a draw that would read past one of its arrays (the bind pose read through TEX7 included, the
807013b1e Vulkan crash), logging `gx: vertex array overrun:` once per array. A texture coordinate no
texgen reads is not checked: retail's packed-lightmap draws index TEX0 past its array that way, which
nothing reads. It costs about 1 ns per
vertex per indexed attribute, so it is off in release builds unless set (`0` turns it off), and on in
builds without `NDEBUG`.

    python3 tools/mpsoak.py --evidence-dir build/s-release-<date>/soak
    python3 tools/mpsoak.py --dry-run --evidence-dir /tmp/soak-dry    # the commands only, no game

One game per (mode, toggles), `soak-<mode>-<toggles>`, started with `--turbo`. Each room is one
`mprig shot` that warps into it (`shots` starts and stops a game of its own, so it cannot share one
process across two MSAA values). Screenshots land in `<evidence>/<mode>-<toggles>-msaa<N>/<NN>-<room>.png`;
`summary.tsv` has a row per (mode, toggles, msaa, room) with `ok`, `warn` or `FAIL` and the first
matching log line, and the exit status is 1 when any row FAILs.

A room FAILs on `port: crashed:`, `WebGPU error`, `Device lost`, `gx: vertex array overrun:`, an aurora
`[fatal]` line (desktop aurora logs `fmt`'s `[level] [module] message`, so `Log.fatal`/`FATAL` starts
`[fatal]`) and on the game process going away; `not supported; skipped` lines are `warn`. After a death
it writes `mprig crash <run>` to `<evidence>/<mode>-<toggles>-msaa<N>-<room>-crash.txt`, restarts the run
(`--replace`) and carries on with the next room. It stops its own runs by exact name at exit (`--keep`
leaves them up).

`--rooms` overrides the tour (comma-separated names or `MLVL:MREA`; a name that does not resolve or that
matches two rooms lists the candidates; `room@<editor id>` also turns to face that object with `look`
before the shot), `--modes`/`--toggles`/`--msaa` narrow the matrix, and `--build` / `--disc` go to
`mprig start`. The default tour is 12 rooms across all seven worlds: Exterior Docking Hangar, Tallon
Canyon, Root Cave, both ends of the Tallon Overworld <-> Magmoor elevator, Phendrana Shorelines, Main
Plaza, Main Quarry, Sunchamber, Hive Totem, Crater Entry Point, and the Frigate's Reactor Core. Tallon
Canyon and the Magmoor elevator room face their water and lava: a fog volume only copies depth while it
is on screen, and a spawn view that misses it hid issue #200 (an `@id` tour entry reproduces it on the
build before the fix). Keep rooms whose warp lands on a save station out of the tour: the save prompt
pauses the game and every later warp fails as "not ticking" (Landing Site does this). The full matrix
takes a few minutes: the warps are the cost (~2 s in a cached world, ~10 s into a Frigate room).

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
for one run. `MP_DAWN_ENABLE` / `MP_DAWN_DISABLE` take comma-separated Dawn toggle names; they apply after the
F1 > Video > Compatibility > Driver workarounds checkboxes, and either can turn off one of Aurora's defaults
(`skip_validation`). `MP_PIPELINE_THREADS=<n>` sets
how many threads compile pipelines (default half the cores, at most 8; GL always 1). Clear the
properties (`setprop debug.mport.env ''`) when you're done.

## Particles

Console `fx` (also in `docs/CONSOLE.md`) shows the live generators of the particle engine
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

The port-only material versions and override targets are documented in [REMASTERED_VFX.md](REMASTERED_VFX.md).

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

For "the world is black on this GPU" reports (issues #7, #8): renders known patterns into 32x32 offscreen
targets through the same GX -> generated WGSL -> pipeline -> bind-group path the game uses, reads them back and
compares them (RGB, +-2 per channel; alpha is shown, not compared). Cases: direct, 8-bit and 16-bit indexed
vertices, konst/register TEV colour, a 3-stage TEV combine, RGBA8 / IA8 / I8 / CMPR textures, alpha blend and
alpha-compare, depth in both draw orders, EFB copy sampled back.

Every case goes draw -> EFB copy -> readback, so two more checks leave stages out (issue #143):
`readback` copies a texture written from the CPU straight back (no GX), and `efb-clear-copy` clears the EFB
with a copy and copies it again (no draw). The readback buffer starts filled with `A5` bytes, so a block no copy
reached says "nothing was copied back" instead of reading as black. When the failures point at one stage, a
`diagnosis:` line names it: readback broken (the other results mean nothing), EFB copies broken, or draws not
reaching the framebuffer. When everything passes, the diagnosis says so and what that leaves: the cases draw
offscreen and never present, so a fault the player still sees is in presenting the frame (swapchain, present
mode) or in drawing the cases don't cover. Tell the reporter that rather than "your GPU is fine".

The count includes `readback` and `efb-clear-copy`, so it reads `x/12`; reports from before these checks
(issue #143) read `x/10` with the same ten draw cases.
A silent warm-up pass runs first so the async pipeline compiles finish, then the logged pass (a few frames later).
Each case's own pipeline is waited for (a blocking lookup), so a draw is never skipped because its pipeline is
still compiling: a FAIL means the draw happened and the device rendered it wrong. The wait can delay the call
by one compile of a fresh case's pipeline (~300 ms) however busy the compile queue is otherwise.

    F1 > Video > Compatibility > "GPU self-test"      # or: console `gpuselftest`, or MP_GPU_SELFTEST=1 (once, ~120 frames in)
    grep 'gpu selftest' game.log

    gpu selftest: backend Vulkan, adapter "...", clamped storage loads active|off
    gpu selftest: tev-3-stage: PASS (0.2 ms)
    gpu selftest: readback: PASS (0.0 ms)
    gpu selftest: indexed16-pos: FAIL (pixel 4,4 got 000000 alpha FF want 00FF00, 144/1024 pixels wrong) (0.3 ms)
    gpu selftest: 11/12 passed (35 ms)

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
`docs/CONSOLE.md` (full console command list) and `docs/SETTINGS.md` (env variables),
`build/mpr/re.sh` + `build/mpr/TOOLS.md` (local Remastered reverse-engineering toolkit).
- `fx` mute hides draws only; muted generators keep emitting and updating (so their children still count in
  `fx stats` particles). Owner objects are not shown, and CRSC (crossfade/swoosh descriptions that are not
  generators) are not listed. Roots are the generators nobody else lists as a child.

## Disc image check

F1 > System > Disc image > "Check disc image" (console: `disc verify`, then `disc verify status`)
reads the whole mounted image on a low-priority thread through its own nod handle (ISO, RVZ, CISO, GCZ;
the game's reads are untouched) and compares each 1 MiB with the CRC32s of the Redump image of the
revision named by the disc header (`platform/port_disc_verify_table.inc`, generated by
`tools/disc_chunk_table.py` from the four Redump ISOs), plus the whole-image SHA-1. A mismatch is
reported with the damaged MiB count, how many are all zeros, and the files from the disc's FST that
overlap them (e.g. issue #176: zeros inside Metroid4.pak). The result is logged as
`disc check: ...`. A full check takes about 10 s (cached) to 25 s (cold ISO) on a desktop.

The same section shows the image path and a "Replace disc image..." button (always; the advice and the button lead when the check finds a damaged or unknown image; console: `disc replace <path>`). It asks for a file without blocking, refuses anything that is not a supported Metroid Prime disc, and remembers the new image for the next start (log: `disc image replaced: <path> (<revision>)`); the mounted disc is never swapped. On Android the picked file is copied to `disc.iso.part` on a background thread with a progress bar and renamed over `disc.iso` only when complete; Cancel keeps the old image. Disabled while a check or copy runs.
