# Controls and settings


- Keyboard defaults: WASD / IJKL for sticks, X/Z/C/V for A/B/X/Y, Return for
  Start, arrows for D-pad, Q/E for L/R, and F for Z. Existing mappings take
  precedence. SDL controllers are supported.
- F1: debug overlay. F10: 60 FPS cap on/off (off: F1 Video > Frame rate "FPS cap", `fps_cap=`, `MP_FPS_CAP`; 0 = unlimited). F11: fullscreen.
  F12: screenshot.
- Settings search: the box at the top of the F1 overlay (Ctrl+F) lists the
  settings whose name, page, section or keywords contain every typed word;
  picking one opens its page, sub-tab and collapsed section, scrolls to it and flashes it. Escape
  clears the box. The index is `platform/port_settings_index.inc` (one row per
  widget, in draw order; ctest `port_settings_index` checks it against the
  sources, so a new setting must be added there). `MP_DEBUG_SEARCH=<query>`
  types a query into the box when the overlay first opens, for captures.
  `MP_DEBUG_SEARCH_AUDIT=1` (with `MP_SHOW_DEBUG_UI=1`) opens every row in turn
  and logs `search audit: found|MISSED: <row>`, then a total: a row missed on
  every platform and in every state is in the wrong place. Rows only drawn on
  Android sit in an `#if defined(__ANDROID__)` block of the index.
- Fullscreen (F1 > Video > Display, persisted as `fullscreen`): a borderless window over
  the whole screen on desktop, toggled with F11; on Android it hides the status
  and navigation bars (on by default there; a swipe from the edge shows them
  for a moment).
- Settings changed in the F1 overlay (aspect, vsync, render scale, frame limit,
  mouse aim/inversion/sensitivity, audio mutes) are saved to
  `port_settings.ini` in the user folder (`user/` beside the executable, see above) and restored on the next launch. The System page shows the
  path and has a **Save settings now** button. Environment variables still
  override the file for that run, and are written back into it if any setting is
  changed during that run. Each key is one row of `kSettingKeys` in
  `platform/debug_ui/settings.inc`, which both reads and writes it, with two sample
  values; ctest `port_settings_roundtrip` (`metroid_prime_port
  --settings-selftest`) checks that every sampled key reads back as written and
  that the whole file survives being read back. A new setting is a new row.
- F1 > System > Settings > **Reset configuration...** puts every port setting and
  control mapping back to its default: after a confirmation it saves the current
  settings, moves `port_settings.ini`, `controls.toml`, `imgui.ini`,
  `controller_ports.dat`, `keyboard_bindings.dat` and the `*.controller` mapping
  files into a new `config-backup-<date>-<time>/` folder inside the user folder
  (older backups are never reused), and exits; the next launch starts from
  defaults (`platform/include/port_config_reset.h`, test `port_config_reset_tests`).
  Only those names are touched, a symlink among them makes it refuse, and a failed
  move is rolled back and the error shown without quitting (if the rollback itself fails, the message names the files left in the backup folder). After a successful reset every later config write (settings, `imgui.ini`, Aurora's controller/keyboard files) is blocked for the process (`aurora_disable_config_persistence`). A fresh
  `port_settings.ini` keeps just `disc_path`, `remastered_nsp` and `remastered_keys`,
  so the disc isn't asked for again. Saves, save states, mods, caches, the disc and
  the data folder location are not touched. Game options that retail keeps inside
  a save file (invert Y, volumes...) come back when that save loads; the port's
  global copy (`game_options=`) is in the reset settings. After the reset nothing
  writes the settings (`SaveSettings` is disabled, ImGui's ini off) until the
  process exits. To undo, copy the files back from the backup folder.
- Original experience (top of F1 > Game and the pause-menu options, persisted as
  `original_experience`, `MP_ORIGINAL=1`, console `original [on|off]`): plays the
  game as retail without touching the saved settings, so turning it off restores
  them. It forces 640x480 (EFB scale 1x, no dynamic resolution, MSAA and
  anisotropy off), 4:3 without the widescreen HUD or Vert+, FOV 55, HUD scale
  100, the game's crosshair, the helmet and visor effects, cutscene bars, unskippable cutscenes and
  elevator rides, a 60 Hz simulation with the frame cap and no interpolation,
  the disc's font and GameCube button prompts, no mods, unlocks or revealed map,
  and retail controls (no spring ball, fast morph, sticky or rapid charge,
  lock-on toggle, turbo fire or scan/X-ray swap). Mouse aim, gyro, twin stick,
  touch controls, bindings, cheats, save states, the randomizer and Archipelago,
  rich presence and the timer stay available; the settings it forces are greyed
  out in F1.
- EFB scale (F1 > Video > Quality, persisted as `render_scale`): 1x-4x of the GameCube's
  640x528 EFB, or the window's own size with "Auto render scale". Above 2x it
  supersamples (4x at 16:9 is 3755x2112; with MSAA 4 the targets take
  several hundred MB). "Dynamic
  resolution" (`dynamic_res`, `dynamic_res_target` in fps, 0 = the display's
  rate) lowers the scale while the GPU cannot hold the target, down to "Lowest
  scale" (`dynamic_res_min`: 0.5, 0.75 or 1, default 1), and raises it back when
  there is room. Where the GPU can time itself (timestamp queries, Aurora's
  `gpu_prof`, switched on while dynamic resolution is) it reads the GPU's time per
  frame: a slow scene whose GPU has time to spare is CPU-bound and left alone, and
  a GPU-bound one goes toward the scale predicted to fit 85% of the frame budget
  (GPU time taken as a fixed part plus a part growing with the scale squared, the
  fixed part learnt from two scales, capped at half the GPU time and forgotten
  after a minute), at most to 0.6x of the scale in one step. The GPU time is the
  span of the frame's passes, or their sum when the passes cover less than half
  the span (gaps where the GPU waited). It lowers after two slow seconds and,
  once settled, checks the frame rate rose: if not it undoes the step and holds
  off, and after twice stops using GPU times. It raises after a steady stretch,
  undoes a raise that proves too slow and waits longer before the next, and skips
  changes under 0.1x, since each one reallocates the EFB and stutters. Without GPU
  times (none, none published within 5 s of seconds with frames to judge, or
  `MP_DYNRES_GPU_TIMES=0`, which forces this path for testing) it steps on the
  frame rate in quarter steps (half above 2x). F1 > Video shows the GPU time, the budget and
  why the scale is what it is; the log has a `port: dynamic resolution` line per
  change. The controller is `platform/port_dynamic_res.cpp` (test
  `port_dynamic_res_tests`). A step keeps Aurora's cached bind groups, dropping two
  frames later those the new frames did not use (the old size's), and clears the
  offscreen targets. A step does not wait for the render worker to finish the
  frames it already has (that wait was most of a step's stutter on a busy GPU):
  those frames are encoded against the old targets and the worker takes the new
  ones after them; a window or surface change still waits, and
  `MP_RESIZE_WAIT=1` makes every resize wait, to rule this out. The log times
  each EFB resize and its parts (`aurora::gpu: EFB WxH: resized in N ms (worker
  wait W, caches C, targets T)`) and the two frames after it. "Sharpen when upscaled"
  (`upscale_sharpen`: `off`, `dynamic` (default: only while dynamic resolution has
  lowered the scale) or `always`; strength "Sharpening", `upscale_sharpen_amount`
  0-100, default 50) applies CAS-style contrast-adaptive sharpening to the
  stretched image in the present resample (not AMD's CAS proper, which scales and
  sharpens in one filter), and only to a frame smaller than the window. The file also
  takes a manual `render_scale` under 1.
- `MP_ASPECT=4:3|16:9|window` (default `window`, persisted as `aspect`); the legacy `MP_WIDESCREEN` selects 16:9.
- `MP_TWIN_STICK=1` (Controls > Options, persisted as `twin_stick`): twin-stick aiming. The
  right stick feeds the first-person aim through the same path as the mouse (so
  the same sensitivity/invert apply, tuned by `stick_aim_rate`, default 900 px/s, 50-10000; the F1
  slider and the pause menu's Stick Aim Speed row, both shown only with twin stick
  on, go up to 8100; with `stick_aim_split=1`, the F1 "Separate vertical speed"
  checkbox, `stick_aim_rate` sets only horizontal speed and `stick_aim_rate_y`,
  same range, default 900, the vertical)
  and is consumed, so it no longer drives the game's free-look. Under mouse aim
  a C-stick held from the keyboard is not consumed, so keyboard beam keys still
  pick beams, and an L from a key or mouse button stays lock-on. Fire
  stays on whatever is bound to A; remap it in Controls > Remap.
- Gyro aim (Controls > Touch & gyro): tilting the pad or phone aims through the
  same free aim as the mouse, so it needs mouse aim, twin stick or the touch
  controls. `gyro_mode` is 0 off, 1 hold to aim (aims while the "Gyro aim" action
  is held), 2 always, 3 hold to pause (aims except while it is held, to recentre
  the pad: ratcheting), 4 toggle to aim (starts off; press once to enable,
  again to disable). Changing modes resets the toggle. "Gyro aim" is right
  stick click and left ctrl by default and can be remapped on the Remap page
  ("Gyro" group). With toggle mode, leave the binding itself as a normal press.
  `gyro_source` is 0 auto, 1 controller, 2 phone. `gyro_sensitivity` (0.1-10,
  default 2) is camera angle per pad angle: 1 turns the camera as far as the pad
  turned. It ignores the mouse sensitivity and invert. An older `gyro_rate` (aim
  pixels per rad/s, which also scaled with the mouse sensitivity) is read once as
  `gyro_rate × mouse_sensitivity` and no longer written.
  `gyro_space` picks which turn moves the camera left/right (GyroWiki's terms):
  1 player (default: turning the pad, flat or upright), 2 world (turning about
  the real vertical only), 0 local yaw (the pad's flat turn), 3 local roll (the
  turn of a pad held upright) or 4 local yaw and roll (both, Overwatch-style).
  Player and world use the accelerometer; without one they act as local yaw.
  `gyro_auto_calibrate` (default on) learns the gyro's drift while the pad is
  still; F1 shows the calibration confidence and has a Recalibrate button. Both
  use GamepadMotionHelpers (`extern/GamepadMotionHelpers`).
  `gyro_smoothing` and `gyro_tightening` (deg/s, 0-50, both 0 = off by default)
  are JoyShockMapper-style: turns below the smoothing threshold are averaged over
  0.125 s, fading to direct by twice it; turns below the tightening threshold are
  scaled by speed/threshold.
  With pointer aim on, the gyro moves the pointer like a mouse cursor:
  `gyro_cursor_angle` (degrees, 5-180, default 40) is how far the pad turns to
  sweep it across the screen, independent of the mouse sensitivity. With
  `gyro_cursor_camera` (default off) the gyro also turns the camera at
  `gyro_sensitivity` while it moves the pointer.
- Spring Ball (Controls > Options, persisted as `spring_ball`, off by default): C-stick
  up in morph ball jumps, as in Metroid Prime Trilogy and the randomprime discs
  the Archipelago world makes, once the Morph Ball Bombs are held. It is a bomb
  jump from the ball's position that keeps the horizontal speed, with the same
  conditions (on the ground, not in shrubbery, spider ball or an energy drain)
  and 40-frame cooldown as randomprime's patch. With twin stick on, the right
  stick's up still triggers it. A connected Archipelago seed decides it instead.
  **Spring Ball on gyro flick** (`spring_ball_flick`, off by default) also
  springs when the pad or phone is tilted up faster than `spring_ball_flick_rate`
  (default 6 rad/s), like Trilogy's nunchuk flick: once per swing, and a flick up
  to 0.2 s before landing still counts. It reads the gyro aim's source but works
  with gyro aim off. The phone's gyro is turned to the screen's orientation, so
  pitch and yaw stay right in landscape.
  **Spring Ball on swipe up** (Android, Touch controls, `spring_ball_swipe`, on
  by default) springs on a quick swipe up of an aiming finger (the free-area
  drag, or Fire/Jump slid): 40 dp up, more up than sideways, within 150 ms of
  where the stroke began, once per stroke (`TouchControlsView.trackSwipe` →
  `PortDebug::TouchSpringSwipe` → the Spring Ball request). Touch aim does
  nothing in the ball, so the swipe takes nothing away there.
  **Touch controls opacity** (Android, Touch controls, `touch_opacity`, 10-100,
  default 100, saved in `controls.toml`): fades every gameplay control, so more of
  the game shows through the overlay; the layout editor and the menu buttons (the
  pointer screens' back and cog buttons, the map button) stay fully opaque. The
  slider is the only way to set it and the view reads it every draw
  (`TouchControlsView.onDraw` draws the gameplay controls inside a
  `canvas.saveLayerAlpha`, F1's value through `nativeTouchOpacity` /
  `PortDebug::TouchOpacity`), so it applies at once, and at 100% exactly as
  before. Side margin, Stick inset and Button inset on the same page
  (`touch_side_margin`, `touch_stick_inset`, `touch_button_inset`) move those
  controls instead, in dp.
  **Touch aim** (Android, `touch_aim`, on by default; `touch_aim_speed`, default
  2.25 aim px per dp): with twin stick on, the right stick is gone and a finger
  dragged on the free screen area turns the view by the distance dragged, like a
  mouse (`AddTouchAim`, drained in `BeginFrameMouse`). Off, the right stick sets
  a turn rate as before. In the GameCube layout (neither mouse aim nor twin
  stick) a drag on the free area outside the pills, D-pad, C-stick and left
  stick also works: sideways turns Samus by the distance dragged
  (`CPlayer::UpdateTouchLook`, direct yaw, movement stays tank), up/down looks
  like R + stick free look while the finger is down (clamped to the free-look
  limit) and eases back level after release. Not while locked on, in the morph
  ball, or when the game blocks turning. Console: `touchaim <dx> <dy> [hold s]`
  (hold keeps the finger "down" that long).
  **Tap minimap for map** (Android, `touch_map_tap`, on by default): a tap on the
  HUD minimap (its screen rect, padded 15%, published each frame by
  `CInGameGuiManager`) presses Z for one poll, which opens the map; the GameCube
  layout's Z pill is hidden while it is on. Console: `minimap` prints the rect,
  `maptap` sends the tap.
  **Beam and visor wheels** (Android, `touch_wheels`, on by default; F1 > Controls > Touch & gyro): in both layouts the D-pad is replaced by two round buttons,
  Visor and Beam. Holding one opens a 4-sector wheel centred on the press
  (clamped on screen) in the stock directions (visors up Combat, right X-Ray,
  down Thermal, left Scan; beams up Power, right Wave, down Ice, left Plasma);
  sliding lights the sector, release picks it, release in the centre cancels.
  Items not owned are greyed (owned/current mask from `CPlayer::Think` via
  `PortDebug::SetWheelState`, polled by `nativeWheelOwned`; all disabled with no
  player). A pick calls `PortDebug::RequestVisor/RequestBeam`, which holds the
  command for 120 ms in `ControlMapper::GetPressInput/GetAnalogInput`: stock
  rules still apply (no beam change in the morph ball or while charging), and
  Swap Scan/X-Ray doesn't matter. `touch_visor_tap_scan` (off by default, "Tap
  Visor for Scan Visor"): a tap (<250 ms, <12 dp) on Visor picks Scan. The
  wheels don't open while the map screen is open. Console: `visor <name|n>`,
  `beam <name|n>` (no argument prints the current one).
  **Map pan** (always on): while the map screen is open, a one-finger drag on any
  free part of the overlay (outside the left stick, which rotates) pans the map so
  the content follows the finger (`CAutoMapper::ProcessMapPanInput`: a drag of the
  view height moves the area point by the visible world height; same bounds as the
  stick). Console: `mappan <dx> <dy> [hold s]` (dp, view height taken as 400 dp; the finger counts as down for `hold` seconds, which stops the map drifting back to its area; ignored
  while the map is closed). A second finger pinches to zoom (finger spread doubles: camera distance halves, same clamp as L/R) while the midpoint pans; when one finger lifts the other carries on panning. Twisting the two fingers rotates the map's yaw with them (a clockwise twist turns the content clockwise; pitch is left alone). Console: `mapzoom <ratio>`, `maprotate <degrees>` (positive turns the content clockwise, as the stick's right does; both ignored while the map is closed).
  A press of the beam shift springs too (beams don't change in morph ball), as
  X does in Remastered; jump (B) stays the Boost Ball's alone: every shift input is
  bound to the Spring Ball action alone in the Morph Ball context.
- Beam shift (Controls > Options): there is no hidden modifier. Each shift input
  forms default chords with the D-pad inputs of its own family (shift + up = Power,
  down = Ice, left = Plasma, right = Wave), which fire the direct beam actions
  (not the visor) and show in Controls > Remap; the plain D-pad still picks visors.
  Shifts: two key slots (`shift_key`, `shift_key_alt`; default left shift under
  twin stick), a pad slot (`shift_pad`, a button or trigger, default none, because
  Aurora maps LB on many pads to L; under twin stick LB is the shift while
  `shift_pad` is unbound and LB presses no other pad button), a mouse button, and the
  touch Beam button (`touch:beam_shift`, with the touch D-pad). The L trigger is no
  longer a shift. A saved `controls.toml` row for the retired `beam_shift`
  action is skipped (and logged) when loaded.
- Turbo fire (Controls > Options, "Turbo fire (hold)"): while it is held, A is
  pressed and released on alternate ticks, as if fire were mashed, so each beam
  shoots as fast as its own shot delay allows (it doesn't charge). Two key slots
  (`turbo_key`, `turbo_key_alt`) and a pad slot (`turbo_pad`), all unbound by
  default; presets leave them alone. On Android, F1 Touch controls "Turbo fire
  button" (`touch_turbo=`, off by default) adds a "T" button to either touch
  layout, movable and resizable in the layout editor (issue #10).
- Alt controller buttons (the pad buttons' second binding in Controls > Remap, persisted as
  `pad_alt`, 16 comma-separated native codes indexed by the PAD bit, -1 for
  none): a second controller button or trigger per GameCube button. Aurora maps
  one native button to each PAD button, so the port reads the alt one itself and
  ORs it in (`PortControls::HeldAltPadButtons`, from `CDolphinController`).
- Control presets (Controls > Remap, "Reset..." beside the device list: Keyboard & mouse and Controllers). Keyboard: **Classic** (the first-run layout)
  and **Mouse & keyboard** (WASD, E fire, Space jump, left ctrl/C morph, F
  missile, Q lock on, left alt free look, Tab/M map, 1-4 beams and 5-8 visors
  through whichever C-stick direction or D-pad button the disc's tweak gives
  each, arrows on the D-pad too; turns mouse aim and twin stick on). Both also reset the mouse buttons and the beam
  shift keys. Controller: **GameCube** (Aurora's default), **Remastered**
  (Remastered's Dual Sticks: RT or right face button fire, LT lock on, bottom
  face button or LB jump, left
  morph, RB missile, Start map, Back pause, right stick click free look, top
  face button as the pad beam shift (which springs in morph ball), Scan and X-Ray swapped; twin stick on),
  **Modern** and **Southpaw**. Only Remastered sets `shift_pad`, `pad_alt` and
  `swap_scan_xray`; the others clear them. Remastered and Modern are off for a
  GameCube adapter.
- Swap the Scan and X-Ray visor buttons (Controls > Options, persisted as
  `swap_scan_xray`, off by default): each visor takes the other's D-pad
  direction, as in Remastered (D-pad right Scan, left X-Ray).
- Fast Morph (Controls > Options and pause Options > Controller, persisted as
  `fast_morph`, off by default): morph ball transitions in the style of Metroid
  Prime 4. Morphing and unmorphing each take 0.2 s instead of 1 s; both
  keep the player's velocity instead of stopping them. Unmorphing on the ground
  holds the speed at walking speed; unmorphing in the air keeps the whole arc
  until landing. Samus's curl-up animation is not shown: the ball forms inside
  the transition flash while the camera eases out, and an unmorph cuts to first
  person behind the transition filter.
- Toggle Lock-On (Controls > Options and pause Options > Controller, persisted as
  `lock_on_toggle`, off by default): a press of L that locks on (target, scan
  point, grapple point) latches it held and the next press lets go. With nothing
  to lock on, L is a plain hold (strafe while held). The latch also lets go by
  itself when its lock ends (target dead or out of range). Sticky
  Charge (`sticky_charge`, off by default, needs the Charge Beam): holding fire
  for 0.35 s or more keeps the charge held after letting go, and the next press
  fires it; shorter taps shoot as usual. Both are off in morph ball and while
  input is disabled. The console's `status` prints the game's L/A and the charge.
- Remastered charge (Controls > Options, "Remastered charge (rapid fire)", persisted as
  `rapid_charge`, off by default, `MP_RAPID_CHARGE=<0|1>`): as in Remastered, holding
  fire fires a few shots before charging (3 in all with Power, 2 with Wave or Plasma,
  1 with Ice), then charges faster, so a full charge takes about as long as before.
  Without the Charge Beam it fires the same shots, then waits for fire to be let go.
- Remastered movement (Controls > Controller settings > Stick aim, persisted as
  `remastered_movement`, on by default, `MP_REMASTERED_MOVEMENT=<0|1>`): under twin
  stick or mouse aim, diagonals are as fast as straight ahead and the air drag is
  steady, so a jump keeps a smooth arc, as in Remastered and PrimeHack. Off, jumps
  move as on the GameCube. Classic controls always move as on the GameCube. Both
  are off under the original experience. What the port implements on the ground
  under this setting, and what it does not, is in `docs/REMASTERED_MOVEMENT.md`.
- The overlay's **Controls** page keeps settings only: "Mouse settings" (inversion,
  crosshair) and "Controller settings" (stick aim and dead
  zones). **Remap > Active controller** selects any detected gamepad, including
  one assigned to player 2 or later; the choice is saved and restored on reconnect.
  Select it here before recording its bindings in Remap. Remap's "Controller bindings for" selects
  binding profiles, not the controller used to play. Every binding is on **Controls > Remap**, below.
- **Controls > Remap** lists all devices' bindings together, with keyboard, mouse, controller
  and touch icons. "Controller bindings for" selects all controllers or a controller kind/model
  profile, including saved profiles. Actions are grouped (game buttons,
  move and aim, beams, visors, Morph Ball, port, layers) with the GameCube button's game function
  beside it; default inputs are greyed out. Click an input to edit it, or **+** to open a new
  binding editor beneath that action and start recording. The action is fixed to the row:
  press an input and release it, then **Save** (or **Cancel**); **Record** captures again.
  Record accepts keyboard, mouse or controller input; repeated identical
  mappings are ignored. Editing a default makes it yours, and "Revert" gives the default back. Keyboard &
  mouse, Touch and Controllers are the Default profile restricted to that family of input;
  a kind or model is its own profile on top. "Reset..." restores a preset (after a confirmation).
  "Separate menu buttons" (on by default; `menu_standard_buttons=`,
  `MP_MENU_STANDARD_BUTTONS`) makes a controller read the "Menus and map" rows instead of
  the game buttons in the pause screen, front end, map, save stations and upgrade messages
  (the scan text takes only its A from them, so the held scan lock stays). Their defaults are the pad's own
  A/B/X/Y, shoulders, triggers and D-pad (A confirms, B backs out, the triggers page and
  zoom), and they can be rebound like any row; the prompts there follow them. Start and the
  map button keep their gameplay inputs (a Menu row on either input is ignored); bindings
  set for Menu or Map under Advanced still apply. Keyboard and mouse inputs bound to the
  rows apply in menus and the map with or without a controller, replacing what that key
  does in gameplay there; touch is unchanged. With
  the option off, menus and the map follow the gameplay bindings and the rows do nothing.
  "Point and click menus" (on by default; `pointer_menus=`, `MP_POINTER_MENUS`) lets
  the mouse and touch work the game's own menus (front end, pause screen, save stations,
  upgrade messages) and the map. A hover selects (a tap on touch), a click confirms (a
  tap on the selected item), right click (or Android back) backs out, and a click on a
  button hint presses that button. The wheel or a drag pages the log book text, a press
  or drag sets a slider, and in the Samus doll view a drag turns the doll, a right drag
  or two fingers move it and the wheel or a pinch zooms.
  On the map a left drag rotates (one finger pans), a right or middle drag or two
  fingers pan, a two-finger twist turns it, the wheel or a pinch zooms, a click selects a world or room and a click
  on the selected one opens it. While such a screen is up the mouse is released and the
  cursor shown; the button prompts show mouse glyphs after the mouse was last used; on
  Android the gameplay touch buttons hide, leaving a back button. Escape pauses, backs out
  of menus (resuming from the pause screen's top level) and closes the map while this is
  on, outside Original experience.
  It binds any action to an input or a chord of up to four
  (hold the first ones, press the last; or "Any order" within the chord window). The editor's
  Advanced section holds the typed input text, the trigger (press, tap, hold, double tap,
  toggle), turbo, a scale/invert for analog actions and where it works (everywhere, or only in
  gameplay, morph ball, map, menus, scan visor, or one of four layers: a layer is active while
  its "Layer n (hold)" binding is held, like a shift key, and its bindings win over the same
  input's normal ones). Actions are the pad's buttons and stick halves, plus beam and
  visor picks (direct or next/previous), Spring Ball, the port menu, save/load state,
  screenshot and Original experience. Bindings are layered over the other pages:
  binding an action from one kind of device (keyboard & mouse, controller,
  touch) replaces what the other pages bind to it from that kind only, and
  "Removed built-in bindings" drops one without adding another. A profile can
  follow the connected controller's kind (SDL type) or that controller model
  (SDL GUID); it goes on top of the default profile, GUID before type. Saved as
  `controls.toml` in the user folder (format in
  `platform/include/port_input_bindings.h`). Edits to the file are picked up
  within a second; a file that doesn't parse is logged and the last good
  bindings stay. Touch inputs are named after the overlay controls (`touch:a`,
  `touch:fire`, `touch:missile`, `touch:morph`, ..., plus `touch:beam_shift`,
  `touch:turbo_held`, `touch:map_tap`); a bound touch control no longer sends
  its own button. The overlay hides while F1 is open, so Record can't see touch
  controls: the editor's "Add a touch control..." list picks them instead.
  The D-pad, the sticks and the menu button can't be remapped.
- `MP_CINEMA_BARS=0|1` (Video > Display "Cutscene black bars", persisted as `cinema_bars`,
  default off): the scripted 16:9 cutscene letterbox. Off, cutscenes narrower than 16:9
  show the cinematic camera's full shot (Vert+); elevator rides and credits keep theirs.
- `MP_SHARP_SCAN_WINDOW=0|1` (Video > Display "Sharp scan window", persisted as
  `sharp_scan_window`, default off; off under Original experience): the scan visor's
  magnifier renders the world a second time through a zoomed camera instead of
  stretching a copy of the screen centre, so it stays sharp at high resolution and with
  HD textures. Costs a second world render while the scan visor is up; the faceplate
  decor isn't magnified.
- `MP_HUD_WIDE=0|1` (Video > Display, persisted as `hud_wide`, default on): widescreen HUD. The
  aspect-matched in-game HUD frames keep each element's shape but move it away
  from the screen centre, so edge elements (scan panels, energy bar, map) reach
  the true wide corners instead of being pulled inward. Under a perspective
  camera the element is rotated rigidly about the eye rather than slid sideways,
  since sliding turns off-axis elements away from the viewer and shears them;
  the rotation leaves what is seen of the element unchanged and the angle is
  derived from the aspect ratio, so it holds at any aspect rather than only
  16:9. Menus, the credits and other non-aspect-matched frames are unaffected.
- Field of view (Video > Display and pause Options > Display, persisted as `fov`,
  45-90, retail 55): the first-person camera's vertical FOV. The overlay also
  shows the horizontal FOV it gives at the current aspect. The arm cannon is
  drawn at the retail FOV whatever the setting (a view-model FOV), so it keeps
  its size and place. Shots start where the cannon is drawn and are aimed at
  where retail's would arrive (the aim target at its depth when assist aimed
  the shot at it, else what lies on retail's line), so they hit what retail's
  hit (#134). Morph ball, cutscene and other scripted cameras keep
  their own FOVs; cutscenes that end in Samus's eyes ease to the setting.
- Arm cannon offset (Video > Display, persisted as `gun_offset=right,forward,up`,
  metres, each -1 to 1, default 0,0,0; like PrimeHack's): moves the arm
  cannon from its retail place along the camera's axes. It moves the real gun,
  so shots leave from the moved muzzle; the grapple arm follows. Off in the
  original experience. Console: `gunoffset <r> <f> <u>`.
- Anti-aliasing and anisotropic filtering (Video > Quality, persisted as `msaa`,
  1 or 4, default 1, and `anisotropy`, 1-16, default 16; pause Options >
  Display has an Anti-Aliasing on/off row): 4x MSAA on the scene framebuffer
  (WebGPU only guarantees 1x and 4x) and the anisotropy cap for mipmapped
  textures, which retail asks for as GX_ANISO_4. Both apply at the start of the
  next frame without a restart; an MSAA change rebuilds the framebuffers and
  pipelines, so it hitches once. Console: `msaa <1|4>`, `aniso <1..16>`.
- Per-pixel lighting (Video > Quality, persisted as `per_pixel_lighting`,
  default off, off in the original experience; issue #113): the GX lights on
  lit draws (Samus, enemies, items and other vertex-lit models) are summed per pixel
  from the interpolated view-space position and normal, as Dolphin's option
  does, instead of per vertex. Remastered (PBR) models are per pixel already
  and unchanged. Lit pipelines rebuild on their next draw, so turning it on
  hitches once. Console: `pixellight [0|1]`.
- Sun shadows (Video > Quality, persisted as `sun_shadows`, default on, not
  forced by the original experience): the port's 4-cascade shadow from the sun
  in outdoor rooms. Remastered does not draw sun shadows; turn off for the
  Remastered look. Off behaves like a room with no sun (no cascades, no
  receiver sampling); room point/spot lights are unaffected. Console:
  `sunshadow [0|1]`. `MP_SHADOWS=0` turns them off for the run. For tuning:
  `MP_SHADOW_DIR=<x,y,z>` (the way the light travels; else the room's sun, then
  the area's first directional light), `MP_SHADOW_COLOR=<r,g,b>` (the sun light's
  colour; else the room sun's) and `MP_SHADOW_DISTANCE=<units>` (how far from the
  camera it shadows, default 350). Actors the frustum or PVS rejects still cast,
  as base models only, as in Remastered; `MP_SHADOW_OFFSCREEN_ACTORS=0` stops that.
- Projected actor shadows (Video > Quality, persisted as `projected_shadows`,
  default on): Remastered's actor shadow. Each shadowed actor renders depth
  only into a 256x256 map looking straight down, and the room geometry under it
  is redrawn with a multiply decal (fades towards the top of the shadow box).
  Only with Remastered room geometry loaded; otherwise (and when off) the
  retail blob quad is drawn. Known gaps: dynamic receivers (platforms) and
  sorted/cutout geometry are not decaled; at most 16 actors per frame.
  Console: `projshadow [0|1]`.
- Renderer (Video > Compatibility, **Windows only**, persisted as
  `graphics_backend`: `auto`, `d3d12`, `vulkan` or `d3d11`, default `auto`,
  issue #135): which graphics API the port asks Aurora for. `auto` asks for
  nothing in particular, so Aurora takes its own order (Direct3D 12, then Vulkan,
  then Direct3D 11); an explicit choice is tried first, and if the driver can't
  start it, that run falls back to the same order. The setting is left as it was
  by a fallback, so the page keeps showing the choice and names the API actually
  in use, including "no graphics device" when nothing started.
  `MP_GRAPHICS_BACKEND=auto|d3d12|vulkan|d3d11` overrides the setting for one run
  and outranks it (per-run `MP_*` precedence, like `MP_OPENGLES`); any other value
  warns and starts on the saved selection. While an override is set the page names
  the API it starts on and says the saved selection applies once the variable is
  unset, and the fallback message describes what the override asked for, not what
  the file says. Read at window creation, so a change needs a restart: the page
  shows "Restart to apply" while the saved selection differs from the one this run
  started with, and selecting the startup choice again clears it.
  On Windows this replaces asking for OpenGL ES: Dawn's Windows build has no GLES
  backend. Elsewhere the setting is ignored and Android's choice stays `opengles` /
  `MP_OPENGLES`.
- Adreno shader fix (Video > Compatibility, persisted as `storage_clamp`: -1 Auto,
  the default, 0 Off, 1 On): reads GPU buffers with a clamp instead of the
  bounds-check branch some Adreno Vulkan drivers miscompile (the world draws
  black, issue #7). Auto turns it on for Vulkan on an Adreno 7xx. Read once when
  shaders are first made, so a change needs a restart. `MP_STORAGE_CLAMP=1`
  forces it on and any other non-empty value off, over the setting. When the
  setting is On or Off (and the variable is unset), the port sets
  `MP_STORAGE_CLAMP` itself before starting Aurora; the log's `port: env:` line
  then ends in `(from storage_clamp)`, so it isn't mistaken for a user's variable.
- Driver workarounds (Video > Compatibility, a collapsed group, persisted as
  `driver_workarounds`: a comma-separated list of the ticked ones' names, empty by
  default, issue #143): opt-in Dawn toggles for a GPU that draws wrong, applied when
  the GPU device is made, so a change shows "Restart to apply". `keep_depth` ("Keep
  depth buffers") turns off `d3d11_use_discard_view`; `gpu_constants` turns on
  `d3d11_disable_cpu_buffers`; `unmonitored_fence` ("Alternative GPU sync") turns on
  `d3d11_use_unmonitored_fence`; `validation` ("Driver error checks") turns off
  `skip_validation` and `disable_robustness`, so Dawn names a rejected command in the
  log (slower). The first three are passed only to a Direct3D 11 device; on another
  API the log lists them as `(ignored: not Direct3D 11)`. The log's `Dawn toggles`
  block lists what the device got, each one also as a `driver workaround on|off:` line;
  `MP_DAWN_ENABLE` / `MP_DAWN_DISABLE` apply after them.
  - Driver error checks last one start: the box is unticked as the start applies it,
    the log says `driver error checks are on for this run`, and the section shows it.
  - A way out when one breaks the start: while any are on, the user folder holds
    `driver_workarounds_starting` until the game's 60th frame. If a start leaves it
    behind (a crash or hang before then), the next start unticks them all, logs
    `did not reach the game; turning them off` and says so in the section.
  - `MP_DRIVER_WORKAROUNDS=0` starts one run without them, whatever is ticked;
    `MP_DRIVER_WORKAROUNDS=keep_depth,validation` (names as above) uses those
    instead. The setting is left as it was, and so is the crash marker: a run on the
    override neither trips it nor clears it. After a start clears them, the marker
    stays until a start reaches frame 60, so repeated crashes keep them off.
- HUD scale (Video > Display and pause Options > Display, persisted as `hud_scale`,
  50-100 percent, default 100): shrinks the combat HUD, radar, beam and visor
  menus and the minimap toward the screen centre, each frame as a whole so the
  pieces stay on the visor frame. The helmet is not scaled. The minimap eases
  back to full size as it opens into the map screen.
- Scan image spread (Video > Display, persisted as `scan_spread`, 0-100 percent,
  default 100, 0 on Android): how far the widescreen HUD moves the scan visor's side panels and
  the scan images out toward the screen edges. At 0 they keep their 4:3 place
  near the centre, clear of a phone's touch controls; the rest of the HUD still
  spreads. No effect at 4:3, without the widescreen HUD, or under Original
  experience. Console: `scanspread <0..100>`.
- Crosshair opacity and colour (Video > Display, persisted as `crosshair_opacity`,
  0-100 percent, default 100, and `crosshair_color`, `RRGGBB` or empty for the
  game's own colour): the centre crosshair of free aim, mouse aim and twin stick.
  The opacity is the crosshair's alpha, so 100% draws it at full strength (the
  game's own alpha is 19%).
  Original experience ignores both.
- Secondary crosshairs (Video > Display, `secondary_crosshairs`, `MP_SECONDARY_CROSSHAIRS`,
  default on): off hides the next-target reticles: the combat visor's seeker and the
  X-ray ring and thermal crosshair on the target Lock-On would pick. The lock-on
  reticle and the scan visor's target box stay. On under Original experience.
- Scan speed (Game > Cutscenes, `scan_speed`, `MP_SCAN_SPEED`, 100-500, default
  100): percent speed of the scan visor's progress bar and the scan's minimum
  time. 100 under Original experience.
- Auto-aim (Controls > Options > Aiming, `auto_aim`, `MP_AUTO_AIM`, default on):
  off clears the gun's aim target unless Lock-On holds one, so free-aimed shots go
  straight to where you aim instead of turning toward the enemy nearest the screen
  centre (the decomp's `gkAutoAimAtOrbitedObject` behaviour). On under Original
  experience.
- Hide helmet and hide visor effects (Video > Display and pause Options > Visor,
  persisted as `hide_helmet` and `hide_visor_effects`, off by default): the
  first drops the helmet frame (the dome and the lights at the bottom); the
  second drops the faceplate decoration, Samus's face reflection and the
  on-visor billboard effects (rain, splashes, steam). Console:
  `hudscale <50..100>`, `helmet <0|1>`, `visorfx <0|1>` (0 hides).
- Speedrun timer and LiveSplit (F1 > Game > Speedrun; the timer also in pause Options >
  Display as In-Game Timer). `speedrun_timer` draws the in-game time (the play
  time the save shows, which stops in cutscenes, menus and loads) in the bottom
  right while a game runs. `livesplit` connects to LiveSplit's TCP server
  (right-click LiveSplit > Control > Start TCP Server) at `livesplit_address`
  (default `127.0.0.1:16834`), retrying every 3 s. A new file sends `reset`,
  `starttimer` and `pausegametime`, so LiveSplit's Game Time then follows the
  in-game time (`setgametime` every 0.1 s of it); compare against Game Time. It
  splits when an upgrade or artifact is first gained (`livesplit_split_upgrades`,
  on by default; not expansions or energy tanks, and items found again after the
  frigate split again) and on the final blow (the EndGame special function).
  Loading a save does not start the timer, and nothing resets it but a new file.
  The logic is `PortLiveSplit::Tracker` (`platform/include/port_livesplit.h`,
  covered by `port_livesplit_tests`). Console: `timer <0|1>`, `igt <seconds>`,
  `livesplit <0|1> | addr <host:port> | send <command> | status`.
- Update check (F1 > System > Updates; on by default, `update_check`,
  `MP_UPDATE_CHECK=0` turns it off for a run). At every launch it asks
  `api.github.com/repos/Odrannnn/MetroidPrimePort/releases/latest` (an HTTPS
  GET with nothing but a User-Agent naming the version) whether that tag is newer
  than this build's version (`MP_BUILD_VERSION`, from `versionName` in
  `android/app/build.gradle`). If it is, a toast shows at the top for 12 s
  (click or tap it to open the release page) and F1 offers "Open release page". The answer is kept in `<user>/update-check.txt`,
  so a known newer release shows at once, offline too; after a failed check,
  F1 "Check now" tries again. Logic in `platform/port_update_check.cpp`, covered by
  `port_update_check_tests` (`MP_UPDATE_LIVE=1` adds a real request). mprig runs
  set `MP_UPDATE_CHECK=0`. On Windows, TLS (this and Archipelago `wss://`) trusts
  the system ROOT store, since the static OpenSSL has no default one.
- Discord Rich Presence (F1 > System > Discord; desktop only, on by default,
  `discord_presence`). Shows the current room as the activity, with energy,
  missiles (once the launcher is found) and the item percentage below it, a
  picture of the current world with the logo in its corner, the world and Hard
  mode in the picture's tooltip, and time elapsed since the game was loaded. The
  menus show "In the menus". It talks to the local Discord client's IPC socket
  (`$XDG_RUNTIME_DIR/discord-ipc-N`, also the Flatpak and Snap Discord paths;
  `\\?\pipe\discord-ipc-N` on Windows), retries every 5 s and sends at most one
  update per 4 s. It uses the port's own Discord application ("Metroid Prime",
  `PortDiscord::kDefaultAppId`), whose art assets are `logo` and `world_frigate`,
  `world_tallon`, `world_chozo`, `world_magmoor`, `world_phendrana`,
  `world_mines` and `world_crater` (the pictures live on Discord, not in the
  repo). The Flatpak build is granted both socket locations.
  Turning it off clears the activity. The wire format is in
  `platform/include/port_discord.h` (covered by `port_discord_tests`). Console:
  `discord <0|1> | status`.
- Map and progress tracker (F1 > Tracker). Reveal map (`reveal_map`, also
  pause Options > Visor, off by default) shows every world's map as if its map
  station had been used and lists every world on the star map. It only changes
  what the map screen asks, never the save; rooms a map station leaves hidden
  stay hidden. Pickup marking (`map_pickups`, also pause Options > Visor, off by
  default; Tracker page: Off, Seen, Rooms, All; the pause row cycles Off, Seen
  and All, with Rooms shown as the third value when set elsewhere): All draws a
  plain white dot on the map and minimap for every pickup not yet collected, in
  rooms the map shows; Seen draws the dot only once you have looked at that
  pickup for half a second (within 40 units, in the middle half of the view,
  with a clear line from the eye; see-through surfaces such as gratings don't
  block it; kept per save, `pickups seen`); Rooms draws no dots but tints the rooms that
  still hold an uncollected pickup amber. An old saved `1` stays All. In Seen,
  Map stations show pickups (`map_station_pickups`, on by default) also marks
  every pickup in the rooms a map station has mapped (or a hint, or Reveal map,
  which makes Seen show as much as All). Show major and minor items
  (`map_pickup_kinds`, off by default) draws a major item as a filled dot, a
  minor one (missile or power bomb expansion, energy tank) as a ring, and a
  collected pickup as a grey cross; in a randomized game it tells what kind of
  item a pickup holds (from the seed; in Archipelago an item for another game is
  major when it is progression, an unscouted pickup is a dot). The disc's Missile
  Launcher is an ordinary missile pickup, so it shows minor (also in a legacy
  seed); only an Archipelago launcher item is major. Off, every dot is the same
  plain white and a collected pickup's goes away. An Archipelago game colours them by its logic
  (`map_logic_colors`, see `ARCHIPELAGO.md`). The positions come from
  `tools/gen_map_pickups.py` (`platform/port_map_pickups.inc`). With Show
  progress on (`tracker_progress`, off by default so it spoils nothing), the
  Tracker page shows item percentage, energy tanks, missile
  capacity (in packs of 5, launcher included), power bombs, artifacts, missing
  upgrades, logbook scans per category (artifacts count at the game's 50%),
  rooms visited per world and the current world's unvisited rooms (their names
  load on first view). Extra map UI (`map_extra_ui`, off by default) adds the
  port's own lines to the world map screen: legend entries for the pickup shapes
  in use (see Show major and minor items) and for the marked-room colour, and the
  "Mark room" prompt in the map's upper right, clear of the room title, the
  legend and the prompts along the bottom. Off, the legend is the disc's own text
  at the disc's own size, and there is no prompt; the legend is drawn scaled by
  its row count against the disc's so the port's entries never reach the
  Key/Legend prompt under it. Console: `reveal <0|1>`, `pickups <off|seen|rooms|all>`, `pickups kinds|station <on|off>`, `pickups collect <dot>`, `mapui <0|1>`, `tracker`.
  Room marks: on the world map screen X marks or unmarks the shown room. With
  Extra map UI on, a click or tap on the "Mark room" prompt does the same (with
  pointer menus; touch needs the setting on, since the prompt is what is tapped).
  Marked rooms draw cyan on the map and the minimap, over the Rooms amber,
  whatever `map_pickups` is, and marking works whether Extra map UI is on or off. A save keeps up to 64 marks, and which pickups it has seen,
  in a small side file beside the card: `mapdata/<card>-<slot>.bin` under the
  port's user folder. The card's identity keys the name - the directory it
  resolves to, the game id and maker its files are created with, and the card
  port - so the port's card, a shared emulator card, an Archipelago room's card
  and even a PAL and a USA card under one directory each keep their own, and
  nothing is written into the emulator's folder. The file holds a fingerprint of
  the exact slot bytes and is only used on the card it was written for, so a
  save replaced elsewhere - Dolphin, an import, a conversion, an older build -
  falls back to what the slot itself carries. It is written after a memory card
  write has finished (never for one that failed) and read back when that slot is
  loaded. Beside it the slot still holds an "RMMK" trailer after the
  pickup-seen one - the first 20 marks, exactly the format earlier builds wrote,
  so either build reads the other's save - and that trailer is all a slot gets
  without the side file: a USA slot with an Archipelago trailer has no room for
  it at all, a save copied to another machine by hand or brought in by an import
  or a conversion carries the 20 until the game next saves it, and the front end
  has no slot-copy feature of its own. Save states carry the whole list
  themselves (the blob's tail), so a state round trip keeps every mark. The map
  legend lists the pickup shapes in use and the marked-room colour. Console:
  `mark`.
- Skippable cutscenes (F1 > Game > Cutscenes, pause Options > Visor,
  `skippable_cutscenes`, off by default; always on in randomizer and
  Archipelago games) lets Start skip every cutscene, including the ones retail
  never lets you skip and ones not yet watched. It applies randomprime's
  "skippable" room-script patches (`tools/gen_skippable_cutscenes.py`
  generates them from two randomprime ISOs) plus its engine tweaks (rotations
  finish at once, a Reset ends a beetle's emergence, no skip without a
  cutscene camera). Rooms a mod replaced are left unpatched. Archipelago
  games also skip the Landing Site intro: Samus starts on top of her ship.
- 1.00 sequence-break compatibility (F1 > Game > Cutscenes > Revision
  compatibility, `revision_100_compatibility`, off by default; always on in
  randomizer and Archipelago games, where the box shows ticked and disabled)
  restores 1.00 sequence breaks that newer discs (USA 1.01/1.02, PAL) fixed.
  It is independent of Skippable cutscenes and is not under the Original
  experience lock. Changing it needs a game reload: the room scripts are patched
  at load. Script blockers restored on rev2 (USA 1.02): Geothermal Core, Main Quarry and Ore
  Processing lose their added lock actors; Ruined Courtyard gets 1.00's
  trigger/relay wiring back; Elite Research's pickup-gated exit unlock is
  rewired so the Phazon Elite's death unlocks it directly. USA 1.02 Hive
  Totem gets randomprime's boss trigger box back and loses its added door lock;
  PAL keeps Ore Processing's destructible rock and Main Quarry's layer-7 door
  lock switched off and loses Central Dynamo's extra door lock. Only these
  script locks are fixed; missing 1.00 geometry (such as the Ruined Shrine
  root) is not restored, so the setting is not exact all-1.00 behavior.
- Elevator ride (F1 > Game > Cutscenes, pause Options > Visor,
  `elevator_ride`: 0 Original, 1 Fast, 2 Skip; Original by default) sets the
  ride shown between worlds. Retail holds it at least 5 s whatever the load
  takes (`CWorldTransManager`). Fast starts the closing dissolve at 0.5 s and plays it at
  double speed, about 2 s in all. Skip shows black until the next world is
  loaded (about 0.7 s on the development machine), and like retail's
  no-model transition it stops all sound effects at the switch. The elevator
  room's own cinematic is not affected. `MP_SMOKE_ELEVATOR`'s `passed` line
  prints the ride's wall time.
- Boot logos (F1 > Game > Cutscenes, `boot_logos`: 0 Show, 1 Skip with A or
  Start, 2 Off; 1 by default, Show under Original experience) sets the
  Nintendo, Retro and Dolby logos at launch. Skip with A or Start ends the
  current logo with its 0.5 s fade out; Off goes straight to the opening
  (which A or Start already skips to the title). Read at launch. The quick
  resume prompt takes A and Start first while it is up.
- F1 > Debug > Audio holds the audio switches and the MusyX voice list (collapsed), and F1 > Debug > Cheats holds the
  cheats: health, items, ammo, area and world teleport. The cheats stay hidden
  until Show cheats is ticked (`cheats`, off by default). Invulnerable
  (`invulnerable`, off by default) makes Samus take no damage; it stays on
  across runs until unticked, and `MP_GODMODE=<0|1>` overrides it for one run (see below).
- F1 > System > Log, "Write the log to a file" (`logging`, on by default; the older `log_file` key is ignored):
  everything the game prints to stdout/stderr, including the
  line Aurora prints before it aborts, also goes to `metroid_prime_port.log` in
  the user folder; the previous run's is kept as `metroid_prime_port.old.log`.
  Ticking it starts the log at once; unticking stops it at the next start.
  On Linux a forked copy process tees a pipe to the terminal and the file, so
  nothing written before a crash is lost; on Windows the streams go to the file
  only. On Android the logcat lines (port, Aurora, SDL, and stdout/stderr,
  which are copied into logcat under the `stdout` tag while the log runs) are
  written to the file one line at a time; it lives in
  `Android/data/org.metroidprime.port/files/` (reachable over USB) unless the
  data folder was moved to shared storage, where it sits in that folder.
  The file is meant to be attached to bug reports, so the home folder becomes
  `~`, a removable drive's owner (`/run/media/<name>`, `/media/<name>`) becomes
  `<user>`, and on Windows the profile folder becomes `%USERPROFILE%` (any
other `X:\Users\<name>`: `X:\Users\<user>`); the
  Archipelago client leaves out server and slot names. The terminal copy is
  unchanged. Only `MP frame 1` is printed unless `MP_FRAME_LOG=1` is set.
  On Android, unless the data folder was moved, a copy is written to `Documents/MetroidPrime/metroid_prime_port.log` (old
  run: `.old.log`), which the phone's own file manager can open; Android 13+
  hides `Android/data` from it. That needs no permission on Android 11+; a
  file left by an earlier install is not writable, so `metroid_prime_port (2).log`
  and up are tried next.
  `MP_LOG_FILE=<0|1>` overrides the setting for one run (see below).
- Crash report (`platform/port_crash.cpp`): a crash (bad memory access, abort,
  illegal instruction, ...) writes `port: crashed: ...` lines to the log with
  the cause, the faulting `module+0xoffset`, the build and the stack, then hands
  the crash on to the system (core dump, Windows Error Reporting, Android
  tombstone). Linux and Android offsets go to `addr2line -f -C -e <module>`
  of the same build. On Windows the stack has function names and lines when
  `metroid_prime_port.pdb` is beside the exe, and a minidump is written to
  `metroid_prime_port.dmp` in the user folder (open it in WinDbg or Visual
  Studio). `MP_CRASH_TEST=segv|abort` crashes on purpose at startup to check it.
- Save states (F1 > Save states): eight slots in `savestates/` under the pref
  folder (`slot<N>.mpss`). F5 saves to the selected slot and F9 loads it
  (`savestate_hotkeys`, on by default). A state holds the whole game save
  (items, ammo, doors, pickups, map, scans, world layers) plus Samus's room,
  position, facing and morph ball state; loading reloads that world and puts
  her back. The room marks and the seen pickups come with it, whatever the
  card's `mapdata` side file holds: the state carries the whole list in its own
  blob, so a round trip does not shorten it. With `savestate_room_state` (F1 >
  Save states, on by default) it
  also keeps the live script objects of the loaded rooms by editor id: killed
  enemies stay dead, enemies, platforms and moving objects keep position,
  velocity and health, doors keep open or closed (set directly, without firing their connections), timers and counters keep
  their count (about 5 KB, 0.2 ms to capture). Enemy AI state is not kept
  (they resume from their start behaviour), nor are projectiles, cutscene or
  boss-fight progress. States without the section, or with the setting off,
  bring the room back as if just entered.
  A door saved while it was closing comes back closed with that close still owed:
  the door finishes closing, the messages a live close sends still go out (its
  shield rearms) and it opens again as usual. Its close animation replays from the
  start rather than from the exact frame it was interrupted at. A close a refused
  opening or a neighbouring door asked for is kept as well. Snapshots written
  before this carry the open flag alone and are not repaired: a door an earlier
  restore left stuck stays as it is.
  A room's objects are restored only while its script still matches the
  snapshot: each captured room records a fingerprint of the room script it had
  loaded, and a room whose script changed since keeps nothing of the saved
  objects — it comes back as a memory card load leaves it, while the rooms that
  do match still restore. Scripts change when a room is patched differently: a
  mods reload, the Original experience setting (which turns the
  skippable-cutscene room patches on and off), and the scripts a randomizer or
  Archipelago game generates (a changed blast-shield state, for example), which
  fall back room by room the same way (this has not been exercised with a live
  Archipelago game). A snapshot is never rewritten, so a room a mismatched
  restore already damaged stays as it is: load the memory card save to recover
  it. States written before this check existed carry no fingerprint and restore
  as they always did. Every load
  first writes slot 0, so "Undo last load" goes back to where you were.
  Saving is refused while Samus is dead. A state saved on an elevator pad
  rides the elevator after loading, like stepping onto it. Console:
  `state list | last | save [n] | load [n] | undo | slot <n>`.
- Quick resume (F1 > Save states, `quick_resume`, on by default): when the app
  goes to the background (Android) or you quit (desktop, window close, the
  overlay's Exit game or the console `quit`) during play, one more state is
  written to `savestates/resume.mpss`, outside the eight slots (slot 0's undo is
  not touched). The game keeps the last safe point in memory (taken two seconds
  after each room load and then every 30 s while Samus is alive, out of
  cutscenes, elevators and transitions; about 0.3 ms and 4 KB each), so what is
  written is the current moment when that is safe and otherwise that last safe
  point, at most about 30 s old. Each new safe point is also written at once,
  so after a crash, a hang you had to kill or a power loss the next launch
  still offers the last one; the prompt then says the game closed
  unexpectedly (a `savestates/running` marker, removed on a clean quit and
  while Android has the app in the background). If the game closes
  unexpectedly again after resuming, before a newer safe point, the prompt says
  so and starts on No. On Android the write runs from the system's
  pause notification itself, not from the game loop, which the system may block
  before it runs. The file is written to a temp file and renamed. It also records the memory card, file slot and the randomizer seed
  or Archipelago session. At the next launch a window in the middle of the
  screen asks "Resume where you left off?" with the room and time, from the
  Nintendo splash on and through the other splashes, loading and opening logos
  (it holds the attract movie off). Tap or click Yes or No,
  or use the pad or keyboard (D-pad left/right to choose, A or Start to
  answer); Yes starts the file it came from and loads the state like a
  save state, straight from black without the file-select movie (a Yes during a
  splash skips the rest of the splashes; with `savestate_room_state` the room keeps its enemies and doors as saved, for the rooms whose script still matches: see the note under Save states; boss progress is not kept),
  No dismisses the prompt and keeps the file; B does nothing. The file stays after Yes too, until the next safe point replaces it. Nothing is written
  (and a previous resume file stays) on the title screen or before the first
  safe point of a session.
  A resume from another card, randomizer seed or Archipelago session is never
  offered. Console: `resume [status|save|load|clear]`.
- Memory card transfer (F1 > System > Memory card): moves saves between the
  port's card (a GCI folder, `user/USA/Card A` beside the executable, or the current
  Archipelago game's) and Dolphin's. Import takes a Dolphin `.gci`, a whole raw
  card image (`MemoryCardA.USA.raw`, every Metroid Prime file in it) or, from the
  console, a folder of `.gci` files; only GM8E/01 files are taken. The game
  alternates between `MetroidPrime A` and `B` and loads the newer, so an import
  replaces the whole set: the card's existing game files move to `_replaced/`
  (timestamped, never deleted). Imports are refused in game, since the next save
  would overwrite them; at the front end the file select re-reads the card once
  it is idle (`PortGci::RemountIfChanged` in `CSaveGameScreen::Update`). Export
  copies the game files to a folder, or into a raw image when the path ends in
  `.raw` (backed up to `.raw.bak` first; game files not being exported are
  removed from it). Desktop also has Import from / Export to Dolphin, which finds
  Dolphin's user folder (`XDG_DATA_HOME`/`~/.local/share/dolphin-emu`, the
  Flatpak's, `~/.dolphin-emu`; the registry's UserConfigPath or Documents on
  Windows) and uses its `GC/USA/Card A` folder and/or `GC/MemoryCardA.USA.raw`
  (import takes the more recently written one); close Dolphin before exporting.
  Android uses the system pickers: one save dialog per file on export, so keep
  Dolphin's names (`01-GM8E-MetroidPrime A.gci`). "Export as" picks the release:
  the card's files as they are, USA or PAL GameCube (the newest save converted),
  or the Wii Trilogy, written into an existing `save.bin` (Dolphin's
  `Wii/title/00010000/52334d45/data/save.bin`, or a decrypted one; backed up to
  `save.bin.bak`): the newest save replaces the Metroid Prime game the Trilogy
  last used (it shows one per Mii, so start one there and save once first);
  Echoes/Corruption are kept. A Wii `data.bin` is console-signed and
  cannot be written. The buttons also work with PrimeHack (its Flatpak's
  `~/.var/app/io.github.shiiion.primehack/data/dolphin-emu` is searched too).
  `emulator_folder` (empty = auto) names the Dolphin/PrimeHack user folder (the
  one with `GC/`; a `GC`, region or `Card A` folder is accepted too) for portable
  installs; when set it is the only folder looked at. `share_emulator_card`
  (default off, desktop, console `gci share on|off`, `gci emulator <folder|auto>`)
  mounts the emulator's `GC/<region>/Card A` as the port's card, so both read and
  write the same files live; it applies at the title screen (and at boot, so
  quick resume saves there too) and is refused in game. Never run both at once:
  one overwrites the other's saves. GameCube Metroid Prime only; the Trilogy save
  cannot be shared. The card needs to exist (set the emulator's Slot A to "GCI
  Folder", start the game once). Randomizer/Archipelago keep their own cards.
  "Copy port saves into shared card" copies the port's own card over (its old
  files move to `_replaced`). Logic in `platform/port_gci.cpp` and
  `platform/port_save_convert.cpp`, covered by `port_gci_tests` and
  `port_save_convert`. Console: `gci list | import <path> | export [usa|pal]
  <dir or .raw> | export trilogy <save.bin> | dolphin import | dolphin export
  [usa|pal|trilogy]`.
- Unlocks (F1 > Game > Unlocks, persisted as `unlock_hard_mode`, `unlock_fusion_suit`,
  `unlock_galleries`, all off by default): offer what finishing the game
  unlocks without finishing it. Hard mode adds Normal/Hard to a new file; the
  Fusion Suit adds its Disabled/Enabled row under Metroid Fusion Connection
  Bonuses (retail needs a GBA link) and implies normal mode beaten; galleries
  opens all four image galleries. The save's own flags are never written, so
  switching an unlock off locks the extra again (switching the Fusion Suit off
  also takes the suit off unless the save really has it). Metroid (NES) stays
  locked, since its emulator is a PowerPC REL the port can't run.
- The in-game pause and map screens (`FRME_PauseScreen`, `FRME_PauseScreenInstructions`,
  `FRME_MapScreen`) are aspect-matched like the HUD, so they keep their
  proportions and spread across a wide viewport instead of stretching.
- The front end (`FRME_FrontEndPL` title/menu, `FRME_NewFileSelect`, the GBA
  screens) is aspect-matched too, so the title screen is pillarboxed rather than
  stretched.
- The mouse cursor is hidden while the game has focus and is shown only over the
  F1 overlay. The overlay is also openable and navigable with a controller: the
  Start+Back chord toggles it (Start is suppressed for the game while Back is
  held, so it does not also pause), the D-pad or left stick moves, A activates, B
  cancels and the shoulder buttons switch tabs (ImGui gamepad navigation).
- `MP_MOUSE_AIM=1`, `MP_MOUSE_SENS=0.0035`: relative mouse aim. Motion is ignored
  while the overlay is visible or relative capture/focus is absent. Mouse mode
  uses immediate yaw/pitch with an approximately ±87° pitch range. Up moves aim
  up by default; `MP_MOUSE_INVERT_X=1` and `MP_MOUSE_INVERT_Y=1` invert either axis.
  `MP_MOUSE_SENS` and `mouse_sensitivity` are radians per mouse count; the F1
  slider shows them on the Quake/Source/Apex scale (0.022 degrees a count per
  unit, so the default 0.0035 is about 9.12), with a cm/360 readout for
  `mouse_dpi` (display only, default 800).
  SDL and the compositor own pointer locking; capture is released outside
  playable first person (menus, cinematics, and scripted input locks). Morph ball
  has no mouse aim but keeps the pointer locked, so a bomb click can't land
  outside the window (the pause and map screens release it).
  Mouse aim replaces the R-button free look, so switching it on in F1 also
  switches on twin stick (still its own setting): the mouse and a pad's right
  stick then add into the same aim.
- In mouse mode the five mouse buttons act as pad buttons. They're bound on
  Controls > Remap (Keyboard & mouse); the saved `mouse_left`, `mouse_middle`,
  `mouse_right`, `mouse_x1`, `mouse_x2` lines (none, a pad button, a D-pad
  direction or the beam shift) are only its default rows. By default **left-click fires / holds a charge / releases a charged
  shot** (A), **right-click holds lock-on** (L) and **middle-click fires
  missiles** (Y); the side buttons are unset. Either keyboard
  preset restores them too. They feed the normal PAD/gun input path, preserving charge
  timing and weapon cooldowns, and L/R also press the analog trigger fully.
  Keyboard/controller bindings still work alongside them.
  `MP_DISABLE_MOUSE_BUTTONS=1` opts out. Outside playable first person (morph
  ball, text boxes, menus) only the buttons set to A or B count, so the left
  button still lays bombs and advances text. Every mouse button only counts
  after it has been seen released.
- The mouse wheel cycles beams (up: previous, down: next; off under Original
  experience), as PrimeHack does, in the pause screen's order (Power, Wave,
  Ice, Plasma), skipping beams not yet found. Remap also has "Next/Previous
  visor" (Combat, Scan, Thermal, X-ray), unbound by default; bind them, or the
  beam ones, to any input, such as the side mouse buttons or `wheel:left|right`.
- Outside lock-on, A/D (the left-stick lateral axis) strafe in mouse mode rather
  than applying the console's turning torque. Movement uses the current mouse
  heading and the game's acceleration, friction, surface restraints and collision
  handling. Diagonal input/speed is bounded. Lock-on keeps its native orbit/dash
  behavior; closing F1 does not require reacquiring a lock to strafe.
- Mouse mode requests the GC aiming crosshair without holding R or entering the
  console's movement-restricting free-look mode. While locked on (aim follows the
  target) the crosshair shrinks away as in the original and returns on release
  (issue #112). `MP_DISABLE_MOUSE_CROSSHAIR=1`
  opts out. The Controls > Mouse settings sub-tab exposes inversion, weapon-button and crosshair toggles.
- Crosshair size (Controls > Mouse settings and pause Options > Controller, persisted as
  `crosshair_size`, 25-100 percent, default 50): scales the free-aim crosshair
  under mouse aim and twin stick, where it is always shown and the retail size
  covers much of the view. Holding R without either keeps the retail size.
  Console: `crosshair <25..100>`.
- Pointer aim (Controls > Options > Aiming, `pointer_aim`, default off,
  `MP_POINTER_AIM=1`; needs Mouse aim, Twin stick or the modern touch layout): the
  Trilogy/Prime 3 scheme. The mouse, twin stick's right stick, gyro aim or a touch aim
  drag moves the crosshair over the screen and shots leave along the ray through it;
  the gun model turns toward it, at most 25 degrees. Past the dead box (`pointer_aim_box`,
  20-95 percent of the half screen, default 70) the view turns toward that edge, up to
  `pointer_aim_turn` degrees/second (30-720, default 180) at the edge itself. The mouse
  sensitivity slider scales the pointer speed for every input (the default follows the
  cursor), on top of the stick aim, gyro and touch aim speeds; Invert X/Y do not apply. The crosshair is always shown. Lock-on, scanning and grappling
  re-centre the pointer and aim as before; auto-aim does not bend pointer shots.
  Lock-on owns the camera while held; releasing it resumes at the actual locked
  direction rather than at accumulated mouse angles. Jump/fall auto-pitch is
  bypassed during free mouse aim. Capture/UI transitions cancel held charges and
  require mouse-button release before another mouse shot can begin.
- `MP_DISABLE_AI_AUDIO=1`: start streamed AI audio muted. It can subsequently be
  enabled from the overlay. MusyX mute is independent.
- `MP_FAST_BOOT=1`, `MP_SKIP_CUTSCENES=1`, `MP_CUTSCENE_SPEED=8`,
  `MP_SHOW_DEBUG_UI=1`: development controls. On/off variables share one rule (`platform/include/port_env.h`): unset
  leaves the default, empty/`0`/`false`/`off`/`no` mean off, anything else on. Cutscene speed is restricted to 1–32.
  Cutscene skipping is only available this way, for tests: it is not a player
  setting, since skipping every cinematic at once broke script state.
  `MP_DEBUG_TAB=<page>` (e.g. `Video/Quality`; `Chat` opens Archipelago on its Chat sub-tab) opens the desktop overlay on that page,
  enlarged, for captures. A sub-tab takes `Page/Subtab` (e.g. `Video/Quality`), and the old names still work as aliases: `Input` for Controls, `Render`/`Performance` for Video, `Extras` for Game, `States` for Save states, `Session`/`Chat` for Archipelago. The console's `shot` leaves the overlay out; grab the
  X display instead (PIL `ImageGrab.grab(xdisplay=':99')` under Xvfb).
- `MP_BOOT_WORLD=<MLVL hex>[:<MREA hex>]`: tests only. Skips the splash screens and
  the front end and starts a new game (default options, no save card) in that world:
  in its default area, or in the given MREA. It applies once, so quitting the game
  returns to the normal front end. For example, `83F6FF6F` gives Chozo Ruins' default
  room (MREA 3E6B2BB7) in first person about 5 s after launch, and
  `39F2DE28:B2701146` gives the Landing Site. It needs no smoke build, and the
  console's `status` answers as soon as the room is up.
- `MP_TURBO[=<ticks>]`: lockstep for automated runs. Every frame runs exactly
  `<ticks>` fixed ticks (default 1, at most 16) with no frame limiter and no
  vsync, so a run goes as fast as the machine renders it; game time per tick
  stays exact. Under Xvfb presentation caps near 100 fps, so extra ticks per
  frame are what give the speedup: 3000 ticks took 50 s at real time, 30 s at
  `MP_TURBO=1`, 7 s at `4` and 3.3 s at `8`. Audio and streamed music do not
  keep up. Not saved to the settings file. On exit the port prints
  `MP run: <frames> frames in <s> s`.
- `MP_PRESENT_T=<0..1|cycle|tick>`: force the frame-interpolation factor, even
  under `MP_TURBO` or with the frame limiter on (testing only, not saved).
  `cycle` steps through 0, 0.25, 0.5 and 0.75 per drawn frame; `tick` draws the
  plain tick state, as the frame limiter does. The console's `present` sets it
  live; see `docs/FRAME_INTERPOLATION.md` (section 7) for the comparison
  recipe.
- `MP_PASS_LOG=<N>`: logs every render pass of one frame in N (draws, load and
  store ops, resolve copy), to find what splits the frame into passes. In
  Landing Site gameplay the world pass is broken by bloom and, on frames with
  heat haze or rain, by the indirect-particle screen copy.
- `MP_CONSOLE=<port>`: debug console; see `docs/CONSOLE.md`.
- Free camera (F1 > Debug > Camera, or the console's `freecam`): the world is
  drawn from a viewpoint of its own while the player stays put, without the
  HUD and the arm cannon. It starts at the game camera; the left stick moves,
  the C stick (or a captured mouse) turns, R is four times as fast, L and Z
  (or the d-pad's down and up) go down and up. `freeze` stops the game's simulation while it flies,
  which also makes two screenshots of one view comparable. Samus's body is
  drawn where the player stands (`player off` or "Show Samus" hides it). The Debug page's Remastered section has the console's `view`, `probe`, `hdfont`, `roomgeo` and
  `roomenv` switches.
- `MP_PBR_PROBE=<off|on|mirror|window>` (or 0-3): the reflection probe PBR mod
  materials reflect, on by default. The console's `probe` changes it live.
- `MP_PBR_ANISO=<1-16>`: the most anisotropic filtering a PBR mod's native maps
  take, 2 by default whatever the Anisotropy setting says: higher levels turn a
  tiled floor into streaks towards the horizon.
- `MP_GODMODE=<0|1>`: the Invulnerable cheat for this run, whatever the setting
  says. The console's `god [on|off]` changes the setting itself.
- `MP_LOG_FILE=<0|1>`: the file log (`metroid_prime_port.log` in the user
  folder) for this run, whatever the `logging` setting says.
- `MP_COLLISION_VIEW=<off|overlay|only>`: draw collision from the start, as the
  console `collision` and F1 > Debug > Rendering > Collision.
- `MP_ROOM_GEO=<0|1|overlay>`: whether a mod's `.roomgeo` replaces an area's
  geometry (default 1; `overlay` draws both). Console `roomgeo [on|off|overlay]`,
  which also prints what is loaded and what the last frame streamed.
  `MP_FRAME_BUFFERS=<1..16>` scales the buffers a frame's vertices, indices,
  arrays and uniforms are streamed through (1 = 5 + 2 + 8 + 24 MiB); it is 12
  when a mod has room geometry and `MP_ROOM_GEO_RESIDENT` is off (6 on Android),
  and 2 with it on (the default).
  A frame that outgrows them aborts with a buffer overflow.
  `MP_STORAGE_BUFFERS=<1..16>` pins the storage buffers' scale, which otherwise
  follows it and, with `MP_ROOM_GEO_RESIDENT` on, grows after an overflow (up to 6x).
- `MP_ROOM_GEO_RESIDENT=<0|1>` (setting `room_geo_gpu`, F1 > Debug >
  Remastered room models > Keep on the GPU; on by default, read at
  startup; an old `room_geo_resident=1` still turns it on). On a POCO F8 Ultra it
  gives 8 to 27% more frames per second in the heaviest rooms; on a desktop GPU it
  makes no measurable difference. On an integrated GPU (Ryzen 7000 Raphael) the
  results were mixed: GPU time was the same or lower in every room, and up to 4 ms lower in Frigate Crash Site
  (65 to 97 fps), but turning around in Main Plaza ran at 80 fps with it against
  96 without. Each room geometry model's vertex arrays and display lists are
  uploaded to the GPU once when it loads and drawn from there, instead of being
  copied into the frame's buffers every frame. The frame buffers then start at 2
  times the usual size instead of 12 (6 on Android), and 256 MiB (128 on
  Android) is set aside on the GPU for the models, two fifths each for vertices
  and arrays and a fifth for indices: `MP_ROOM_GEO_RESIDENT_MB=<16..2048>` sets
  that. Console `stats` and the F1 > Debug > Rendering frame statistics show how much of it is in use. A
  model that finds no room left is sent every frame as before, which can
  overflow the smaller frame buffers: raise the room if the log warns of it.
- `MP_ROOM_GEO_MIN_PX=<n>` (setting `room_geo_min_px`, F1 > Debug >
  Remastered room models > Skip small models, console `roomgeo minpx <n>`; default 0): leaves out room
  geometry instances that span fewer than n pixels of the game's 480-line
  screen.
- `MP_ROOM_GEO_LOD=<scale>` (setting `room_geo_lod`, F1 > Debug >
  Remastered room models > Detail distance, console `roomgeo lod <scale>`;
  default 1): the import also converts each room model's coarser Remastered
  levels of detail (a level is kept only when it has at most 3/4 of the
  triangles of the one before) and lists them in `roomgeo/lods.bin`. An
  instance switches to a level once the eye is past that level's distance from
  its bounds, in the model's own units, times the scale; 0 keeps the full
  models. Levels load after the full models and hold nothing up. Merged copies
  (`MP_ROOM_GEO_MERGE`) get each level merged too, once all are in, and switch
  together by the distance to the cluster's bounds (so the eye inside a cluster
  keeps it at full detail). `roomgeo mat` overrides reach only the full model.
  `roomgeo` prints how many instances were drawn coarser.
- `MP_ROOM_GEO_MERGE=0`: draw every room geometry instance on its own. By
  default, once an area's models are in, the copies of a small opaque model
  that nothing moves, hides or fades (four or more in the area) are built into
  one model per 64-unit cell, at their world places, sharing one set of lights:
  the Landing Site's 4133 instances become 188 merged models and 176 single
  ones. A phone pays for every draw on each screen tile, so this took a POCO F8
  Ultra from 24 to 35 fps there. Needs the mod's CMDLs stored uncompressed.
  F1 > Debug > Remastered > Draw merged copies (not saved)
  switches back to single draws at runtime, to compare how they look.
- `MP_ROOM_LIQUID=0`: ignore the mods' `.roomliquid` files and draw the
  retail fluid planes (console `roomliquid [on|off]`, which also counts what is
  loaded and drawn).
- `MP_ROOM_ENV=0`: ignore the mods' `.roomenv` files (console `roomenv
  [on|off]`, which also counts what is loaded). For tuning:
  `MP_ROOM_ENV_GAIN` (exposure, default 1), `MP_ROOM_ENV_LOD` (a cap on
  the mip a roughness of 1 reflects; unset, the cube's own top mip, which is
  what Remastered reads) and `MP_ROOM_ENV_AMBIENT` (scale of the
  baked ambient light, default 1; 0 keeps the game's ambient colour).
  `MP_ROOM_ENV_EXPOSURE=0` (console `roomenv exposure on|off`) exposes each
  cube on its own instead of the frame by the camera's room, and drops the
  room's tone curve. Files older than version 4 lack the exposure bias and
  the curve's contrast, so re-import to get the right levels.
  While room geometry is on screen the exposure follows the drawn picture's
  average, held to the room's AutoExposureHint range and eased by its sigma
  (Remastered's rule); a room change eases, a world load or warp snaps.
  `MP_ROOM_ENV_AUTO_EXPOSURE=0` (console `roomenv auto on|off`) keeps the
  probe-based exposure. Version 7 adds the hint's sigma and static lerp; older
  files use 32 and 0.5.
  Opaque PBR models' glow and unlit colour are drawn at the room's static
  exposure (the hint range at its static lerp), as Remastered's opaque pass
  does, so they scale by 2^(static EV - frame EV); blended surfaces keep the
  frame's exposure. `MP_ROOM_ENV_STATIC_EXPOSURE=0` (console `roomenv static
  on|off`) draws every glow at the frame's exposure; `roomenv info` prints the
  static EV and the scale.
  A PBR model lit by the bake (room exposure on, or the per-pixel volume)
  leaves out the area's lights and keeps only the runtime ones (gun light,
  ball glow, projectiles), as Remastered, which bakes the area's light into
  the grid; adding them as well blew actors out near lamps.
  `MP_ROOM_ENV_AREA_LIGHTS=1` (console `roomenv arealights on|off`) adds them
  back. This also applies to room geometry under `MP_ROOM_GEO_AREA_LIGHTS=1`.
  `MP_HDR_SCENE=1` opts into experimental HDR scene rendering. It is default-off and only
  applies to Remastered room-environment scenes with a valid tone curve in the Combat or Scan
  visor, outside visor transitions; Thermal/X-ray, HUD/UI, and inventory remain LDR. This is
  not full beam/effect parity: other legacy effects may still use inherited exposure.
  The morph ball's light is Remastered's HDR one on PBR surfaces: a hotspot
  within 2.3 units of the ball, which brightens with boost and in water (see
  `docs/REMASTERED_EFFECTS.md`, "HDR dynamic lights").
  `MP_REMASTERED_BALL_LIGHT=0` keeps retail's light, and
  `MP_REMASTERED_BALL_LIGHT_SCALE` scales it (console `roomenv balllight
  on|off|<scale>`).
  A power bomb tints the baked light of PBR surfaces orange, as Remastered's
  does (the tint rises linearly from 1x (no tint) to 35x between 1.75 s and 3.5 s into the blast, holds to 4 s, and is gone by 4.5 s; it
  also dims the probe occlusion less). `MP_REMASTERED_BOMB_TINT=0` turns it off.
  Reflection probes blend around the camera, as Remastered's do: up to four
  of the loaded areas' probes, each fading out over its padding outside its
  box, higher priority first, are mixed into one cube that every PBR model
  reflects, and the reflection is dimmed where the baked light is below the
  probes' intensity range (Remastered's probe occlusion). Version 8 adds the
  padding, priority and intensity range; older files fade over 1 m with no
  occlusion. `MP_ROOM_ENV_BLEND=0` (console `roomenv blend on|off`) lets each
  model pick its own probe again; `roomenv info` lists the blend's probes,
  weights and fades, and every probe's box centre.
  `MP_BLOOM=0` (console `roomenv bloom on|off`) drops Remastered's bloom
  (version 5: the room's BloomEffect, else the world's), and `MP_COLOR_GRADE=0`
  (console `roomenv grade on|off`) its colour grade (version 6: the world's and
  the room's global ColorGrade LUTs with their layers and fade times; the last
  one whose layer is active is drawn, crossfading when it changes). Neither
  is drawn under the Thermal or X-ray visor.
  Room geometry is lit by the baked ambient grid per pixel, as a 3D texture,
  and takes no area lights (Remastered has no lightmaps; this grid is its room
  lighting). `MP_ROOM_ENV_VOLUME=0` goes back to the area's lights, as does
  `MP_ROOM_GEO_AREA_LIGHTS=1`. For tuning: `MP_ROOM_ENV_VOLUME_BIAS` (metres off
  the surface a sample is taken; default 0, as Remastered samples at the
  surface) and `MP_ROOM_ENV_VOLUME_SHOW` (1 draws the texture coordinates, 2 the light alone, 3 the shading normal).
  `MP_ROOM_ENV_FILL_LAYERS=<n>` (for comparisons; default 0, as Remastered) lets
  the grid's empty points take their lit neighbours' light first, n layers deep.
  The rooms' baked lightmaps light room geometry in place of the coarser grid:
  `MP_ROOM_ENV_LIGHTMAP=0` leaves them out, and `MP_ROOM_ENV_LIGHTMAP_SCALE`
  scales them (default 1; 0 drops them too).
  `MP_ROOM_LIGHTS=0` drops Remastered's point and spot lights of the loaded areas
  (only drawn while the rooms are exposed). `MP_REMASTERED_GLOW_EXPOSURE=0` scales
  glow by a fixed 0.10, the converter's old factor, instead of the room's exposure.
  `MP_VOLFOG=0` (console `roomenv volfog on|off`) drops Remastered's volumetric fog
  and fog regions (files of version 12 on). `MP_THERMAL_POST=0` and
  `MP_XRAY_POST=0` keep the retail Thermal and X-ray visors instead of
  Remastered's visor post effects (both are retail under the original experience,
  and Thermal also with a mod that lacks the imported gradient).
- `tools/pbr_shots.py`: contact sheets of models under PBR, for comparing mod
  builds. One game per (variant, place), each booted straight into the room
  with `MP_BOOT_WORLD` on its own console port and an Xvfb display, about 10 s
  a game. `--variant name=<mod dir>|none` (columns), `--place
  chozo|tallon|frigate|phendrana|mines|magmoor|crater`, `<MLVL>[:<MREA>]` or
  `name=<file.mpss>` for an exact spot from a save state, `--model <cmdl>`,
  `--probe off,on`, `--yaw`. Writes `<out>/<place>.png` and a `.tsv` with the
  mean luminance of each cell, and exits 1 naming any game that never reached
  first-person gameplay. Needs a smoke build (`--build`, default
  `build/fm-smoke`) and `MP_ISO` or `--iso`. Do not use `MP_SMOKE_WORLD` for
  this: it starts a new game and only warps once the Frigate intro has played.
- `MP_TOUCH_UI=1`: force the page layout for the debug overlay even when
  System > Overlay > "Overlay as a floating window" is set. The page layout is the default
  everywhere (the only one on Android): a full-screen window inside the safe
  area, with a page list instead of tabs, larger hit targets, drag-to-scroll
  with fling for touches and a Close button.
- `MP_VALIDATE_SAMPLES=1`: log MusyX sample-directory validation.
- `MP_SIM_RATE=<hz>`: experimental simulation tick rate (30–480, default 60).
  60 is console-accurate; higher values step the game logic at the display rate
  instead of interpolating the camera. `MP_SIM_ADAPTIVE=1` instead takes one
  step per frame with `dt` = the measured frame time (clamped 30–480 Hz), so a
  variable frame rate is matched exactly. Both are also settable from F1 > Video > Frame rate and persisted. See `docs/HIGH_FPS_AUDIT.md` for what still
  assumes 60 Hz.

The simulation uses a fixed-step accumulator (60 Hz by default) independently of
the presentation cap. Ordinary slow frames catch up; pauses/debugger stalls are
capped to 250 ms of simulation work per iteration. Audio runs on
wall-clock/device consumption.
Fractional simulation time is retained through frame jitter. The capped scheduler
can borrow at most 0.25 ms near a tick boundary and carries that debt forward, so
it does not alternate zero/two ticks merely due to microsecond sleep jitter.
The Video > Frame rate page distinguishes the **60 FPS target** from measured render FPS
and simulation ticks/second. `MP_TRACE_TIMING=1` logs both rates once per second.
Hidden windows continue pumping events and main-thread audio without recording
rendered frames. Restart-to-menu rebuilds the game architecture instead of
attempting a console reboot.

In uncapped presentation, free mouse aim uses the current simulation orientation
so the visible reticle does not lag the shot direction; camera translation still
interpolates. The held cannon/arm and muzzle effects render against the matching
simulation camera, then restore the world view before world-space effects. This
keeps the viewmodel stable instead of mixing an interpolated view with a cached
60 Hz gun transform. Weapon animation and projectile simulation remain 60 Hz.

"Smooth uncapped frames" (`smooth_frames`, F1 > Video > Frame rate, on by default)
turns on the four parts below together; they only apply with the frame limiter
off. The console's `interp actor|pose|particle|all <0|1>` sets a part on its own
for testing, which isn't saved.

Per-frame look turns the presented view every rendered frame by the
look input the next tick will consume: pending mouse and gyro deltas, plus
twin-stick velocity times the time since the tick. The tick still applies the
whole amount, so aim and shots are unchanged; the free-aim crosshair is rotated
with the view so it stays centred. It only applies with the frame limiter off,
under free mouse look (mouse aim, gyro aim or twin stick). The game's own stick
look is not previewed.

Smooth actor motion draws each moving actor, Samus and the morph ball included,
between its last two tick transforms when the frame limiter is off. Moves of
more than 4 units or 45° in a tick snap. The arm cannon's bob and sway blend
too. Queued particles, shadows and the HUD sway still step at 60 Hz. `docs/FRAME_INTERPOLATION.md` has the design and scopes the rest
(particles, projectiles).

Smooth animation skins animated models with a per-bone blend of the poses built on
the last two ticks when the frame limiter is off. The animation tree is not
touched, so events, sounds and particles are unchanged; attachments on locators
and swarms stay on the tick pose. A bone that turns more than 45° or moves more
than 4 units in a tick snaps the whole pose.

Smooth particles draws particle effects, projectile effects included, between
their last two 60 Hz positions when the frame limiter is off, using the game's
own sub-frame particle path. Beam trails (swooshes), electric effects and
the flamethrower still step at 60 Hz.

## Diagnostics and test drivers

For development only; none is saved. A variable marked "set" takes effect with
any value, even `0`.

- `MP_WATCHDOG=0`: no stall watchdog. On by default: with no frame for 5, 15, 30
  and 60 s it logs where the main and render threads are, the main thread's stack
  at 5 and 30 s, and on Android a logcat dump. `MP_WATCHDOG_TEST_STALL=<frame>`
  sleeps 8 s on that frame and `MP_WATCHDOG_TEST_THROW=<frame>` throws an exception
  there, to test it and the crash report.
- `MP_HEAP_CHECK=1` (not empty, not starting with `0`): deep checks of the game
  heap's block links on every normal-heap free and of each frame's delayed deletions (queued
  twice, freed or reused before the flush); a fault logs `HEAP CHECK:` lines and aborts.
- `MP_PIPELINE_SEED=0`: start from an empty pipeline cache instead of the shipped
  seed (`tools/pipeline_seed.py` records with it).
- `MP_HOLD_FRONTEND_SHADERS=<0|1>`: hold the seed's background shader builds while no
  game is loaded. On by default under D3D11 only, whose drivers made the front end
  crawl behind them (issue #54).
- `MP_SKIN_LEGACY=1`: skin with the retail locked-cache path; `MP_SKIN_VERIFY=1`
  runs both paths and reports any difference; `MP_SKIN_STATS=1` logs skinning time
  every 5 s. `MP_SKIN_THREAD_MIN=<vertices>` (default 20000): models at least
  that big are skinned across threads. (`1` here means not empty and not starting with `0`.)
- `MP_LOG_HUD` (set): log HUD models and frames that are not loaded, HUD state
  transitions and visor/HUD visibility changes to stderr.
- `MP_STREAM_TRACE` (set): per-chunk streamed-music refill tracing (`mpstream:`;
  logcat on Android); starts, stops and stale completions are always logged.
- `MP_ELC2_LOG` (set): log Remastered electric effects (ELC2) as they are made,
  and their bolt count every 30 frames.
- `MP_ACTOR_SHADOW_DEBUG` (set): every 120 frames, log each projected actor
  shadow's opacity, fade and map and receiver boxes.
- `MP_SHADOW_ACTOR_CHECK=1`: log the first off-screen actor drawn into the sun
  shadow, and every 300 frames how many were eligible and drawn.
- `MP_VISORMESH_LOG` (set): every 60 frames, log how many surfaces the visor and
  shadow bits skipped.
- `MP_ROOMENV_TIMING=1`: log where a `.roomenv` load's time goes.
  `MP_ROOM_ENV_EXPOSURE_LOG=1`: log the room exposure state (area, hint values,
  EV, target) each time it is worked out.
- Remastered import: `MP_REMASTERED_REUSE=0` makes every stage again instead of
  reusing the previous import's unchanged ones (`textures`: only the converted
  textures are reused); `MP_REMASTERED_JOINTS=1` logs the models' skin matching
  (each joint's vote and the bones it got) to stderr; `MP_REMASTERED_MISS` (set)
  lists each room's untraced script connections by kind in the import log.
- `MP_RANDO_SWEEP_LAYERS=1`: with `MP_RANDO_SWEEP`, tour each area once per script
  layer (up to the world's widest area), with only that layer active.
- Smoke driver (`-DMP_ENABLE_SMOKE_DRIVER=ON` builds, see `docs/NATIVE_PORT.md`):
  `MP_SMOKE_WORLD_RESTARTS=<n>` repeats the `MP_SMOKE_WORLD` jump n times (default
  1); `MP_SMOKE_WORLD_TICKS=<n>` waits n gameplay ticks before each (default 120,
  and when set it does not wait for the first-person camera);
  `MP_SMOKE_MORPH=1` presses X 120 and 360 ticks into first-person gameplay
  (morph, unmorph); `MP_SMOKE_SAVE=<ticks>` opens the in-game save screen that
  many ticks in and confirms it (answering one "file corrupt" by deleting the file);
  `MP_SMOKE_WATER=<cycles>` puts the player in the area's first active water for
  2 s, walking forward, then back out for 2 s, that many times; `MP_SMOKE_RESIZE=<w>x<h>`
  resizes the window at frame 300.

## Photo mode

A frozen free camera for screenshots, entered from the pause menu's "Photo
mode" row (works with touch), the Port binding "Photo mode" (F8 by default,
rebindable on the Controls page), F1 > Game > "Photo mode (keys)", or the
console's `photo`. It is refused, with a toast, outside gameplay and while
Samus is dead. It works during cutscenes too (the pause menu is not available
then, so use F8, the F1 button or `photo on`): the cutscene freezes where it
is, the free camera starts at the cinematic camera's view, and leaving resumes
the cutscene from the same frame, without skipping it. Entering freezes the
simulation and starts the free camera at the game camera; the HUD, arm cannon, helmet and visor effects
are hidden. Leaving restores all of them, and the free camera and freeze
states the session had before.

The panel (top right; H brings back a hidden one) has FOV (20-120), Roll
(-180 to 180, a real roll of the view), Show Samus, Camera speed, the
1x / 2x / 4x supersample choice, Capture, Hide panel and Exit. The camera
moves as the free camera does. On a pad: A captures, B or Start exits, the
d-pad rolls, X plus the d-pad changes the FOV, and the sticks fly. Y shows a
hidden panel, and when it is shown gives it the pad: the d-pad moves between
the widgets (a slider changes with left/right), A presses or toggles, and Y or B
hands the pad back to the camera (Start still exits). The camera is still while
the panel has the pad. None of this reaches the game. The sliders also take
mouse or touch. On a touchscreen the touch controls hide for all of photo
mode: a finger on the left half is an invisible stick centred where it lands,
one on the right half drags the view, and a hidden panel leaves a small bar
(Panel, Capture, Exit) in its place. With twin-stick aim the right stick turns
the camera.

On a keyboard and mouse (whatever the mouse aim setting): the mouse looks
around while the panel is hidden, or while the right button is held with it
shown; a left click then captures. The wheel changes the FOV, P captures, H
shows or hides the panel, and Esc exits. The keys bound to the pad buttons
above still work as those buttons.

Capture writes `metroid-prime-photo-<time>.png` to the screenshot folder with
the panel, overlays and toasts hidden. On Android it is then moved to the
shared `Pictures/Metroid Prime` folder, where the gallery shows it (if that
fails it stays in the app's `screenshots`). 2x and 4x raise the render scale
temporarily (the base is the window's pixel height / 480) and render the
frame at that size. The result is at least N times the 1x size in both
directions and exactly N times where the rounding allows: the frame buffer is
the scaled height with the width rounded from the aspect, then fitted to the
render mode's 854:480, so a 1280x720 window (1280x719 at 1x) gives 2562x1440 or
5124x2880 rather than exactly 4x. A size over the GPU's texture limit or
past 48 M pixels (96 M samples with MSAA) steps down to 2x, then 1x, and the
Saved toast says so. Very large captures can stall a frame for a second or
more.

Saved: `photo_supersample` (1, 2 or 4) and `photo_speed` (camera speed).
Filters, vignette and depth of field are not part of it.
