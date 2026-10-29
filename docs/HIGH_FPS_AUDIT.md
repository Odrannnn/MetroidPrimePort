# High-FPS simulation audit

Goal: run the *simulation* at the display rate (120/144 Hz) instead of the fixed
60 Hz tick, with no render interpolation. This is the audit of how far the game
logic is from being frame-rate independent and what has to change. The other
route, 60 Hz logic with an interpolated render, is scoped in
`docs/FRAME_INTERPOLATION.md`.

## Current architecture

- `PortTiming::FixedStepClock` (`platform/include/port_timing.h`) fixes the step
  at `kPeriod = 1/60`. `Advance()` returns the number of whole 60 Hz ticks due
  this rendered frame; `Interpolation()` (leftover time / period) only
  drives the uncapped camera blend (`main.cpp`, `CCameraManager`).
- `CGameArchitectureSupport::UpdateTicks` (`src/MetroidPrime/main.cpp:459`) runs
  that many ticks, each pushing `CreateTimerTick(kAMT_Game, 1/60)`, and calls
  `CInputGenerator::Update(1/60, ...)` per tick.
- The renderer draws the latest ticked state at the display rate (duplicated
  frames on high-refresh displays).
- `dt` reaches game code as `CMain` -> `CStateManager::Update(1/60)` ->
  `CActor::Think(1/60)` etc.

Conclusion: the plumbing is a single fixed step, so raising the simulation rate
means (a) making the step equal to the display frame time and (b) removing every
remaining 60 Hz assumption in the logic.

## Summary

| Subsystem | State | Effort |
| --- | --- | --- |
| Player movement, physics, collisions | dt-scaled | none |
| Animation (`CAnimData`, `Kyoto/Animation`) | dt-scaled, seconds-based timeline | none |
| Cameras, cutscenes, cinematic timing | dt-scaled | none |
| Input generator | dt parameter passed through | none |
| Script objects, layer manager, world/area | dt-scaled | none |
| GUI (`GuiSys`), HUD timers | dt-scaled | none |
| **Particle systems** (`CElementGen`, `CParticleElectric`, `CParticleSwoosh`) | time-driven, fixed 60 Hz substeps | none |
| **Decals** (`Weapons/CDecal`) | time-based (`x58_frameIdx = t * 60`) | done |
| Scattered AI/player/HUD frame counters | mixed | low-medium |
| Tick plumbing + projectile tick period | fixed or adaptive (`sim_rate`, `sim_adaptive`) | done |

The important result is that the engine core is **already dt-scaled**, and the
particle systems run by real time with fixed 60 Hz substeps, so they already
scale. The remaining real work is the scattered per-frame counters.

Status: the tick plumbing, the projectile tick period, decals, the particle
seeding, and the per-tick rate accumulators are implemented (see
"Implemented" below), behind the experimental `sim_rate` / `sim_adaptive`
settings. What is left is the short list under "Findings".

## Findings

Findings below use function and symbol names; the converted items live in
"Implemented".

Coverage note: `CPlayerDynamics.cpp` (and the morph-ball friction) were missed by
the first pass - its scope was `CPlayer.cpp` - and were found from a report that
walking slowed at a raised rate. That file is now audited and fixed; the per-tick
friction below was the cause.

### Still open (everything else is in "Implemented" below)

- `CGroundMovement`: `x14_waterLandingVelocityReduction` is applied inside the
  collision sub-step loop. Probably a one-time landing response, but if it
  re-applies while skimming water it is rate-dependent; verify against a real
  water surface before touching it.
- Short debounces, deliberately left as-is (a few frames of timing error at a
  doubled rate): `CPlayer::x2b0_outOfWaterTicks` and
  `xa2c_damageLoopSfxDelayTicks`, `CSpacePirate::x63c_frenzyFrames`, the
  morph-ball spider-electric `x8_curFrame` / `x4_lifetime`.
- Packed bitfields, left to avoid disturbing the layout:
  `CWallCrawlerSwarm` boid `x7c_6_remainingLaunchNotOnSurfaceFrames` /
  `x7c_24_framesNotOnSurface`.
- Throttle counters, benign (they only skip expensive work between frames, so
  at a raised rate they run more often): `CParasite` / `CSeedling`
  `x5d4_thinkCounter`, `CSpacePirate` / `CFlyingPirate` `% 7` cadences,
  `CFishCloud::x118_thinkCounter`, `CBallCamera::x478_shortMoveCount`.
- HUD memo countdown (`CStateManager::xf80_hudMessageFrameCount`, via
  `CMFGame`'s `IncrementHUDMessageFrameCounter`): ticks down once per `Update`,
  so the memo clears faster in wall time at a raised rate.

Done and moved to "Implemented": the tick plumbing, the projectile tick
period, decals, the `x8d8_updateFrameIdx` seed (now a `float` of 60 Hz frame
units via `TickFrames()`), and the per-tick rate accumulators
(`CSpacePirate` cloak-delay and `x7bc_attackRemTime`, `CFlyingPirate` `x7e4_`,
`CMetroidBeta` `x834_particlePhase`, `CScriptPickupGenerator`
`x44_delayTimer`, `CIceProjectile` trail spawn, `CMorphBall` wall-spark
countdown, `CNewFlameThrower` flame-contact lifetime, player/morph-ball
friction, `CFishCloud` steering). The particle systems need no conversion:
all three accumulate real time and step it in fixed 1/60 substeps.

### Confirmed dt-scaled (no change needed)

- `src/MetroidPrime/CPhysicsActor.cpp:112-160` `PredictMotion/Angular/Linear`
  (`dt * velocity`, `q3 * dt`, `torque * dt`)
- `src/MetroidPrime/CModelData.cpp:317` `AdvanceAnimation(float dt)` and
  `src/MetroidPrime/CAnimData.cpp:262` `AdvanceAnim` (timeline is `CCharAnimTime`
  seconds; the `1.f/60.f` at `CAnimData.cpp:843` is a one-time align integral)
- All cameras and cutscene timing (`CCinematicCamera`, `CInterpolationCamera`,
  `CPathCamera`, `CCameraManager`, `CFirstPersonCamera`, `CBallCamera` timers)
- `CWorld`/`CGameArea` `AliveUpdate(dt)`, `CScriptLayerManager`, `CWorldTransManager`
- `CInGameGuiManager`, `CSamusHud` dt methods, `GuiSys` panes/sliders
- `Kyoto/Animation` animation readers (`CAnimSourceReader`, `CAnimTree*`)

## Implemented: experimental `sim_rate`

- `PortDebug::SimRate()/SetSimRate()/SimPeriod()` (env `MP_SIM_RATE`, settings
  key `sim_rate`, slider in F1 > Video > Frame rate, range 30..480, default 60).
- `PortDebug::SimAdaptive()/SetSimAdaptive()` (env `MP_SIM_ADAPTIVE`, settings
  key `sim_adaptive`, checkbox on the same page). When set, `UpdateTicks` uses
  `period = clamp(frameTime, 1/480, 1/30)` instead of `1/SimRate()`, i.e. one
  step per frame with `dt` equal to the measured frame time, so a variable
  frame rate is matched tick-for-tick and the fixed-step accumulator only
  subdivides when a frame exceeds 1/30 s.
- `CGameArchitectureSupport::GetTickPeriod()` exposes the step actually used so
  `RsMain` feeds the same `dt` to `CSfxManager::Update` and streamed audio.
- `PortTiming::FixedStepClock` gained a runtime step (`mPeriod`,
  `SetPeriod()`/`Period()`); `kPeriod` stays the `constexpr` 60 Hz default.
- `CGameArchitectureSupport::UpdateTicks` sets the clock period from `SimRate()`
  and uses it for `CreateTimerTick` and `CInputGenerator::Update`.
- `CMain::RsMain` recomputes `dt` per frame and passes it to
  `CSfxManager::Update`; `CMain::UpdateStreamedAudio` uses the same period, so
  audio stays wall-clock (ticks x period == elapsed).
- `CProjectileWeapon::GetTickPeriod()` stays `1/60`: weapon descriptions give
  velocity and gravity per 1/60 s frame and `CProjectileWeapon::Update` steps
  in those frames by elapsed time, so the per-second speeds that aim
  prediction (`CProjectileInfo`, `CTargetableProjectile`, `CBloodFlower`)
  derives must divide by 1/60 at any sim rate. (It briefly returned
  `SimPeriod()`, which doubled predicted speeds at 120 Hz.)
- Adaptive steps end on the present time, so `UpdateTicks` drops the clock's
  remainder after a step within the 1/30 s clamp (a leftover would turn a
  later short frame into two steps) and `RsMain` presents the current state
  (interpolation -1, as with the cap on) instead of the previous tick.
- `CGraphics::TickRenderTimings` advances the render clock (texture scroll, UV
  animation, HUD pulses) by the seconds the ticks covered, with the fraction
  of a 1/60 s frame carried, instead of 1/60 s per tick; the present lag is
  `(1 - t) * dt`. `TickSimTime` carries its 1/600 s rounding. Both are
  unchanged at 60 Hz.
- The loader's idle budget under adaptive is the sim rate's frame, not the last
  frame's length, so loading frames don't lengthen one another.
- `CTargetReticles::x388_triOffScreenTicks` (port-added, Remastered's
  update-count threshold) counts 60 Hz updates via `TickFrames()`.
- `CDecal` ages by real time (`x6c_elapsedTime += dt`) and derives its 60 Hz
  frame index from it, so decals keep their authored duration above 60 Hz.
- `PortDebug::TickPeriod()/SetTickPeriod()` (set by `UpdateTicks`) exposes the
  step being simulated so per-tick constants can scale. Used to make the player
  and morph-ball friction real-time:
  - `CPlayerDynamics.cpp:55` `GetDampedClampedVelocityWR` subtracted a fixed
    `friction` from the local velocity every tick, so a raised rate decelerated
    proportionally faster — walking slowed down while the friction-free jump did
    not. Now scaled by `TickPeriod() * 60`.
  - `CPlayerDynamics.cpp:311` the rotation friction multiplies angular velocity
    per tick; now `powf(friction, TickPeriod() * 60)`.
  - `CMorphBall.cpp:1809` `ApplyFriction` and `:1819`
    `DampLinearAndAngularVelocities` have the same per-tick forms and are scaled
    the same way.
- `MP_SMOKE_WALK=<ticks>` holds the stick forward and reports distance, peak
  flat speed and speed over the walk, so the two rates can be compared directly.
- `CFishCloud` boid steering (`ApplyRotation`/`ApplyAlignment`/`ApplyWander`/
  `ApplyCohesion`/`ApplySeparation`/`ApplyAttraction`, `CFishCloud.cpp:786-868`)
  added a per-tick acceleration to `xc_vel`, which the position then integrates
  by dt, so the flock's turn rate scaled with the tick rate. Scaled by
  `TickPeriod() * 60`, as is the per-tick vertical damping at `CFishCloud.cpp:481`.
- Also checked and confirmed dt-scaled: `CGroundMovement` (the collision
  response sub-steps by `remainingDt`), the first-person camera gun-follow
  (`angularStep` starts as `dt`), and the bomb-jump velocity factors (applied
  once per jump). A second targeted sweep over all of `src/` for the
  per-tick-physics-constant class found nothing else.
- Randomness index `CStateManager::x8d8_updateFrameIdx` now accumulates 60 Hz
  frame units (`TickFrames()`) instead of counting ticks, so the particle,
  decal and projectile seeds (`SetGlobalSeed`) and `GetUpdateFrameIndex()` give
  the same sequence for the same real time at any rate. It became a `float`.
- Per-tick *rate* accumulators scaled by `TickFrames()`: `CSpacePirate`
  cloak-delay (715, 1117) and `x7bc_attackRemTime` (2803), `CFlyingPirate`
  `x7e4_` (1829), `CMetroidBeta` `x834_particlePhase` (995),
  `CScriptPickupGenerator` `x44_delayTimer` (158).
- Per-tick cadence/duration counters converted to 60 Hz frame units:
  `CIceProjectile` trail spawn (`x180_frameCount`, now a `float`, was `% 4`),
  `CMorphBall` wall-spark countdown (`x1e38_wallSparkFrameCountdown`, now a
  `float`), `CNewFlameThrower` flame-contact lifetime
  (`SSortedListEntry::x4_remainingTime`, now a `float`).
- Confirmed *not* needing changes: `CPlayerGun::x30c_rapidFireShots` decays via
  a dt-based 0.2 s timer; `CWallCrawlerSwarm::x368_boidGenCooldownTimer` is a
  duration set to `1/rate` and decremented by dt.

Verified: with `MP_SIM_RATE=120` the timing trace reports `simulation=120.0
ticks/s` with the render at 60 FPS; with `MP_SIM_ADAPTIVE=1` and the cap on it
reports `render=60.0 simulation=60.0` (exactly one step per frame), and with the
cap off it follows the render rate up to the 480 Hz clamp (`render=1678
simulation=476`). The mouse smoke still passes at the default 60 and all port
tests pass. For real high-refresh gameplay, turn the F10 frame cap off (or set
adaptive) so both the renderer and the tick run at the display rate.

Known caveats:

- Tick-indexed particle seeding is fixed (see above).
- Open item: `CGroundMovement.cpp:765` applies
  `velocity *= 1.f - x14_waterLandingVelocityReduction` inside the collision
  sub-step loop. It is probably a one-time landing response, but if it re-applies
  every tick while skimming water it is rate-dependent; left unchanged because
  scaling a collision response by dt may over-correct. Verify against a real
  water surface before touching it.
- Remaining per-tick counters, deliberately left as-is:
  - Short debounces of a few frames (`CPlayer::x2b0_outOfWaterTicks` and
    `xa2c_damageLoopSfxDelayTicks`, `CSpacePirate::x63c_frenzyFrames`,
    `CMorphBall` spider-electric `x8_curFrame`/`x4_lifetime`): the timing error
    is a few frames at a doubled rate, and converting them needs new fractional
    state for no visible gain.
  - `CWallCrawlerSwarm` boid `x7c_6_remainingLaunchNotOnSurfaceFrames` /
    `x7c_24_framesNotOnSurface` are packed bitfields; changing them to time
    units would disturb the layout, so they are left.
  - Throttle counters (`CParasite`/`CSeedling` `x5d4_thinkCounter`,
    `CSpacePirate`/`CFlyingPirate` `% 7` cadences, `CFishCloud::x118_thinkCounter`,
    `CBallCamera::x478_shortMoveCount`): these exist to skip expensive work
    between frames, so at a raised rate they simply run more often. That is
    benign (slightly more responsive AI, a little more CPU), not a speed error.
- Adaptive mode gives the game a variable `dt`, so physics results vary with the
  frame time; the fixed rates keep a constant step.

## Conversion plan

Phase 1 - decals (done)

- `CDecal` now accumulates real time and derives `x58_frameIdx = int(t * 60)`,
  so the existing table reads and frame-unit lifetimes are unchanged in meaning
  while the decal ages in real time. The extra `float` fits the existing
  padding, so no layout assert moved.
- Particles need no conversion: the three systems already integrate real time in
  fixed 1/60 substeps (verified above). Only the substep granularity quantises
  effects to 60 Hz if sub-frame fidelity is ever wanted.

Phase 2 - scattered counters (done, except the items "Findings" leaves open)

- Each per-tick rate accumulator is scaled by `TickFrames()` and each
  per-tick cadence/duration counter converted to 60 Hz frame units; the
  particle RNG seed is accumulated time (`x8d8_updateFrameIdx`) rather than a
  tick count. See "Implemented".

Phase 3 - tick plumbing (done)

- Implemented as the `sim_rate` / `sim_adaptive` settings; see "Implemented"
  above. Input is stepped once per tick, which at a display-matched rate is once
  per frame.

Phase 4 - verification

- For interpolation, this sweep is covered by `docs/FRAME_INTERPOLATION.md`
  section 7 (t = 0/0.5/1 captures compared against tick frames, plus the ASan
  tour with interpolation forced on). For `sim_rate` itself, still to do:
  add a port smoke scenario that runs the same scripted input at 60 and 120 Hz
  and asserts that travelled distance, turn angle, jump height and animation
  phase match within tolerance (the existing `MP_SMOKE_*` driver already injects
  deterministic input).
- Re-check cutscene sync (cinematic timing is dt, but scripted POI events fire at
  particle/animation boundaries), door/elevator transitions, and save/load.

## Risks

- **Behaviour divergence.** Even with dt-scaled logic, floating-point summation
  order changes, so physics/collision outcomes and speedrun-frame tricks will
  differ from the 60 Hz original. This is a gameplay change, not just a port fix.
- **Frame-indexed data.** Particle PRT/PSLT/PISY tables and decal tables are
  authored per 60 Hz frame; running at 120 Hz means either sampling them at
  `frame = t*60` (visually fine) or authoring sub-frame interpolation.
- **`close_enough(dt, 1/60)` workarounds** in `CProjectileWeapon.cpp:148`,
  `CElementGen.cpp:425` and elsewhere assume the canonical step and must be
  revisited.
- **RNG determinism.** Particle, decal and projectile seeds now come from the
  accumulated 60 Hz frame count (`TickFrames()`), so the same real time gives
  the same sequence at any rate.
