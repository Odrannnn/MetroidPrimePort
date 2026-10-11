# Remastered particle effects (GENP)

Metroid Prime Remastered stores its particle effects as `GENP` assets (1434
unique in the romfs). They replace retail's `PART` and fold its separate
swoosh, electric, weapon, collision and decal assets into the same file. This
page describes the format as far as `platform/port_remastered_effect.cpp`
reads it, how it maps onto retail, and what does not parse yet. The import
converts effects to retail PART (see "In the import") when its particle-effects
option is on.

## Container

- RFRM header (`RFRM`, then `GENP` at 0x14), with the root generator at 0x3c.
  A `FOOT` form follows the data and is ignored.
- FourCCs inside the form are stored byte-reversed (`CNST` is the bytes
  `TSNC`), so a little-endian u32 read gives the packed big-endian value.
- A generator is `GPSM` plus a 21-byte header. The u32 at +21 is 1 for the
  root. Then comes a flat list of properties: FourCC, a **tag byte**, value.
- **Tag byte**: when the value is evaluated. `00` once (static: flags, modes,
  textures), `01` per emitter (rates, counts), `03` per particle (colour,
  size, velocity, lifetime). `04` marks `_END`. It agrees with how retail
  evaluates each property; retail just did not store it.
- The root's `_END` is followed by a u32 count (at most 64), then that many
  children, each a 16-byte id and a form:
  - `GPSM`: a child generator. These replace retail's ICTS/IDTS/IITS/KSSM
    child PART ids. When the child was a retail PART, its id is that PART's id.
  - `SWSH` ≈ SWHC, `ELC2`/`ELSM` ≈ ELSC, `WPSM` ≈ WPSC, `CRSM` ≈ CRSC,
    `DPSM` ≈ DPSC, plus `EPSM` and `SPSM` (no retail equivalent found).
  - A child's own `_END` is a plain FourCC and tag.

## Ids

Ids inside effects are little-endian UUIDs. `EffectGuidString` prints them
that way. The pak reader prints ids in stored order (`IdToString`), so to look
one up in a pak, swap bytes 0↔3, 1↔2, 4↔5 and 6↔7. Assets carried over from
retail use `10000000-0000-f000-f000-0000XXXXXXXX` (in pak order), where
`XXXXXXXX` is the retail asset id. Some effects keep that id ("exact pairs",
80 GENPs). The rest got fresh UUIDs but often kept retail's name (the gun
effects: `PowerMuzzle`, `WaveCharge`...).

References from the 1306 parsed effects (5430 in all):

| Resolves to | Count | Notes |
|---|---|---|
| MATI | 2563 | Material instance. All 2317 distinct ones name an MTRL, and 2259 also bind a TXTR. |
| (no asset) | 2035 | Parameter/variable ids (VARF/DPVF/TPVF..., SEMR arguments) |
| CMDL | 665 | Particle models (PMDL) |
| VECF | 87 | Vector fields |
| TXTR | 79 | TEXR/TIND, mostly retail TXTR ids in the carried-over form |
| SWSH | 1 | |

## Grammar

Nothing records an element's arity or a value's length. One FourCC means
different things in different slots: `CNST` holds an int, a float, three or
four elements, a byte or an id. So the reader is a memoised backtracking parse
that keeps the reading that lets the file parse to its last `_END`. The
signature table (`kElementSigs`) is retail's element arities merged across
slots, plus Remastered's changes. Properties default to one of `e`, `b`,
`b g`, `g` or `b e`, with overrides for TEXR, SMVR, SORT, FRMD, PSPS, MTIN,
SNRD and SNRA. KSSM (spawn table), PVAR, TMTR, PMTR and SMTR have dedicated
readers. GRAD, ARRY and KEWS are read as raw blocks.

The untyped table alone lets nested values be read across their neighbours (a
vector's `CNST` taking four arguments and eating the next one, or three nested
`CNST`s read as an id). So a property retail knows is read first as the type
retail reads it as (int, real, vector, mod vector, colour, emitter), from
per-type tables (`kIntSigs` and the rest: retail's elements and arguments plus
Remastered's MPCB, MPAC, ANCR, ASPR, RNDV, DFCP/DFCS, MPRD and the parameter
reads). Inside a typed value every argument is typed in turn, so a vector's
`CNST` is exactly three reals and a real's `CNST` one word. An element the type
does not list is read from the untyped table; one it lists is only read typed.
The whole property falls back to the untyped defaults when the typed reading
does not parse, so typing only ever chooses between readings that already
parsed. Remastered's flags are a `CNST` and a byte (IMPL/LMPL/EMPL/BNCE's
last argument, ELPS, ATEX). Emitter shapes pinned from the shipped bytes:
`PLNE(v, v, v, r, r, r)`, `ELPS(v, v, v, r, flag)`,
`PLNV(v, v, r x8, byte)`; colour `MDAO(c, r)` and `SLCT(r, ARRY)`;
`SMOV(EXTT, v, EXTR, NONE, NONE)` with EXTT/EXTR/EXTS as leaves, `TRST(r x5, word)`,
`GRAV(v, byte, NONE)` (in SMOV's slot), `ATX2(id, e x4)` in TEXR. A 16-byte id
holding an element FourCC is not read as an id.

KSSM is `NONE`, or `CNST`, u32 1, 1, the end frame and an SEVT event count,
the events (`SEVT(SMOV(...))`), then a u32 table count and that many tables.
A table is a u32, a selector element and a u32 frame count; each frame is a
u32 frame and a u32 spawn count, each spawn a 16-byte child id, the child's
form (GENP, SWSH, ELC2 or ELSM), a u32 and an element (`NONE`, or what looks
like a chance). The ids are embedded children of the same effect, at any
depth. `ParseSpawnTable` reads one back for the converter.

The GPSM header u32 at +21 is a flag word, not a boolean: the top-level
GPSMs carry 1 (1113 files), 2 (250), 4 (47), 5 (18), 3 (5) and 6 (1), and any
non-zero value may have a child list after `_END`. A flagged embedded GPSM can
also have none: its `_END` is then followed directly by the next sibling's id.
The reader takes the child list when a count and children parse there, and
ends the node at its `_END` otherwise (only the effect's own root must have
the list). Property tags go up to 05 (LRAD).

KEYF with the 30-byte header (first u32 2) is followed by the element that
drives it (`KEYF(keys, GTCP)`, `KEYF(keys, PSA0)`). SMTR/PMTR entries are an
element then a `CCHn` tag and two bytes (channel, slot), five count-prefixed
groups. More emitters: `SPEV(v, r x6, byte)`, and in a vector slot
`ANCV(REUL, r x7, byte)`.
Unknown elements try their larger arities first when building the tree.

New elements, with arities known:

- **SEMR** replaces SETR as the emitter.
- **MPCB** wraps every vector: either a cartesian vector, or `MPAC` angles
  plus a radius.
- **MPRD** (random int), **RADD** and **REUL** (rotation), **PEOD**, **SPAx**,
  and the **DPVx/TPVx/VARx** parameter reads.

Elements whose arity is only inferred are listed in `kLooseElements`.

## Mapping to retail PART

Compared property by property over the 67 exact pairs that have a retail
PART on the disc:

- **Direct**: value identical, or the same shape with different numbers.
  - 176 property values were byte-identical, 20 kept their shape but changed numbers.
  - TEXR/TIND: retail `CNST(CNST(id))` becomes `CNST(id), NONE`, with the
    same TXTR id.
  - LTME becomes **LTM2**, one frame longer: across the 47 exact pairs that
    convert, LTM2 is always the disc's LTME + 1.
- **Approximate**: same meaning, re-encoded.
  - EMTR: SETR becomes SEMR. VEL1-3, PMOP, PMRT and the emitter's vectors
    are wrapped in MPCB; stripping it gives retail's vector.
  - ROTA is negated: `SCAL(5)` becomes `MULT(SCAL(5), -1)`, and 180 becomes -180.
  - LFOT and LTYP: int CNST becomes an enum byte. The exe reads LFOT's byte
    as retail's value and swaps LTYP's 1 and 2 (retail 1 is `#02`). The pairs
    seen in the data (LFOT 2 → `#03`) look like a shift by one, but are retuned
    effects, not the encoding.
  - PSLT loses its inline PSTS, which becomes its own property.
  - Child PART ids (ICTS/IDTS/IITS/KSSM) become embedded children.
- **Dropped**: properties retail stored at their default. Of the omissions,
  983 were `CNST #00` and 391 were `NONE`. Remastered omits defaults; nothing
  is lost.
- **No retail equivalent**:
  - On every effect: **DVVN** (byte, always `#04`), **IEXP** (`#01`),
    **XFMD** (int, e.g. 5).
  - On about half: **PBDM** (int, blend mode?).
  - Rarer: PSTS, INTT, ITEN, INTR, LIRD, LORD, SSZE, SMVR, SCTR, FXBR, DBIS,
    SBIS, VGD2/VGD3, MTIN (byte plus MATI id: a material instead of a bare
    texture), PVAR/TMTR/PMTR/SMTR (parameter tables).
  - These need a renderer feature or a default when converting.

## Coverage

1430 of 1434 unique GENPs parse (99.7%); 1433 of 1437 counting duplicate
copies (2026-10-05). A texture value can hold TXP2 (id, three elements), TXFB
(id, one to four), ATX2 and a property ANTH (an ANTH element: id, TRST, two
keyframe blocks, a real), and TIND takes the same `CNST(id), NONE` pair as
TEXR. The 4 failures stop at an EMTR (92278041, C05C4D6B, C64A1F1A) and a
TEXR (FAEE17B6).

Port-only properties (the runtime reads them; contract in the effect lane's
notes): **PATL** (`CNST id` + five ints cols, rows, count, mode, flipX: an atlas
TXTR, tile k at column `k % cols`, row `k / cols` from the top; mode 0 random
tile, 1 life flipbook, flipX a random mirror) replaces a TEXR/TIND of TXP2
(`IRND(0, 1f)` is a real over the whole atlas: 4x4 is 16 tiles) or TXFB
(an array TXTR packed into one atlas, at most 2048 a side; TRST must be the
identity or the `KPIN(CREL(LTHN(RAND(0,1)), .5, 1, -1))` mirror). **PMDV**
(`CNST n`, n model ids) follows PMDL (the first id) for a
`SLCT(IRND(0, n-1), ARRY)` of Remastered-only CMDLs, which the import converts
standalone under fresh ids. **ATX2** (id, columns, rows, a cycle, loop; only one effect, C6B5B5A9, uses it) is retail's ATEX as a grid; its cycle `ILPT(100)` is retail's CIELifetimePercent, the whole life in frames, so it becomes a life PATL over every tile. Anything else of those shapes is left out.
With 437 matched fresh effects (below), 489 of 512 effects import (1736 PARTs, 176 textures of which 1 flipbook, 94 models). PlasmaCharge, PlasmaMuzzle and PlasmaAuxMuzzle (D3053354, 8D7BBFB2, B0F9DBE6), 06B3F06E and C6CBF848 keep theirs, since their root's material texture doesn't resolve. 6 more keep theirs for the same reason in a child.

The `scan` command prints each failure with its offset and the bytes there;
those are the grammar gaps to close next.

The parsed effects embed 3571 GPSM, 95 SWSH, 23 ELC2 and 8 ELSM children; all
of them convert (below). The other child forms (weapon, collision, decal) do
not occur embedded.

Only 80 of the 1434 effects keep a retail id; 1354 have fresh UUIDs. Mapping
those back to retail PARTs (checked 2026-10-05):

- By name: 98 GENPs have a pak name (`# name` in `scan`'s dumps), all player
  and global effects. The 8 with a retail id all match the retail pak's name
  for that id, so a name is reliable where there is one. Of the 90 fresh ones,
  58 have exactly one retail PART of the same name (the beam muzzles, charges,
  Xfers, wakes, grapple, bombs...); 32 are new or renamed (`BallInnerGlow_*`,
  `*ChargeMuzzleFlash`, `pwrBase_placeholder`...). The other 1264 fresh GENPs
  have no name. The import takes the 58 as candidates through `kMatchedEffects`
  (`port_remastered_effect_import.cpp`, a table of id and retail PART, since
  the importer doesn't read pak names).
- By reference: 20 GENPs are named by another GENP, 19 of them fresh, and every
  one of those 19 is named only by fresh effects. No effect with a retail id
  leads to a fresh one.
- By property values (2026-10-05): each converted root against every disc
  PART, scored by the share of its non-asset (fourcc, value) pairs the disc's
  has. On the 78 retail-id effects the top match is right 49 times; on the 58
  name-matched fresh ones only 5 times, so Remastered re-authored the fresh
  effects and their values don't identify them. Only 23 of the fresh ones
  clear a rule that is exact on the known pairs, and that rule is calibrated
  on retail-id effects. Not used.
- By placement (2026-10-05): every fresh GENP is named by some asset (ROOM
  1233, CHPR 472, and others; `build/fx-refs/`). A ROOM `EffectMP1` that names
  one effect, at the spot (under 0.1 m, through the room-to-area transform) of
  a retail object that names one PART, pairs all 97 such placements of the
  retail-id effects correctly. On the fresh ones, 211 pair the same way in
  every placement, and 37 pair with different PARTs in different rooms. 150
  are kept in `kMatchedEffects`: the 211, minus those whose PART another fresh
  effect also claims (51), one that a retail-id effect already carries (9),
  and one that a name match carries (1). Enemies name several PARTs, so they
  don't pair this way. Script: `build/fx-room/effpair.py`.
- By character (2026-10-05): a CHPR's id is its retail ANCS's id (retail-id
  form), and a dependency list near its end (`01`, a u32 count, then fourcc +
  guid; `PNEG` = GENP) names its effects. The list's order isn't the ANCS's,
  and its particle events (no names, no float times) aren't decoded, so the
  pairing is by set: in a CHPR with one unpaired fresh GENP, whose ANCS plus
  EVNTs has one PART not paired yet, the two pair. Hiding a known pair, the rule
  re-derives 9 of 9; one GENP named by 13 more CHPRs that lack the PART is
  dropped. 10 pairs are kept. Unit propagation finds a few more but can't be
  checked, and most CHPRs leave 6 or more candidates. Scripts: `build/fx-chpr/`.
- By event (2026-10-05, from the Switch exe's CCharInfo reader): after the
  skeleton and animations a CHPR has four optional blocks, each behind a bool:
  the effect set (the dependency list above: i16 effects, i16 param sets, 20-byte
  tags of reversed fourcc + guid), CActionData (i32 streams, i16 n8, i32 ints,
  i32 names, i32 name bytes, 4 bools; then per stream an i16 size, a u8 op and
  bytecode; per stream an (i16 start, i16 count) into the i32 effect-set
  indices; n8 x 8 bytes; names), STransData, and the event streams (i16 key,
  u16 size, then words `count<<24 | time`, each followed by count u32 ids, ended
  by a zero count). Op 0 spawns a particle; its bytecode holds the bone as a u16
  of bone index x 192 (`[05|06] u16 [08-0a]? 0b`). Event ids `0xD0000000 | n`
  fire action n; times are 1/480 s, retail EVNT's own. An unpaired fresh GENP
  pairs with the ANCS's PART event at the same bone and frame when that match is
  unique, every CHPR naming it agrees, and no other effect claims the PART.
  Hiding a known pair, it re-derives 7 of 7, none wrong (9 give no match: the
  rig lacks retail's locator, retail has no such event, or the times differ).
  67 pairs are kept (`3d0a74cd`). Then (`4441d0cb`), with paired PARTs taken
  out and repeated until nothing changes, looser rules: the same bones and at
  least half the frames (within one), the same frames on other bones, or shared
  bones and half the frames. Hiding a pair from both event blocks, each rule
  alone re-derives 46 to 67 of 83, none wrong; 38 more pairs. 121 of the 232
  fresh GENPs in CHPRs are still unpaired, among them 4B55EA17's two claimants
  (Ridley's and FA049A5D's). Scripts: `build/fx-evt/` (`NOTES.md`).
- By script slot (2026-10-05): Remastered keeps retail's property order
  (dropping null fields, adding some of its own), so a ROOM object and the
  retail SCLY object it stands for (same type, same spot) name their assets in
  the same order. Assets both sides share (retail-id assets, effects already
  paired) are anchors; between two neighbouring anchors, a stretch with as many
  fresh GENPs as PARTs pairs in order. A GENP is kept if every placement agrees
  and no other effect claims the PART. Hiding a known pair, it re-derives 169 of
  193 (11 of 17 on enemies), none wrong. 114 pairs are kept: 64 Effect objects
  (11 more dropped where the placement rule picked another PART at a stacked
  spot) and 50 enemy slots (Burrower, Thardus, OmegaPirate...). Remastered
  sometimes folds several retail PARTs into one GENP or the reverse, and puts a
  GENP where retail has a WPSC; the count rule skips those stretches. None of
  the 121 CHPR-unpaired GENPs is in a script slot. Scripts: `build/fx-scr/`.
- Pickup pairs audited (2026-10-05): all hold. `03_over_pickup` and
  `pickup04` in the table are room names, not effect names (the placement
  pair agrees in 6 of 7 placements; the slot pair has one). The by-character
  pickup and beam pairs (IceBeam, WaveBeam, PlasmaBeam, Health, ShieldBase,
  powerbomb...) are each named by one CHPR whose ANCS has that PART only.
- By event, one character at a time (2026-10-05): Remastered reuses one GENP
  for different retail PARTs in different characters (3ec1f3bd stands for five)
  and sometimes for two PARTs in one, so the table may list an effect more than
  once and the importer writes it under each PART. Within one CHPR, an unpaired
  GENP and an unpaired PART of its ANCS pair when their event bone sets are the
  same and unique there (or, among several, their frames match only each other),
  any frames meet within one, and every ANCS using the PART agrees: 75 of 135
  re-derived, none wrong. Then frames alone, any bones (at least two frames meet
  on each side, covering half of either, the only such partner on both sides):
  33 of 135, none wrong. 18 pairs from the rules plus 6 by hand (frames, and
  textures for Thardus's arcs); 4B55EA17 goes to Ridley's 500b48b1, which three
  ANCS agree on. Import: 513 of 536. Later (2026-10-07), Ridley's c3a53327 and
  d701c231 by which effects fire together in one animation (the event key's
  group against the retail EVNT's), and their generators. Scripts: `build/fx-hand/` (`bones2.py`,
  `frames.py`, `view.py` lists what is left per character, `sheet.py` draws
  texture contact sheets).
- By action name (2026-10-08): six CHPRs (2ECB9FBF, 65177C3A, 6B45F06D, 8169653A,
  8DC8052E, F9F4B18F) keep CActionData's names block (ext block: ne pairs of u16
  start/stop stream ids, then ne NUL-terminated names), the same named effect
  list retail's ANCS holds. A spawn stream (op 0: tag index at d[7:9], bone
  u16/192 after 05) gives the slot's GENP and bone; the retail ANCS effect of
  the same name whose locator is that bone gives the PART. It re-derives the 15
  known slots (3 GENPs) with none wrong, and adds 10 GENPs (Scuba*, Eyes,
  JetPack, LandingSmoke, Fire1-3's three, TwoEyes). Left out: SpeedSwoosh (a
  retail SWHC, not a PART) and 74d36fa3 on D3BD37FC (one slot, against four for
  4d818e87). Only these six CHPRs carry names. Import: 539 imported (529 before).
  Scripts `build/fx-name/` (`vote.py`).
- Still unpaired (2026-10-08): the CHPR effect sets name 242 fresh GENPs, 103
  of them unpaired (93 of the 232 in a retail ANCS). These rules were tried and
  fail when a known pair is hidden, so none is used:
  - Texture ids: only 2 GENP textures are carried-over copies.
  - LTME/MAXP: the child generators' values differ from retail in 291 of 423.
  - Tag order against the ANCS/EVNT PART order: about half the pairs are
    inverted, which is random.
  - The op-0 u32: it is not a name hash.
  - Texture image similarity (retail TXTR against the GENP's textures,
    `build/fx-tex/`): 17 of 77 right. Above a 0.3 margin it gains only 2 pairs
    over elimination, and a wrong pair sits at 0.289.
  - Intersecting the PART sets of every CHPR that shares a GENP: 1 right, 3
    missed. A shared GENP stands in for different PARTs in different
    characters.
  - Converted properties against retail's, only among one character's PARTs
    (`build/fx-hand/props.py`, `pdump`): 28 of 96 right among PARTs on the
    GENP's bones, 31 of 139 in the whole character. Exact bytes, numeric
    closeness and FourCC sets all stay under 45%, at any margin.
  What is left is mostly enemy CHPRs (MainNode's 15 L_Ball_SDK effects against
  16 PARTs, Sheegoth's 18 against 19) whose Remastered events carry no frames,
  on one bone, with look-alike textures; or groups with no free PART at all.

### Projectile weapons (WPSM)

A Remastered WPSM has the name of the disc's WPSC (PowerBeam, WaveBall, SuperMissile...;
`Retail::WpscId`, lower-case). Its APSM/APS2 GENP and ASW1-3 SWSH refs (`<reversed tag> 00 <guid>`)
replace the PART/SWHC the WPSC's field of that name holds (`ProjectilePairings` in
`port_remastered_import.cpp`, fed to the converter as `EffectImportIO::pairings`, method `wpsm`).
PowerAuxMuzzle and BusterSwoosh1/2 are paired by name. Beam and ball share some retail ids (Ice/Wave
APS2, Ice ASW1): the later one converted wins. The disc has no PART for the `*MuzzleFlash`es or
BusterImpact, so those stay unpaired.

## Converting to retail PART

`platform/port_remastered_effect_convert.cpp` writes a parsed effect as retail
PART. It is driven by retail's reader (`CParticleDataFactory`): each property
retail knows is written as the type retail reads it as, and each element in it
must be one retail has for that type, with retail's arguments. On the way it
undoes the re-encodings above: LTM2 is written as LTME less one, LTYP's 1 and 2 are swapped
back (LFOT is kept), MPCB is stripped (cartesian form only), ROTA is negated back (a
`MULT(x, -1)` becomes `x`, anything else is wrapped in one), TEXR's
`CNST(id), NONE` becomes `CNST CNST id`, an MTIN stands in for a missing TEXR
through its material's texture, words and keyframe blocks are byte-swapped,
and ids become retail ids through a callback (by default only the ids carried
over from retail resolve).

What does not convert is left out and listed: Remastered-only properties, an
element retail does not have in that slot (RADD, REUL, MPRD, the parameter
reads...), MPCB's angle form and an id with no retail id. Retail then uses its default for the
property. `droppedRetail` counts the left-out properties retail does read, so
a caller can skip effects that lose something that matters.

Embedded children come out as files of their own under their child ids, typed
by their form: GPSM as PART, SWSH as retail SWHC (`CSwooshDescription`), ELC2
and ELSM as retail ELSC (`CElectricDescription`). So SSWH/SELC/GPSM references
between children resolve. Each child type has its own property table from
retail's reader. In a swoosh, SBDM (0/1) is written as AALP, and MTIN stands in
for TEXR. A swoosh's PROT is left out: retail's IROT/ROTM are not the same
property. ELC2 is Remastered's electric form with no LWD/LCL, so as ELSC it
draws only the generators and swoosh it names. A zero id is written as NONE.
On the US disc's one exact child pair, ELSC 624A7606 (in FF5DC7A2) converts
byte for byte. SWHC 88D02992 (in 1A14AD75) matches except COLR (re-authored
keys) and LENG (`ADD(12, 1)` against the disc's 12, which is authored, not an
offset).

Keyframe blocks keep retail's layout; a colour's keys may be four halves
(8 bytes), which are widened to floats.

KSSM: retail's is `CNST`, four ints (0, 1, the end frame, 0) and a frame table
where each frame has a count and 16-byte entries (a PART id, three zeros);
retail spawns only generators, and only while the frame is below the end frame
and PSLT. Remastered's tables (above) are merged into that one table: its
GENP spawns go to their frames, the first SWSH spawn becomes the generator's
SSWH started at SSSD, and the first ELC2/ELSM its SELC at SESD (unless the
generator has its own). Noted as approximated: the SEVT events (moves) are
dropped, tables are merged, a selector other than `CNST(0)` is ignored, and
spawn conditions are ignored (every spawn starts). Extra swooshes or electric
children are left out (29 spawns in 27 effects). All 1211 KSSMs in the parsed
effects convert.

`SplitRetailEffect` (`SplitRetailPart` for PART) reads a retail PART, SWHC or
ELSC back the same way, property by property. The tests use it to check that
retail's reader takes every converted file, and `effect_tool convert` uses it
to compare converted effects with the disc's. Run on the disc's own files, it
also checks the type tables against real files: all 3202 PARTs, 78 SWHCs and
60 ELSCs on the US disc split.

Notes from comparing converted effects with the disc's (`effect_tool convert`):

- `MPCB(MPAC(xb, yb, xr, yr), m)` is retail's `ANGC(xb, yb, xr, yr, m)`; the
  disc writes the same values under `IVEC(ANGC(...))`.
- MPRD with two elements is a random int, written as RAND. In LTM2, the bounds
  of RAND/IRND/MPRD come down by one each, as the disc's IRND shows.
- DFCP (2 or 3 elements) and DFCS (3) scale a size, colour or speed by
  something retail has no element for; they are written as 1 and listed as
  approximated.
- PMRQ (a model particle's rotation) `REUL(x, y, z, #00)` is retail's PMRT
  `CNST(x, y, z)`: `CERotationEuler::QuatGeneric` order 0 is Rz·Ry·Rx in
  degrees, as `CElementGen` builds PMRT. All PMRQ REULs on the disc use order
  0. An angle with IRND is dropped: retail evaluates a varying PMRT each frame
  and IRND gives 0 after frame 0. With PIRN (below) a per-particle PMRT keeps
  its IRND, so the angle could be kept; that hasn't been tried.
- Remastered's IRND is per particle and holds for its whole life. Retail's
  (`CREInitialRandom`, `CIEInitialRandom`) writes only at frame 0, so nested
  inside MULT or ADD it gives 0 afterwards and the particle vanishes or freezes.
  Every converted PART therefore ends in a port-only `PIRN CNST 1`
  (`CGenDescription::xPortIrnd`). With it, IRND during a particle's evaluation
  returns a splitmix64 hash of the particle's seed and the element's address,
  the same value every frame. Retail PARTs never carry PIRN and are unchanged.
- PBDM: 0 alpha, 1 premultiplied, 2 additive, 3 opaque. Retail has only alpha
  and AAPH, so 2 is AAPH, and 1 and 3 are taken as alpha blending (listed as
  approximated).
- GPUA (free GPU time) is written as 1, and SPAx parameter reads as their
  default (retail passes no parameters); both listed as approximated.
- A generator with no TEXR, MTIN or PMDL draws nothing in Remastered; it gets
  `SIZE CNST 0` so retail doesn't draw an untextured quad.
- MAXP and SIZE differ from the disc on purpose in a few effects, as do some
  COLR curves. The disc also sets the COLR header's second flag byte where
  Remastered leaves it 0; retail does not use it.

## Shapes retail has no element for

None of the effects paired with the disc uses these, so the mappings follow
from what retail's elements compute (`CVEAngleCone`, `CVEAngleSphere`,
`CCEKeyframeEmitter`, `CIELifetimePercent`) and are unconfirmed in game:

- `ANCR(REUL(x, 0, 0), xr, yr, m)` becomes `ANGC(-x, -0, xr, yr, m)`. ANGC's
  direction is `(-sin y cos x, sin x, cos x cos y)`, so an X bias of 90 points
  the cone at +Y as `REUL(-90)` turns +Z to +Y. That is exact on the cone's
  centre line only; rotations about Y or Z are not converted.
- `ASPR(origin, REUL, xr, yr, a, b)` becomes `ASPH(origin, -x, -0, xr, yr, a,
  b)`, taking `a` as the radius and `b` as the speed (b is the one that is
  sometimes random).
- `RNDV(m)` becomes `ANGC(0, 0, 360, 360, m)`.
- `GRAD` becomes `KEYP` with 101 colour keys over the particle's life. The
  element after the stops is what the position runs over: `ILPT(CNST(n))` is
  n% of the life, `CNST(n)` is n frames (scaled by a constant lifetime). The
  last byte set makes the gradient repeat.

## In the import

The import menu's "Particle effects (experimental)" checkbox, or
`MP_REMASTERED_EFFECTS=1` (which wins when set), adds an effect step to the Remastered import
(`port_remastered_effect_import.cpp`, called from `port_remastered_import.cpp`
after the models). Every effect whose id was carried over from retail and is
on the disc is converted and written as `<ID>.PART`, replacing the disc's.
Its embedded children are written under new ids, and a texture it names that
the disc does not have (its material instance's first TXTR, or a TXTR of
Remastered's own) is written as an RGBA8 `<ID>.TXTR`, at most 256 on a side.
When both the converted root and the disc's PART have a light (LTYP), the
root's whole light block (LTYP LFOT LCLR LINT LOFF LDIR LFOR LSLA) is replaced
by the disc's. The reason is that Remastered leaves out LOFF/LDIR/LFOR/LSLA, its
LIRD/LORD don't map onto them (LORD 1 stands for LFOR 3 in one effect and 5 in
another), and it differs from the disc in places (LFOT, LINT). Embedded
children have no disc PART, so they keep their own light, and `CElementGen`
uses its defaults for whatever they leave out.
A material texture with a nil id is Remastered's opaque black texture, and is
written as one. A quad generator whose material's colour texture (BCLR) is nil
is taken as a placeholder that draws nothing: these generators only carry
spawns and lights (PlasmaCharge's root), and drawing them black would cover
their children. Model generators keep the black material. An effect in which
only placeholders would draw (PlasmaMuzzle, PlasmaAuxMuzzle) keeps the disc's,
except where Remastered draws it elsewhere (`kStubReplaces`): the Artifact
Temple laser hit 53861B29 spawns the same five PARTs as Ridley's 2D16014C,
which Remastered's c3a53327 now carries, so its stub replaces it.
Effects with no retail id (most world effects) are not used yet: nothing on
the disc names them. The step is off by default. Like the rest of the import, the files take effect
at the next mods reload (`mods reload` or the debug menu), no restart needed.

## HDR dynamic lights

The morph ball's inner glow is one of five particle effects in Remastered, one
per glow index (0 Power, 1 Varia, 2 Varia with Spider Ball, 3 Gravity, 4
Phazon). Its light is a particle light in scene-linear HDR. The port doesn't
load these effects. `platform/port_remastered_ball_light.cpp` computes the same
light from the values the exe binds into them (`CMorphBallMP1::UpdateEffects`)
and from the effects' light elements:

- **Colour:** retail's `skBallLightModulationColors[glow]`, converted from sRGB
  to linear.
- **Intensity (LINT):** `lerp(lerp(dry, 450, b), lerp(wet, 450, b), f)`.
  - `dry`/`wet` are 30/60, or 30/90 for Phazon.
  - `b` is the boost: the charge fraction, then `1 - drain/drainTime` while
    boosting.
  - `f` is the water factor. Its target is 1 inside a fluid. In normal water
    it is `0.5 x depth under water / ball radius`. It rises towards the target
    at 2/s, drops to the target at once, and falls at 4/s out of the water.
- **Falloff:** quadratic `(1-t)^2` from 0 to 2.3 units around the ball's centre.
- **Fade:** in over the first half of the morph and out over the unmorph.
- **When:** the light applies only while the inner glow's light is in use, not
  during the transition flash.

The intensity is divided by pi because the port's PBR Lambert has no `1/pi`.
That divide, and approximating Remastered's in-fluid flag with
`IsInsideFluid()`, are assumptions; nothing was compared with Remastered
on screen.

The light reaches the shader through `GXSetPBRLightHdr` (Aurora command
`GX_AURORA_SET_PBR_LIGHT_HDR`). That call gives a GX light slot its own linear
colour, view position and falloff (none, linear, quadratic, 1 - smoothstep;
r1 0 = off) in PBR draws, in place of the GX light's colour and attenuation.
`CLight::SetPortHdr` carries these values, and `CGraphics::LoadLight` sends
them, or "off", for every light it loads. Retail-material draws keep the
retail light.

`MP_REMASTERED_BALL_LIGHT=0` keeps retail's light, and
`MP_REMASTERED_BALL_LIGHT_SCALE=<x>` scales the intensity. The console's
`roomenv balllight on|off|<scale>` does the same while the game runs.

There is no gun light to port. Remastered's arm cannon has no charge-beam
dynamic light: the retail gun light (`UpdateGunLight`) colours the gun's own
particle effect.

The converter maps LTYP and LFOT the way the exe reads them: LTYP 1 is
retail 2 and LTYP 2 is retail 1, and LFOT is unchanged. The import replaces
the root effect's light block with the disc's, so this matters only for
embedded children's lights.

## Tools

`tests/port_remastered_effect_tool.cpp` is a dev tool, the CMake target
`remastered_effect_tool` (not in `all`; desktop only):

```
cmake --build build/port-gcc --target remastered_effect_tool
effect_tool=build/port-gcc/remastered_effect_tool
$effect_tool dump <file.GENP>             # one effect as text
$effect_tool mtin <file.GENP>...          # one TSV line per generator with a
                                           # material instance: file, form, MATI
                                           # (pak order), TEXR, PBDM, PMTR items
                                           # (group/slot=value), SMTR item count
$effect_tool scan <romfs> [outdir]        # coverage, references, failures;
                                           # outdir gets one dump per effect
$effect_tool extract <romfs> <outdir>     # every unique GENP as raw <id>.GENP,
                                           # for grepping what dumps show as raw
                                           # blocks (PVAR, parameter tables)
$effect_tool convert <romfs> <retail|-> <outdir>
                                           # every effect and child as retail
                                           # PART/SWHC/ELSC, what was left out by
                                           # type, children by form, and (with a
                                           # folder of the disc's <id>.<type>) a
                                           # comparison of every retail-id file
                                           # plus a split of every disc file
$effect_tool import <romfs> <retail> <outdir>
                                           # the import's effect step, with the
                                           # ids of the files in <retail> as the disc;
                                           # also writes <outdir>/effects.tsv
$effect_tool pdump <file.PART|SWHC|ELSC>   # a retail effect's properties as text
$effect_tool pdiff <a> <b>                 # only the properties that differ
$effect_tool explain <romfs> <retail> <GENP id>
                                           # the import step on one effect: pairing,
                                           # result, dropped/approximated with reasons
$effect_tool mat <romfs> <model id|name> [material index]
                                           # one model's material decisions
```

The reports the import writes into the mod, and how to read them, are in
`docs/DEBUGGING.md` "Converter reports".


`tests/port_remastered_effect.cpp` (`port_remastered_effect_tests`) checks
the reader on a synthetic effect, and `tests/port_remastered_effect_convert.cpp`
(`port_remastered_effect_convert_tests`) checks the converter against
hand-written retail bytes.
