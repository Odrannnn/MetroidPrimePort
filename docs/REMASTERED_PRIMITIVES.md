# Remastered generator primitive dispatch

Effects import stage: **111**. Evidence is from the shipped Switch binary;
addresses are offsets in decompressed `main.raw`.

Material/readiness follow-up: [REMASTERED_MATERIAL_READINESS.md](REMASTERED_MATERIAL_READINESS.md).
The 41 changes are eligibility changes: 23 nodes use a header MTRL and 18 use
nil-BCLR MATIs. None has a null resource. Actual authored output includes
zero-capacity/zero-alpha nodes, additive black and two alpha-black sprites.

## Removed suppression heuristics

1. PART without TEXR, MTIN or PMDL was treated as drawing nothing.
2. PART without TEXR/PMDL whose MATI contained nil BCLR was treated as a
   placeholder: MTIN was skipped and the generator hidden.
3. PART with PMDL but without TEXR/MTIN was classified as models-only.

All three suppressed sprite draws by discarding SIZE and appending SIZE=0.
The importer also retained disc effects for all-placeholder trees, except
three asset-ID exceptions. Those placeholder rules and exceptions are removed.

## Binary rules

- LoadGeneratorDesc (`0x256880`) decodes GPSM+21 as primitive+1.
- SetupPrimitiveTypes (`0x29069c`) enables sprite for primitive 1,
  transformed for 2, transformed+model for 3. A model with no models is made
  inactive. Types 0/4/5 disable ordinary geometry, preserving the other
  generator roles (children/lights).
- FixAndClean (`0x2908a4`) fixes active blocks and clears inactive blocks.
  Material/texture absence and nil BCLR are not primitive-selection inputs.
- Static-state constructor (`0x28bfc4`), table `0x1d0b9b4`: primitives
  1/2/3 become render kinds **1/2/0**.
- UpdateRenderDataForContext (`0x2b0cac`) selects ordinary sprites,
  transformed planar squares (`0x2ba3f8`), or models.
- Render (`0x2b23e8`) sends kinds 1 **and 2** to RenderSpriteEntries
  (`0x2b13a8`); kind 0 goes to RenderModelEntries (`0x2b176c`).
- SSpritePrim::Fixup (`0x28e57c`) consumes sprite SIZE. Transformed Fixup
  (`0x28ec98`) moves SIZE from SParticle+0x70 to transformed+0x68 and
  aliases SParticle+0x40. Thus types 2/3 must retain the evaluator too.
- SolveParticleTransform (`0x246f14`) applies position, orientation, PMOP,
  local rotation, PMSC and PMPV. It does **not** multiply SIZE into geometry.
  PMSC components are clamped to 1/256 at `0x2472ac`.
- Planar-square serialization (`0x2bac2c`/`0x2bad3c`) carries that matrix;
  shader 7498e54a, PMOD=0, permutation 060_1 transforms the unit quad through
  it. Default extents are (-.5,-.5,.5,.5), initialized at `0x28a600`.
  Port-local corners are B(u-.5,v-.5,0)=(.5-u,0,v-.5).

The port carries non-default primitives in `PPRM CNST n`; a converted PIRN
PART defaults to sprite (1). Retail PARTs retain their original dispatch.
Primitive 2 reuses the retail transformed-square/model-transform path, with
its sprite material, COLR, UV handedness and depth/blend states. VMAT squares
use a transformed quad rather than the camera-facing sprite routine. SIZE
remains evaluated; it is not used to hide any active primitive.

## Full-corpus census (2026-10-25)

Reproduce with `remastered_effect_tool census <romfs> <output-dir>`.
The TSV records each root/embedded GPSM occurrence in each unique GENP;
duplicate assets across pak files are counted once. These are **draw-path
eligibility changes**, not a measurement of visible pixels or particle counts.

- 340 pak files; 1,434 unique GENPs; all parsed, 0 failures.
- 5,011 GPSMs: types 0/1/2/3/4/5 = 1174/2455/104/1037/237/4.
- Old sprite suppression predicates → new sprite dispatch:
  **41 suppressed→drawn, 495 drawn→suppressed**.
- 489 of the latter are model generators: their model pass remains enabled.
  Whole-generator primitive eligibility: **41 suppressed→drawn, 6 drawn→suppressed**.

The 495 figure compares the old converter's `drawsNothing || modelsOnly`
predicate with explicit primitive dispatch. It does not assert 495 extra cards
were visible: the old VMAT renderer also had a PMDL-based sprite guard.

Five suppressed→drawn examples (asset / node; all primitive 1):

| Asset | Node | Pak name if present |
|---|---|---|
| 00aadc3b-c393-4e1b-b379-2b114bc1bbe8 | root | |
| 10000000-0000-f000-f000-0000572c7dc6 | root/3 | |
| 10000000-0000-f000-f000-0000b071be30 | root | |
| 10000000-0000-f000-f000-0000b0f9dbe6 | root | EmptyMuzzle |
| 12c0d553-a2da-49e0-b9da-55ac6756bba9 | root | GenericGPSM |

Five sprite drawn→suppressed examples (all retain primitive-3 models):

| Asset | Node |
|---|---|
| 003b9860-8a9b-45a8-a690-78a3f7baebd2 | root/0 |
| 00eb9ff4-1027-4b50-9fed-8f236d3439ab | root/2 |
| 0165da4a-67ee-4238-a7ed-3e24a415170e | root/0 |
| 0165da4a-67ee-4238-a7ed-3e24a415170e | root/1 |
| 0201e622-77de-4184-a13e-097b69094cf6 | root/0 |

Five whole-generator drawn→suppressed examples:

| Asset | Node | Primitive | Pak name if present |
|---|---|---|---|
| 10000000-0000-f000-f000-000052e2d0fb | root | 0 | OutOfBall |
| 10000000-0000-f000-f000-0000ca8be45f | root/6 | 5 | |
| 541f775f-c171-4b2e-a182-6ada992a21dc | root | 0 | Plasma2nd_1 |
| af780373-de26-4349-8c27-dc49df6a9f10 | root | 0 | |
| c78576e0-3e6e-49f2-a5c3-16cbbd6f9b41 | root/1 | 4 | |
