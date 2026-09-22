#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "message_builder.h"

// The single home for mod-authored spoken strings and, more importantly, for the spoken CONVENTIONS:
// role words, how a position in a list reads, what "nothing here" sounds like. Game-provided text
// (item names, tooltips, menu labels) passes through verbatim; this is only the language we add
// around it. English only, by decision (2026-08-21). No inline literals in speech calls elsewhere.
namespace gd::strings {

// ---- roles (spoken after a control's label) ----
inline constexpr std::string_view kButton = "button";
inline constexpr std::string_view kToggle = "toggle";
inline constexpr std::string_view kSlider = "slider";
inline constexpr std::string_view kRadio = "radio button";
inline constexpr std::string_view kComboBox = "combo box";
inline constexpr std::string_view kTab = "tab";
inline constexpr std::string_view kTextField = "edit";
inline constexpr std::string_view kList = "list";
inline constexpr std::string_view kHeading = "heading";

// ---- states ----
inline constexpr std::string_view kSelected = "selected";
inline constexpr std::string_view kDisabled = "disabled";
inline constexpr std::string_view kHit = "hit";       // incoming hit announcements (Zira, per attack that reaches you) and brief outgoing
inline constexpr std::string_view kYouDied = "You died";
inline constexpr std::string_view kMiss = "miss";     // brief outgoing: the game's Miss and Dodge
inline constexpr std::string_view kBlockedHit = "blocked";   // brief outgoing: the game's Block
inline constexpr std::string_view kOutgoingModes[3] = {"off", "brief", "full"};
inline constexpr std::string_view kOn = "on";
inline constexpr std::string_view kOff = "off";
inline constexpr std::string_view kNotSelected = "not selected";
inline constexpr std::string_view kEmpty = "empty";
inline constexpr std::string_view kEditing = "editing";  // a text field took the keyboard: type, Enter or Escape when done
inline constexpr std::string_view kNameNotTaken = "the game did not take the name";  // checkpoint after editing failed
inline constexpr std::string_view kSpace = "space";      // the typed-character echo for a space
inline constexpr std::string_view kExpanded = "expanded";
inline constexpr std::string_view kCollapsed = "collapsed";

// ---- feedback ----
inline constexpr std::string_view kNoTooltip = "no tooltip";
inline constexpr std::string_view kNothingToCompare = "nothing to compare";
inline constexpr std::string_view kNoDetails = "no details";
inline constexpr std::string_view kNoMatch = "no match for";
inline constexpr std::string_view kSearchCleared = "search cleared";
inline constexpr std::string_view kNothingThere = "nothing there";
inline constexpr std::string_view kNoTextOnScreen = "no text on screen";
inline constexpr std::string_view kUnsupportedScreen = "unsupported screen";
inline constexpr std::string_view kModName = "Grimdark";
inline constexpr std::string_view kModLoaded = "Grimdark loaded";
inline constexpr std::string_view kModLoadedNoSpeech = "Grimdark loaded, no speech backend";

// ---- composed shapes: push_* helpers so call sites never concatenate ----
// "3 of 7"
gd::core::MessageBuilder& push_position(gd::core::MessageBuilder& m, int index1, int count);
// "<label>, <role>[, selected][, disabled]" as list items
gd::core::MessageBuilder& push_control(gd::core::MessageBuilder& m, std::string_view label, std::string_view role, bool selected, bool disabled);
// "<n> items" / "1 item"
gd::core::MessageBuilder& push_count(gd::core::MessageBuilder& m, int count, std::string_view singular, std::string_view plural);
// "x 59, z 97, life 250 of 250, region <name>" -- the in-game "where am I" readout.
gd::core::MessageBuilder& push_where(gd::core::MessageBuilder& m, float x, float z, std::string_view region, double life, float life_max);

// ---- in-game ----
// "Hangman Jarvis, 5 away, 2 o'clock, 1 of 3" -- a review-cursor landing (game label verbatim).
gd::core::MessageBuilder& push_scan_item(gd::core::MessageBuilder& m, std::string_view label, float distance, int clock_hour, int index1, int count, bool distant, std::string_view note = {});
// "level 3 Arcanist, hardcore" -- a main-menu character row's value (class empty = no mastery yet).
// "Hellhound down" / "Hellhound summoned" -- a pet event line (game label verbatim + our word).
gd::core::MessageBuilder& push_pet_event(gd::core::MessageBuilder& m, std::string_view label, std::string_view event);
gd::core::MessageBuilder& push_character_summary(gd::core::MessageBuilder& m, unsigned level, std::string_view class_name, bool hardcore);
// The MonsterClassification rarity word (0 Common -> "", 1 champion, 2 hero, 3 boss, 4 quest, 5 super boss).
std::string_view classification_word(int classification);
// "walking undead level 5 hero" -- an enemy review label (the game name, its level, its rarity word if any).
gd::core::MessageBuilder& push_enemy_label(gd::core::MessageBuilder& m, std::string_view name, int level, int classification);
// "100 percent health, frozen, stunned" -- the / inspect readout of the current target (no name repeat).
gd::core::MessageBuilder& push_target_inspect(gd::core::MessageBuilder& m, int health_percent, const std::vector<std::string>& effects);
// "5 away, 2 o'clock" -- the distance and bearing part alone (a riftgate row's value).
gd::core::MessageBuilder& push_distance_bearing(gd::core::MessageBuilder& m, float distance, int clock_hour);
// "no enemies nearby"
gd::core::MessageBuilder& push_nothing_nearby(gd::core::MessageBuilder& m, std::string_view group_plural);
gd::core::MessageBuilder& push_cursor_mode(gd::core::MessageBuilder& m, bool polar);   // "cursor mode polar" / "cursor mode default"
// "<speaker>: <speech>" -- a conversation node (speaker may be empty).
gd::core::MessageBuilder& push_speech(gd::core::MessageBuilder& m, std::string_view speaker, std::string_view speech);
inline constexpr std::string_view kEnemies = "enemies";
inline constexpr std::string_view kUnknown = "unknown";
inline constexpr std::string_view kSecondaryBag = "secondary";   // the game's selected bag: where pickups overflow to once bag 1 is full (bag 1 is always first)
inline constexpr std::string_view kNotABag = "not a bag";
inline constexpr std::string_view kOffering = "offering";   // a shrine's required item row: "offering 1, Aether Crystal"
inline constexpr std::string_view kModMenu = "Grimdark menu";              // F1 anywhere
inline constexpr std::string_view kSoundGlossary = "sound glossary";         // every WAV the mod plays, landing plays it
inline constexpr std::string_view kUnsupportedBuild = "unsupported game build";   // the version gate's one line
inline constexpr std::string_view kExeTimestamp = "exe timestamp";
inline constexpr std::string_view kSupported = "supported";
inline constexpr std::string_view kModOff = "the mod is off";
// "Grimdark unsupported game build, exe timestamp 6a85fbec, supported 1.3.0.8 Steam, the mod is off"
gd::core::MessageBuilder& push_unsupported_build(gd::core::MessageBuilder& m, std::string_view exe_ts, const std::vector<std::string_view>& supported);
inline constexpr std::string_view kModOptions = "mod options";            // the F1 menu's switches screen
inline constexpr std::string_view kDevServer = "dev server";              // its loopback HTTP server toggle (off by default)
inline constexpr std::string_view kGlossaryWallTones = "wall tones";
inline constexpr std::string_view kGlossaryWallAhead = "wall ahead";
inline constexpr std::string_view kGlossaryWallRight = "wall right";
inline constexpr std::string_view kGlossaryWallBehind = "wall behind";
inline constexpr std::string_view kGlossaryWallLeft = "wall left";
inline constexpr std::string_view kGlossarySonar = "sonar";
inline constexpr std::string_view kGlossaryEnemy = "enemy";
inline constexpr std::string_view kGlossaryLoot = "loot";
inline constexpr std::string_view kGlossaryEntrance = "dungeon entrance";
inline constexpr std::string_view kGlossaryDestructible = "breakable";
inline constexpr std::string_view kGlossaryShrine = "shrine";
inline constexpr std::string_view kGlossaryInteractable = "interactable";
inline constexpr std::string_view kGlossaryReviewPings = "review pings";
inline constexpr std::string_view kGlossaryPingStraight = "straight walk";
inline constexpr std::string_view kGlossaryPingPath = "path around";
inline constexpr std::string_view kGlossaryPingUnreachable = "unreachable";
inline constexpr std::string_view kGlossaryHazards = "harmful ground";
inline constexpr std::string_view kGlossaryHazardAhead = "harmful ground ahead";
inline constexpr std::string_view kGlossaryHazardRight = "harmful ground right";
inline constexpr std::string_view kGlossaryHazardBehind = "harmful ground behind";
inline constexpr std::string_view kGlossaryHazardLeft = "harmful ground left";
inline constexpr std::string_view kGlossaryHazardInside = "standing in harmful ground";
inline constexpr std::string_view kGlossaryHazardExit = "way out: one pulse per exit in turn, higher is north, panned east or west";
inline constexpr std::string_view kGlossaryTelegraphs = "telegraph cues";
inline constexpr std::string_view kGlossaryTelegraphSwing = "swing: a melee attack from next to you, get out of reach";
inline constexpr std::string_view kGlossaryTelegraphStomp = "stomp: a blast all around the attacker, step away from it";
inline constexpr std::string_view kGlossaryTelegraphWave = "wave: a strip travelling the way the attacker faces, step sideways";
inline constexpr std::string_view kGlossaryTelegraphShot = "shot: a projectile or beam aimed at you, sidestep";
inline constexpr std::string_view kGlossaryTelegraphRing = "ring: projectiles in every direction, run outward";
inline constexpr std::string_view kAnnouncements = "announcement config";    // the T overlay (and its F1 row)
inline constexpr std::string_view kAnnounceOutgoing = "outgoing announcements";   // your hits, kills, XP (Mark)
inline constexpr std::string_view kAnnounceIncoming = "incoming announcements";   // your health steps, effects on you (Zira)
inline constexpr std::string_view kAnnounceIncomingHits = "incoming hit announcements";   // "hit" per attack that reaches you
inline constexpr std::string_view kAnnounceTelegraph = "telegraph cues";          // off / your target / highest tier / all
inline constexpr std::string_view kAnnounceSwitches = "announcement settings";   // the T overlay's two Tab stops (context labels)
inline constexpr std::string_view kAnnounceShapes = "telegraph filter";
inline constexpr std::string_view kTelegraphShapeLabels[7] = {"swing cues", "stomp cues", "wave cues", "shot cues", "ring cues", "area cues", "charge cues"};   // the T overlay's second stop (telegraph::kShapeNames order)
// ---- the Ctrl+T overlay (screens/cue_settings.cpp, src/cues.h): which positioned cues play, channel volumes ----
inline constexpr std::string_view kCueSettings = "sonar config";   // the Ctrl+T overlay (and its F1 row)
inline constexpr std::string_view kCueSwitches = "cues";            // stop 1: one on/off row per cue
inline constexpr std::string_view kCueVolumes = "volumes";          // stop 2: percent per channel
inline constexpr std::string_view kCueWallTones = "wall tones";
inline constexpr std::string_view kCueHarmfulGround = "harmful ground";   // the lanes, the bed and the way-out pointer together
inline constexpr std::string_view kCueEnemies = "enemy pings";
inline constexpr std::string_view kCueLoot = "loot pings";
inline constexpr std::string_view kCueEntrances = "entrance pings";
inline constexpr std::string_view kCueBreakables = "breakable pings";
inline constexpr std::string_view kCueShrines = "shrine pings";
inline constexpr std::string_view kCueInteractables = "interactable pings";
inline constexpr std::string_view kVolumeWalls = "wall tone volume";
inline constexpr std::string_view kVolumeHazards = "harmful ground volume";
inline constexpr std::string_view kVolumeEnemies = "enemy ping volume";
inline constexpr std::string_view kVolumeOther = "other ping volume";   // loot, entrances, breakables, shrines, interactables
inline constexpr std::string_view kVolumeMark = "enemy voice volume, Mark";   // combat lines spoken at the enemy
inline constexpr std::string_view kVolumeZira = "your voice volume, Zira";    // your health, effects, places
inline constexpr std::string_view kVoicePreviewMark = "250 crit";           // the Mark slider's preview line (Zira's is push_health_percent 70)
inline constexpr std::string_view kPets = "pets";                 // the [ / ] review group and the pet overlay
inline constexpr std::string_view kStanceNormal = "normal";       // Monster::ControllerType 0 / 1 / 2 (docs/pets.md)
inline constexpr std::string_view kStanceAggressive = "aggressive";
inline constexpr std::string_view kStanceDefensive = "defensive";
inline constexpr std::string_view kPetDown = "down";              // "<pet> down" (Zira) when a pet leaves the list
inline constexpr std::string_view kPetSummoned = "summoned";      // "<pet> summoned"
inline constexpr std::string_view kDeselected = "deselected";
inline constexpr std::string_view kAllPets = "all pets";
inline constexpr std::string_view kNoPets = "no pets";
inline constexpr std::string_view kDisbanded = "disbanded";
inline constexpr std::string_view kPetsAttack = "attack locked target";   // overlay command rows
inline constexpr std::string_view kPetsRecall = "recall";
inline constexpr std::string_view kSelectedPets = "selected pets";
inline constexpr std::string_view kNeutrals = "people and objects";
inline constexpr std::string_view kCharacters = "characters";       // the C review group: player characters (you, party members)
inline constexpr std::string_view kYou = "you";                     // its note on your own character
// "claude, you, 1 of 1" -- the landing on your own character (no distance or bearing to yourself).
gd::core::MessageBuilder& push_scan_self(gd::core::MessageBuilder& m, std::string_view label, int index1, int count);
inline constexpr std::string_view kBystanders = "bystanders";
inline constexpr std::string_view kLoot = "loot";
inline constexpr std::string_view kLootFilter = "loot filter";
inline constexpr std::string_view kCrafting = "crafting";        // the blacksmith's window
inline constexpr std::string_view kCanMake = "can make";         // "can make 2"
inline constexpr std::string_view kCrafted = "crafted";
inline constexpr std::string_view kMissing = "missing";          // Enter on "can make 0": "missing 2 Aether Crystal, 2,595 iron bits"
inline constexpr std::string_view kNoRecipe = "no recipe";
inline constexpr std::string_view kInventor = "inventor";        // the Inventor's salvage / dismantle window (docs/inventor.md)
inline constexpr std::string_view kChamber = "chamber";          // the window's item slot: "chamber, Scrapmetal Razor with component" / "chamber, empty"
inline constexpr std::string_view kResults = "results";          // the dismantle tab's two output slots
inline constexpr std::string_view kTake = "take";                // Enter on a result slot: back into the bag
inline constexpr std::string_view kNothingFits = "nothing fits"; // the chamber picker when no bag item qualifies
inline constexpr std::string_view kNotLearned = "not learned";   // the Dismantle tab before the Inventor knows the trade
inline constexpr std::string_view kDynamite = "dynamite";
inline constexpr std::string_view kWithAugment = "with augment";
inline constexpr std::string_view kSalvageCost = "salvage cost";     // "salvage cost 120 iron bits"
inline constexpr std::string_view kDismantleCost = "dismantle cost";
inline constexpr std::string_view kTooExpensive = "too expensive";
inline constexpr std::string_view kNoDynamite = "no dynamite";
inline constexpr std::string_view kSalvaged = "salvaged";            // "salvaged, Serrated Spike" (what you kept)
inline constexpr std::string_view kAugmentRemoved = "augment removed";
inline constexpr std::string_view kDismantled = "dismantled";        // "dismantled, Scrap Metal, Vicious Jawbone"
inline constexpr std::string_view kCancelled = "cancelled";
inline constexpr std::string_view kEquipped = "equipped";        // a picker row for an item you are wearing (the component attach list)
inline constexpr std::string_view kShowingAllItems = "showing all items";   // O: the review groups and the sonar ignore the loot filter (and the game shows every label)
inline constexpr std::string_view kLootFilterOn = "loot filter on";
inline constexpr std::string_view kSetToDefaults = "set to defaults";       // the last row of each loot filter column
inline constexpr std::string_view kDefaults = "defaults";
inline constexpr std::string_view kSonarOn = "sonar on";
inline constexpr std::string_view kHardcore = "hardcore";
inline constexpr std::string_view kNoted = "noted";
inline constexpr std::string_view kNoteUseHint = "Enter reads it into the codex";
inline constexpr std::string_view kNoteFailed = "could not write the note";
inline constexpr std::string_view kSonarOff = "sonar off";
inline constexpr std::string_view kTransitions = "dungeon entrances";
inline constexpr std::string_view kEntrance = "entrance";                  // an unnamed DungeonEntrance in the N group (a one-way exit shaft has no name)
inline constexpr std::string_view kNoTarget = "no target";
// the free cursor (Shift+WASD, Z): the only thing it speaks is the mode
inline constexpr std::string_view kCursorMode = "cursor mode";
inline constexpr std::string_view kCursorGrid = "default";
inline constexpr std::string_view kCursorPolar = "polar";
inline constexpr std::string_view kInGame = "in game";
inline constexpr std::string_view kMessage = "message";  // the game's generic message box
inline constexpr std::string_view kPauseMenu = "pause menu";
inline constexpr std::string_view kLoading = "loading";
inline constexpr std::string_view kTip = "tip";      // a tutorial tip, shown in our own overlay
inline constexpr std::string_view kConversation = "conversation";
inline constexpr std::string_view kClose = "close";
inline constexpr std::string_view kNotInWorld = "not in the world";
// rooms (docs/rooms.md): the place announcement, the X description, the V exit cycle
inline constexpr std::string_view kNoRoom = "no room data here";
inline constexpr std::string_view kNoDescription = "no description yet";
inline constexpr std::string_view kExits = "exits";               // the review group's plural ("no exits nearby")
inline constexpr std::string_view kBlocked = "blocked";            // the exit item's note (push_scan_item)
inline constexpr std::string_view kRoom = "room";                  // untitled room: "room 12"
// "Devil's Crossing, the prison, cell block corridor" -- only the parts that changed, in that order
gd::core::MessageBuilder& push_place(gd::core::MessageBuilder& m, std::string_view region, std::string_view subregion, std::string_view room);
inline constexpr std::string_view kOptions = "options";       // the main menu's unlabeled icon buttons
inline constexpr std::string_view kExitGame = "exit game";
inline constexpr std::string_view kYes = "yes";               // the game's message-box answers (answered through its DialogManager)
inline constexpr std::string_view kNo = "no";
inline constexpr std::string_view kOkay = "okay";
inline constexpr std::string_view kConfirmation = "confirmation";  // Delete Character's type-DELETE box
inline constexpr std::string_view kOptionsScreen = "Options";
inline constexpr std::string_view kBehindInterface = "behind the interface";   // J/I on a target whose screen point lies on the HUD (the click would hit a hotslot)
inline constexpr std::string_view kTooFarAway = "too far away";  // a click on a reviewed thing the camera does not show
inline constexpr std::string_view kDistant = "distant";          // the same thing while cycling through the review list
inline constexpr std::string_view kPercent = "percent";
inline std::string percent(int value) { return std::to_string(value) + " " + std::string(kPercent); }   // "70 percent"
inline constexpr std::string_view kKeyBindings = "key bindings";
// ---- combat (spoken through the positional voices: Mark at the enemy, Zira for the player) ----
inline constexpr std::string_view kHealth = "health";
inline constexpr std::string_view kEnergy = "energy";
inline constexpr std::string_view kCrit = "crit";
inline constexpr std::string_view kVoiceUnavailable = "combat speech is unavailable; using the screen reader";
// "health 250 of 250, energy 100 of 100" (the H key)
gd::core::MessageBuilder& push_vitals(gd::core::MessageBuilder& m, double life, float life_max, float energy, float energy_max);
// "health 70 percent" -- a 10 % step crossed
gd::core::MessageBuilder& push_health_percent(gd::core::MessageBuilder& m, int percent);
// "456" / "456 crit" -- the number the game drew over the enemy
gd::core::MessageBuilder& push_combat_hit(gd::core::MessageBuilder& m, std::string_view number, bool crit);
// "Miss" / "Dodge" / "Block" -- game text, verbatim
gd::core::MessageBuilder& push_combat_word(gd::core::MessageBuilder& m, std::string_view word);
inline constexpr std::string_view kExp = "exp";       // deliberately terse (the game shows XP only as a filling bar)
inline constexpr std::string_view kKilled = "killed";
// Kill feedback, coalesced per window (the game shows an enemy dying only graphically), spoken in Zira. A single
// kill is just the XP ("300 exp", "0 exp" when none). A pack is "N killed", plus ", M exp" when it yielded XP.
gd::core::MessageBuilder& push_kills(gd::core::MessageBuilder& m, int count, uint64_t xp);
// ---- the in-world windows (src/screens/codex.cpp, factions.cpp, inventory.cpp, skills.cpp, quickbar) ----
inline constexpr std::string_view kCodex = "codex";
inline constexpr std::string_view kQuests = "quests";
inline constexpr std::string_view kCompletedQuests = "completed quests";
inline constexpr std::string_view kLore = "lore";
inline constexpr std::string_view kTracked = "tracked";
inline constexpr std::string_view kNotTracked = "not tracked";
inline constexpr std::string_view kDone = "done";
inline constexpr std::string_view kNoQuests = "no quests";
inline constexpr std::string_view kNoObjectives = "no objectives";
inline constexpr std::string_view kObjectives = "objectives";
inline constexpr std::string_view kReward = "reward";
inline constexpr std::string_view kFactions = "factions";
inline constexpr std::string_view kRiftgates = "riftgate travel";
inline constexpr std::string_view kNoRiftgates = "no riftgates discovered";
inline constexpr std::string_view kYouAreHere = "you are here";
inline constexpr std::string_view kAllRiftgates = "all";               // the riftgate screen's first Tab stop: every discovered gate, nearest first
inline constexpr std::string_view kAct = "act";                         // "act 2": the per-act Tab stops (the game names acts only in achievements)
inline constexpr std::string_view kShrinesStop = "shrines";             // the riftgate screen's world list of discovered devotion shrines
inline constexpr std::string_view kNoShrines = "no shrines discovered";
inline constexpr std::string_view kDesecratedShrine = "desecrated shrine";   // the map icon's corruptedShrine flag
inline constexpr std::string_view kRuinedShrine = "ruined shrine";           // the map icon's ruinedShrine flag
inline constexpr std::string_view kRestored = "restored";                    // Player::GetShrineUIDs holds it
inline constexpr std::string_view kNotRestored = "not restored";
// The map-marker picker (Ctrl+M) and the follow key (').
inline constexpr std::string_view kMapMarkers = "map";
inline constexpr std::string_view kQuestMarkers = "quest markers";
inline constexpr std::string_view kMapPoints = "points of interest";
inline constexpr std::string_view kNoMarkersHere = "nothing on the map here";
// Map icon kinds the game has no rollover text for (the others speak the game's tagMapSymbol* words; docs/map-icons.md)
inline constexpr std::string_view kObstacle = "obstacle";           // a barricade / rubble wall drawn with a dynamic-obstacle symbol
inline constexpr std::string_view kHeroMonster = "hero monster";
inline constexpr std::string_view kBossMonster = "boss";
inline constexpr std::string_view kYourGrave = "your grave";
inline constexpr std::string_view kIllusionist = "illusionist";
inline constexpr std::string_view kMapMarker = "marker";            // an icon kind not yet seen (its type number is logged)
inline constexpr std::string_view kNoQuestMarkers = "no quest markers";
inline constexpr std::string_view kFollowing = "following";
inline constexpr std::string_view kNotFollowing = "not following anything";
inline constexpr std::string_view kNoFactions = "no factions known";
inline constexpr std::string_view kInventory = "inventory";
inline constexpr std::string_view kEquipment = "equipment";
inline constexpr std::string_view kBag = "bag";
inline constexpr std::string_view kStats = "stats";
inline constexpr std::string_view kEmptySlot = "empty";
inline constexpr std::string_view kWithComponent = "with component";   // the bag tile's component badge, on the row label
inline constexpr std::string_view kNothingEquipped = "nothing equipped";   // Backslash on an item: the slot it fits holds nothing
inline constexpr std::string_view kNotEquipment = "not equipment";         // Backslash on a potion / component / note
inline constexpr std::string_view kNotUsable = "not usable";               // Enter on a bag item that is neither equipment nor a consumable/note (crafting materials, quest items)
inline constexpr std::string_view kYourStanding = "you are";               // "you are Friendly"
inline constexpr std::string_view kIronBits = "iron bits";
inline constexpr std::string_view kSkills = "skills";
inline constexpr std::string_view kSkillPointsLeft = "skill points";
inline constexpr std::string_view kLocked = "locked";
inline constexpr std::string_view kMastery = "mastery";
inline constexpr std::string_view kQuickbar = "quickbar";
inline constexpr std::string_view kInterfaceShown = "interface shown";    // the game's Toggle UI key (]) flipped, quickbar_tick
inline constexpr std::string_view kInterfaceHidden = "interface hidden";
inline constexpr std::string_view kHotbar = "hotbar";        // the hotbar manager screen
inline constexpr std::string_view kBar = "bar";
inline constexpr std::string_view kClear = "clear";          // the picker's clear-this-slot entry
inline constexpr std::string_view kCleared = "cleared";
inline constexpr std::string_view kDefault = "default";      // the mouse picker's reset-to-basic-attack entry
inline constexpr std::string_view kHealthPotion = "health potion";
inline constexpr std::string_view kEnergyPotion = "energy potion";
inline constexpr std::string_view kAssignSkill = "assign skill";  // hotbar-manager picker title
inline constexpr std::string_view kWeaponSet = "weapon set";
inline constexpr std::string_view kSlot = "slot";
inline constexpr std::string_view kLeftMouse = "left mouse";
inline constexpr std::string_view kRightMouse = "right mouse";
// How a slotted skill aims (appended to the slot readout; docs/skills-targeting.md).
inline constexpr std::string_view kAimSelf = "self";
inline constexpr std::string_view kAimAround = "around you";
inline constexpr std::string_view kAimPoint = "at a spot";
inline constexpr std::string_view kAimTarget = "at a target";
inline constexpr std::string_view kAssigned = "assigned";
inline constexpr std::string_view kBasicAttack = "basic attack";   // the weapon's default attack (equipment tab -> mouse)
inline constexpr std::string_view kNothingToAssign = "nothing to assign";
inline constexpr std::string_view kCannot = "can't";
inline constexpr std::string_view kClass = "class";
inline constexpr std::string_view kLevel = "level";
inline constexpr std::string_view kExperience = "experience";
inline constexpr std::string_view kAttributePoints = "attribute points";
inline constexpr std::string_view kSkillPoints = "skill points";
inline constexpr std::string_view kDevotionPoints = "devotion points";
inline constexpr std::string_view kOffensiveAbility = "offensive ability";
inline constexpr std::string_view kDefensiveAbility = "defensive ability";
inline constexpr std::string_view kDps = "damage per second";
inline constexpr std::string_view kRequirementsNotMet = "requirements not met";
inline constexpr std::string_view kInactive = "inactive";   // an equipped item the game has detached (requirements no longer met), on the slot's name
inline constexpr std::string_view kAttach = "attach";
inline constexpr std::string_view kComponent = "component";
inline constexpr std::string_view kNoCompatibleItems = "no compatible items";
inline constexpr std::string_view kNoClass = "no class";
inline constexpr std::string_view kModifier = "modifier";
inline constexpr std::string_view kRequiresMastery = "needs mastery";
inline constexpr std::string_view kNothingToPickUp = "nothing to pick up";
inline constexpr std::string_view kQuestReward = "quest reward";
inline constexpr std::string_view kRewards = "rewards";              // the quest reward window's reward rows' stop
inline constexpr std::string_view kSkillPoint = "skill point";        // reward rows: "1 skill point" / "2 skill points"
inline constexpr std::string_view kAttributePoint = "attribute point";
inline constexpr std::string_view kDevotionPoint = "devotion point";
inline constexpr std::string_view kLevels = "levels";
inline constexpr std::string_view kTribute = "tribute";
inline constexpr std::string_view kReputation = "reputation";          // "300 Devil's Crossing reputation"
inline constexpr std::string_view kUnknownItem = "item";               // a reward item whose name could not be read
inline constexpr std::string_view kAccept = "accept";
inline constexpr std::string_view kShrine = "shrine";
inline constexpr std::string_view kOffer = "offer";
inline constexpr std::string_view kVendor = "vendor";
inline constexpr std::string_view kBuy = "buy";
inline constexpr std::string_view kSell = "sell";
inline constexpr std::string_view kBought = "bought";
inline constexpr std::string_view kSold = "sold";
inline constexpr std::string_view kSellHowMany = "sell how many of";   // "sell how many of 12" -- the partial-sell count prompt
inline constexpr std::string_view kNotAStack = "not a stack";
inline constexpr std::string_view kStash = "stash";
inline constexpr std::string_view kTransfer = "transfer";
// The caravan (stash) screen, 2026-09-04: tabs "your stash" / "shared stash" (each: your items, then the stash), "manage stash" (buy tabs).
inline constexpr std::string_view kYourStash = "your stash";
inline constexpr std::string_view kOf = "of";                          // "2 of 5 tabs"
inline constexpr std::string_view kSharedStash = "shared stash";
inline constexpr std::string_view kManageStash = "manage stash";
inline constexpr std::string_view kYourItems = "your items";            // the first Tab stop of a stash tab: what you carry
inline constexpr std::string_view kInTheStash = "in the stash";        // the second: what the stash holds
inline constexpr std::string_view kTabsOwned = "tabs";                 // "your stash, 2 of 5 tabs"
inline constexpr std::string_view kBuyTab = "buy tab";                 // "buy tab 3, 50000 iron bits"
inline constexpr std::string_view kAllTabsBought = "all tabs bought";
inline constexpr std::string_view kStashFull = "no room in the stash";
inline constexpr std::string_view kBagFull = "no room in your bags";
inline constexpr std::string_view kMoved = "moved";
inline constexpr std::string_view kDetailsHint = "Ctrl+Space for details";   // appended to a short tooltip whose detailed form says more
inline constexpr std::string_view kSelectClass = "select class";
inline constexpr std::string_view kUndoClassSelection = "undo class selection";
inline constexpr std::string_view kClassChosen = "chosen; spend a point on the mastery to make it permanent";
inline constexpr std::string_view kSecondClassAt = "second class available at level";
inline constexpr std::string_view kPointSpent = "point spent";
inline constexpr std::string_view kNoPoints = "no points";
inline constexpr std::string_view kAtMaximum = "at maximum";
inline constexpr std::string_view kRequires = "requires";        // "requires <base skill>" for a modifier's prerequisite
inline constexpr std::string_view kModifies = "modifies";        // "modifies <base skill>" for a modifier skill
// Spirit-guide reclamation (the skills window opened in reclaim mode): a non-interactive hint row at the top of
// the skill list, and each skill's reclaim cost. The game's own word is "reclaim".
inline constexpr std::string_view kSpiritGuide = "spirit guide";
inline constexpr std::string_view kReclaimHint = "Backspace to reclaim a skill point";
inline constexpr std::string_view kToReclaim = "to reclaim";     // "<N> iron bits to reclaim" on each skill row
inline constexpr std::string_view kEach = "each";
inline constexpr std::string_view kReclaimed = "reclaimed";
inline constexpr std::string_view kNotEnoughBits = "not enough iron bits";
inline constexpr std::string_view kNothingToReclaim = "nothing to reclaim";
inline constexpr std::string_view kRemoveModifiersFirst = "remove points from its modifiers first";   // a base skill's last point while modifiers hold points (the game's tagReclaimBase); the modifiers follow as list items
inline constexpr std::string_view kDetachPowerFirst = "detach its celestial power first";           // a skill's last point while a celestial power is bound to it (tagReclaimDevotion)
inline constexpr std::string_view kUndoPoints = "undo points";   // the skills window's own button: revert the points spent since it opened
// Devotion (the skills window's Constellations / Celestial Powers tabs; docs/devotion.md)
inline constexpr std::string_view kConstellations = "constellations";
inline constexpr std::string_view kCelestialPowers = "celestial powers";
inline constexpr std::string_view kCelestialPower = "celestial power";
inline constexpr std::string_view kAffinities = "affinities";
inline constexpr std::string_view kNoAffinity = "no affinity";
inline constexpr std::string_view kAvailable = "available";
inline constexpr std::string_view kUnavailable = "unavailable";   // the constellations tab's third Tab stop: locked behind an affinity
inline constexpr std::string_view kStar = "star";                  // "star 3" -- stars have no names of their own
inline constexpr std::string_view kNeedsStar = "needs star";       // "needs star 2": the linked star is not learned yet
inline constexpr std::string_view kNeeds = "needs";                // "needs Chaos 4": the constellation's affinity requirement
inline constexpr std::string_view kGives = "gives";                // "gives Chaos 3, Eldritch 2": the completion bonus
inline constexpr std::string_view kLearned = "learned";
inline constexpr std::string_view kComplete = "complete";
inline constexpr std::string_view kConstellationComplete = "constellation complete";
inline constexpr std::string_view kAttachedTo = "attached to";     // a celestial power's host skill
inline constexpr std::string_view kNotAttached = "not attached";
inline constexpr std::string_view kHas = "has";                    // "<skill>, has <power>" in the host picker
inline constexpr std::string_view kFrom = "from";                  // "from Bat": a power's constellation
inline constexpr std::string_view kAssign = "assign";              // the host picker's title: "assign Twin Fangs to"
inline constexpr std::string_view kTo = "to";
inline constexpr std::string_view kHave = "have";                  // "requires Eldritch 1, have 4"
inline constexpr std::string_view kNone = "none";
inline constexpr std::string_view kReplaced = "replaced";
inline constexpr std::string_view kNoCelestialPowers = "no celestial powers learned";
inline constexpr std::string_view kNeededByStar = "needed by star";      // "needed by star 3": a learned star hangs off this one
inline constexpr std::string_view kWouldLock = "would lock";             // "would lock Raven": losing the bonus drops that constellation below its requirement
inline constexpr std::string_view kAetherCrystals = "aether crystals";
inline constexpr std::string_view kNotEnoughAether = "not enough aether crystals";
inline constexpr std::string_view kReclaimDevotionHint = "Backspace to reclaim a devotion point";
inline constexpr std::string_view kAnd = "and";
inline constexpr std::string_view kLost = "lost";                       // "constellation complete lost": the bonus went with the star
// "Waking to Misery: Enter the Cave under Burial Hill" -- one open objective of a tracked quest
gd::core::MessageBuilder& push_quest_objective(gd::core::MessageBuilder& m, std::string_view quest, std::string_view objective);
// "enter 1 to 12" -- a count prompt refusing an out-of-range value
gd::core::MessageBuilder& push_range_hint(gd::core::MessageBuilder& m, unsigned lo, unsigned hi);
// "<name>, x 3" -- a stacked item
gd::core::MessageBuilder& push_stack(gd::core::MessageBuilder& m, std::string_view name, unsigned stack);
// "<label>: <value>" -- a sheet row
gd::core::MessageBuilder& push_stat(gd::core::MessageBuilder& m, std::string_view label, std::string_view value);
// The game's inline text markup, removed: "{^b}Text" / "^bText" carry a colour letter (b, r, g, y, w, o, E ...),
// "{^n}" / "^n" is a line break (becomes a space). Everything spoken from a game string that may carry markup
// (map icon names, tag texts) goes through here; a doubled "^^" is not markup.
std::string strip_markup(std::string_view text);
// "<name>, level 3 of 12" -- a skill row
gd::core::MessageBuilder& push_skill_level(gd::core::MessageBuilder& m, unsigned level, unsigned max_level);
// ", Physique 391 of 392" -- one failed requirement (what the character has, what the item needs)
gd::core::MessageBuilder& push_shortfall(gd::core::MessageBuilder& m, std::string_view label, int have, int need);
// "<name>, <level name>, 1500 of 5000" -- a faction row
gd::core::MessageBuilder& push_faction(gd::core::MessageBuilder& m, std::string_view name, std::string_view level_name, float value, int low, int high);

inline constexpr std::string_view kUnsupportedGameVersion = "this game version is not supported by Grimdark; menus will not be read";

}  // namespace gd::strings
