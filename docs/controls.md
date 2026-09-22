# Grim Dawn default controls (v1.3.0.8, read from Options -> Controls on 2026-08-21)

The game's keys are remappable by the player in Options -> Controls (the keyboard-icon tab): click a row's
Primary or Secondary cell, press the new key, Apply. Changed maps are written to
`%USERPROFILE%\Documents\My Games\Grim Dawn\Settings\keybindings.txt` (mouse movement) and
`alternate_keybindings.txt` (keyboard movement, `action: primary secondary` in the button codes; the move actions are 63..66 =
forward/backward/left/right). With no changes the files do not exist and the defaults below are in effect -- EXCEPT the four
move actions: switching Movement Type to Keyboard creates them UNBOUND, and only the Keybinding tab's Default binds them
(measured 2026-09-19; it also resets every other binding). gdlaunch therefore OWNS that file: before every launch it is
rewritten as the game's default keyboard map with W/S/A/D (`core/game_settings.h`; the mod's lifted chords assume the
defaults, players are told not to rebind), and movementType/evadeFollowCursor are forced in options.txt. `Engine::GetKeymapPath()` /
`SetKeymapPath()` are exported, so the mod could point the game at its own map file. "Movement Type" on the
same page is `movementType` in options.txt (Mouse = 0, Keyboard = 1).

The list widget does NOT draw through the RenderText2d hooks (like text fields), so this was read from
screenshots. Rows in the game's order; "-" = unassigned.

| Function | Primary | Secondary |
|---|---|---|
| Forward / Backward / Left / Right | W / S / A / D | - |
| Character Window | C | I |
| Skill Window | N | - |
| Codex Window | Q | - |
| Map Window | M | Gamepad Down |
| Loot Filter Window | O | - |
| Chat Window | Enter | - |
| Group Window | K | - |
| Game Menu | G | - |
| Help Window | H | - |
| Factions Window | J | - |
| Achievements Window | V | - |
| Quickbar Slot 01..10 | 1..9, 0 | Gamepad A/X/Y/RTrigger/Left/Right (slots 1-6) |
| Camera Zoom In / Out | Wheel Up / Wheel Down | - |
| Camera Max Zoom In / Out | - | - |
| Camera Default View | - | - |
| Camera Rotate | Middle Mouse | - |
| Camera Rotate Left / Right | , / . | - |
| Drink Energy Potion | E | Gamepad RBumper |
| Drink Health Potion | R | Gamepad LBumper |
| Center Map (Map Window) | - | - |
| Drop Item | B | - |
| Personal Riftgate | L | Gamepad Up |
| Switch Weapons | - | Gamepad LThumb |
| Interact | U | - |
| Pickup | - | - |
| Evade (see docs/evade.md: turn Evade To Cursor off) | Space | Gamepad B |
| Show Items (No Filter) | Alt | Right Alt |
| Show Item Tooltips | X | - |
| Show Items (Filter Common) | Z | - |
| Toggle Hide All Items (Loot Filter) | - | - |
| Target Pet (Hold Key and Click) | Ctrl | Right Ctrl |
| Stationary Attack (Hold Key and Click) | Shift | Gamepad LTrigger |
| Pause Game (Single Player Only) | P | - |
| Toggle Pet Display | Backspace (taken by the mod: the pet overlay) | - |
| Toggle Party Display | \ | - |
| Quickbar Switch | Y | Gamepad RThumb |
| Select Pet 1..5 | F2..F6 (taken by the mod: its own selection, see Pets) | - |
| Select All Pets | F7 (taken by the mod) | - |
| Push To Talk | Tab | - |
| Toggle UI | ] | - |

Not on this page: Escape = game menu (pause), left mouse = move/attack at cursor (in Keyboard movement mode:
attack/interact at cursor), right mouse = secondary skill slot, F1 = not bound by the game (F2-F7 are pets; F8 and
F9 are free).

## How the mod uses this (2026-08-21)
In the world the mod owns the keyboard. Direct pass-through (the frequent keys): WASD, 1-0, Space, E, R, U,
Escape, Alt/Right Alt, F2-F7. Everything else in the table is LIFTED to Ctrl + its default key
(Ctrl+C character, Ctrl+M map, Ctrl+N skills, ... see the `game.*` actions in src/app.cpp): the chord is the
mod's, which injects the plain key into the game's poll. The game's own map is never changed, so a player's
rebinds in Options -> Controls do not matter to the mod (and would break the lifted chords if they moved a
default key; a later version reads keybindings.txt). Not yet handled: the hold-and-click modifiers (Shift
stationary attack, Ctrl pet targeting) and typing into the game's chat field.

## The mod's in-world keys (first cut, 2026-08-21; wotr's review-cursor layout)
| Key | Action |
|---|---|
| . / Shift+. | Next / previous enemy (nearest first from the player); an enemy reads "name level N rarity" -- the rarity word (champion / hero / boss / quest / super boss) is spoken only when the enemy is one (a common enemy is just "name level N") |
| , / Shift+, | Next / previous of only the highest-rarity enemies nearby (find the boss, or a summoner's adds); same readout and landing as . (an inert camera key otherwise, since the camera is locked) |
| / (slash) | Inspect what you are targeting: "N percent health" then its status effects, with no name repeat. Silent when nothing / no living enemy is targeted. A screen-reader readout, like H |
| N / Shift+N | Next / previous person or object: the important non-loot things -- NPCs with a conversation (`Npc::HasConversation`) and what the game's Interact key would use that is not loot (rifts, shrines, doors, levers: `FixedActor` whose `IsOfInterest()` says so) |
| B / Shift+B | Next / previous bystander (NPCs without one) or breakable (barrels, crates, jugs, quest destructibles -- hold J to smash) |
| M / Shift+M | Next / previous loot: items on the ground and containers (`Item` / `FixedItemContainer` whose `IsOfInterest()` says so). Ground items also obey the player's loot filter (`Item::PassLootFilter(0)`, the same predicate as the game's labels and G) unless O has latched "show all"; anything the game does not show at all (`Entity::GetVisibility() == 0`, e.g. a placed quest item already collected) is never listed in any group (docs/loot-filter.md) |
| O | Show all items on / off: the loot group, the loot sonar and the game's own labels ignore the loot filter while on (the game's Alt modifier byte, re-asserted every frame). The window itself is Ctrl+O |
| Ctrl+O | The loot filter window: four Tab stops (Quality / Type / Damage / Character), toggles in the game's order, Enter = `Player::SetLootFilter` (immediate, saved with the character) + the drawn box mirrored, Space = the box's `tagLootFilterNNInfo` tooltip, last row = that column's factory defaults |
| (blacksmith) | The crafting window (`InGameUI+0x3aa80`, docs/crafting.md): tabs = the game's five category buttons pressed through its radio registry; rows = the window's own list box (game order, grouping, "[N]"); the value line from the formula exports (`GetMaximumCraftable`, `GetCreationCost`, `GetReagentN*`); Enter = `ListBox::SelectByData` + the window's Combine button (never `SendCreateArtifactCmd`, which validates nothing) when N >= 1, else the shortfall; Space = the template result's tooltip |
| (inventor) | The Inventor's "enchanter" window (`InGameUI+0x30dd8`, docs/inventor.md, docs/re_inventor_exe.md): tabs = the game's tab buttons through its radio registry (Convert / Reroll only when the game enables them -- expansions). Item-first: Salvage rows = bag items with `ItemEquipment::HasRelic` / `HasEnchantment`, value = `trunc(0.05 x GetItemCost)`; Enter = picker of the applicable panel buttons; picking = the exe's chamber drop (box SetItem + `PlayerInventoryCtrl::RemoveItem`) + the button through the panel's registry -> the game's confirm dialog (message_box) -> on Yes the kept item is taken out of the chamber (`ControllerPlayer::GiveItemToPlayer`), on No the item is returned the same way. Dismantle rows = bag equipment with `GetItemClassification(true) > 0`, value = `itemLevel*10+150` (+ the component's salvage cost); Enter = the same put + Dismantle button; the two result boxes are emptied into the bag and listed in the reward notice overlay (`screens/reward_list.h`, shared with the quest reward window's rows; layer 30, Close row / Escape). Never `SendEnchanter*Cmd` directly |
| Alt + . N B M V | The NEAREST of that group, whatever is reviewed now (the enemy that just ran up to you) |
| \ (backslash) | Sonar on / off: each nearby enemy, loot drop, breakable, devotion shrine (a ruined one has its own cue, a restored one the loot cue) and dungeon entrance repeats its own ping, faster as it nears you and panned to its direction (Ctrl+\ is the game's party display) |
| J (or Enter) | Left mouse button at the reviewed thing (or the real cursor when nothing is reviewed): attack / talk / open / move, exactly as a click. HOLD to hold (sustained attack, skill, move). A reviewed thing the camera does not show, or whose point lies on the HUD: aimed at BY DIRECTION (the press lands on the player-to-target line at the edge of the aimable screen, backed off the HUD; `world::aim_along_line`, 2026-09-17) -- the game fires point-aimed skills that way, searches near the point for an enemy, walks / attacks a weapon attack toward it; the charges refuse natively. Exception: a reviewed ITEM on the ground -- sighted players click its floating label, which only exists while the loot filter shows it -- gets the game's own "walk there and pick it up" command (`ControllerPlayer::ItemAction`, docs/re_pickup.md) on the press instead of a click; a reviewed door / ladder / chest / lever / shrine (a FixedActor the Interact key would use) likewise gets the game's "walk there and use it" (`ControllerPlayer::InteractAction`) -- the click at the parked point does not always hit such an object's body (a ladder's is up the wall above its floor point) |
| I | Right mouse button, same rules (the right-slot skill; hold to hold) |
| U | The game's own Interact: uses the nearest usable object (door, chest, shrine ...) or NPC within 10 units of the character, no aiming, walks there if needed |
| (camera) | Locked by the mod: zoom at the far end of its range, yaw 0 = the world grid's -z up, which is NOT the game's north: the dialogue's compass is screen-up at the default camera (yaw 0.8727), 50 degrees clockwise of ours, so the game's "north" is the player's 11 o'clock (`docs/compass.md`); the game's wheel/rotate inputs are re-pinned every frame. Zoom was found not to change what you hear (2026-08-22) |
| ; (semicolon) | Ping the reviewed thing again: one of three sounds for the route from you to it (straight walk / path around / unreachable), panned toward it, fading with distance; also played on every landing, and re-sounded automatically whenever the route kind changes while a thing is under review (walk into line of it, an obstacle clears, it goes unreachable) -- no key needed |
| Ctrl+M | The map: opens the game's aerial map and presents it accessibly as **one flat list**, nearest-first, with distance and clock bearing. Each icon is the game's own map marker, named as the game names it (docs/map-icons.md): a point of interest by the nugget's own text (quest-bound ones exist only while their task is active), a person / merchant by the entity under the icon, a barricade "obstacle" (from its dynamic-obstacle symbol), the rest by the map's rollover word (tagMapSymbol*). The zoom is pushed to the wheel's maximum (135 = a 405 x ~650 unit view, the sighted reach) while the screen is open and restored after. Enter picks a marker as the follow target ("following \<name\>") and closes the map. The marker set is whatever the game currently draws on the map (the loaded area) -- see docs on scope. (No quest/non-quest tab split: the marker source does not classify quest markers yet, so the split was empty.). A second Tab stop lists every devotion shrine the character has discovered anywhere, nearest first: "desecrated shrine, Burrwitch" / "not restored, Burrwitch Village Rift, 1200 away, 3 o'clock" (positions to chunk precision, src/shrine_table.h generated from the map; Enter follows it). Built 2026-09-04, not yet verified live |
| ' (apostrophe) | Follow the picked map marker: plays the route ping toward it (straight walk / path around / unreachable, panned, fading with distance, like ;) and speaks "\<name\>, N away, H o'clock". Tracks a moving NPC; "not following anything" until you pick one from Ctrl+M |
| K, Ctrl+Shift+P | Where am I (position, life, region) |
| C / Shift+C (Alt+C nearest) | Next / previous PLAYER CHARACTER (`ScanGroup::Players` = is-a `Player` in the sphere query): yourself today with the note "you" and no distance / bearing ("claude, you, 1 of 1"), party members in multiplayer; the landing locks the cursor on the character, which puts a cursor-placed skill (Inquisitor Seal, totems, traps, summons) at your own feet |
| ] / [ (Alt+] nearest) | Next / previous of YOUR PETS, nearest first: "Hellhound, aggressive, 2 away, 1 o'clock, 1 of 2"; the landing parks the review lock on the pet like any group (docs/pets.md) |
| Backspace | The pet overlay: one row per pet ("Hellhound, normal, selected"). Left/Right = stance (normal / aggressive / defensive -- per SUMMONING SKILL, so every pet of that skill follows, and a resummon remembers it), Enter = toggle selected, Backspace = disband, Space = where it is. Then two command rows for the selected pets (all when none are selected): "attack locked target" (the review lock's enemy) and "recall" (back to your side); a command closes the overlay and clears the selection. Escape closes |
| F2..F6 / F7 | Toggle pet 1..5 selected / select all (list order = the game's portrait order), announced; the selection only matters to the next command. The game's own F-keys and their hidden "next click commands the pets" mode are not used |
| Shift+Backspace | The selected (or all) pets attack the locked target, without opening the overlay |
| (automatic) | "Hellhound summoned" / "Hellhound down" in Zira when a pet joins or leaves the game's pet list. Pets never count as enemies for the review groups, the sonar or the combat voices; the game's "Pet Attack" default skill can be put on a quickbar slot from the hotbar manager and works against the locked target like any aimed skill |
| H | Health and energy in full ("health 250 of 250, energy 100 of 120") through the screen reader |
| (automatic) | Hits you or your pets land: the number the game draws over the enemy ("17", "45 crit", "Miss", "Dodge", "Block"), spoken by Mark panned to where it happened, as many at once as there are hits. Needs the game's "Display damage numbers" option (`displayDamage`, default on). Health: "health N percent" in Zira each time it crosses a 10 % step, down or up |
| (automatic) | A status effect applied TO you (an enemy's debuff) is named in Zira, panned toward whatever cast it. (Announcing the debuffs YOU apply to enemies is deferred -- naming each by its full skill per hit is too verbose; a terse effect lexicon is a later research project) |
| (automatic) | Kills, in Zira, panned toward where they died (coalesced over ~0.5 s): a single kill reads just its experience ("300 exp", or "0 exp" when it gave none); a pack reads "N killed", plus ", N exp" when it gave any. Fires even on 0-XP kills, since the game shows a death only by the body dropping. XP with no kill (quest turn-ins) is not announced here. Kill signal = `PlayStats::IncrementKills`; XP is polled from `GetExperiencePoints` (single-player has no XP event) and attributed only when it lands next to a kill |
| (automatic) | The game's banner strip (level up, "Enemy Hero Killed", quest updated, "You have died") and its red action-failed popups ("That skill is not ready", "Energy Too Low", "Invalid Target") are read through the screen reader, once each |
| Ctrl+<game key> | The game's own windows and functions (see above) |
| Q | The objective tracker: each tracked quest's open objectives ("Waking to Misery: Enter the Cave under Burial Hill, ...") |
| Y | The game's own Quickbar Switch (cycles which of the two skill bars the HUD shows); the mod announces the new bar ("quickbar 2"). Passes straight through -- it is the game's key, we only add the readout |
| Ctrl+1..0 | Read quickbar slot 1..10 of the displayed bar: the skill and how it aims -- "Cadence, at a target" / "War Cry, around you" / "Word of Pain, at a spot" / "Overguard, self" ("empty" for an empty slot). See docs/skills-targeting.md |
| Ctrl+- , Ctrl+= | Read the left / right mouse skill, same form ("left mouse Fire Strike, at a target") |
| F | Swap the active weapon set (the game's "Switch Weapons", which it leaves unbound); announces "weapon set N" and the two hands. Only the two hands change between sets; everything else is shared |
| Ctrl+` | The hotbar manager: the current weapon set's two number bars, then the left/right mouse buttons, then the read-only health/energy potion slots. Activate a bar slot or mouse button to open a picker (clear/default, then every learned skill you can assign); the mouse "default" restores the basic attack. Proc/auto item skills (e.g. Ice Spike) and passives are excluded. In any picker, Space reads the tooltip (the skill's text / the item's stats) |
| G | Pick up the nearest item on the ground (the game's own Pickup action: within 10 units, loot filter applied; auto-equips into an empty slot like the game does) |
| (automatic) | Place changes, in Zira: the region when it changes, the sub-region when it changes, then the room's title ("Devil's Crossing, the prison, cell block corridor"; "room 193" while a room has no title yet). See docs/rooms.md |
| X | The current room: title, then the authored description ("no description yet" until then), through the screen reader |
| F1 | Grimdark menu (anywhere): sound glossary (every mod sound as a tree; landing on a row plays it), in the world announcement config (T) and sonar config (Ctrl+T), and mod options: the dev server on/off (off by default; Enter flips it now and remembers it) |
| T | Announcement toggles overlay. Stop 1: outgoing announcements off / brief / full (brief = hit, crit, miss, blocked; Dodge reads as miss), incoming announcements on/off, incoming hit announcements on/off ("hit" per attack reaching you, from the victim-side resolver, so it works while invincible), telegraph cues off / your target / highest tier / all (Enter cycles, Left/Right step). Stop 2: swing / stomp / wave / shot / ring / area / charge cues on/off. Enter flips a row and speaks the new state, Escape closes. Persisted in `%LOCALAPPDATA%\\Grimdark\\settings.txt`. (The authoring note that used to live on T is the dev route `/note`.) |
| Ctrl+T | Sonar config overlay (`screens/cue_settings.cpp`, state in `src/cues.h`, same shape as T). Stop 1 "cues": on/off per positioned cue -- wall tones, harmful ground (lanes, bed and pointer together), enemy / loot / entrance / breakable / shrine / interactable pings (a sonar group that is off is not collected at all). Stop 2 "volumes": wall tones, harmful ground, enemy pings, other pings, the Mark voice, the Zira voice, 0..100 percent (Left/Right 5, Enter +5 wrapping to 0; each step previews the channel at the new level: wall tone ahead, sizzle ahead, the enemy ping, the loot ping at the sonar trim, "250 crit" in Mark, "health 70 percent" in Zira), multiplied onto the dev knobs (`/walltones?vol=`, `/hazard?vol=`, `/sonar?vol=`, `/voice?vol=`; the voice factor is applied on the voice worker at play time), so 100 = the tuned defaults; the percent is a dB scale (-60 dB at 0 = silence, 3 dB per step), so equal steps sound equal. Persisted as `cue.*` / `volume.*` keys in settings.txt. Escape closes; a game window covers and closes it |
| V / Shift+V | Next / previous exit of the current room: one more review group like . N B M -- destination title (or "room N"), "blocked" if the live mesh refuses the opening, distance, clock bearing, "i of n"; the landing pings the route, ; re-pings, the cursor parks on the opening |
| Shift+W A S D | The free cursor (2026-09-20, `world::free_cursor_step`, README "Advanced targeting"): moves the virtual cursor's WORLD POINT instead of the character; app.cpp's key filter swallows the Shift+WASD PRESS so the game never moves (the release always passes, else a W held before Shift leaves the exe's held byte set). The first press seeds the point from the current cursor (the locked entity's position, the lock point, or the player's feet), the point overrides the lock's projection each frame (`world::tick`) while the lock itself is kept, so the reviewed thing stays reviewed and J / I / `aim_along_line` aim at the point; any landing (`lock_target` / `lock_point`) or `unlock_target` drops it. Repeating (typematic). Silent. y = `WorldVec3::PutOnFloor` at the new xz. Dev: `/freecursor?key=w&n=3`, `?mode=toggle`, `?off=1` |
| Z | Cursor mode toggle, spoken "cursor mode default" / "cursor mode polar" (`src/core/cursor_step.h`, unit-tested; persisted as `cursor.polar`). Default = 1 unit along the screen axes (`world::screen_axes`, screen-up = world -z at the pinned yaw 0). Polar = the line from the player through the cursor: W / S 1 unit out / in (never past the player), A / D turn it 30 degrees counterclockwise / clockwise about the player at the same distance. The line's heading is remembered (`g_free_heading`): S down to the player then W goes back out the same way, never flipped; a turn on the player turns the remembered line; a fresh free cursor with no line starts screen-up |
| (automatic) | A locked entity that vanishes (grace expiry) or dies no longer unlocks: `world::tick` records its position every frame it is found (`g_lock_last_pos`) and converts the lock to `lock_point` there, so the cursor stays where the thing was last seen; during the grace the stale point is projected rather than the override being dropped (2026-09-20) |

## The main menu (2026-08-22)
Three Tab stops: the general buttons (Create, Multiplayer, Game Guide, Community, DLC, Credits, options, exit
game); the character list, present only when there is more than one character ("test1, level 3 Arcanist,
selected, 4 of 4"; Enter selects, and the game swaps the preview); then the character-specific row (Start,
difficulty, game mode, Delete). Tab into the list lands on the selected character.

## The in-world windows (2026-08-22, first pass)
Each game window is a screen of the mod while the game shows it (Ctrl+C/I inventory, Ctrl+N skills, Ctrl+Q
codex, Ctrl+J factions; NPC windows when an NPC opens them). Layout rule: a tab list across the top (Left/Right
moves AND opens the tab it lands on (2026-08-26; Enter is a no-op re-select); **Ctrl+Tab / Ctrl+Shift+Tab switch tabs from anywhere**), one vertical column below it
(Up/Down; Tab moves between the tab row and the column). Escape closes the window. Backslash on an item row = the
game's comparison: the equipped item in each slot the focused item fits, with its tooltip (2026-08-27). Everything is read from the
game's own objects, never from the screen.
| Window | Tabs | Rows and keys |
|---|---|---|
| Inventory (C or I) | Equipment, one per bag, Stats | Equipment: "slot, item" (an item carrying a component reads "item with component" -- the grid tile's badge, 2026-08-27; the tooltip names the component); **Enter opens a picker of everything across your bags that fits the slot** (weapons/off-hands go to the ACTIVE weapon set -- swap with F first to arm the other); its first entry, "empty", unequips. **Backspace** unequips directly. Space = the game's tooltip. On a **weapon slot** (Right/Left Hand), Ctrl+J / Ctrl+I also put the weapon's **basic attack** on the left / right mouse (Ctrl+1..0 to a quickbar slot) -- recover a basic attack after re-slotting the mouse. Bag: "item, x N" in reading order; Enter = the game's right-click (equip / drink / read), Space = tooltip. **A component/augment (records/items/materia) instead opens the attach picker on Enter: every item across your bags AND equipped that it fits (the game's own `Player::GetCompatibleItems`); picking one attaches + consumes it (`Character::UseItemOn`) -- no blacksmith needed. Space reads the target item's tooltip.** Stats: the character sheet; on Physique / Cunning / Spirit, Enter spends an attribute point. **Ctrl+Enter on a bag tab** makes it the secondary bag = the game's selected bag, where pickups overflow once bag 1 is full (PlayerInventoryCtrl::AddItem, read 2026-08-26: stack merge anywhere, then bag 1, then the selected bag only); its tab reads "bag 2, secondary". Browsing the tabs no longer changes the selected bag |
| Skills (N) | One per mastery slot, Constellations, Celestial Powers (once one is learned) | Without a class: the six masteries (Space = description, Enter chooses; "undo class selection" until the mastery takes a point). With a class: "skill points N", the mastery bar, then the skills in tier order ("Cadence, level 0 of 16, needs mastery 1"; a modifier reads "Discord, modifies Cadence, needs mastery 5"). Enter spends a point but respects the game's requirements -- it refuses with the reason ("needs mastery N", "requires Cadence" for a modifier whose base isn't learned, "no points"). **Refunding is only at a spirit guide:** talking to one opens this window in reclaim mode, when a hint row ("spirit guide -- Backspace to reclaim a skill point, N iron bits each") appears at the top and each spent skill shows "N iron bits to reclaim"; Backspace then reclaims one point, refused with the reason when the game would grey the icon: a base skill's last point while its modifiers hold points ("remove points from its modifiers first, Discord"), a skill's last point with a celestial power attached ("detach its celestial power first, Twin Fangs"), the mastery bar while a learned skill needs its current level ("Cadence needs mastery 1") or at its last point ("cannot reclaim points from the mastery"), and "not enough iron bits". Outside a guide Backspace does nothing. Space = the game's skill text. **Ctrl+1..0 put the focused skill on quickbar slot 1..10, Ctrl+J on the left mouse button, Ctrl+I on the right**. Spending goes through the pane's own icon click, so the game's **"undo points"** row (under the mastery bar while anything is pending) reverts every point spent since the window opened. **Constellations** (2026-08-27, docs/devotion.md): "devotion points N available, M of 50", "affinities Chaos 2, Eldritch 4", then three Tab stops -- **learned** (a star taken, complete ones included), **available** (open to take), **unavailable** (locked behind an affinity) -- each an alphabetical list of tree groups named with their progress, "Bat (2/5), celestial power Twin Fangs, gives Chaos 2, Eldritch 3" / "Raven (0/5), needs Eldritch 1, gives Eldritch 5", "empty" when a stop has none (2026-09-18); Space = the constellation's description, requirement ("requires Eldritch 1, have 4") and bonus. Right expands the stars breadth-first from the root: "star 1, available", "star 2, needs star 1", "Twin Fangs, celestial power, needs star 4"; Enter spends a devotion point (reasons: "needs star N", "needs Chaos 4", "no points"; "constellation complete" when the bonus lands), Space = the game's star tooltip. Taking a celestial power opens the host picker at once; Enter on a learned power reopens it. **Celestial Powers**: "Twin Fangs, level 1 of 20, attached to Cadence, from Bat"; Enter = the picker of the skills it can trigger from (the game's own filter: your learned active class and item skills; a skill already carrying a power reads "has X" and picking it replaces), "none" detaches; Space = tooltip. **At a spirit guide** (the window in reclaim mode) a hint row appears ("spirit guide, Backspace to reclaim a devotion point, 25 iron bits and 1 aether crystals each"), each learned star shows its cost, and Backspace reclaims it -- refused with the game's own reasons: "needed by star 3" (a learned star hangs off it), "would lock Raven" / "Devotion Point Cannot be Removed" (losing the completion bonus would drop a constellation below its affinity requirement), "not enough iron bits", "not enough aether crystals"; a reclaim that breaks a complete constellation says "constellation complete lost" and takes its affinity back. Verified live 2026-08-27 |
| Codex (Q window) | Quests, Completed quests, Lore | Quests are groups (Right expands): "name, act, tracked"; Enter toggles tracking; inside: tasks, objectives ("done" when met), rewards. Lore notes: Enter or Space reads the note. How notes work (measured 2026-08-23): picking one up puts it IN THE BAG (with a 2-line toast); USING it from the bag (the bag row's Enter) opens the game's reader and registers it here -- only then is it in this list |
| Factions (J) | - | "faction, standing, progress of tier" |
| Illusionist (NPC) | Equipment, appearances, cost / Apply (Tab stops) | Up/Down browses; Enter selects equipment or previews an appearance. All native appearance pages are flattened into the list. Space/Ctrl+Space reads item details. Apply opens a Cancel-first confirmation with the total cost and staged items; Escape closes without committing previews. |
| Vendor (NPC) | Buy tabs per stock type, Sell. A **faction vendor**: one tab per reputation tier (Friendly / Respected / Honored / Revered) + Buyback, a locked tier says "locked, requires Respected", and a line gives the faction and your standing (built 2026-09-04, not yet verified live) | Buy rows carry the game's price line; Enter buys. Sell = your bag; Enter sells the whole item (a stack entire). **Ctrl+Enter on a stack sells part of it**: "sell how many of N", type the number, Enter (Escape cancels; out of range says "enter 1 to N"). Verified live 2026-08-26 |
| Caravan / stash (NPC) | your stash, shared stash, manage stash | "your stash" and "shared stash" each have two Tab stops: your items (Enter puts one into the first sack with room, "no room in the stash" otherwise) and what that stash holds across all its sacks, each row naming its sack (Enter takes it back, "no room in your bags" otherwise). "manage stash" shows "your stash, 2 of 5 tabs" / "buy tab 3, 50000 iron bits" rows; Enter buys at the game's price (SubtractMoney + AddSack, then the window's own tab-list rebuild). Whole stacks only, like the game's drag. Built 2026-09-04, not yet verified live |
| Quest reward, Shrine | - | Quest reward (opens over the dialog when a quest or step completes): title, quest name, XP line, one row per reward (items, iron bits, reputation, points), Close. Shrine: the window's text lines and buttons (Offer / Close) |
| Riftgate travel (use a rift under N, or Ctrl+L once the personal riftgate is yours) | Tab stops: all / act 1..4 | "all" = your personal rift, then Devil's Crossing, then every discovered gate nearest first ("Devil's Crossing Rift, 359 away, 5 o'clock", "you are here" on the one you stand at); one stop per act in the game's master-table order (src/riftgate_zones.h, generated). Enter on a gate travels (the game fades and closes its map), Escape closes. Reworked 2026-09-04, not yet verified live |

A landing speaks "name, [distant,] distance away, clock bearing, i of n" -- "distant" when the camera does
not show it, so it cannot be clicked right now -- and parks the game's cursor on the thing while it is on
screen, so the game hovers and targets it itself. (Decided 2026-08-22, deviating from wotr: the player is
always embodied here, perception and interaction are what the camera shows; the camera is the player's.) The review cursor remembers an object ID, never a pointer: each
step rebuilds the nearest-first list live and continues from that ID, or enters at the nearest if it is gone.

## Which plain keys are whose in the world
Passed straight to the game (src/screens/in_game.cpp `passes_key`): WASD, 1-0, Y, Space, E, R, U, Escape,
Alt/Right Alt (held: show items), F2-F7. Every other game function is reachable only as Ctrl + its default
key (the `game.*` lifts in src/app.cpp: C/I N Q M O K G H J V L B X Z P, Backspace, \, ], Enter, Tab, `,` `.`).
The mod's plain keys: `.` `,` N B M V (review groups; Shift = back, Alt = nearest), `;` `'` `/` `\`, J I G F,
K H Q X T Z, Shift+W A S D (the free cursor: the press is swallowed before the game, the release passes), and
Ctrl+1..0, Ctrl+- Ctrl+=, Ctrl+`, Ctrl+T, Ctrl+Shift+P. Still free: F8, F9, Insert/Delete/Home/End/PgUp/PgDn,
the arrow keys, numpad. Ctrl+letter chords arrive with flags and are unused by the game.
