# Item randomizer

## Built-in randomizer

F1 > **Randomizer** makes and plays logic-checked seeds without Archipelago
installed. Pick the options (goal, item pool, elevators and door colours,
logic and tricks; saved as the `rando_settings=` line of the settings file),
type a seed text or leave it empty for a random one, and press **Generate &
Play**. The page lists saved seeds with Play, a spoiler view, "Write
spoiler file" and Delete (the seed, its progress and its save card; not the
seed being played). Energy tank capacity (10-200, default 100) sets the
energy each tank holds; the suit alone holds one less, so 50 gives 49 + 50
per tank. Combat logic still counts tanks as if they held 100. Shuffle
Unlimited Missiles / Power Bombs put that item in the pool (in place of a
Missile Expansion), as the apworld options do. While a seed is being
played, **Play the normal game** above the list goes back to the normal game
and its own save card; the seed keeps its progress, and its Play resumes it.
The console has the same as `rando gen [seedtext]`, `rando play <name>`,
`rando off`, `rando delete <name>` and `rando list`.

Each seed has its own save card, which the game switches only at the title
screen and file select. So while a game is loaded, Play is off for other
seeds and Generate only saves the new seed: quit to the title screen to
switch.

- **Generator** (`platform/port_rando_gen.cpp`): the Metroid Prime apworld's
  item pool (`ItemPool.py`) and options, filled with an assumed fill against
  the port's own copy of the apworld logic (`PortApLogic`, the same rules the
  tracker uses). Each seed is then walked again sphere by sphere: every one of
  the 100 locations must be reachable and the final inventory must finish the
  game, or the next attempt is tried. The same seed text and options always
  give the same seed (own PRNG, no `std` distributions). Elevator
  randomization, or a shuffled Scan Visor without pre-scanned elevators,
  starts at Save Station 1, as the apworld does, so sphere 0 has a location.
- **Start, beam, doors.** Starting room (normal, safe, buckle up) with the
  apworld's loadouts and prefills (`StartRoomData.py`), random starting beam,
  door colours, blast shields (replace existing, mix it up, with frequency and
  beam combos) and locked doors, with the Power Beam and Morph Ball Bomb door
  options. Differences from the apworld, where it would make a dead seed:
  a normal start that needs Save Station 1 keeps its prefill even with
  shields or tricks; replace existing keeps the Missile shields on the
  regions mix it up leaves alone for the start (Save Station 1's Morph Ball
  and Bombs sit behind Main Plaza); and an early item with no free sphere-0
  location is placed like the rest instead of displacing the prefill. The
  Sunchamber → Sun Tower Access rule the tracker pack lacks is added in
  `tools/gen_ap_logic.py`.
- **Playing** reuses the Archipelago client unchanged: a seed is served by an
  in-process server (`platform/port_ap_solo.cpp`, server `solo:<name>`, slot
  `Samus`), so the item models, scan text, temple hints, elevator and door
  patches, tracker and per-seed save cards all come from slot data as in a
  multiworld.
- **Files** (`platform/port_rando_seed.cpp`): `<user dir>/randomizer_seeds/
  <name>.json` holds the options, slot data, start items, placements and the
  spoiler; `<name>.state.json` beside it the locations checked, in order
  (items are numbered by it). A different seed under a taken name is saved as
  `<name>-2`, `-3`, ....

## Seed files from other tools

The port can also rewrite item pickups at load time from an externally made
seed file and record what it did. Nothing here changes behaviour when no seed
is present.

## How it works

- **Locations** are keyed by `%08X:%08X:%08X` — world asset id (MLVL), area asset
  id (MREA), entity editor id. All three are stable for a given disc, so a seed
  generated for `GM8E01_00` (USA v1.00) applies to any run.
- `ScriptLoader::LoadPickup` calls `PortRandomizer::ApplyPickup` before the
  pickup is constructed; a seed entry rewrites the item type, amount and
  capacity. This is the only place `CScriptPickup` is constructed, so every
  placed pickup goes through it.
- `CScriptPickup::Touch` calls `PortRandomizer::RecordCheck` when a pickup is
  collected.
- The port layer is `platform/port_randomizer.cpp` + `platform/include/port_randomizer.h`.
  It reads `PortPaths::UserFolder()` for its files, matching the
  settings file: `MP_USER_PATH` when set, else the folder next to the
  executable when writable, else the per-user folder (`SDL_GetPrefPath`).

### Scans and the Artifact Temple

In a randomized game (a seed file or Archipelago), every pickup is scannable and
its scan names the item it now holds; under Archipelago, it also says whose
world the item is for (`Hookshot` / `for Link (A Link to the Past)`, or
`for you`). The scans are made-up SCAN/STRG pairs (`PortCustomRes::TextScan`,
ids from 0xDEAF8000, one pair per location). `PortHints` (`platform/port_hints.cpp`)
keeps them, and the Artifact Temple totems' STRGs, "watched": `CStringTable`
asks for the text again whenever the game reads it, so a scout reply or hint
that arrives after the room loaded still shows. An Archipelago location not yet
scouted reads `Archipelago item`.

Each totem names where its artifact is: the seed file's placement offline, or
the server's hints under Archipelago.

The Artifact Temple is patched for randomized games (`TempleOps()` in
`platform/port_skip_cutscenes.cpp`, plus `PortArtifactTemple` in
`CStateManager.cpp`). The central pickup no longer plays the artifact theme or
the totem cinematic. Every totem lights (found artifacts) or shows its hint each
time the room loads, and holding all twelve arms the cinematic's trigger, whose
end still starts the Meta Ridley fight.

With `map_pickups` set to All, the map and minimap show a plain white dot for
every location not yet collected (`PortMapPickups`, `CMapWorld::DrawPortPickups`),
as randomprime's do. It is always white, so the map never gives away what an
item is. It is a setting like in normal games, off by default (modes: see
`docs/SETTINGS.md`).

Items are named after `CPlayerState::EItemType` without the `kIT_` prefix:
`Missiles`, `EnergyTanks`, `MorphBall`, `PowerBombs`, `Newborn` (artifacts), and
so on. `ItemFromName` is case-insensitive; `tests/port_randomizer.cpp`
drift-checks the table against the real enum.

## Seed file

Path: `$MP_RANDO_SEED` if set, else `randomizer_seed.json` in the user
directory. Schema:

```json
{
  "seed": "poc-0001",
  "locations": {
    "39F2DE28:B2701146:0000007E": { "item": "EnergyTanks", "amount": 1, "capacity": 100 }
  },
  "models": {
    "EnergyTanks": { "model": "86908399", "acs": "F37BCBC7", "character": 0, "animation": 0 }
  }
}
```

`item` is required; `amount` and `capacity` are optional and leave the vanilla
values untouched when absent, and must be whole numbers from -9999 to 9999.
Unknown keys are ignored whatever they hold; a repeated key keeps its last value.
The file is strict JSON (no comments or trailing commas). A seed that cannot be
used disables the randomizer rather than applying half of it, and says why in the
log: `randomizer: seed parse error at byte offset N: ...` for malformed JSON,
`randomizer: seed error: location "...": ...` for a well-formed file with a bad
entry.

`models` is optional and maps an item name to the assets a pickup of that item
is drawn with. A rewritten pickup uses that entry so it looks like the item it
grants; without one it keeps the retail model (the item is still correct). The
port copies the entry into the pickup's static model and animation parameters
exactly as the area data would, and the engine already prefers the animation
when one is present. In the retail data a pickup's `model` and `acs` are both
ANCS assets, with the animation driving the visual.

## Workflow

1. Dump every pickup location:

   ```sh
   MP_RANDO_DUMP=1 MP_RANDO_SWEEP=1 <game> <disc.iso>
   # writes <user dir>/randomizer_locations.log:
   #   LOC 39F2DE28:B2701146:0000007E Missiles amount=5 capacity=5 \
   #       model=FFFFFFFF acs=90C6A0EE character=0 animation=0
   ```

   `MP_RANDO_SWEEP=1` makes one run visit every world and area (a world restart
   per area, waiting for streaming to go idle) so a complete dump needs no
   playthrough. It takes about twelve minutes and logs `[sweep]` progress. Add
   `MP_RANDO_SWEEP_WORLDS=<hex>[,<hex>...]` to tour only some worlds (the ids
   are the `WORLD:` column of the dump); the tour logs `complete: N worlds, M
   areas` when it finishes. The run needs a display; see `docs/NATIVE_PORT.md`
   for the Wayland/X11 notes.

   A full tour writes 2773 LOC lines covering 1451 distinct pickup keys in 180
   areas, of which **100 are item locations** — the same count retail Prime has,
   and a seed built from that one dump covers all of them. The dump includes
   pickups on script layers that are inactive when the area loads (read from
   the area's layer buffers, not built); the Artifact of Wild is only there.
   The Phazon Suit has a second, model-less pickup in Elite Quarters
   (`001A04C5`, beside the visible `001A04B8`); `rando_seed.py` gives it its
   visible sibling's placement and the port keeps it invisible. Areas also list their
   enemy drop templates as pickups (health and ammo refills with `capacity=0`);
   those are not item locations and are filtered out below. All eight worlds
   sweep to completion, Impact Crater included: its 12 areas are reached
   (`complete: 1 worlds, 12 areas`) and dump 85 pickups, every one of them a
   `capacity=0` drop, so it contributes no item locations — which matches retail,
   where the crater holds no items.

2. Shuffle the dump into a seed:

   ```sh
   python3 tools/rando_seed.py --from-dump randomizer_locations.log \
       --out randomizer_seed.json --seed my-seed --shuffle-seed 1
   ```

   Locations not present in the seed keep their vanilla items, so a partial dump
   is a valid partial randomizer. `--from-dump` may be repeated to merge several
   dumps, and `--include-drops` keeps the zero-capacity drop templates as
   locations. The tool also derives the `models` map from the dump, so a full
   dump gives full model coverage.

3. Play with the seed. Applied rewrites are appended to
   `randomizer_placements.log` (once per location per session, with the model
   used) and collected checks to `randomizer_checks.log`.

The randomizer announces itself at startup (`randomizer: seed 'x', N placements,
M item models`, `randomizer: dump mode (MP_RANDO_DUMP)`) and is otherwise silent.

## Verified

- `port_randomizer_tests` covers the seed parser (valid, partial, malformed,
  model entries with and without assets), the location-key format, item-name
  mapping, model lookup and the no-seed no-op path.
- On a real USA v1.00 disc, dump mode logged live pickups with their models, and
  a seed both moved items and swapped their models: Tallon Overworld's landing
  site missile expansion became an energy tank
  (`PLACE 39F2DE28:B2701146:0000007E Missiles -> EnergyTanks amount=1 capacity=100`),
  and in Chozo Ruins the swapped pickups resolved to the right assets
  (`PLACE 83F6FF6F:D5CDB809:0002012C Missiles -> EnergyTanks ... model=86908399
  acs=F37BCBC7`, both `ANCS`).
- Dumps of the first areas of Tallon Overworld and Chozo Ruins produced 5 real
  Chozo item locations and 2 item models after filtering drops.

## Not done yet

- **Logic.** The seed-file path below has none; it applies what an offline
  tool placed. The built-in randomizer above is logic-checked.
- **Model coverage.** Models come from a dump, so an item the dump never saw
  keeps its retail model. There is no built-in table yet.
- **Coverage.** `CScriptPickup` is handled; `CScriptPickupGenerator` drops and
  script-granted items (artifacts via `CArtifactDoll`, suit upgrades) are not, so
  a full randomizer needs those locations too. Drop templates must stay out of
  the seed, which the tool does by default.
- **Checks in the save.** Checks are logged to a sidecar file and counted per
  session; they are not persisted into the save game yet.
- **Archipelago.** Implemented; see `docs/ARCHIPELAGO.md`. Checks are reported
  from `CScriptPickup::Touch` and items are granted by `PortAp::Poll` each
  simulation tick, over a native WebSocket client. The Metroid Prime world's
  item and location tables are built in, so a server address and slot name are
  enough; `tools/make_ap_config.py` remains for checking the location table
  against a pickup dump.
