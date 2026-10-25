# Debug console

`MP_CONSOLE=<port>` (any build, `1` = 4777, POSIX only): a debug
command console on 127.0.0.1 (a port above 65535 refuses to start; a client
whose first line is an HTTP request, as a web page's would be, is
disconnected unread). Its `press`/`stick` input is read even when
the window has no keyboard focus. `tools/mpcon.py` is the client: one-shot
(`tools/mpcon.py 'warp chozo 492CBF4A' 'objs eyeball' shot`), a script
(`-f file`) or an interactive prompt with no arguments. Commands: `status [--json]`
(world, area, position, whether the camera is first person or a cinematic
has it, the sky, and the probe mode, weight and PBR draw count; `--json` is one
line for scripts: frame, game_state, world, area, mrea, pos, yaw, hp, hp_max,
morph, visor, beam, first_person, cinematic, freecam, `physics` (see below) and
`fade`, true while the in-game fade-in filter after a cinematic skip or world
load is still black or lifting, i.e. the screen is not fully visible;
`tools/mprig.py` waits on it),
`pipetrim [frames]` (releases built pipelines unused for that many frames, 16 minimum, as Android's onTrimMemory does; the log says how many),
`worlds`, `areas`, `warp <world id or name prefix> [mrea]` (replies once the
new world runs), `tp x y z`, `goto <id> [dist]` (stands on the floor dist units, default 6, from an actor, facing it: the side toward the player first, then around it and closer in, preferring a clear line of sight; says when it has none), `enter <area>` (makes an area of the current
world the current one, as walking into it would; `tp` alone does not),
`room <area>` (the F1 room list's teleport, to the area's spawn point),
`lift <id> <dz>` (moves an actor by dz up, or down when negative),
`fx <PART id> [dist] [scale] [loop]` (plays one particle effect `dist` units in
front of the camera, replacing the last one, and prints its generator `#id`; `loop` respawns it when it
finishes; `fx off` removes it; `fx list [filter]`, `fx tree <#id>`, `fx stats`, `fx addons` (the current area's Remastered effect actors from a mod's `.roomfx`: editor id, unique id, PART, live state, plus how many retail effects they replaced; `MP_REMASTERED_EFFECT_ACTORS=0` turns them off), `fx mute <asset>|clear|list`,
`fx solo <asset|#id>`, `fx timescale <s>` inspect and bisect live generators, see `docs/DEBUGGING.md` "Particles"),
`face <yaw>`, `look <id>`, `objs [filter]`,
`obj <id>` (AI state, health, a door's open or closed, body state and animation, connections, whether
it is frustum-culled),
`send <id> <msg>`, `give <item> [n]`, `take <item> [n]`, `fusion [0|1]` (wear the Fusion Suit, no unlock needed), `items`, `heal`, `kill <id> [damage]` (deals an actor its remaining health, or the given damage, as Power
Beam damage, so bosses run their death scripts), `phazongun [off]` (lets the arm cannon fire the Phazon Beam, or stops it), `god [on|off]`,
`hurt [n]` (hits the player for n, default 10, with the HUD flash and damage sound), `die` (empties the health),
`hint show` (shows the hint the game would show next, as F1 > Game > Show current hint), `memo <text>` (shows the text as a HUD message), `strg <id> [index]` (a string table, or one string of it, as the game loads it, mods included),
`language [en|<code>]` (the text language, switched while the game runs; no argument prints it),
`visor [combat|xray|scan|thermal|0-3]` and `beam [power|ice|wave|plasma|0-3]` (switch as the touch wheels do, so the game's own rules apply: owned items only, not in the ball; no argument prints the current one), `press <a+b> [frames]`
(`sx:<n>`, `sy:<n>`, `cx:<n>`, `cy:<n>` tokens hold stick axes along with
the buttons, e.g. `press x+sy:127 30`; `lt:<n>`, `rt:<n>` hold a trigger's
analog travel, 0..255, without its click),
`stick`/`cstick <x> <y> [frames]` (frames `0` on `press`/`stick`/`cstick`
keeps holding until the next one), `gyro <pitch> [yaw] [frames]` (stand-in
gyro rates in rad/s), `shot` (prints the bmp path), `present
<0..1|cycle|tick|off>`, `hold <0|1>` (stop ticking), `step [ticks]` (run
that many ticks while held), `interp [actor|pose|particle|all <0|1>]`,
`aspect <4:3|16:9|window>`, `original [on|off]`, `keypreset classic|mouse` (applies and saves a keyboard preset, as the Controls page's buttons), `padpreset gamecube|remastered|modern|southpaw` (the same for pad 1's controller presets), `fov <45..120>`, `gunoffset <r> <f> <u>` (arm cannon offset in metres, ±1 each), `pointeraim [on|off|<x> <y>]` (pointer aim mode, or places its pointer in -1..1 of the half view, right/down positive; prints the mode, whether the pointer steers this tick and where it is), `pointer <x> <y> move|click|dclick|tap|dtap|down|up [l|r|touch]|drag <x1> <y1> [l|r|touch]|wheel <n>|pinch <ratio> | back | where | hits` (scripted mouse or touch input for pointer menus and the map, in GUI pixels as screenshots show them; `where` prints the pointer, its last device and whether a pointer screen is active, and `hits` lists the screen's click targets), `window [<w> <h>]` (resizes the window, leaving fullscreen, e.g. for square screenshots; prints the size), `msaa <1|4>`, `aniso <1..16>`, `pixellight [0|1]` (per-pixel lighting; no argument prints it), `sunshadow [0|1]` (the port's sun shadow; no argument prints it), `projshadow [0|1]` (Remastered projected actor shadows; no argument prints it), `hudscale <50..100>`, `scanspread <0..100>`, `helmet <0|1>`, `visorfx <0|1>`, `dreadcannon [0|1]` (the Dread arm cannon in first person, `docs/DREAD_SAMUS.md`; not saved; no argument prints it), `crosshair <25..100>|style classic|remastered`, `hudopacity <xray|thermal|scan|energy> <visor|0..100>` (that visor's HUD opacity, or `visor` to follow Visor Opacity; `energy` is the energy readout, see `docs/SETTINGS.md`), `reveal <0|1>`, `pickups <off|seen|rooms|all>` (map pickup marking; 0 and 1 mean off and all), `pickups seen [clear|all]` (selects Seen and prints how many pickups this save has seen and their dot indices in the current world; `clear` forgets them, `all` marks every one), `pickups kinds [on|off]` (major/minor shapes; no argument lists the current world's dots as index plus `M` major, `m` minor, `?` unknown), `pickups station <on|off>` (in Seen, map stations show their rooms' pickups), `pickups collect <dot>` (test aid: posts that dot's memory relay to its world's mailbox, so the map treats the pickup as collected), `stackbeams [0|1]` (the stack_beams setting, see `docs/SETTINGS.md`; no argument prints it), `walkspeed [50..200]` (the walk_speed setting in percent, see `docs/SETTINGS.md`; no argument prints it), `mapui [0|1]` (the map's extra legend entries and the Mark room prompt, see `docs/SETTINGS.md` "Extra map UI"; no argument prints it), `beamdoors [status|on|off|list [here]|unlock <door index|all>|reset]` (the beam doorways opened for good, see `docs/SETTINGS.md` "Unlock beam doors once opened": `on` and `off` set the setting, and the loaded doors follow on the next frame; `list` prints each doorway's number, whether it is open and its doors' table indices, `here` only the current area's; `unlock` opens one by a door index from the list, or every one, and restyles the loaded doors now; `reset` locks them all again (loaded doors keep their blue look; rooms loaded afterwards have their own colours); refused outside a game), `mark [list|clear|toggle <area>]` (the map's room marks, see `docs/SETTINGS.md`: lists them as world:area, `toggle` flips an area of the current world), `tracker`, `state list | last | save [n] | load [n] | undo | slot <n>` (a state keeps the rooms' enemies and doors while `savestate_room_state` is on), `statedump <file>` (writes every live object's state as JSON lines: class, editor id, active, hp, position, velocity, door, timer/counter, AI and body state, boss block, and under `fields` every plain-data member the generated entity schema lists for its class, `docs/SAVESTATE_SCHEMA.md`; runs between ticks, also under `hold 1`; a debugging aid for `tools/statecheck.py`, see `docs/DEBUGGING.md`), `resume [status|save|load|clear]` (quick resume's `savestates/resume.mpss`: `status` names it, `save` writes it now even with `quick_resume` off (the current moment when safe, else the last safe snapshot, nothing before the first one), `quit` writes it like a window close, `load` loads it into the running game of the same card and seed, `clear` deletes it), `viewmodel <cmdl> [dist] [yaw] [pitch] | off | status | light <0|1>` (draws any model, retail or a mod's, in front of the camera with the arm cannon hidden; dist 0 fits its bounds; `light 1` swaps the flat white ambient for a key light, which PBR mod materials need to shade), `probe [off|on|mirror|window]` (the PBR reflection probe, live: `mirror` and `window` show the probe itself on PBR materials, as a reflection and looked straight through; no argument prints the mode), `remastered [start <image.nsp> [key file] | dread <dread.nsp> [key file] | cancel]` (the Remastered model import, or the Dread Samus import into the `dread-samus` mod (`-` for the image with `MP_DREAD_ROMFS` set; see `docs/DREAD_SAMUS.md`), and its progress), `mods [reload]` (what is loaded; `reload` reads the mods folder again), `roomgeo [on|off|overlay | at <x> <y> <z> [margin] | hide <cmdl> | show [cmdl]]` (a mod's room geometry: in place of the retail area, off, or drawn over it; `at` lists the instances whose box holds a point and `hide` stops drawing a model, for finding which one a surface belongs to; no argument prints what is loaded and drawn), `roomgeo lights on|off` (light room geometry with the area's lights even where the room has baked light), `roomgeo script` (Remastered's own visibility scripts in each loaded area: the camera in area space, each camera zone, counter and relay, and how much of each geometry group is shown), `roomgeo group <n> show|hide` (sets that group in every loaded area until its script next changes it), `roomgeo minpx <n>` (skip room geometry instances smaller than n pixels; see `MP_ROOM_GEO_MIN_PX`), `roomgeo lod <scale>` (see `MP_ROOM_GEO_LOD`), `roomgeo pick` (the instances the middle of the view looks through, nearest first, with each model's materials), `roomgeo mats <cmdl>` (a model's materials: flags, PBR or TEV, the PBR record; any CMDL drawn since `drawlog on` or `view drawid`, not just room geometry), `roomgeo mat <cmdl> <material> <field> <value...> | mat clear` (changes a value of a material's PBR record as drawn, until cleared or the next start; fields `emissive`, `backlight`, `height`, `mode`, `kind`, `strength`, `p0`-`p3`, or an index 0 to 18; emissive multiplies the emissive map, so it shows only on a material that has one), `roomliquid [on|off]` (a mod's liquid surfaces in place of the retail fluid planes; no argument prints what is loaded and drawn), `collision [off|overlay|only]` (draws what Samus collides with: the areas' static collision shaded by facing, walls grey, floors blue, ceilings red, lava orange, phazon cyan, grates yellow, with each triangle's edges, and active solid actors such as gates and platforms as orange boxes; `only` hides the world but Samus, so walls with no surface on them show), `colldump <x0> <y0> <z0> <x1> <y1> <z1> <file.obj>` (the current area's collision triangles touching a box, as an OBJ with each face's material bits in a comment), `roomenv [on|off|bloom on|off|grade on|off|volume on|off|ambient <scale>|show off|coords|light|info [<x> <y> <z>]|balllight on|off|<scale>]` (room environments: `volume` is the baked light per pixel, `ambient` scales the baked ambient, `show` draws the grid's coordinates or light in place of the surface, `info` prints exposure, tone curve, probe and baked ambient at the view or a point), `view [off|albedo|normal|rough|metal|ao|ambient|reflection|glow|exposure|kind|sun|drawid]` (what PBR surfaces show: one input of the shading in place of the result), `drawlog [on|off|dump <file>]` (numbers every model surface drawn and records it; `dump` writes the last frame as TSV: serial, CMDL, material, owner, record, PBR or TEV, shader hash), `pick <x> <y>` (the draw at a window pixel, top-left origin, by way of `view drawid`), `shader dump <dir>|override <dir>|off|reload` (the generated WGSL as `<hash>.wgsl` plus `index.tsv`, also `MP_WGSL_DUMP`; compile edited copies in place of the generated ones, also `MP_WGSL_OVERRIDE`; see `docs/DEBUGGING.md` "Shaders"), `gpuselftest` (GPU self-test: known patterns rendered offscreen through the game's GX path, read back and logged as `gpu selftest: <case>: PASS|FAIL`; also F1 > Video > Compatibility "GPU self-test" and `MP_GPU_SELFTEST=1` once after the first frames; see `docs/DEBUGGING.md` "GPU self-test"), `gputimes on|off|show` (per-render-pass GPU times from timestamp queries, 60-frame averages: ms and passes per frame for each pass name, plus the total and first-begin-to-last-end span; also F1 > Debug > Remastered "GPU pass times"; free while off), `stats` (the last frame's draws and buffers, texture, render target and Remastered lighting totals, Dawn's own GPU memory estimate and allocator totals (sampled once a second on the render thread), the heap, room geometry and environments), `stats textures [n]` (the n, default 25, most numerous live texture groups: count, size, mips, format, MiB and creation label, then each direct `CreateTexture` call site outside `TextureRef` with how many textures and MiB it has made so far, which is cumulative, not live; for finding what piles up; Dawn's figures are sampled by the render thread at most once a second, so they lag a moment), `shadow [casters on|off]` (the sun's shadow map last frame: light direction and colour, cascade range, caster draws and how many of them are out-of-view room models, and the room's suns; `casters` switches those room casters), `hdfont [on|off]`, `touchpad [attach|detach|stick <x> <y>]` (a virtual gamepad of the kind Android's touch overlay uses, to test controller hotplug against it on any platform), `minimap` (the minimap's screen rect as 0..1 fractions of the window, top-left origin, or `invalid` when it is not drawn: morph ball, map screen, hidden HUD), `maptap` (queues one Z press, as a tap on the minimap does on Android), `touchedit` (opens the touch layout editor, as F1 > Controls > Edit layout; touch builds), `finger down|move|up <id> <x> <y> | tap <x> <y> | drag <x0> <y0> <x1> <y1> [steps]` (SDL touch events at window pixels, with the touch-mouse SDL makes from a real touch; drives the drawn touch overlay of iOS or a `-DMP_TOUCH_OVERLAY_DESKTOP=ON` desktop build, capture it with x11grab since `shot` reads the EFB), `touchaim <dx> <dy> [hold s]` (touch aim travel in dp, right/down positive; the finger counts as down for hold seconds, which holds the GameCube scheme's free-look pitch), `mappan <dx> <dy> [hold s]` (drags the open map screen by dx,dy dp, as a finger would), `mapzoom <ratio>` (pinch zoom of the open map screen; above 1 zooms in), `maprotate <degrees>` (twist of the open map screen's yaw; positive is clockwise), `freecam [on|off|freeze on|off|player on|off|speed <n>|pos <x> <y> <z>|look <yaw> <pitch>]` (see below), `thirdperson [on|off] [distance <m>] [shoulder <m>] [height <m>]` (the opt-in third-person gameplay camera, setting `third_person`: behind Samus's right shoulder while unmorphed in first-person play, her body shown, the arm cannon and helmet hidden; distance 0.5..10 (3), shoulder -2..2 (0.6), height -1..2 (-0.6) in metres; prints enabled/active, the parameters and the camera's position), `photo [on|off|capture [1|2|4]|fov <20..120>|roll <-180..180>|samus on|off|panel on|off|super 1|2|4|speed <n>]` (photo mode, see `docs/SETTINGS.md`: `on` is refused with the reason outside gameplay, while Samus is dead (it works during cutscenes); no argument prints the state; `capture` saves a PNG, at 1x, 2x or 4x the render size, with the panel hidden), `timer <0|1>`, `igt <seconds>`, `livesplit <0|1> | addr <host:port> | send <command> | status`, `disc replace <path>` (checks that the file is a supported Metroid Prime disc image, plain or RVZ/WIA/WBFS/CISO/GCZ, refuses a wrong game in the words of the boot check, and otherwise remembers it as the disc for the next start; the mounted disc is not swapped; also F1 > System > Disc image > "Replace disc image..."), `disc verify [status|cancel]` (checks the whole disc image against the Redump record of its revision, a MiB at a time on its own thread; `status` prints progress while it runs, then the result and the damaged files; also F1 > System > Disc image),
`discord <0|1> | status`, `gci list | share on|off | emulator <folder|auto> | import <path> | export [usa|pal] <dir or .raw> | export trilogy <save.bin> | dolphin import | dolphin export [usa|pal|trilogy]`, `ap [connect <server> <slot> [password] | disconnect | recent | resume <n> | say <text> | chat]`, `rando [gen [seedtext] | play <name> | delete <name> | list]` (the built-in randomizer: `gen` makes a seed from the F1 Randomizer page's saved options and plays it, or only saves it while a game is loaded, an empty seed text picks one at random; `play` replays a saved seed, from the title screen or file select once a game is loaded; `delete` removes a seed with its progress and save card, not the one being played; `list` names the saved seeds), `wait <frames>`, `quit`, `title` (quits the game to the title screen, through the attract sequence as after a game over); `help` lists them. Ids are hex editor ids, `u<n>`
unique ids or exact debug names. Every reply ends with `=> ok` or
`=> err: <why>`, and the client exits 1 if any command failed. Game commands
run inside the state manager tick, so they fail with "not ticking" on the
title screen or while paused. Pair with `MP_TURBO` for speed.

## `status --json` `physics`

`physics` is telemetry added for the movement parity work (issue #117). It is
read-only and changes no behaviour, and it reports what the state holds, not what
a mover decides: nothing in it predicts which mover (retail or Remastered) the next
tick selects. `ground_mover_runs` is the one field that speaks about the past — it
counts ticks that already took the Remastered ground mover (see below).

- `tick`: the simulation tick this command is dispatching in —
  `CActor::PortTickGeneration()`, bumped exactly once per dispatched tick in
  `CGameArchitectureSupport::UpdateTicks` before that tick's messages and object
  update. It is not a count of completed world ticks: `status` runs early in the
  tick, so most fields below are still the previous tick's result, and the
  counter also advances on dispatched ticks that update no physics — the game
  paused, or the debug free camera frozen (`CStateManager::Update` returns after
  the console tick). It is a `uint32` that skips 0 on wrap.
- `pos`, `vel`: the player's translation and world velocity, per component as
  `%.9g`, so a float round-trips through the JSON (the top-level `pos` stays
  `%.3f`). A whole number prints bare, as `0`, and a non-finite component (a
  velocity that went NaN or infinite on one axis is the realistic case) is `null`
  rather than the `nan`/`inf` text that `%.9g` would print, which no JSON parser
  accepts.
- `movement_state`: `on_ground`, `jump`, `apply_jump`, `falling`,
  `falling_morphed` (`NPlayer::EPlayerMovementState`).
- `ground_collider`: the player's `kMT_GroundCollider` **material** flag. This is
  not "grounded": Remastered drops that material in the air, and `movement_state`
  is the field that says whether the player is on the ground.
- `ground_mover_runs`: how many ticks have entered
  `CGameCollision::MoveGroundedRemastered` **for this player** since the process
  started — a `uint32`, monotonic, incremented at that function's entry only, after
  the dynamic-contact fallback guard and before the entry impulse, and never reset
  (a save-state load rebuilds and teleports the player without clearing it). It
  answers "is the Remastered ground mover actually running, or is something
  silently disabling it?" — a morph-ball tick, a save-state load, the
  dynamic-contact fallback, the retained-state dispatch in `CGameCollision::MovePlayer`,
  retail movement (`remastered_movement` off) and every airborne tick leave it
  unchanged. It only says whether the mover ran, and it is not a
  statement that ground replacement is complete or matches Remastered
  (`docs/REMASTERED_MOVEMENT.md`). The getter is
  `TARGET_PC` only; the field is absent in a build without it.
- `retained_ground_state`: the port's retained Remastered physics object state
  (`CPhysicsState+0x13c`; the retained ground/air result, one of the fields the
  movers persist):
  `1` grounded, `0` no floor, `null` when the port has **no trustworthy value for
  this tick** (a retail mover ran: a tick outside the mover gate — classic
  movement, direct aim/mouse controls off, morph ball — the ground dynamic
  fallback, or a save-state load; the Remastered mover then stands aside until the
  next Remastered airborne tick). Read-only observation, also `TARGET_PC` only.
- Neither field above is a clock: `tick` is the simulation generation, and
  `ground_mover_runs` counts mover entries, which are per-tick, per-player and
  0 on any tick that took another path. Nothing in the mover reads either.
- `dashing`, `dash_done`: `CPlayer::IsSidewaysDashing` and
  `GetDoneSidewaysDashing`.
- `orbit_state`: `no_orbit`, `orbit_object`, `orbit_point`, `orbit_carcass`,
  `forced_orbit_object` or `grapple`.
- `target`, `target_editor`: the orbit target's unique id (`1023` is none) and its
  editor id as 8 hex digits, `00000000` when there is no target.
- `remastered_movement`: the setting in effect (`remastered_movement`, or
  `MP_REMASTERED_MOVEMENT`), not a record of which mover a tick used.
- `sim_rate`, `sim_adaptive`: `PortDebug::SimRate()`, the fixed clock's step (60,
  or the F1 Debug simulation rate, which `original` pins to 60), and whether the
  game instead steps once per frame at the measured frame time (adaptive, off
  under `MP_TURBO`), where `sim_rate` is not the step in use.

Phase: `status` is a tick command, run by `PortConsoleTick` early in
`CStateManager::Update`, before that tick updates the objects and the player
(`x8d8_updateFrameIdx` is bumped near the end of the update). The values are
therefore mostly what the previous tick left, while `tick` is the generation of
the tick that is dispatching now. A `press`, `stick` or other tick command sent
in the same `cmd` client call takes effect in a later tick.
