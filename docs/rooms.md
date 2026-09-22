# Rooms: where the player is, and how we know

A blind player's perception of place is what the mod announces; nothing else says "room". This document is the
model, the data, the tooling and the runtime of that feature (designed with the user 2026-08-22; the
authoring workflow and the description rules come next).

## Vocabulary
- **region** -- the game's named area ("Devil's Crossing"), identified by the world-map location record of
  its chunks (`riftgatemap1a_devilscrossing`). The HUD's area name.
- **sub-region** -- our named division of a region ("the prison", "the road north"); assigned by the
  authoring workflow so a player without the map can plan a route. Table `subregions`.
- **room** -- a piece of the walkable ground produced by the segmentation: tens to a few hundred m², a box
  or an approximation of one. Has a stable key, a class, an anchor, and (once authored) a title and a
  description.
- **chunk** -- the engine's `GAME::Region`: one `.lvl` inside `world001.map`, 128x128 units (a few are
  96 wide), the unit of streaming and of `WorldVec3` (region-relative position). 633 chunks; the overworld
  ones are laid out in one shared frame: **tile coordinates are world coordinates**, and a chunk's
  `GetOffsetFromWorld()` IntVec3 is simply its tile frame's min corner (verified live against
  `World::GetRegion(i)` for all 633 on 2026-08-22). Region-relative = world - offset.

## What the game ships (read offline by `tools/gdmap`)
- `resources/Levels.arc` -> `world001.map` (819 MB): a "MAP" v9 header with the quest list, a "Unique
  Entities" table and the chunk table (per record: IntVec3 offset, 16-byte GUID, location record, devotion
  shrine record, lvl path, body offset + size, 6 placement ints), then every chunk's "LVL" body.
- Inside a body: **Detour tile-cache layers** (`DTLR` v1; 32x32-unit tiles at 128x128 cells = 0.25
  units/cell; uncompressed 49208-byte blobs of heights/areas/cons). area 0 = unwalkable, 1/2 = the
  engine's 8-unit checkerboard (polygon splitting, NOT surface classes). Quest gates (doors, barricades)
  are runtime obstacles on top of this bake, so the bake is "everything walkable regardless of quest
  state". The bake leaves an un-eroded 4-cell band along tile seams (walkable strips inside walls; the
  game does not walk them): `seam_filter` drops band cells not backed by interior ground.
  The runtime mesh is up to ~1.5 units wider than the bake at edges (polygon simplification), and an
  upper floor (the prison's cell block at height 14 over ground at 8) shares (x, z) with the ground:
  multi-layer cells are unioned.
- **Terrain texture layers**: `records/terraintextures/*.dbr` names each followed by a 128x128 u8 opacity
  mask at 1 unit/sample (region-local; world = offset + local). Roads are painted layers (gravel,
  cobbles, flagstone, fieldstone...); which records are roads is a judgment (`ROAD_RE` in rooms.py for
  now), `surfaceType` is only footstep audio (Grass 68, Dirt 63, Stone 29, Wood 2, Snow 1, Unwalkable 1).
  Roads are an overlay for road helpers, never a segmentation input (decided).

## Segmentation (`tools/gdmap/rooms.py`, a port of wotr-access's RoomMap)
Per region, all its overworld chunks stitched (a room never needs to stop at a chunk seam):
**seam-stitch** (`bridge_walk_seams`, 2026-08-24: reconnect walkable cells across internal 32-unit tile seams
the baked tile-cache erodes -- per-tile builds drop a thin band the runtime navmesh actually keeps continuous,
which otherwise islands rooms the player walks to freely, e.g. the riftgate courtyard. A gap <= `max_gap`=6
straddling a seam with matching floor height on both sides is filled; heals the riftgate + ~10 island rooms
per region, validated cell-by-cell vs the live navmesh) ->
seam filter -> islands (components not connected to the main one; under `island_min` m² dropped, larger
ones flagged) -> furniture mask (interior islands <= `furniture_max` cast no clearance shadow) -> clearance
(EDT) -> persistence watershed (basins from clearance maxima, split where they meet across a dip deeper than
`persist`) -> BFS fill of sub-`cut_floor` slivers -> small-region merge (< `min_area`) -> **walk cap**:
a room whose longest walk (double-sweep Dijkstra on the 8-neighbour cell graph) exceeds `max_walk` is
bisected with an **axis-aligned line** across its bounding box's longer axis, in the middle third, where the
cross-section is narrowest (decided: the announcements ARE the perception, so a boundary must be simple to
hold in the head -- a north-south or east-west line, never a geodesic contour) -> merge again -> stats
(anchor = the cell of maximum clearance, class from area/elongation/clearance as in wotr). Rooms are ordered
by anchor (z, x). `area --write` still records per-pair exits in the db, but **intra-region exits are now
computed at RUNTIME** (below) and the mod ignores those stored rows; only the cross-region `seams` rows are
read. Devil's Crossing after the 2026-08-24 reseg: ~50,000 m², 197 rooms.

## Exits (computed live at runtime, 2026-08-24)
The exit table was flat-bake-derived and wrong: the segmentation runs on the baked Detour **tile-cache**,
which is flat (lowest layer), per-tile seam-eroded, and off-mesh-blind, so stored exits were false (2-D
adjacency across a wall or a height step), missing (seam erosion; stairs/quest doors are off-mesh, not in the
bake), or duplicated. The map holds only the tile-cache -- the game assembles the real 3-D navmesh from it at
load -- so the correct source for connectivity is that live navmesh, at the player's actual position and quest
state. So the mod **finds exits at runtime** (`src/rooms.cpp exit_items`, on V / room change only):
- `LabelGrid::neighbors_within(x, z, y, radius=28u)` returns every OTHER room whose cells fall within the
  radius (height-aware via `at(col,row,y)`), each with its nearest cell.
- `world::find_path` (= `GAME::Player::FindPath`, dev route `/findpath`; result 0 = reachable) validates each:
  a neighbour is an exit iff the game's own pathfinder can reach it. This is correct for quest state, doors,
  destructibles and stacked floors for free -- `NavBlocker` (`dynamicblocker_invisible`), `FixedItemDoor` and
  `Destructible` gate `find_path` exactly as they gate the player, and unreachable elevated floors are dropped.
  **"Reachable" means the route ARRIVES** (2026-08-30): Detour snaps an unreachable target onto the nearest
  polygon and reports result 0 with the endpoint on OUR side -- at the player (a room through a wall) or on the
  lip of the ledge we stand on (Flooded Cellar, "ruined camp cavern 6 away" across a drop-off: endpoint 4.9 u
  short, same height, corridor uncomputable so the additive rule kept it). The gate now requires the reached
  point within `kReachTol` 1.5 u of the destination cell or inside the neighbour's own label; the old "made
  progress toward the target" test let any target under ~6 u through.
- The bearing is to the neighbour's nearest cell and **may point through a wall** at a room reached by going
  round (decided with the user: same as the old adjacency exits; line-of-sight is not required).
- **The route must be DIRECT** (2026-08-24, on the user's report): "near and reachable" is not enough -- a room
  whose only navmesh path from here runs *through* a third labelled room is that third room's exit, not ours.
  So each reachable candidate's actual corridor (`world::find_path_corridor` = `GAME::NavManager::FindPath`,
  whose 7th arg is a `mem::vector<WorldVec3>` straight path; dev `/findpath?...&corridor=1`) is classified
  cell-by-cell against the label grid (`LabelGrid::path_is_direct`, engine-free + unit-tested): the candidate
  is dropped if the corridor lingers more than a ~2u tolerance inside any room that is neither the current room
  nor the candidate. Off-grid / unlabelled samples (a gap between rooms) don't count as a detour, and LOS is
  still not required (the corridor may curve round a wall, it just must run from our room into the neighbour).
  The test is **strictly additive**: a candidate is dropped only on a positive detour finding; if the corridor
  cannot be computed the candidate is kept (the reachability gate already passed) -- so a corridor hiccup never
  loses a real exit.
Consequences: no false doors, no duplicates, no exits that are really two rooms away, seam-islanded and
off-mesh-reachable rooms appear when actually reachable; a room whose opening is > radius away shows once you
near it (accepted). The offline `exits_of` / `fill_label_seams` / `rooms.py exits` machinery is now vestigial
for intra exits.

**Cross-region exits** (`rooms.py seams [--write]`, 2026-08-23; still used at runtime): the per-region
watershed sees its grid
edge as a wall, so an opening at a REGION boundary (the Devil's Crossing -> Lower Crossing road at z=-256,
found on the user's real save) never becomes an exit. The seams pass scans every region pair in the db for
adjacent walkable cells across the seam (heights within 1 unit when known), clusters them into openings
(>= 0.75 units wide) and writes exit rows into BOTH regions with the far side's full room key as `room_b`.
The mod keeps an exit whose `room_b` is not local as a foreign exit and speaks its destination as
"<region name>, <room title>" ("Lower Crossing, stump clearing"). **Re-run `seams --write` after any
`area --write`** -- re-segmenting a region deletes all its exit rows, the cross-region ones included.

Defaults (`Params`): persist 0.7, min_area 12, cut_floor 0.45, furniture_max 12, max_walk 60,
min_split_area 40, island_min 50. Sensitivity on Devil's Crossing: persist 0.5 -> 164 rooms, 0.7 -> 144,
1.0 -> 129, 1.5 -> 114 (five-chunk cluster); the cap made 9 cuts at 60 and 1 at 80 there, 20 over the
whole region (the open eastern chunks). Per-region params live in the db (`rooms.py area X --set k=v`).

## The database (`assets/rooms.db`, `tools/gdmap/roomsdb.py`)
Written by the tools, read-only for the mod (vendored SQLite, `src/db.*`).
- `regions(key, name, location_record, chunks json, params json, algo_version, signature, stale)` --
  key = the location record basename; signature = blake2 over the chunks' bodies (a game patch that changes
  a chunk marks the region stale: `rooms.py status`).
- `grids(region_key, x0, z0, w, h, cell, labels, label_keys json)` -- the label grid, run-length encoded as
  (int16 value, uint16 run) pairs row-major (`rle_encode` / `LabelGrid::decode_rle`); label -> room key.
- `rooms(key, region_key, subregion_key, anchor_x, anchor_z, cls, area, walk, bbox json, island, title,
  body, status)` -- **key = `<region>:<round(anchor x)>:<round(anchor z)>`**: authored rows (title, body,
  sub-region) survive a re-segmentation as long as a room still contains that anchor; vanished keys become
  `orphan`. status: unseen, shot, described, verified, orphan, stale.
- `subregions(key, region_key, name, summary)`, `exits(region_key, room_a, room_b, x, z, width, cut)`,
  `shots`, `terrain_types`, `coverage` (for the authoring workflow).

Commands: `uv run tools/rooms.py regions|grid|segment|area|status|plan` (docstring has the forms);
`area devilscrossing --write --name "Devil's Crossing"` writes the region; `plan devilscrossing` renders
what the mod will use. Floor plans: `build/rooms/*.png`, north (-z) up, exits as dots (red = cap cuts).

## Runtime (`src/rooms.cpp`, `src/core/rooms_model.*`)
- Per frame from the in-game screen: chunk (`world::region_name()`) -> region (the db's `chunks`) ->
  the region's grid (loaded once) -> `label_at(x, z, ring 8 cells)` -> room. `Hysteresis`: after 1 s in a
  room (`settle`) any change is announced at once; within that first second a change must persist 400 ms
  (`dwell`) so boundary flapping stays quiet (`/room?dwell=&settle=`); the first room is immediate.
- On a confirmed change one line in the player's own voice (Zira, `voice::kGroupInfo`): the region name
  if it changed, the sub-region name if it changed, the room title -- or "room N" for untitled rooms while
  nothing is authored (`/room?untitled=0` to silence). "Devil's Crossing, room 193" at the spawn.
- **X** speaks the room's title, then its description, through the screen reader ("no description yet"
  until authored).
- **V / Shift+V** = the scanner's `ScanGroup::Exits`: `rooms.cpp` provides the current room's exits as point
  items (`world::set_exit_provider`; id = `kPointIdBase` + exit index, label = the destination's title or
  "room N", note "blocked" when the live `IsPointOnPathMesh` refuses a 5-point cross at the opening although
  the bake allows it -- a shut gate or barricade) and `world::cycle_review` does what it does for every
  group: continue from the reviewed id, nearest first, `lock_point` on the opening, the route ping on landing,
  `;` re-pings, "distant" when off screen. "room 190, blocked, 5 away, 4 o'clock, 1 of 3".
- Dev: `/room` (the lookup chain, the current room, its exits with live walkability; `?say=1` repeats,
  `?reload=1` reopens the db after a tools rewrite), `/regions?max=`, `/portals`, `/navprobe?x0=&z0=&x1=&z1=&step=`
  (the live mesh sampled; `build/rooms/compare_navprobe.py` diffs it against the bake).
  Authoring: `/teleport?x=&z=[&check=1]` (world coordinates; refuses unloaded chunks and off-navmesh landings;
  `tools/shots.py` hops 60 units at a time so chunks stream in), `/project?pts=`, `/fog?x=&z=&radius=`,
  `/pause?set=0` (a hot reload in the world can leave the game paused).

## Authoring (done for Devil's Crossing 2026-08-22)
`uv run tools/shots.py region <region> --status unseen` photographs every room (the game's own teleport, fog
reveal, outline + exits drawn from `/project`, terrain and entity facts in `meta.json`); then the Workflow
`tools/workflows/rooms_author.js` (every agent Opus): sub-regions once (`subregions: true`), one describer per
room (`rooms: [keys]`, self-checked with `author.py check`, every 10th room reviewed), then the consistency pass
(`consistency: true, subregion_keys: [...]`, optional `notes`) that reads each sub-region as a whole. Rules:
`docs/rooms-description-rules.md`. Copy `assets/rooms.db` next to the DLL (a build does it) and `/room?reload=1`.

## Shots pacing: `/settle` (2026-09-15)
The tour used to sleep a flat 1.6 s per sample "to let the chunk stream" (~3 s a room, 8 h for the campaign). The
engine exports the real condition: `ResourceLoader::IsIdle()` (on `Engine::GetResourceLoader()`: a zero-timeout wait
on the loader's work event, nothing queued) and `Region::IsLoadingFinished()` (level attached, loading byte clear).
Dev route `/settle[?radius=160]` reports `idle`, `loading` (chunks near the player still loading), `unloaded_near`,
the exe app state (10 = loading screen) and the engine tick; `shots.settle()` polls it every 50 ms until idle, no
loading chunk, not on the loading screen, then two more frames + 0.3 s grace. Measured: a shot 0.25 s after a
2000-unit hop already showed every prop (the loader flickers busy for ~3 s after on mip streaming that does not
show at shot size); same 15 Gloomwald rooms 3.06 -> 1.27 s a room, shots equivalent by eye. `--legacy-wait` keeps
the old sleeps for A/B. `shots.py all --status unseen` tours every region nearest-first and relaunches the game on
a crash (`ensure_game`: kill, `gd.py launch`, press Start on the selected character, wait for the world).

## Painted area names (2026-08-31, `tools/gdmap/sectors.py`, `rooms.py areas --write`)
The game's HUD area name ("Lower Crossing", "Burrwitch Slums", "Anguish") is **painted per cell**, not a
volume: each chunk body carries a "sector" section -- `[u32 1][u32 ntab]`, `ntab` GUID tables (`[u32 n]` + n
x `[u8 editor-id][guid 16]`; ids are arbitrary), then `[u32 w][u32 h]` and `w*h*ntab` bytes of per-cell ids,
x-major, 0 = none, spanning the chunk footprint at ~1 unit/cell. **Table 1 is the area layer**; its GUIDs
resolve through the map header's unique-entities table (name, guid, two editor RGBA colours, `tagMap*` tag)
and Text_EN. `Engine::GetAreaNameTag` (Engine+0xb40, exported; `world::area_name()`) is the live readout of
the same layer. Decoded offline and validated against 643 live reads across the whole map (100 %; the only
diffs were `{^n}` formatting). The other 13 tables are the engine's other painted layers (`*SectorData`:
ambient, climate, bloom, boss, ...), unmapped.

Consequence measured on the way there: the location-record region partition genuinely spans several named
areas (the `devilscrossing` region's ground is 60 % painted "Lower Crossing"; `alpinefort`'s majority is
"Plains of Strife"), so the REGION name is the wrong first layer for speech. **The spoken place is now
"painted area, sub-region, room"** (decided with the user 2026-08-31): `rooms.area_name` (schema v3 column)
holds each room's majority painted name (10,382 of 10,503 rooms; unpainted rooms fall back to the region
name), `announce()` speaks it as the first layer, and a foreign exit's label uses the destination ROOM's
painted name. Regenerate with `uv run tools/rooms.py areas --write` (also names the placeholder regions:
c01a = Old Grove [cut content SE of Devil's Crossing], a03a/a04a = the cut Prospect Hill corner NW of
Burrwitch whose tag text was deleted, map01_gatex01a = Obsidian Throne). Region `name` stays the riftgate
zone name -- the travel label, still used by dev output and as the fallback.

## Expansion maps (2026-09-14, `tools/gdmap/gamefiles.py`)
Each expansion ships a COMPLETE replacement `world001.map` in its own `Levels.arc`, not a patch: base 633 chunks,
Ashes of Malmouth 876, Forgotten Gods 1582, and Fangs of Asterkarn 2050 region records.
The game mounts base < gdx1 < gdx2 < gdx3, last wins per file and per database record. The base chunks are recompiled
with the expansion layers on -- new NPCs and
props in the old towns (Devil's Crossing gains the Emissary, the illusionist, six Black Legion soldiers), new side
areas (Broken Hills' Lost Ruins, Pine Barrens, Twin Falls), new shrines and totems, a 14th painted sector table --
so 417 of 633 bodies differ but only 164 walkable grids do, 99 of them by < 1 %. Three dungeons MOVE intact by
(+224, +160) (Warden's Laboratory, Underground Transit, the Burrwitch necropolis crypt), the cut Prospect Hill
corner and four Burrwitch Village chunks are deleted and Gloomwald (0H) sits on their footprints, Malmouth (0I)
hangs off Ugdenbog's north, Forgotten Gods (0J) is an island far south-west reached only by the Emissary's portal.
Details of the survey: the 2026-09-14 session (mapdiff / bodydiff / anchor tests).

Consequences for the tools and the db:
- **Every offline tool reads through `gamefiles.py`**: the highest installed layer's `Levels.arc`, the databases
  overlaid (`arz.load()` returns a `Layered` view, later records override), the Text arcs merged (the DLC zone names
  `tagGDX1Rift*` / `tagGDX2Rift*` / `tagGDX3*` live only there). `GRIMDARK_GAME_LAYERS=base` forces the base world.
- The expansion region record has a third string slot for a skybox. Fangs sometimes roots it at `art/terrain/`
  instead of `records/`; `mapfile.py` recognizes the length-prefixed path rather than assuming one root.
- **The level-body cache is per map** (`build/rooms/cache/<map_id>/`): gdx2 rewrote moved chunks at the SAME byte
  size and the shared name+size cache served the base bodies (Warden's Laboratory came out at its old place).
- **One db per supported world**: `assets/rooms_gdx3.db` is Fangs, `assets/rooms.db` is Forgotten Gods, and
  `assets/rooms_base.db` is the frozen base game. `src/rooms.cpp` selects the highest installed expansion. An
  Ashes-only install gets the base db and a log line.
- **Regen recipe** (what was run): `rooms.py shift <region> --dx 224 --dz 160 --write` for the three moved
  dungeons -> `rooms.py rebuild --write --prune` (re-segments every cluster of the current map under the stored
  region whose chunk set it overlaps most, so `write_segmentation` re-attaches authored rooms by anchor key; new
  keys for new clusters; prunes regions whose chunks are all gone) -> `rooms.py rehome --write` (the key rule needs
  the anchor CELL to reproduce; an orphan whose anchor now lies inside a still-unauthored room hands it its text --
  the containment rule the docs always claimed) -> `areas --write` -> `seams --write`. `coastroad_2` = 0W015, a
  water chunk with no nav tiles, fails harmlessly.
- Untagged in the DLC: the Shattered Realm pieces (`Levels/EndlessDungeon`, 306 "DF" pieces have no baked nav
  tiles at all) and staging levels -- location-less, like the Void.
- 0I023 (Lone Watch) is the one content chunk whose nav tiles lie outside its footprint (480 u south): a stale bake, the
  game has no navmesh there (every teleport into its 28 rooms timed out on the tour). `build_area` now skips any chunk
  whose tiles fall outside its own footprint.

## Open
- One room of Devil's Crossing (`-70:-183`) is unseen: its anchor is bake-only ground the live mesh refuses.
- Exits across region boundaries, road helpers (skeleton graph), the procedural DLC (segment live from the
  in-memory data; the anchor-keyed design allows it), player-authored marks.
- Untagged content: the Void (Ashen Waste + Bastion of Chaos, 33 chunks, ~169k m², paints "Obsidian Throne"
  etc.) and the cut-content corners (0C/0E/0A085 chunks with no location record) are not segmented -- `build`
  iterates location records, and location-less chunks need a grouping path of their own.

## Data layout (2026-09-20)

The rooms data is committed as text, not as the SQLite files (which were 41 / 70 MB binaries git could not diff and
GitHub caps at 100 MB): `data/rooms/<world>/` with `meta.json`, `regions/<region>.jsonl` (one JSON object per line:
the region row, then its sub-regions, rooms, exits and shots, each sorted -- a title edit is a one-line diff) and
`grids/<region>.bin` (the label / height / overlay RLE blobs, zlib-compressed ~0.3, behind a JSON header). Worlds:
`gdx3` = Fangs (`rooms_gdx3.db`), `gdx2` = Forgotten Gods (`rooms.db`), and `base` = base game (`rooms_base.db`).
`tools/rooms_pack.py` moves between the two forms and is stdlib-only:

- The mod's dbs are BUILD PRODUCTS: CMake runs `rooms_pack.py build` into `build/ninja/assets/` (re-run when any data
  file changes) before the DLL builds, and `tools/package.py` takes them from there. Nothing under `assets/` is a db.
- The authoring tools (`rooms.py`, `author.py`, `describe_or.py`, `shots.py`) work on `build/rooms/rooms.db` (and
  `rooms_base.db`), created by `rooms_pack.py unpack --world gdx3|gdx2|base`. After an authoring session run
  `rooms_pack.py pack --world ...` and commit the text. `pack` refuses when the text changed after the db was written
  (unpack first), `unpack` refuses when the db is newer than the text (pack first); `--force` overrides;
  `status` says which side is ahead. `verify` proves a db and its text are identical table by table.
- Migrated 2026-09-20 from the committed dbs with a verified identical round trip for both worlds; the old blobs
  remain in git history.

## Duplicate titles (2026-09-20)

A tester reported "unsuffixed duplicate room names" in the Flooded Passage. Four things stack:

- Titles are unique WITHIN A SUB-REGION by rule (`author.py save_description` adds the " N" suffix against the same
  sub-region only). Across sub-regions a title may repeat; the spoken line disambiguates with the sub-region name,
  which is spoken only when it changes. The Flooded Passage has "ruined campsite" once per sub-region, unnumbered, next
  to true twins that are numbered ("hanging vine cavern" / "hanging vine cavern 2") -- a player infers a rule and
  reports the repeat as a missing suffix.
- The exits list labels an exit by the destination's title alone, so two exits to same-titled rooms in different
  sub-regions read the same. (Open; a runtime disambiguation would append the sub-region when the title repeats.)
- The describer and the sub-region pass are independent commands. Where describing ran first (most regions), every
  room had a null sub-region, so the suffix was effectively region-wide; the painted sub-regions then straddled the
  numbering (1410 of 1980 base-db suffixes had no base in their own sub-region: "lily pad shallows 4 / 5 / 6" in three
  sub-regions of Burrwitch Road). Burrwitch Village, authored sub-regions first, had none.
- `describe_or.py` saved from 32 worker threads through a read-then-write dedupe: 61 same-sub-region duplicates in the
  DLC-only regions ("tall grass path" twice in one sub-region of Aetherfire). `save_description` is now serialized.

Decision (the user): no re-tagging -- the pipeline costs money and does not reproduce titles, so a retag loses every
title anyway; manual / mechanical fixes of both dbs are fine. `author.py [--db path] retitle [--write]` is the
mechanical pass: per (region, sub-region, base title) group, orphans excluded, one room -> the bare base; several ->
kept if they already read base, base 2 .. base n, else the bare one stays bare and the rest take 2.. in anchor (x, z)
order (deterministic, so the two dbs agree wherever their twin sets agree). Run 2026-09-20: base 1403 changes (794
stale suffixes lifted, 609 twins renumbered), DLC 1566 (+93 same-sub-region duplicates fixed); afterwards 0 duplicates
and 0 stale suffixes in either db. Rule going forward: run `subregions` BEFORE `describe` for a region, and after any
rehome / resegment run `retitle --write` on both dbs.

Size: the dbs are 41 MB (base) and 70 MB (DLC) with zero free pages, so VACUUM does nothing. 80 % is the `grids`
table, mostly `heights` (24 / 41 MB of RLE int16 decimeters), then `labels`; zlib takes the RLE blobs to ~0.3, so
compressing the three grid columns (inflate at load; the DLL has no zlib yet) would give ~15 / ~25 MB. `shots`
(1.6 / 2.9 MB) is authoring data the mod never reads. The zip already deflates them for download; the cost is on
disk and in git, where only the changed pages of a db edit cost (git delta-packs binaries).
