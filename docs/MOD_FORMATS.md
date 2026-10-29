# Mod file formats

What a mod folder can hold, for mod authors and port developers. Installing and using mods is in
[`MODS.md`](MODS.md). The binary layouts are also in
`platform/include/port_{mods,room_geo,room_liquid,room_env,hd_font,hud_bars}.h`, which are
authoritative.

`tools/extract_textures.py`'s `disc_files`/`pak_resources` get a resource's original bytes to
edit. The loader was checked with a `0552A456.STRG` (the file select's "New Game") read from the
disc and from a whole-file `MiscData.pak` mod.

| File | What it does | Layout |
| --- | --- | --- |
| a disc path (`Metroid1.pak`, `Audio/frigate.dsp`) | replaces that disc file | — |
| `<id>.<type>` (`1A2B3C4D.TXTR`) | replaces or adds one PAK resource | `port_mods.h` |
| `<TXTR id>.dds` | native full-size texture | — |
| `<name>.sdfont` | distance-field HD font | `port_hd_font.h` |
| `<FRME id>.hudbars` | energy-bar shapes of a HUD frame | `port_hud_bars.h` |
| `<MREA id>.roomenv` | reflection probes, baked ambient, exposure | `port_room_env.h` |
| `<MREA id>.roomgeo` | an area's static geometry | `port_room_geo.h` |
| `<MREA id>.roomliquid` | an area's liquid surfaces | `port_room_liquid.h` |
| `<MREA id>.roomfx` | extra particle effect actors | `port_room_effects.h` |
| `.txt`, `.md`, `.json`, `.png`, ... | ignored | — |

All ids are 8 hex digits. Every file except a disc-path file may be anywhere in the mod.

## Disc files

A file at a disc path (`Metroid1.pak`, `Audio/frigate.dsp`, `Video/attract0.thp`; case does not
matter) replaces that file. Only existing disc files can be replaced; others are reported and
ignored.

## Loose resources

A file named `<8 hex digits>.<type>` (`1A2B3C4D.TXTR`, `0552A456.STRG`), anywhere in the mod,
replaces that resource, uncompressed, in every PAK that holds it.

- The PAK is served as a virtual file with the loose file appended past the original data,
  32-byte aligned (see `platform/include/port_mods.h`).
- A loose resource also applies inside a PAK another mod replaced whole.
- An id that no PAK holds is added to `NoARAM.pak` (loaded from boot, so any model or frame can
  reference it): the table grows by one entry and the original data moves down to keep its
  32-byte alignment.
- An id another PAK holds under a different type is reported and ignored.

A resource can be any size (the port keeps PAK sizes whole; retail stops at 4 MB), though the game
heap still bounds what fits: a 16 MB texture runs it out of memory. `MP_MEM1_MB=<n>` raises the
emulated MEM1 arena (default 24 MB, capped at 1024) and the game heap with it, for mods heavier
than the retail heap allows; the log says `port: MEM1 arena raised to <n> MB`.

## Native textures (`.dds`)

A file named `<8 hex digits>.dds`, anywhere in the mod, is a native texture: the full-size image
of the TXTR with that id, with a full mip chain.

- **Formats:** the importer writes BC7 (BC5, two-channel, for PBR normal maps), or ASTC 4x4
  (normal maps as RGBA with B=0, A=255) when the GPU has ASTC and no BC; the loader also reads
  BC3, BC1 and RGBA8. `MP_REMASTERED_TEXTURE_FORMAT=bc|astc` forces the choice (for testing);
  `--import-remastered` and `--import-dread-samus` have no GPU and write BC unless told
  otherwise. The import logs `remastered: writing <BC7|ASTC 4x4> textures`.
- **ASTC encoder:** ARM's astc-encoder (`extern/astcenc`, FAST preset, about 2 s for a 2048 px map
  with its mips on one desktop core).
- **The TXTR still loads** (the mod's own, which can be a few texels across, or the disc's) and
  supplies the wrap and filter state; the `.dds` is what is drawn. A later mod's TXTR without a
  `.dds` drops an earlier mod's `.dds` for that id.
- **Memory:** Aurora streams it on worker threads into GPU memory, so its size never counts
  against the game heap: a mod of 2048 px maps runs at the default arena.
  `MP_TEXTURE_MAX_DIM=<px>` caps the size a `.dds` is loaded at (0 = no limit; default 0, 1024 on
  Android) and `MP_TEXTURE_CACHE_MB=<n>` the memory the loaded ones may keep cached (default 4096,
  1024 on Android).
- **Unsupported GPUs:** on a GPU with neither BC nor ASTC the TXTR is drawn instead; a BC `.dds`
  on a phone (an import made on a desktop) is refused the same way.

The log and the Mods panel count them (`mods: <n> native texture(s)`).

## HD fonts (`.sdfont`)

A file named `<name>.sdfont`, anywhere in the mod, is a distance-field font (each texel holds how
far it is from the glyph's edge), which stays sharp at any resolution.

- The port draws its glyphs in place of the disc's bitmap ones for the Deface fonts (HUD, menus,
  scans, logbook), fitted to the disc's capital H.
- The layout is still the disc's (advances, kerning, line breaks), so text sits where it did.
- From a character the file lacks, the rest of that run of text is drawn from the disc.
- The last mod with one wins, and `MP_HD_FONT=0` turns it off.

The layout is in `platform/include/port_hd_font.h`.

## HUD bars (`.hudbars`)

A file named `<FRME id, 8 hex digits>.hudbars`, anywhere in the mod, gives the energy-bar widgets
of that HUD frame their own shape: per bar, named after its widget, a strip of stations (two
points and their texture coordinates) from the empty end to the full end.

The game still supplies the value, the colours and the texture; the port fills the strip by its
measured length, so stations need not be evenly spaced, and blends it as the widget's draw flags
say. The last mod with one for a frame wins, and the layout is in
`platform/include/port_hud_bars.h`.

## Room environments (`.roomenv`)

A file named `<MREA id, 8 hex digits>.roomenv`, anywhere in the mod, is that area's lighting
environment for PBR materials. The layout is in `platform/include/port_room_env.h`.

- **Reflection probes:** each a box of the world with a prefiltered HDR cube map (BC6H) of what
  surrounds it. A PBR model reflects the cube of the smallest box it stands in (else the
  nearest), in place of the port's live probe, exposed so the cube's average is middle grey. A
  probe's scale turns its cube (stored normalised) back into radiance.
- **Baked ambient light** (optional): a grid of points through the room, each with the light
  arriving there and the direction most of it comes from, per colour. A PBR model standing in a
  grid takes the colour and direction of its ambient light from the points around its origin; the
  game's ambient colour only sets how bright it is.
- **Exposure:** a file also carries the room's tonemap values and its auto exposure settings.
  Those set one exposure for the frame, the way Remastered does: the room the camera is in is
  exposed by its own radiance, its exposure bias and the range its auto exposure is held to,
  cubes and baked ambient keep their level relative to that, and the result goes through that
  room's tone curve (the formulas are Remastered's own; the frame's average is stood in for by
  the room's middle probe).

Files load and unload with their areas, and a cube is decoded on first use.

## Room geometry (`.roomgeo`)

A file named `<MREA id, 8 hex digits>.roomgeo`, anywhere in the mod, is that area's static
geometry: a list of models, each with the transform that places it in the area, drawn in place of
the area's own surfaces (its actors, doors and pickups are drawn as before). The layout
(little-endian, versions 1 to 3) is in `platform/include/port_room_geo.h`.

- The models are ordinary CMDLs of the mod and are loaded with the area.
- The Thermal and X-Ray visors draw the retail area.
- A game started with such a mod installed takes a 256 MB arena and larger frame buffers (2 times
  the usual size with `MP_ROOM_GEO_RESIDENT`, the default; 12 without it, 6 on Android; see
  `MP_FRAME_BUFFERS`), since these rooms are many times retail's vertex count.
- A mod loaded into a running game without them is not drawn until the next start.

## Room liquids (`.roomliquid`)

A file named `<MREA id, 8 hex digits>.roomliquid`, anywhere in the mod, holds that area's liquid
surfaces (water, poison, lava) as models. The layout is in `platform/include/port_room_liquid.h`.

- Each is drawn in place of the fluid plane of the area's water object that stands where its
  transform says, at the height the game gives that plane, so a rising or draining liquid still
  moves. The object itself is untouched (fog, splashes, damage).
- Their materials carry a `PBR4` record of kind 5 (water, poison: a colour, an opacity and two
  moving normal maps) or 6 (a lava pool: a pattern carried along a flow map, coloured by a ramp).
- The Thermal and X-Ray visors show the surface too: the Thermal visor's passes shade it as they
  shade a fluid plane (the hot pass adds it), and the X-Ray visor draws it unchanged.

`MP_ROOM_LIQUID=0` or the console's `roomliquid off` draws the retail planes instead.

## Room effects (`.roomfx`)

A file named `<MREA id, 8 hex digits>.roomfx`, anywhere in the mod, adds particle effect actors to
that area: retail Effect objects, per script layer, created after the layer's own objects while it
is active, and a list of the area's retail objects not to create (the effects they replace).

- Their editor ids use instance numbers 0xF000 to 0xFFFF, which no retail area has; nothing saves
  them, and nothing can send them a message.
- The Remastered import writes one for each room with effects of Remastered's own that nothing
  connects to.
- The same file may also override a retail Effect object that a Remastered effect actor matched:
  the object is created as retail has it, connections and all, then gets the actor's PART and its
  particle variables.

The layout is in `platform/include/port_room_effects.h`. `MP_REMASTERED_EFFECT_ACTORS=0` ignores
the files; the console's `fx addons` lists the current area's.

## PBR material records

A PBR material (flag bit 14) may end in a 28-byte record: six big-endian floats (emissive
multiplier rgb, backlight) and the tag `PBRM`, inside the material's own span in the offset
table. Older records read as factors 1 and 1 (`platform/include/port_pbr_record.h`).

- **Emissive multiplier:** scales the emissive map.
- **Backlight:** the backlight triple of a lit material is Remastered's CharacterBacklight: the
  strength of the light from behind, of the light from above, and the power of its fade up the
  model's height plus 1 (0 = no fade); `MP_REMASTERED_BACKLIGHT=0` turns it off. The two
  strengths follow the room's Backlight hints (roomenv v11, picked and faded like the colour grade
  hints; none = top 2, back 4; console `roomenv grades`). On a ColorUnlit material it is the
  gain, three times. A material without the record gets 1 and 0.

Longer forms follow the same six floats:

- **`PBR2`** (36 bytes) adds a height-blend threshold and a mode (1 unlit: the material's own
  colour and glow; 2 the base map's alpha scales the glow; 4 the vertex colour tints the surface;
  summed). A colour attribute tints a PBR material's albedo only with mode 4.
- **`PBR3`** (56 bytes) adds a second layer's edge width and the scale and offset of both layers'
  heights. A `PBR3` material binds three more maps (base, metal/roughness, normal of the second
  layer) and blends the two by the vertex alpha and the base maps' alphas.
- **`PBR6`** (92 bytes, the 19 floats, the wrap word, then diffuse and F0 factors) carries
  Remastered's LITS (LightBleedScale) for a two-sided mesh's back copy. `GXSetPBRLightScale`
  scales only the PBR diffuse and F0 (never emissive, backlight or specular) and resets to 1, 1 on
  every material. A material with a finite positive LITS gets its back copy as a primitive and
  material of its own, diffuse |LITS| and F0 0 (LITS 1 included). No LITS and -1 keep the older
  copy (factors 1, 1); 0 and negatives other than -1 are unsurveyed and keep it too.

**Geometry contract:** that copy's vertices keep the older back copy's turned-round normals (so
the TEV fallback, thermal and every other reader light it as before), and an F0 factor of exactly
0 is the marker by which the PBR shader turns the normal back to the surface's own, as Remastered
keeps N for a positive LITS. The cotangent frame is built on the stored normal: from behind the
screen's handedness flips with the winding, so that gives the front's own T and B (the back keeps
them, as Remastered does). The converter emits F0 0 only there; every other caller is 1.

## Collision stays on the disc's model

A solid static actor with no authored collision extent (zero or negative) takes its collision box
from its model's bounds. A mod that replaces the model would replace the collider with it (a lava
crust drawn a few centimetres thick walls off a floor the disc's model is flush with), so
`ScriptLoader::LoadActor` reads the box from the original disc's CMDL header instead
(`platform/port_actor_collision_bounds.cpp`).

- It reads via `aurora_dvd_base_*` only, so no overlay or loose mod file is ever consulted,
  indexed once from the base PAK tables and cached per model.
- Only the normal model counts, as in retail, where the X-ray and thermal models are attached
  after the box is taken.
- Authored extents, `ANCS` actors and non-solid actors are unchanged, and a missing or malformed
  disc model falls back to the drawn model's bounds. Rendering is not affected.

## Authoring button prompts

In-game button prompts are ordinary textures (`CFontImageDef` holds one texture per glyph), so
they can be swapped like any other ([Texture packs](MODS.md#texture-packs)).

### Finding and replacing a prompt

To re-author one: dump the textures from a screen that shows the prompt, find the glyph by its
size and contents, then write a replacement with the same stem into the device folder.

- `MP_DUMP_TEXTURES=1` is the supported way to do this and needs no code change: textures that
  already have a replacement are not dumped, so the result is exactly the unclaimed set, and the
  images can simply be looked at.
- To see every texture on the disc instead, `tools/extract_textures.py <disc.iso> <outdir> --png`
  writes them all under Aurora's names, with the developers' PAK names (`LStickN`, `AButtonIn`,
  `DPadU`...) in `index.tsv` and the textures the game's text draws inline (`&image=` tags, the
  HUD hints) in `strg_images.tsv`; that is how the prompt table was completed. It needs
  libxxhash, numpy and PIL.
- The prompt table in `platform/port_prompts.cpp` is the authority on which textures are claimed:
  it lists the game's prompt art by asset id, and each id should be one a dump from the screen
  that shows it carries - two rows were transcribed into the wrong action and that is how the
  pause menu's Exit prompt ended up drawing the C-stick's glyph.

### Generated icons

`tools/make_prompt_glyphs.py <dir>` writes one icon per input that can be bound (every key,
mouse button, and pad button, stick and D-pad, whole and per direction) into `<dir>/bindings/`,
compositing the CC0 icons vendored in `tools/prompt_icons/` (Kenney's Input Prompts pack). The
names are the port's stems for the inputs, not the game's texture names, so the same set serves
every screen. The build copies `textures/` next to the executable (or embeds it), so a built
binary picks the replacements up without a manual copy. Three details matter:

- the icon must keep the game's inset (the glyph is only about 22px in the 32x32 buttons and 28px
  in the 64x32 stick and D-pad art, and a full-bleed icon reads as a square);
- an icon may be any whole multiple of the texture's size, the same on both axes; the generated
  ones are twice it, so they stay sharp on a scaled-up HUD. A file name's `_64x32` suffix is the
  game texture it serves, not the file's own size;
- DDS is used rather than PNG because Aurora's DDS path preserves the texture's alpha.

### Binding-aware prompts

`platform/port_prompts.cpp` registers, at runtime, the icon for the input actually bound to each
action, so rebinding a key or mouse button changes the prompt. The per-input icons live in
`<textures>/bindings/` (also generated by `tools/make_prompt_glyphs.py`) and are served through
Aurora's virtual-replacement callback rather than shipped as a fixed set.

- Each screen draws its own prompt art, so one action maps to several game textures. Every
  button, stick and D-pad prompt texture on the disc is in the table (A, B, X, Y, Start, L, R, Z;
  the stick, C-stick and D-pad whole and per direction), which covers the front end, the pause
  and map screens and the HUD hints (the visor hints' D-pad arrows, the beam hints' C-stick, the
  strafe and R animations).
- The sticks follow their axis bindings: a pad shows the stick that drives the axis (so Southpaw
  swaps them) or, for an axis driven by buttons, those buttons; a keyboard shows the key for a
  direction frame and, for the whole stick, the WASD, IJKL or arrow cluster when the four keys
  form one (the up key otherwise). The stick's diagonal frames take the whole-stick icon.
- A keyboard follows its key bindings (the main key, else the second key); a pad follows its
  button mapping, so a remapped button or a preset shows the button it now uses.
- Letters, digits, punctuation, the named keys, the keypad (its digits share the main row's art)
  and all five mouse buttons have icons; a binding without one leaves the game's own art.
- A GameCube pad (adapter or NSO) on its default mapping keeps the game's own art, and only an
  action moved to another button gets a GameCube icon, named after the button's default action.
- The chosen icon is logged when it changes ("prompt A keyboard_x"), and so is an action going
  back to the game's own art.

`MP_SMOKE_BIND_A=<scancode>` (negative for mouse buttons, -3 is middle) rebinds the A action once,
for checking this without going through the Controls > Remap page; it needs a
`-DMP_ENABLE_SMOKE_DRIVER=ON` build such as `build/smoke-gcc`, and
`MP_FAST_BOOT=1 MP_SKIP_CUTSCENES=1 MP_SMOKE_PAUSE=<ticks>` reaches the pause screen quickly to
see the result.

## Remastered import internals

How the built-in [Metroid Prime Remastered import](MODS.md#metroid-prime-remastered) works.

### Staging and installing

The result is staged in `<root>/.remastered-models.importing` and installed as
`<root>/remastered-models`, replacing an older one, when the import ends. `<root>` is the mods
folder on desktop, exactly as it always was, and the app's own internal storage
(`<internal storage>/remastered-imports`) on Android. The mods stay unloaded while it is moved
into place in the background, then a mod reload loads it. The import it replaces is deleted in the
background after the mods reload. A first import (no earlier one to reuse) checks the free space
up front.

### Android storage

The F1 data folder setting moves the settings, mods, saves, save states and the rest, and leaves
the import where it is, including when the data folder is the app's own storage, where the import
sits inside it.

- **Why app storage:** the mods folder is in the data folder, and the data folder is often moved
  to the shared folder on the card (that is how mods are added with a file manager). In shared
  storage every file of an import goes through Android's media provider and a folder move is a
  file-by-file walk, which is where an install there spent its time. Beside the app the same
  files are plain file-system writes and the finished mod is moved into place as one rename.
- **Loading:** the mod scan loads the mods folder's own folders and the app's
  `remastered-models` together, in one name order (a later name still wins over an earlier one),
  and a `mods/remastered-models` never shadows the app's copy.
- **Old shared copies:** a `mods/remastered-models` from an older version - or from a player who
  has only ever imported there - keeps loading as the fallback for as long as there is no app
  copy: nothing migrates it, and once the app's own import is installed and usable the app
  deletes the old folder at startup (a dead duplicate; a symlink is left alone). A full import
  reuses that folder's files while it is there, read only (hard links, or copies where the card
  and the app's storage are different file systems), so it is left exactly as it was.
- **Movies-only import:** writes into the app's copy too, and with only the shared one it refuses
  and asks for a full import rather than writing the movies there.
- **Pending imports from older versions:** an import left pending by an older version
  (unfinished, or finished but never installed, as `mods/.remastered-models.importing[.held]`) is
  **not** installed automatically: nothing migrates or finishes it. The next full import writes
  and installs the app's own copy, and those hidden leftovers stay exactly where they are - leave
  them until that copy has been imported and checked.

### Import version

A full import writes `.import-version` (the importer's `kImportVersion`, then the build's commit)
into the mod. At each mod scan, `remastered-models` or any mod with that file counts as an import;
one whose stamp is missing or lower than the build's `kImportVersion` is stale, so the log says
so, a toast shows once per launch, and the Mods and import panels say "re-import needed". Bump
`kImportVersion` in `port_remastered_import.h` whenever a change needs a re-import to take effect.
The movies-only import leaves the stamp alone.

- Converter 52 extends correct room lightmap UV selection to 14 additional Remastered shader
  families (15 covered total); existing Remastered-model imports need re-importing.
- Rooms 20: when two Remastered rooms match one area, only the closer match's environment,
  geometry and liquids are written (before, the later room's overwrote the first's). Existing
  imports show "re-import needed"; a re-import makes the rooms stage again, and the room models
  stage again only if the rooms now place a different set of models.

### Models

The port reads the image itself (`platform/port_remastered_nsp.cpp`, `_pak`, `_txtr`, `_cmdl`),
and `_convert` writes a CMDL (plus CSKR for skinned models) over each retail model listed in
`_table`, with its PBR maps as native `.dds` textures. Texture ids are a CRC of the Remastered
texture's id and its role, so a map shared by several models is written once.

Converted gun effects (charge effects, muzzle and charged-release flashes, the Wave charge's
secondary effects) take the sampled retail locator in full, while retail ones take its
translation only (`NPortChargeFxDetail::SetLocalLocator`). The importer already turns their
vectors from Remastered's particle space (X left, Y up, Z forward) into the port's, so no extra
frame goes on top: the Power charge disc's EMTR, Remastered `(0,0,0.15)`, becomes the port's
`(0,0.15,0)` and at scale 2 sits 0.30 m forward of the barrel, as on native's LBEAM.

### Rooms

After the models, `_room` writes a `.roomenv` for every area into the mod's `roomenv` folder: it
matches each Remastered room to the retail area by its doors, and takes the room's reflection
probes, their HDR cubes and its baked ambient tiles (roomenv v21): the area's own 64x64x16 /
128x64x16 tiles of Remastered's baked-lighting texture, keyed by world position and cropped to
their lit points. The game maps one tile per key across all loaded areas, the first loaded area's
copy winning and a later copy taking over when it unloads (`platform/port_room_env_tiles.h`), and
composes each area's grid from the mapped tiles. Files are larger than before (the biggest,
Frigate hangar, is about 146 MB).

Room geometry is converted as the models are, and written as a `.roomgeo` per area in the mod's
`roomgeo` folder; liquid surfaces as a `.roomliquid` per area in the same folder.

### Text

Each changed table is written as `<id>.STRG` in the mod's `text` folder: the disc's own table
with the reworded strings replaced.

- A string is replaced only when its words differ, so layout-only differences stay as on the
  disc.
- Remastered's hand-placed line breaks (fitted to its own boxes) are kept only where the disc's
  string breaks a line too, the rest left to word wrap.
- Highlight colours are kept (`&push;&main-color=…;…&pop;`) and the layout tags a disc string
  opens with are carried over.

The other languages are written as extra language sections in the same tables. The typeface is
the FONT asset with the most characters, its first face.

### HUD and map

The disc's frame stays the skeleton, so every widget the game looks up by name is still there; a
widget Remastered has under the same name takes its placement, colour and model from there, and
the models and pictures Remastered added are placed under their parents. Each frame is written as
`<id>.FRME` with its models (`.CMDL`), its pictures (a small `.TXTR` and, above 64 px, a `.dds`)
and, for the energy, missile and threat bars, a `.hudbars`.

- **Map screen:** Remastered lays it out for a wider view and shows fewer prompts, so its legend,
  area name and hint are set from Remastered on the disc's plane, and the prompts it lacks keep
  the disc's places.
- **Map icons:** the save, missile and elevator stations replace the disc's textures, and the six
  arrows of a door between floors, which the disc draws with one tinted texture, get Remastered's
  own under ids the port looks for (`port_map_icons.h`).
- **Map rooms:** Remastered's map of a world (`CMAP`) names no room of the disc, so its areas are
  paired with the disc's by place and size, and the few it reshaped are written as `<id>.MAPA`
  with the disc's doors and markers; the rest keep the disc's map.

### Movies and gallery

Remastered's movies are H.264; the game plays THP, a JPEG per frame, so each is decoded once and
written again under the disc's names, by default at 30 frames a second (the disc's own rate; the
game's decoder does not keep up with 60 at that size). The decoding is done by ffmpeg, run as a
separate program; Android cannot start a program like that, so there MediaCodec reads the movies
and the port writes the JPEGs itself.

The gallery pictures are the full-size ones in `UI_FrontEnd` (taller than 1000 pixels), written as
`gallery/NNN.jpg`, fitted within 1920x1080, quality 90. The viewer's decoder is the one the game
uses for THP movies, so no extra library is needed.
