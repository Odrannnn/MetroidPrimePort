# Frame interpolation: scope

Status: phase 1 (look input per frame) is done; phase 2 (actor transforms),
phase 3 (skinned poses) and phase 4 (`CElementGen` particles) are done; phase 5
(the sweep, section 7) found and fixed the arm cannon's bob; phase 6 (swooshes,
electric effects and beam weapons, section 3) is done. One setting,
`smooth_frames` ("Smooth uncapped frames", on by default), turns all of them on
or off; the parts are still separate flags in code (`PortDebug::ActorInterpolation`
etc., console `interp`), and the per-part names used below are those flags. The
HUD sway is scope only.

Goal: smooth motion above 60 FPS while the game logic stays at its console rate
(60 Hz fixed step). Rendered frames between two ticks draw the world at a blend
of the last two simulation states, and first-person look input is applied every
rendered frame. The alternative, running the simulation itself faster (`sim_rate` /
`sim_adaptive`), stays a very experimental option, for testing only; see "Why not `sim_rate`" below.

## What exists

- **Clock.** `PortTiming::FixedStepClock` (`platform/include/port_timing.h`)
  keeps the leftover time; `Interpolation()` = leftover / period, exposed as
  `CGameArchitectureSupport::GetTickInterpolation()`.
- **Camera.** `CCameraManager::Update` records the camera transform before and
  after each tick (`sPreviousCameraTransform` / `sCurrentCameraTransform`).
  A camera switch, a jump of more than 4
  units, or a turn of more than 45° (outside free mouse look) resets the
  previous snapshot, so cuts don't smear. `CMain::RsMain`
  (`src/MetroidPrime/main.cpp`) sets the blend
  factor around `IOWinManager().Draw()`, only when the frame limiter is off;
  `GetCurrentCameraTransform` then returns the lerped/slerped view.
- **Mouse aim.** Under free mouse look the view keeps this tick's orientation
  (only translation is blended), so the reticle never trails the shot
  direction. The arm cannon, arm and muzzle effects render against the matching
  simulation camera (`NATIVE_PORT.md`, "In uncapped presentation").
- **Per-frame look (phase 1).** See section 4.
- **Actor transforms (phase 2).** See section 1.
- **Skinned poses (phase 3).** See section 2.
- **Render-time animation.** `CGraphics::TickRenderTimings` advances draw-time
  timers (texture scroll, UV animation) by whole ticks, not by frames.

So at 144 Hz today the camera and free look glide, and actors, animated
poses, particles, swooshes, electric effects and beam weapons blend toward
the next tick when `actor_interpolation`, `pose_interpolation` and
`particle_interpolation` are on (all off by default). Whatever those settings
leave out still steps at 60 Hz: bounds, culling, lighting and shadows stay on
the sim transform, and attachments read the sim pose (sections 1-3).

## What is needed

### 1. Actor transforms (done, `actor_interpolation`)

Implemented as a view shift rather than by swapping the transform in every
`Render` override (about 100 files):

- Every arch tick calls `CActor::PortBeginTickSnapshot()` (bumps a generation);
  `CStateManager::Update` then copies each actor's `x34_transform` into
  `xPortPrevTransform` with that generation. A tick that doesn't reach
  `Update` (pause) and actors created mid-tick leave the generation stale, so
  they draw at the sim transform.
- `CActor::PortPresentedView` builds the rigid blend R (slerp of the normalised
  bases, lerp of the translation) and the rigid current transform C, and
  returns the view `C * R^-1 * view`. Drawing the actor under that view puts
  the model, its attached particles and its lights-relative shading at the
  blend, whatever the override does. Scale stays on the sim transform.
- `CPortActorRenderScope` (RAII) sets and restores that view around each actor
  draw in `CStateManager`: `RecursiveDrawTree`, `RendererDrawCallback`,
  render-first/last lists, area `AddToRenderer`, the thermal passes and the
  morphing player. Nested scopes keep the outer view. The player's
  `AddToRenderer` is not wrapped (it also queues the gun); its body and ball
  draw through the callback, which is.
- Snaps like the camera: more than 4 units or more than 45° in a tick, and
  any actor whose transform didn't change.
- Morph ball: the ball is drawn at `GetBallToWorld()` = player origin +
  (0,0,radius) in world Z, so the player's scope passes that offset as a
  pivot. The blend rotates about the ball centre and lerps the centre
  itself; pivoting about the origin swung the centre by the roll delta each
  frame (visible jitter while rolling). A roll over 45° per tick no longer
  cuts the whole blend for a pivoted actor: it keeps the translation lerp and
  takes the current rotation. Teleports (> 4 units) still cut. The rules are in
  `platform/include/port_presentation_rules.h` (`PortClassifyRigidBlend`).
- The camera snapshot (`CCameraManager`) records the tick generation it was
  written in, and `GetCurrentCameraTransform`/`GetPresentedLookRotation` use it
  only while that matches the current tick (`PortSnapshotFresh`). A tick that
  skips `CCameraManager::Update` (soft pause after a scan completes) falls back
  to the simulation transform instead of replaying the stale prev→cur pair
  every frame, which flickered the camera. Test: `port_presentation_rules_tests`.

Limits: bounds, culling, PreRender lighting and shadows stay on the sim
transform; particles queued to the renderer (not drawn inside the actor's
draw) and the reflected player stay on the sim transform; an override that
draws world-space content unrelated to its own transform is shifted by the
sub-tick delta. Checked by logging the player's blend while rolling in the
Chozo spawn room at `frame_limit=0`: evenly spaced positions across tick
boundaries.

### 2. Skinned poses (done, `pose_interpolation`)

`CAnimData::PreRender` builds the pose from the animation time, which only
moves on ticks. Implemented by **blending two built poses**, not by sampling
the tree ahead: `CAnimData::Advance` mutates the tree in place (transitions,
event dispatch, additive fades), so sampling ahead needs a side-effect-free copy
of the whole tree. Blending leaves the tree alone, so events, sounds and
particles can't fire twice.

- `CAnimData::PortNotePoseBuild` runs before each `BuildNoScale` into
  `x224_pose` (`BuildPose`, `SetupRender`). The first rebuild in a new tick
  stores the pose being replaced, per bone, as a quaternion, a uniform scale and
  an offset (`SPortPoseHistory`, allocated on first use). Rebuilds within the
  same tick (bone tracking, IK and pirates invalidate the pose every PreRender)
  keep that tick's history, so tracking and IK are part of both poses.
- `CAnimData::PortPresentedPose` is what `SetupRender` skins: per bone nlerp
  and offset lerp at the presentation factor, into a second
  `CPoseAsTransforms`. It returns the sim pose when capped, when the kept pose
  isn't from exactly the previous tick (paused, off screen, several ticks in one
  frame), or when any bone turned more than 45° or moved more than 4 units in
  one tick (animation cuts), so the whole pose snaps.
- A pose that isn't rebuilt in a tick is static and draws as is.

Limits: `GetPose()` users (swarms, fish clouds, rag dolls) and locators
(`GetLocatorTransform`: attached particles, beams, the gun's muzzle) stay on
the sim pose, so an attachment can trail its skinned bone by up to one tick's
motion. Checked by counting blend/snap decisions at `frame_limit=0` in the
Chozo spawn room and Chozo area 41 (MREA 492CBF4A: Eyons, Metarees): about 95 % of
rebuilt poses blend, the rest are first frames and cuts; the arm cannon
(skinned, drawn every frame) looks right in captures.

### 3. Particles, projectiles, effects

`CElementGen` (done, `particle_interpolation`) reuses the retail sub-frame
path: particles already keep `x10_prevPos`, and when a system's time doesn't
land on a 1/60 frame the render paths draw `prevPos + (pos - prevPos) *
x80_timeDeltaScale`. No new per-particle state.

- `InternalUpdate` records the tick generation when a system stepped exactly
  one whole frame in this tick (`xPortStepGeneration`); `SetGlobalTranslation`
  keeps the previous tick's global translation (`xPortPrevGlobalTranslation`).
- `CElementGen::Render` (`PortBeginPresent`) temporarily sets
  `x80_timeDeltaScale` to the presentation factor and the global translation to
  the blend, then restores both. `RenderLines` got the same blend (retail draws
  lines at the current position only).
- Skipped: systems that didn't step exactly once this tick (paused, several
  frames or a fractional step: they draw as retail), a global translation jump
  of more than 4 units, and draws inside an actor render scope (phase 2 already
  shifts them; blending again would double the offset).

Projectile effects move through `SetGlobalTranslation` each tick
(`CProjectileWeapon::UpdateChildParticleSystems`), so their queued draws blend
too. Limits: new particles spawn at the current emitter position; a particle
teleported by the effect script lerps across the jump for one tick.

Phase 6 (same setting) covers the rest. `PortTickPair<T>`
(`platform/include/port_tick_pair.h`, unit-tested) keeps the first value
drawn in each tick generation, and blends only when the previous one is from
the tick right before.

- `CParticleSwoosh`: segment positions, rotations and the global transform
  are blended, with segments matched by age (head - index), so an emitter's
  trail slides along rather than growing one segment per tick. Swooshes that
  the owner writes directly (grapple beam, Wave Beam) never step, so age and
  slot are the same there. A segment that jumps more than 4 units (16 for
  owner-written swooshes) snaps.
- `CParticleElectric`: the line transform is blended. The fractal itself
  still changes once per tick, as on console.
- Beam weapons draw in world space, so they opt out of the phase 2 rigid shift
  (`CActor::PortSetOwnPresentation`): `CWaveBuster`, `CBeamProjectile`
  (Plasma and electric beams), `CFlameThrower` and `CNewFlameThrower`.
  `CWaveBuster` blends the spiral transform, the bezier points and the
  spiral offset, and advances its spin and sparks only once per tick.
  `CPlasmaProjectile` blends the transform, length, width and angle. The
  flamethrower's particles already blend through `CElementGen`.
- Checked with temporary counters (`present cycle`): swoosh segments blend
  100 % idle and ~97 % while flamethrowing, electric lines 720/720, and the
  Wavebuster spiral 479/480. Captures of the Wavebuster at t = 0, 0.5 and 0.99
  progress smoothly toward the next tick.
- Limits: `CPlasmaProjectile`'s motion blur stays at the tick position.
  Not tried live: the grapple beam, `CPlasmaProjectile` (bosses only) and
  `CElectricBeamProjectile`.

### 4. First-person view and look input (done)

- `PortDebug::PresentedAimDelta` previews the yaw/pitch the next tick will
  apply (`MouseAimState::Preview`, same clamp and sensitivity) from the pending
  mouse delta, the gyro accumulator (`PollGyro`, per frame with the real frame
  dt) and the twin-stick velocity times `t * dt`.
  `CCameraManager::GetPresentedLookRotation` turns that into a world rotation
  about the camera (yaw about world Z, pitch about the camera's right axis) and
  `GetCurrentCameraTransform` applies it to the presented view. The tick still
  consumes the whole delta, so the shot direction is unchanged.
- It is only active when the last tick applied free aim
  (`sAimAppliedLastTick`), so menus, morph ball and cinematics are untouched.
- The arm cannon stays on the simulation camera, so it is fixed on screen while
  the world turns, like a view model. Its camera-relative pose (bob, sway,
  recoil) is blended with `actor_interpolation` (section 7). The free-aim crosshair
  (`CCompoundTargetReticle::DrawOrbitZoneGroup`) is drawn at a world point placed
  against the simulation camera, so it is carried into the presented camera's
  frame (look rotation and camera blend) to stay fixed on screen.
- The game's own stick look stays per-tick (it is integrated with acceleration
  curves in the game code); only its result is blended like any camera.
- Fixed on the way: gyro aim used to write into the per-tick mouse frame delta,
  which `BeginFrameMouse` overwrote, so most gyro input was lost.

### 5. HUD and 2D

The combat HUD, scan visor and map mostly follow the camera or are static; the
lock-on reticle and damage indicators track world positions and must project
through the blended transforms. GUI frames (`CGuiFrame`) animate by tick and
can stay at 60 Hz for a first pass. The helmet and HUD lag/bob
(`CSamusHud::UpdateHudLag`) is pushed into the helmet and deco interfaces and
the base frame camera once per tick; blending it would mean keeping the last
tick's lag rotation and offset and re-applying a blend around `Draw`. Its
motion is a few pixels per tick, so it is left at 60 Hz.

### 6. Resets and edge cases

Snap everything (prev = current) on: room/world load, save-state load, warp,
cinematic start/end, camera cut, player respawn, pause and unpause, the
`MP_TURBO` lockstep, and any tick where more than one step ran after a stall
(blend only the last one). Scan visor and X-ray/thermal passes copy the frame,
so they work unchanged.

### 7. Sweep (phase 5)

Tools: `MP_PRESENT_T` / console `present <t|cycle|tick|off>` forces the
presentation factor even under `MP_TURBO` or the frame limiter; `hold 1` stops
ticks and `step <n>` runs exactly n, so one tick pair can be drawn at any t
(`NATIVE_PORT.md`). Comparison: hold, `present tick` (shot N), `step 1`, then
shots at t = 0, t = 1, tick (N+1) and 0.5. A correct blend gives t = 0 = N and
t = 1 = N+1.

- Walking in Chozo MREA 492CBF4A with all three settings on: t = 1 vs N+1
  differs by 0 px. t = 0 vs N differed by 627 px (N vs N+1: ~11 000): the arm
  cannon drew with the new tick's bob. Fixed: `CPlayerGun::PortSnapshotPresentedPose`
  keeps the gun's pose relative to the simulation camera before each tick and
  `CPlayerGun::Render` blends it (`CActor::PortBlendRigid`, same snap rule)
  by shifting the view, so the arm, beam and muzzle effects follow. Now 257 px:
  HUD sway lines (section 5), an icon in the top-right HUD (probably the
  minimap, which follows the player per tick) and faint gun shading.
- Rolling in morph ball: the ball's spin and position blend; the remaining
  difference is lighting (the ball's light and reflections are placed at tick
  positions by PreRender), acceptable.
- ASan tour (`MP_RANDO_SWEEP=1`, `MP_PRESENT_T=cycle`, `MP_TURBO=2`, all
  interpolation on, `frame_limit=0`): all 8 worlds and 276 areas, clean (no ASan reports) in
  about 30 minutes.

## Phased plan

1. **Look input per frame** (section 4). Done.
2. **Actor transforms** in `CActor` plus the player, morph ball and door
   paths (section 1). Done, `actor_interpolation`, off by default.
3. **Skinned poses** and the bone-tracking/IK users (section 2). Done by
   blending built poses, `pose_interpolation`, off by default.
4. **Particles and projectiles** (section 3). `CElementGen` done,
   `particle_interpolation`, off by default; swooshes, electric effects and
   beam weapons too (phase 6).
5. **Sweep:** ASan tour (`MP_RANDO_SWEEP`) with interpolation forced on (a
   fake fractional `t` under `MP_TURBO`), captures at t = 0/0.5/1 compared
   against tick frames to find paths that were missed (section 7). Done.

Each phase is useful on its own and behind one setting (`frame_interpolation`,
F1 Performance). Phase 1 is on by default; phases 2 to 4 are off by default
until they have been tried at high refresh. With the frame limiter on (the default 60 FPS
cap) nothing changes, as today.

## Why not `sim_rate`

Raising the tick rate makes every system genuinely smooth, but the game was
tuned for 60 Hz: `docs/HIGH_FPS_AUDIT.md` lists per-frame counters, fixed-step
physics constants, timers that expire when `dt` exceeds their length (the
Sunchamber dish bug) and AI that behaves differently at other rates. Each needs
fixing and testing room by room, and any miss changes gameplay. Interpolation
never changes the simulation, so its failures are visual only. `sim_rate` and
`sim_adaptive` stay in F1 Performance as experiments.
