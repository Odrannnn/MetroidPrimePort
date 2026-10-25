# Metroid Dread Samus models

**Experimental.** The port can build a model mod from your own copy of Metroid
Dread that puts Samus's Power, Varia, Gravity and Phazon Suit bodies in place of
Prime's: in cutscenes, and wherever else her body is drawn (the Morph Ball
transitions). It reads the game's `.nsp` or gamecard dump (`.xci`) and your
console's key file, the way the Remastered model import does. Nothing of either
game ships; the mod is made on your machine from your own copies.

The result is an ordinary mod folder, `dread-samus`, so the Mods page switches
it off and on like any other mod. Power, Varia and Gravity take their Dread
namesake, the Phazon Suit takes Dread's Metroid Suit (`samushyper`), and the
Fusion Suit is not touched. The bodies, Morph Ball shells and their suit materials come from Dread:
Prime's own animations drive them, and no Dread animation, sound or effect is
used.

## Importing it

The key file is not this import's own: F1 > Mods > Built-in mods draws one
"Your Nintendo Switch's prod.keys" row above both sections, and it serves this
import and the Remastered one alike. The Dread section, a sibling of Prime
Remastered rather than a part of it, holds the import directly: type the
image's path, or press **Load Metroid Dread image** and pick it, then **Import**.
The section says "Imported." once the mod is in the mods folder; "Not imported
yet." until then.

A typed key path is kept once it is used, the way the Remastered import keeps it
(`remastered_keys` in the settings file). The image's path is not saved on
desktop, so pick or type it again after a restart; on Android the pick is
kept. The import runs in the background while the game
runs, with a progress bar, Cancel and a "notes" tree of what it decided. It
shares that panel and its worker with the Remastered import, so only one of the
two runs at a time; a mod reload loads the new mod when it ends.

On Android the section has the same "Load Metroid Dread image" button, which
opens the system's file picker. Android may close the game while you pick a
file; the pick is kept, so open the page again. The image is not copied: the
import reads it through the picked file's descriptor. The key file comes from
the "Pick the keys..." button in the shared row above both sections.

## From a terminal

    metroid_prime_port --import-dread-samus <image.nsp|image.xci> [key file]

does the same without starting the game, printing the import's notes as it goes
and its result at the end. The key file defaults to `~/.switch/prod.keys` when
it is left out. The Prime disc has to be open first, as for the game itself:
`MP_DISC`, the disc image the game remembers, or a copy beside the executable.
The mods folder is `MP_MODS`'s if it is set, else the usual one. There is no
GPU loaded, so the mod's textures are written as BC7/BC5.

For development, `MP_DREAD_ROMFS=<extracted RomFS>` reads a RomFS folder instead
of an image, and the image argument is then ignored (pass `-`):

    MP_DREAD_ROMFS=/path/to/romfs MP_DISC=... metroid_prime_port --import-dread-samus -

In the game's debug console (`docs/CONSOLE.md`) it is
`remastered dread <image.nsp|image.xci> [key file]`; `remastered` then prints
its progress and `remastered cancel` stops it.

## Where it lands, and turning it off

The mod is written to `<mods>/dread-samus`, in the mods folder itself rather
than the app's own storage: `mods/` in the data folder, or `MP_MODS`'s
directory, on desktop and on Android alike. A data folder moved to the SD card
or to the app's own storage moves it with everything else.

It is staged in `<mods>/.dread-samus.staging` and moved over
`<mods>/dread-samus` when the import ends, replacing an older one; an install a
quit interrupted applies at the next start. A cancelled or failed import leaves
nothing behind.

To get the retail game back, uncheck it on the Mods page (it is kept as
`dread-samus` in `mods_disabled` in the settings file) or delete the folder.
Either shows when the mods are reloaded, at the next start.

## What it does, in brief

Dread's bodies do not fit Prime's skeletons: pinning and stretching Dread's
mesh onto Prime's bone lengths stretches the skin between the hips, chest and
shoulders. So Dread keeps its own proportions. For each body the import writes
a skeleton with Prime's CINF joint layout but Dread's bind offsets (its own ids,
`D7EAD001`-`D7EAD00C`, one per suit and per Prime skeleton), binds Dread's mesh
to it with Dread's own weights, and writes the CMDL/CSKR/TXTR under the retail
body ids so the game's characters draw them with no code change of their own.
At run time Prime's animations are retargeted onto it rotation-only
(`CAnimData::RecalcPoseBuilder`: each translation is moved by the difference of
the two bind poses), with a fixed rest-pose correction, so the body moves as
Prime's animation does while keeping Dread's shape.

Dread's own skinned arm cannon comes with the body and is drawn with it, in
cutscenes too; Prime's separate cannon model is not drawn for those bodies. The
suit materials are Dread's own (albedo, normal, metal/rough/AO, emissive) on the
Remastered import's PBR shader path.

## After an update

Version 8 also replaces the first-person arm cannon with each suit's Dread
cannon, cut from the body (with the forearm skin that closes its elbow end) and
fitted along Prime's barrel. Its lights take the beam's colour: one copy of the
light material per beam, Prime's beam tints at a peak of 1.5 (the material's own
strength of 11 overexposes every colour to white). The cannon is rolled 30
degrees and moved slightly right and down so its side light faces the camera;
seen straight from behind, its wide back end hides it. The left hand stays
retail. Mods > Metroid Dread's "Dread arm cannon in first person"
(`dread_first_person_cannon`, on by default; console `dreadcannon 0|1`) turns
it off for Prime's own arm cannon, at once, keeping the Dread bodies.

Version 7 also dresses the pause screen's Samus (`ANCS_ItemScreenSamus`, CINF
`F76E70A6`): each suit's first part is the Dread body, and the doll then leaves
out that suit's other parts, its boots, beam and visor models, which the Dread
body carries itself. The grapple beam still shows once it is collected.

Version 6 also imports Power/Varia/Gravity/Hyper Morph Balls (Hyper for Phazon),
including normal, Spider-equipped and low-poly paths. Low-poly paths reuse the
same Dread shell geometry rather than a separately authored LOD. Each shell is
uniformly fitted to the corresponding retail ball's measured bounds and rigidly
bound to its shell joint. The main ball retains Prime's ANCS, skeleton and
animations; no ball physics or animation calls are replaced. Suit-variant IDs
keep Fusion resources untouched. The retail Spider glass is omitted only while
the matching Dread ball is loaded; disabled Dread sonar and spider FX are not
imported. Prime's glow/trails, frozen shell and electrical effects remain retail.

Hyper's `mp_opaque_constant_selfilum` uses a baked alpha-power emissive mask:
`E = (40, .15, .15) * (baseAlpha / 255)^2.2`, with the material's default tint
disabled, and an attributes roughness floor of .05 (other suits use .1).
Textures bake material defaults into the existing port PBR representation;
filtering/mipmaps therefore approximate nonlinear post-sampling operations.
Dread's runtime tint, frostbite and global emissive threshold/multiplier are
not reproduced. Lighting remains the port's lighting. Non-default Hyper tint
or frostbite is rejected rather than guessed.

Any failed target or ID collision aborts publication; the importer buffers the
outputs and the installer publishes its staging directory only on success.
Failed staging writes cannot replace the installed mod.

The mod carries `.dread-samus-version`: the importer's own number, then the
build's commit. When that number is older than the build's, the game says
`dread-samus was imported by an older version; import the Dread Samus models
again` once per launch, and F1 shows the same. Import again the same way; it
replaces the old mod.

## Limits

- The base Dread release (1.0.0) is enough; no update data is needed.
- The image has to be Metroid Dread: its title id is checked and anything else
  is refused. Compressed images (`.nsz`/`.xcz`) are refused ("compressed images
  (.nsz/.xcz) are not supported; decompress it first"), and an `.xci` needs the
  key file's `key_area_key_application_XX` keys.
- In first-person play the body is hidden, as in retail (only the arm cannon is
  Dread's), so the rest of the mod shows in
  cutscenes, on the pause screen and in the Morph Ball transition unless the third-person camera
  (Video > Display, `third_person`, off by default; see `docs/SETTINGS.md`) is
  on. That camera is experimental.
- Only the Power, Varia, Gravity and Phazon bodies and balls: Fusion keeps its models,
  and nothing else of Dread is imported. One Tallon cutscene draws Phazon with a
  second body id (`B333E1D7`) on the same skin; the mod serves the Metroid Suit
  under it too.
