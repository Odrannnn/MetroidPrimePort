# Metroid Prime port — working notes

Goal: turn the Metroid Prime recompilation experiment into an actual, maintainable
PC port by building the PrimeDecomp matching decompilation against the Aurora
compatibility layer (MIT), instead of a static-recomp module on a Dolphin-derived
runtime.

## Architecture

- Game + engine: PrimeDecomp/prime `src/` + `include/` (real C++, targets
  `GM8E01_00`, the same disc we own).
- Platform/GPU/input/disc/saves/UI: `extern/aurora` (MIT). Aurora provides a
  drop-in `dolphin/*` SDK API plus an SDL3 + WebGPU(Dawn) app layer and a
  performant GX implementation.
- Assets: read from the user's own disc (`orig/GM8E01_00/`); nothing is
  distributed.
- Integration reference: Dusklight (Twilight Princess port) — same CMake shape
  (`extern/aurora`, decomp sources listed in a `files.cmake`, `aurora::*` libs).

## Status (2026-09-18)

- Repo cloned, branch `port`; `extern/musyx` and `extern/aurora` submodules present.
- Matching decomp builds and reproduces the retail DOL byte-for-byte:
  `build/GM8E01_00/main.dol` sha1 `949c5ed7368aef547e0b0db1c3678f466e2afbff`.
  Report: SDK 100%, Core Engine (Kyoto) 90.99%, Game 87.50% matched.
  `objdiff.json` is available for reference diffing.
- Aurora `examples/simple` builds on this machine (prebuilt Dawn for
  linux-x86_64 is fetched automatically by CMake).

### Port scaffold build status (2026-09-18)

`CMakeLists.txt` + `files.cmake` (632 sources) + `platform/{compat.h,main.cpp}` build
`mp_game` against Aurora. First full compile went 1269 -> 138 errors after:
- `platform/compat.h` (force-included) restoring the SDK `AUTO`/`AUTO_REF`/
  `AUTO_CONST_REF` macros Aurora omits;
- excluding `src/NESemu/modwrapper.cpp` (raw PowerPC asm) and
  `src/MetroidPrime/TypesMatch.cpp` (decomp-only type scaffolding);
- `-Wno-narrowing`.

Remaining 138 errors are a bounded compatibility-shim queue:
- GX declarations Aurora implements but does not declare in headers:
  `GXSetTexCopyDst/Src`, `GXPixModeSync`, `GXCopyTex`, `GXSetTevColor`,
  `GXInitLightPos/Attn`, `GXSetCullMode`; plus a `GXSetArray` signature
  difference (Aurora adds a 5th arg).
- Missing SDK headers: `dolphin/arq.h`, `dolphin/thp/THPInfo.h`.
- `OSContext` member mismatches (`gpr`, `srr0`) in the game's own view.
- `COBBTree::CNode::operator new` placement-new mismatch.

### Build clears (2026-09-18)

`metroid_prime_port` now **compiles and links**: all 632 decomp sources build
against Aurora with zero errors and produce a 64.5 MB executable that
initializes Aurora and a Vulkan device.

What clearing the remaining errors required:
- `platform/compat.h` (force-included): libc, the GX/SI/PAD/OS/CARD umbrellas,
  `AUTO*`, `nofralloc`, `__abs`, and the `triggerL/R` -> `triggerLeft/Right`
  mapping for Aurora's `TARGET_PC` `PADStatus`.
- `platform/include/dolphin/{arq,gba,PPCArch,thp/*}.h`: SDK headers Aurora omits
  (arq.h re-exports Aurora's `ar.h`; the rest are the original SDK declarations).
- `platform/include/dolphin/gx/GXShims.h` + `platform/shims.cpp`: GX token,
  breakpoint, and write-gather-pipe entry points Aurora lacks, plus GBA/PPC
  shims.
- Decomp source fixes (port-only, documented in code comments): `RAssertDolphin`
  OSContext dump guarded for opaque PC `OSContext`; `rstl/string.hpp` declares
  the member specializations `rstl_strings.cpp` defines (clang requires
  declaration before instantiation); `GXSetArray` call updated to Aurora's
  5-arg form; small conversion casts; `CARDFormatAsync` declaration only (Aurora
  defines it); case-corrected `Kyoto/CCrc32.hpp` include.
- CMake: `LINK_GROUP:RESCAN` around `aurora::core`/`aurora::gx` to break their
  static-library cycle; excluded `src/NESemu` (raw PowerPC asm).

### Boot progress (2026-09-18)

`metroid_prime_port` now boots well into initialization under Aurora:
1. Aurora initializes (Vulkan device, 2240x1680 framebuffer, CARD, ARAM `0x1000000`).
2. Disc mounts via `aurora_dvd_open` (user's ISO).
3. Game entry runs: `CMain` ctor, `RsMain`, `CGameGlobalObjects` ctor, default font
   (zlib) load, `InitializeSubsystems` (AR/ARQ), `PostInitialize`.
4. GX commands reach Aurora's FIFO worker and a frame is presented.

Issues fixed along the way:
- zlib ABI: the bundled zlib 1.1.3 declares a non-standard 3-arg `inflateInit2_`;
  retargeted to the standard 4-arg form so it matches the linked zlib-ng.
- `TOneStatic<T>` 1-arg `operator new` had no definition.
- The guest stack "paint" in `InitializeSubsystems` wrote to address 0 (no emulated
  guest stack); skipped it on PC and gave the dummy `OSThread` a MEM1 stack range.
- Aurora aborted when the game's error handler called `PADRead` before `PADInit`.

Current blocker: the game's custom `CGameAllocator` (main heap) returns null for a
64 KB allocation in `CDvdFile::StartARAMFileLoad` (loading `aram:Tweaks.pak`),
which triggers the error handler (and then Aurora's `PADRead before PADInit`
fatal). Two separate issues:

1. **Heap sizing (fixed).** Aurora's internal framebuffer defaulted to the
   window-scaled 2240x1680, so the game's two `x2c_frameBufferSize` allocations
   took ~15 MB of the 24 MB MEM1, leaving only a ~10 MB game heap. Pinning
   `windowWidth/Height` to 640x480 raised the heap to ~20 MB. (Aurora still
   scales the internal fb to 1120x840 @1.75; a true 640x480 fb needs the DPI
   scale forced to 1.)
2. **Allocator free-list (open).** With ~20 MB free the 64 KB allocation still
   fails, so `CGameAllocator`'s free-block/split bookkeeping is not surviving on
   the 64-bit host (`SGameMemInfo` packs flags in low pointer bits and uses
   `sizeof(SGameMemInfo)`, which is ~4x larger than on GameCube). Needs a focused
   pass over `FindFreeBlock`/`FixupAllocPtrs`/`AddFreeEntryToFreeList`.

Temporary memory diagnostics are in `CGameAllocator::Initialize` and
`COsContext::OpenWindow` (stderr prints of heap/arena/framebuffer sizes).

### Bring-up fixes after the allocator (2026-09-18)

The game now runs through `PostInitialize`/`AddPaksAndFactories` pak loading with
no crashes. Fixes landed:
- **`CGameAllocator` overflow** (root cause of the OOM): `Alloc(0x20)`/`Alloc(0x1c)`
  for `CSmallAllocPool`/`CMediumAllocPool` were 32-bit object sizes; on x86-64 the
  objects are larger and overflowed the next free block's header. Now use
  `sizeof(...)`. Free list stays healthy.
- **ARQ recursion**: Aurora's `ARQPostRequest` invoked the ARAM completion callback
  synchronously, so `CDvdFile::PingARAMTransfer <-> HandleARAMInterrupt` recursed to
  stack overflow. Callbacks are now queued and drained by an iterative `ARQPoll()`
  at explicit pump/wait points. Running the poll inside `ARQPostRequest` was still
  too early: multi-chunk transfers observed stale length/interrupt state and left
  `aram:MiscData.pak` permanently loading.
- **GX breakpoint / VI retrace**: `CGraphics::EndScene` spins on
  `mNumBreakpointsWaiting`, which only a VI retrace decrements. `GXEnableBreakPt`
  now pulses the registered breakpoint + pre/post retrace callbacks (stored by the
  VI shims) so frames complete.
- **`delete` on CMemory memory**: `rstl::aligned_allocator::deallocate` used
  `delete[]` while `allocate` used `CMemory::Alloc`; on clang the game's
  `operator delete`->`CMemory::Free` is MWCC-only, so glibc freed a game pointer.
  Routed the free to `CMemory::Free`; `CDvdFileARAM` buffers now use `rs_new`.

The apparent frame-slot deadlock was normal frame pacing. The repeated
`CGraphics::EndScene` caller was the initial pak-loading loop; after fixing ARQ
completion ordering and synchronous ARAM waits, startup reaches the main loop
(`MP frame` verified through frame 361). Resource buffers passed to `delete`-based
owners now use matching host allocations, and palette frame state is initialized
so ARAM palette storage follows delayed `CMemory::Free` cleanup.

Further host bring-up fixes now sustain the main loop through at least frame 48,601:
- GameCube AGSC payloads are big-endian 32-bit MusyX structures, while the vendored
  host runtime expects native-endian structures (including 64-bit sample directory
  pointers). `CAudioGrpSetLoc` now converts the pool, project, and sample directory
  into host-native layout on little-endian hosts: big-endian `GROUP_DATA`,
  `POOL_DATA`, `MEM_DATA`/`FX_TAB`, ID lists, and `SDIR_DATA_INTER` are byte-swapped
  and expanded to native `SDIR_DATA`; raw curve bodies and PCM samples are left
  untouched. MusyX group push/pop is enabled again and `sndPushGroup` succeeds for
  the boot groups. A `s32`->`size_t` cast in `dataAddSampleReference` fixes truncation
  of 64-bit sample bases.
- CMDL header fields, section sizes, bounds, and per-surface metadata are byte-swapped
  before model setup. Surface parent/next links are expanded in place for 64-bit hosts
  while raw GX display lists remain untouched.
- Movie buffers owned by `single_ptr`/`auto_ptr` use matching host allocations and
  rounded DVD request sizes instead of placing `CMemory::Alloc` pointers behind
  host `delete` owners.
- Aurora keyboard input has initial GameCube mappings when no saved mapping exists:
  WASD and IJKL drive the sticks, X/Z/C/V map A/B/X/Y, Return maps Start, and the
  arrow keys map the D-pad. Existing user mappings and physical controllers win.
- PATH version-4 resources are read field-by-field into native vectors. Packed
  big-endian node, link, region, connectivity, and octree fields are converted and
  their 32-bit indices are rebased only after vector storage is stable.
- CMDL and MREA geometry now share native surface-header conversion, including
  material indices, display-list lengths, pointer-sized renderer links, normals,
  and optional bounds.
- MusyX now uses the upstream `origin/sdl3` PC backend (merged into the vendored
  submodule, with conflicts resolved in favor of the host `s64` typedef and the SDL
  mutex IRQ). It provides a software voice mixer covering ADPCM/PCM decode, pitch
  resampling, ADSR envelopes, and studio/AUX mixing, and feeds interleaved `s16`
  stereo to an SDL3 audio stream. PC sequence playback is enabled again and the
  audio thread drives `snd_handle_irq`.
- Big-endian `ARR` song payloads from `CSNG` resources are converted in place for
  little-endian hosts before sequencing: header offsets, the 64-entry track table,
  per-track `TENTRY` arrays, the `MTRACK` tempo list, the pattern table, pattern
  headers, and `NOTE_DATA` note streams are byte-swapped. Byte-oriented pitch-bend
  and modulation streams need no conversion.
- Native text rendering checks explicit string lengths before dereferencing the
  next character. Palette entries, MREA section buffers, and map buffers now return
  to the allocator that created them during runtime and delayed shutdown cleanup.
- AGSC group buffers are retained for the session on PC. `hwSaveSample` never copies
  samples into ARAM here, and the MusyX 2.0.0 `sndPopGroup` path can leave voices
  referencing sample data after a group is popped, so freeing the buffer left the
  audio thread reading unmapped memory (confirmed with AddressSanitizer).
- `CCameraFilterPass::DrawRandomStatic` previously faked a random main-memory
  address as its texture source (the GameCube renderer ignored the pointer, the PC
  renderer hashes it). It now samples a real scratch buffer of noise, sized for the
  tiled IA4 extent.
- DVD ARAM streaming state is serialized with a recursive mutex: `OSDisableInterrupts`
  is a no-op on PC, and the DVD worker and main threads raced on the transfer
  counters until `mBufferLen` went negative and `ARQPostRequest` memcpy'd a huge
  length. Non-positive transfer lengths are also treated as complete.
- Vertex array byte sizes are threaded through to `GXSetArray`. Aurora uploads
  `size` bytes of each attribute array, and the port was passing 0, so every
  array-based draw (MREA world geometry, CMDL models, skinned models) uploaded
  zero bytes and rendered nothing while immediate-mode effects and the HUD still
  drew. Positions, normals, colors, and UVs now carry the section sizes from the
  MREA/CMDL loaders.
- The PC MusyX mixer render thread is paced to real time. It previously free-ran
  (measured ~29x real time) because it only throttled on the SDL queue depth,
  overrunning the stream and producing dropouts; it now sleeps until the next
  160-sample frame is due, which removed the buffer-boundary discontinuities.
- The AGSC sample directory's trailing ADPCM info blocks are now preserved and
  converted for little-endian hosts, and each entry's `extraData` offset is rebased
  onto the native `SDIR_DATA` array (whose entries are larger than the disc's
  32-bit form). Without this, in-level voices decoded with garbage coefficients and
  produced noise.
- The disc sample directory ends with a 4-byte `0xFFFFFFFF` terminator rather than
  a full entry, so the ADPCM info blocks start at `(count - 1) * entrySize + 4`
  and not `count * entrySize`. The old base was 28 bytes too high, which skipped
  the first block and left samples whose information begins there reading
  coefficients from the entry table; the charge-beam looping layer (id 209) was
  the audible case and buzzed continuously. `MP_VALIDATE_SAMPLES=1` scans every
  loaded sample directory and reports any ADPCM sample whose rebased `extraData`
  is missing or does not hold `numCoef == 8` (verified clean across the front end
  and first areas).
- In-game streamed music now plays. The PC MusyX ARAM layer was entirely stubbed
  (`aramAllocateStreamBuffer` returned 0, `aramGetStreamBufferAddress` returned
  NULL, `aramUploadData` did nothing), so streamed voices got a null sample address
  and the software mixer skipped them; the stream never advanced and
  `UpdateStream` was never called. Each ARAM stream buffer is now backed by host
  memory and `hwFlushStream` keeps the full 64-bit host pointer instead of
  truncating it to `u32` (which faulted on the first upload).
- Streamed audio (front-end music) now decodes correctly: `DecodeMonoAndMix` wrote
  the decoded samples *on top of* the buffer still being played, feeding the
  output back into itself until it saturated into full-scale noise. It writes the
  decoded samples now, `IsReady` waits for every chunk rather than only the last,
  and the non-ARAM `CDvdFile` read is blocking (`DVDReadPrio`) so playback cannot
  start on unfilled buffers.
- `CARAMToken::UpdateAllDMAs` pumps `ARQPoll` before refreshing status. Aurora
  defers ARQ completion callbacks until `ARQPoll` runs on the main thread, but
  the map/pause texture eviction and room-transition code spin on a token
  (`while (texture.IsARAMTransferInProgress()) UpdateAllDMAs();`) and so never
  reached the main-loop poll; the DMA never completed and the game froze with
  audio still playing (opening the map always hung).
- The GameCube audio-interface DMA path is implemented in `platform/ai_dma.cpp`:
  the registered DMA callback is driven from the main loop (`AIPortPoll`) at the
  buffer rate and the submitted buffer is fed to its own SDL stream. Streamed
  audio (front-end/in-game music via `CStaticAudioPlayer`, movie audio)
  previously had no output at all because the AI functions were no-ops. It runs on
  the main thread because the guest mixer is not thread-safe, and the port keeps
  the full 64-bit DMA pointer that the SDK's 32-bit `AIGetDMAStartAddr` truncates.
  `CMoviePlayer::StaticMyAudioCallback` reads the previous DMA buffer through the
  same 64-bit accessor; the truncated form resolved to unrelated memory whose
  bytes were then mixed as audio. `MP_DISABLE_AI_AUDIO=1` isolates this path.
- Skinned vertex generation advances its output cursor explicitly. On the console
  the write-gather pipe advances itself as data is written, so `BuildPoints`,
  `BuildNormals`, and `Calculate`'s padding pass all reused one `pipe` value; on PC
  that made every bone overwrite the same offset and left the rest of the
  workspace uninitialised (vertex explosions). The vertex/normal workspaces are
  also one contiguous allocation, matching the points-then-normals write order.
- Runtime-generated vertex arrays are marked host-native (`le=true`) instead of
  big-endian, matching the port's native skinning/workspace writes. `ClearArray`
  forces the backend to drop its cached copy, which the skinned path needs because
  the workspace pointer is reused every frame. Map-screen mappable-object and area
  arrays are also host-native and provide their real byte sizes.
- `GXSetDrawSync`/`GXReadDrawSync` are real FIFO-ordered tokens in Aurora rather
  than a shim that echoed the last token. The skinned-model circular workspace
  frees a buffer once its token is readable, so an echoed token let the game reuse
  a workspace before Aurora's FIFO thread had copied its vertices, corrupting
  intermittent draws (the reported geometry explosion). The token command is
  processed by the FIFO worker in the same order as the draw that references the
  data, so the fence now holds.
- `F12` asynchronously reads back the resolved EFB and saves a 640x480 BMP under
  `screenshots/`. This avoids compositor-dependent tools and provides captures for
  diagnosing rendering regressions.

Host shutdown now completes cleanly. Game-heap buffers owned by `CGBASupport`,
`CStaticAudioPlayer`, and `SMediumAllocPuddle` are released through `CMemory` instead
of host `delete`, and the PC build skips the guest-stack usage scan that it does not
initialize.

## Next steps

Automated PAD input advances through the front end, loads the first room,
constructs `CInGameGuiManager` and `CMFGame`, and runs beyond frame 48,000 with an
AddressSanitizer-clean run past frame 21,000 and a normal exit on forced SIGTERM.

Verified:
- World, actor, and skinned geometry render (array sizes and array endianness were
  the draw-stopping bugs); the front end, HUD, and combat visor draw correctly.
- Left-stick input moves the player; the walk stays grounded and is constrained by
  room collision, so input, physics, and collision are live.
- The player stays alive (`CPlayer::x9f4_deathTime` remains 0).
- Streamed (front-end) audio decodes to tonal PCM, and in-level MusyX frames are
  tonal rather than noise after the ADPCM info-block conversion.
- Both SDL audio paths maintain a bounded queue instead of depending on exact
  5 ms thread/main-loop scheduling. MusyX also reads raw PCM16 sample payloads as
  GameCube big-endian data instead of host-endian data.
- In-game `.dsp` stream headers are converted from GameCube endianness after the
  DVD read. Without this, the native sample-rate check rejected every stream;
  traced intro playback now allocates a stream at 32000 Hz.
- GPU captures from the current build show the publisher screen, `[ PRESS START ]`
  title screen, and no-memory-card dialog rendering correctly. The user's reported
  blank front end therefore needs a capture at the exact failing transition.

Remaining:
1. Directed input needed to reach and open a door; wandering for ~48k frames never
   triggered a second `CWorld::TravelToArea`, so room transitions are unverified.
2. Retest title music, spaceship music, and effects by ear after queue-depth and
   PCM16-endianness fixes; verify pitch/tempo and that spaceship music starts.
3. Confirm the draw-sync fence removed the intermittent skinned geometry
   explosion with an F12 capture at the failing frame if it still occurs.
4. Replace the session-long AGSC buffer retention with a bounded lifetime.
5. Verify CARD saves and the remaining menu flows.
6. Reproduce the reported blank front-end screen using `F12`; current automated
   captures do not reproduce it.

Wayland presentation keeps the game EFB locked to its configured 640x480 with
`VISetFrameBufferScale(1)`, while Aurora scales that image to the native high-DPI
swapchain. This prevents the title background from disappearing at fractional
display scales. Compositor vsync is disabled because it misses presentation
intervals around the game's own async-idle work; an absolute 60 Hz deadline is
used instead.

Debug shortcuts: `F10` toggles the 60 FPS deadline/unlimited mode; `F12` saves
the resolved framebuffer under `screenshots/`. Unlimited mode changes only the
presentation rate; simulation, input, SFX, and streamed audio remain on Prime's
fixed 60 Hz clock.

Debug env flags for fast iteration: `MP_FAST_BOOT=1` skips the pre-front-end and
drives the title through file select into a new game without input;
`MP_SKIP_CUTSCENES=1` skips cutscenes that set a cinematic skip object and
fast-forwards the ones that do not (the opening frigate sequence deliberately has
no skip object), so control is granted in roughly 15 seconds instead of minutes.

HD textures: `MP_TEXTURES=<dir>` loads replacements with Aurora's
`tex1_<w>x<h>[_m]_<texhash>[_<tluthash>]_<format>.dds|.png` convention (hash
fields may be `$` wildcards). The format may be Aurora's numeric GX format or
Dolphin's name (`CMPR`, `RGBA8`, `C8`, ...); Dolphin also hashes with
`XXH64(data, size, 0)` and uses the same `tex1_` layout, so Dolphin packs should
resolve once the hashed size and paletted tlut handling are confirmed against a
real pack.

Lock-on uses `CPlayer::WithinOrbitScreenBox`/`WithinOrbitScreenEllipse`, which
compare the target's live-viewport screen position against the player tweak's
fixed 640x480 coordinates. Those coordinates are now scaled by the viewport size;
otherwise the lock-on zone sits left of the reticle in widescreen and centred
targets are never acquired.

Aspect ratio is selectable via `MP_ASPECT=4:3|16:9|window` (or the debug
overlay's Render tab, applied live): 4:3 is the original 640x480, 16:9 is a fixed
854x480, and `window` tracks the window on `AURORA_WINDOW_RESIZED`. Each frame the
game recomputes the render-mode width (`CGraphics::PortResizeFrameBuffer`) and
refreshes `CCameraManager`'s cached aspect; Aurora derives the internal EFB from
the render mode, so the horizontal FOV, culling frustum, and present all widen
together. The port enables `AURORA_VIEWPORT_FIT`, so the EFB matches the selected
aspect and the present letterboxes rather than stretching when the window shape
differs. The HUD is anchored to the view edges and scales with it.

Mouse aim (`MP_MOUSE_AIM=1`, or the debug overlay's Input tab) captures the
pointer and drives the first-person camera directly, similar to how PrimeHack
takes over the game's aim: `CPlayer::Update` integrates the relative motion into
a world yaw/pitch (pitch clamped to the tweak's vertical free-look limit) and
keeps the body facing that yaw so movement stays view-relative, while
`CFirstPersonCamera::UpdateTransform` builds the view from those angles. It does
not use the game's free-look angle, which is a limited head offset that wraps
past 90 degrees. Orbit, jump/fall cameras, and lock-on still override aim; the
pointer is released while the debug overlay is open. `MP_MOUSE_SENS` sets radians
per pixel (default 0.0035).

Wayland can report `SDL_SetWindowRelativeMouseMode` as active while the
compositor still leaves the pointer visible, so the port also hides the cursor
(`SDL_HideCursor`) and recenters the pointer with `SDL_WarpMouseInWindow` when it
nears a window edge; the event loop rejects deltas above ~300 px so the warp
itself does not spike the aim.

`F1` toggles an in-game debug overlay (Aurora's ImGui) with sections for
Performance (frame limiter, FPS), Cutscenes (skip and speed), Render (vsync and
internal EFB scale), Audio (mute the streamed/AI path or MusyX independently),
and Session (restart to menu, screenshot). The same settings are read from
`MP_FAST_BOOT`, `MP_SKIP_CUTSCENES`, `MP_CUTSCENE_SPEED`, and
`MP_SHOW_DEBUG_UI` at startup, and the overlay writes them live. `MP_DISABLE_AI_AUDIO`
still exists for isolating streamed audio at startup.

Unlimited presentation interpolates the active world camera between the two
most recent simulation transforms. Camera switches, translations over four
meters in one tick, and rotations over 45 degrees reset interpolation so cuts
and teleports are never blended. Actor poses still update at the fixed
simulation rate. The first-person weapon needs no interpolation because it is
drawn camera-relative (`offsetWorldXf.GetInverse() * CGraphics::GetViewMatrix()`),
so it stays anchored to the camera frame; only its own 60 Hz animation remains
stepped.

`assets/initial_pipeline_cache.db` contains machine-independent Aurora pipeline
descriptions collected from the title, menus, and intro gameplay. CMake copies
it beside the executable; Aurora merges it into each user's persistent cache
and compiles the entries on its background pipeline thread.

## Licensing

- Aurora: MIT. Port-specific code: ours.
- The decompiled game/engine source and the game assets remain Nintendo's; this
  is the usual decomp-port situation. Ship no assets; require the user's disc.
- The earlier recomp path (GPL: DolRecomp / ModernGekko) is preserved separately
  and is not linked into the port.

## Preserved recomp baseline

- git tag `recomp-baseline-2026-09-18` in the MetroidPrimeRecomp repo.
- Backup bundle and build artifacts under `/home/odran/backups/` (`mpr-*`).
