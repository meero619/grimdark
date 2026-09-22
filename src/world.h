#pragma once
#include "core/cursor_step.h"
// The in-game substrate: the main player, its controller, the navmesh and the camera, reached through the
// game's exports. GameEngine is not an exported singleton, so the instance pointers are captured from hooks on
// per-frame members (GameEngine::Update, ControllerPlayer::Update). All calls here are game-thread only.
#include <string>
#include <vector>

namespace gd::world {
struct Vec3 { float x = 0, y = 0, z = 0; };

bool install();   // attach the capture hooks (Game.dll/Engine.dll exports)
void remove();

bool in_world();                 // the engine, the main player and its controller have all been seen
void* game_engine();             // the GAME::GameEngine instance (captured from GameEngine::Update; null before)
void* controller();              // the main player's ControllerPlayer (captured from ControllerPlayer::Update; null before)
bool player_position(Vec3& p);   // world-space position of the main player (feet)
std::string player_name();
unsigned player_id();            // the main player's object id (0 before the player is seen)
bool is_foe(unsigned id);        // the game's faction manager calls this object a foe of the player (false when unknown)
std::string region_name();
std::string area_name();   // the minimap's area name (Engine::GetAreaNameTag localized), "Lower Crossing"; empty when unknown
bool object_is_note(const void* obj);   // is-a ItemNote (any Object, bag items included)
double life();                   // current life (0 when unknown)
bool player_alive();             // Character::IsAlive (false when the player/export is unknown)
float life_max();
float energy();                  // "mana" in the exports, "energy" in the UI
float energy_max();
float camera_yaw();              // the game camera's yaw, as reported (units logged)

// Navmesh probes around the player. dir is a unit vector in the horizontal plane (x, z), distances in the
// game's world units. free_distance walks from the player along dir and returns how far the path mesh stays
// walkable (max_dist when nothing blocks). on_navmesh tests one world-space point near the player.
bool on_navmesh(const Vec3& world_point);
// Snap a point to the floor and test it; `floored` (optional) receives the snapped world point either way.
bool navmesh_probe(const Vec3& world_point, Vec3* floored);
// free_distance follows the terrain (each sample starts at the previous sample's floor height); `free_distance_ex`
// with follow=false is the old flat-y ray, kept only for the A/B probe.
float free_distance(float dir_x, float dir_z, float max_dist, float step);
float free_distance_ex(float dir_x, float dir_z, float max_dist, float step, bool follow);
// The same ray started `lateral` units to the side of the player (positive = to the right of dir, i.e. along
// (-dir_z, dir_x)); a start that is itself off the mesh reads 0. Parallel lanes make a rectangle probe.
float free_distance_lane(float dir_x, float dir_z, float lateral, float max_dist, float step);
// Free distance by the navmesh RAYCAST (NavManager::FindStraightMovePoint): exact hit distance along the ray from
// a point `lateral` units beside the player (positive = left of dir), max `max_dist`; a lane start that is not
// itself on the mesh reads 0. The wall tones' probe (the point walk above reads holes as walkable; see world.cpp).
float free_distance_ray(float dir_x, float dir_z, float lateral, float max_dist, Vec3* hit_world);
// Painted damage sector at a world point (docs/hazards.md): true when the engine's damage layer is painted there;
// rate = fraction of max life per second (0.02..0.30), type = the CombatAttributeType code (11 = Aether). Any
// point, any loaded chunk; the exact lookup TickManager::Tick applies once a second, resistances not consulted.
bool hazard_at(const Vec3& world_point, float* rate, int* type);
bool mesh_contains(const Vec3& world_point);   // the floored point is inside the path mesh (closest-point gate, not the box test)
std::string nav_vwindow(float span, float step);              // dev: PutOnFloor's accepted vertical window at the feet
std::string wall_compare(int dirs, float max_dist, float step);  // dev: A/B flat vs terrain-following rays around the compass

// The aerial map's icons -- the game's own map-marker set, read live from the open map (empty when the map
// is not open). type = the nugget's icon category (state-aware: 0 hero (dropped), 2 person, 3 riftgate,
// 7 merchant, 10 spirit guide, 13 caravan, ...); label/id come from matching the icon to a rendered entity.
// quest markers (the objective overlay, GetMarkerUIDs/StaticMarker) are not yet folded in -- see markers_dump.
struct MapMarker { unsigned id = 0; int type = 0; bool quest = false; std::string label; std::string symbol; Vec3 pos; float dist = 0; };   // symbol: a custom icon's texture path (type 14), dev dump only
std::vector<MapMarker> map_markers();   // nearest first; needs the aerial map open
std::string map_markers_dump();         // dev: /mapmarkers

std::string debug_dump();        // for the dev server: pointers, raw coordinate bytes, probes
const void* object_rtti(const void* obj);   // the object's dynamic RTTI_ClassInfo (virtual GetRTTIClassInfo); null-safe
std::string object_class_name(const void* obj);   // the dynamic RTTI class name ("Player", "ArmorJewelry_Ring", ...); "?" when unknown. NEVER on a Region*
bool object_is_a(const void* obj, const void* class_info);   // is-a via the RTTI_ClassInfo parent walk (class_info = a GetStaticClassInfo result)
std::string classinfo_dump();    // dev: the game's RTTI_ClassInfo layout (parent pointer?)

// What stopped a probe: an OBSTACLE has walkable navmesh a few units beyond it along the probe direction
// (the character walks round it), a WALL has none (cliffs, buildings, level geometry). The nearby-entity
// cache (sphere query, refreshed every ~200 ms; ABBox = centre + half-extents) names the blocker for /blocks.
enum class BlockKind { Wall, Obstacle };
BlockKind classify_block(const Vec3& stop_world, float dir_x, float dir_z);
std::string blocks_dump();       // the four probe stops with their blockers (dev)
std::string settle_status(float radius);   // /settle: loader idle, chunks still loading near the player, app state, ticks (dev)
std::string regions_dump(int max);  // engine Regions (chunks) 0..max-1: index, name, offset from world, loaded, portals (dev)
std::string portals_dump();      // the player's chunk's portals: connected chunk, choke point, open (dev)
std::string markers_dump();      // dev: Player::GetMarkerUIDs (the accumulating quest-marker UID list)
std::string navprobe(float x0, float z0, float x1, float z1, float step);  // IsPointOnPathMesh over a grid (dev)
int find_path(const Vec3& dest_world, float f1, float f2, Vec3* out_world);  // Player::FindPath -> raw PathResult (dev)
// NavManager::FindPath -> the navmesh straight-path corridor from the player to dest, as absolute world points
// (empty on failure). Used to test whether a nearby room is a DIRECT exit; on-demand (V / room change) only.
bool find_path_corridor(const Vec3& dest_world, std::vector<Vec3>& out);
std::string teleport(float x, float z, bool check_only);
std::string set_paused(int want);                                      // dev: -1 = report, 0/1 = GAME::UnpauseGameTime/PauseGameTime (a hot reload in the world pauses the game)                 // dev: Entity::SetCoords on the player (floored); refuses unloaded chunks
std::string project_points(const std::vector<Vec3>& pts);                // dev: world ground points -> screen
std::string fog_reveal(float x, float z, int radius);                    // dev: FogOfWar::AddVisibility

// The game's own display label for an entity near the player (Monster::GetGameDescription /
// Npc::GetRolloverDescription / Player::GetRolloverDescription / Item::GetGameDescription, by class), colour
// codes stripped; empty when the class has no label export. Game text, verbatim.
std::string label_of(unsigned id);
// World-space position of any object by id (through object_by_id). False when the id has no positioned entity.
bool entity_position(unsigned id, Vec3& out);

// How using a skill chooses where it lands, for the quickbar readout (docs/skills-targeting.md): the runtime
// SkillTargetType plus the skill's class. None = not an activated skill (a passive or a modifier).
enum class SkillAim { None, SelfCast, AroundYou, AtPoint, AtTarget };
SkillAim skill_aim(const void* skill_obj);   // skill_obj from gameapi::object_by_id(skill_id)
int skill_target_type(const void* skill_obj);   // the raw SkillActivated::GetTargetType (-1 = not a SkillActivated); dev / docs

// ---- the review cursor (wotr's scanner, adapted) ----
// Groups cycle nearest-first from the player; the landing is remembered by OBJECT ID (session-unique,
// from ObjectManager::CreateObjectID) and re-found in a fresh query on every step -- never by pointer --
// so a despawned target simply drops out and the next step enters at the nearest. The landing also
// parks the virtual cursor on the thing, so the game hovers / targets it natively.
enum class ScanGroup { Enemies, Neutrals, Bystanders, Objects, Exits, Loot, Transitions, Pets, Destructibles, Shrines, Interactables, Players };   // Players (C): every player character incl. yourself (note "you") -- one today, party members in multiplayer   // Interactables: sonar only -- the N group (people + objects of interest) minus the kinds with a cue of their own   // Pets: the player's own summons ([ / ]), note = stance; Destructibles / Shrines: sonar only   // Loot/Transitions: the sonar sweep only
// note: an extra spoken list item ("blocked" for an exit whose opening the live mesh refuses), normally empty.
// level/classification are filled for enemies only (classification -1 = not read: not an enemy, or unknown);
// classification is the MonsterClassification enum (0 Common, 1 Champion, 2 Hero, 3 Boss, 4 Quest, 5 SuperBoss).
struct ScanItem { unsigned id; std::string cls, label, record; Vec3 pos; float dist; std::string note; int level = 0; int classification = -1; };
std::vector<ScanItem> scan(ScanGroup group, float radius = 40.0f);  // nearest first
// The loot filter applies to the review groups and the sonar exactly as to the game's labels (docs/loot-filter.md):
// a ground Item the filter hides is not listed and not pinged; an entity the game does not show at all
// (visibility 0: a placed quest item already collected) is never listed in any group. O latches "show all":
// the groups ignore the filter and the game draws every label (the Alt modifier byte, written per frame).
bool show_all_items();
void toggle_show_all_items();   // speaks the new state
void show_all_tick();           // per frame from the in-game screen: keeps the game's modifier byte in step with the latch
// Point items (room exits) are not entities: ids from kPointIdBase up, positions carried by the item. The
// provider (src/rooms.cpp) returns the current room's exits; the scanner locks/pings/projects the point.
constexpr unsigned kPointIdBase = 0x40000000u;
inline bool is_point_id(unsigned id) { return id >= kPointIdBase; }
void set_exit_provider(std::vector<ScanItem> (*provider)());
// Step the review cursor through a group (dir +1 / -1); returns the landing's spoken line, or the
// group's "nothing" text. Also locks the virtual cursor on it.
std::string cycle_review(ScanGroup group, int dir, bool nearest = false);   // nearest: enter at the closest regardless of the current target (Alt+key)
// The comma key: cycle only the enemies of the highest classification present nearby (find the boss and its
// tier / a summoner's adds). Same readout and landing as cycle_review(Enemies).
std::string cycle_highest_classification(int dir);
unsigned reviewed_id();
// Copy destinations already exposed by the review cursor/map. No wider discovery scan.
struct TravelTarget { unsigned id = 0; Vec3 pos; std::string label; bool enemy = false; };
bool reviewed_destination(TravelTarget& out);
bool followed_destination(TravelTarget& out);
bool movement_step(const Vec3& destination);
void stop_movement();
bool game_paused();
// A devotion shrine (StaticShrine) that has been restored / cleansed (its state 6); false for a ruined or desecrated one.
bool shrine_restored(unsigned id);

// ---- status effects and the target inspector ----
// Inspect what the player is currently targeting (ControllerPlayer::GetCombatEnemy): "<pct> percent health,
// <effect>, ..." with NO name (the review cursor already named it). Empty when nothing / not a living foe is
// targeted (the / key then stays silent). Screen-reader channel (a key readout, like H).
std::string inspect_target();
// A character's active status effects (buffs + debuffs) as display names, deduped: walks the game's own buff
// list (SkillServices GetBuffList) and names each via its skill id. Empty when none / unreadable.
std::vector<std::string> enemy_effects(unsigned id);
// Name one buff/debuff by its record path, resolved on the owner's SkillManager (the character the buff is on).
// Used by combat.cpp to name a debuff caught by the DebufTarget hook. Empty when unresolved.
std::string buff_name(unsigned owner_id, const char* record);
// Enemy vitals for the review readout: health fraction 0..1, char level, MonsterClassification (or -1). False
// when the id is not a readable Character.
bool enemy_vitals(unsigned id, float& pct, int& level, int& classification);
std::string effects_dump(unsigned id);   // dev: /effects?id= (raw buff ids + resolved names)
// The mouse buttons as keys (J left, I right; hold = hold): the button goes down at the virtual cursor --
// the reviewed thing when one is locked, else the real cursor -- and the game decides what that means
// (attack, talk, open, pick up, move, skill). A locked thing the camera does not show cannot be clicked:
// "too far away", nothing happens. Per frame from the in-game screen with the key's held state.
void mouse_key(int button, bool held);
// Whether an entity projects inside the game window (the camera shows it).
bool on_screen(unsigned id);
// Camera lock (per frame from the in-game screen): zoom at the far end of its range, yaw 0 = world -z up = the tile
// grid's axis (what the wall tones call "north"). NOT the game's north: the dialogue's compass is screen-up at the
// default camera (yaw 0.8727); keeping the grid frame is deliberate (docs/compass.md).
void pin_camera();
// Bearing of a world point from the player as a clock hour (12 = screen-up), and distance in units.
int clock_hour(const Vec3& p);
// Pan (-1..1, by ear-frame bearing) and gain (ref/(ref+dist), >= 0.15) of a world point from the player: the
// one rule for positioned sounds and voices.
void ear_frame(const Vec3& p, float& pan, float& gain, float* ahead = nullptr);   // ahead: +1 straight up the screen .. -1 behind
// The rear shelf for a point: 0 dB across the front, -10 dB dead behind (wotr Spatializer), from ear_frame's ahead.
float rear_shelf_db(float ahead);
// The one distance curve for every positioned cue (review pings, the sonar sweep): gain = ref / (ref + dist),
// never below floor. Systems put only a channel volume on top. Defaults for Grim Dawn's scale (2026-08-22).
void set_ping_rolloff(float ref, float floor);
std::string ping_rolloff();
// The voices' own rolloff (the pings keep the sonar curve above): full level out to `near`, falling linearly
// to `floor` at `far`, flat beyond. Defaults from the game's range table (gameengine.dbr): near = moderateRange
// 9, far = bossRange 32 (the farthest a pet's hit can be), floor 0.4. Live-tunable: /voice?near=&far=&floor=.
float voice_gain(float dist);
void set_voice_rolloff(float near_d, float far_d, float floor_g);  // <0 = keep
std::string voice_rolloff();
// A raw WorldVec3 (Region* + Vec3, 24 bytes) to world space; false when it has no region.
bool world_point(const void* worldvec3, Vec3& out);
// The inverse: a world-space point as a WorldVec3 (out = 24+ bytes) in the chunk containing (x, z), reached from
// the player's chunk (World::GetRegionContainingXZ); false without a player or when no chunk holds it.
bool world_vec3_at(const Vec3& world, void* out_worldvec3);
// The review ping (wotr's Semicolon, also played on every landing): one of three sounds for the route
// from the player to the reviewed thing -- straight walk, path around, unreachable -- positioned toward it
// (pan by bearing, volume ref/(ref+distance)). Returns the kind for the log; empty = nothing reviewed.
std::string ping_reviewed();
// The follow target (the quest-following key '): a destination chosen from the map picker -- an entity id
// (re-resolved each ping so it tracks a moving NPC) or a fixed world point. Independent of the review cursor.
void set_follow_target(unsigned id, const Vec3& pos, const std::string& label);
void clear_follow_target();
bool has_follow_target();
std::string follow_target_label();
// The ' key: play the route ping toward the follow target and return "label, N away, H o'clock" (with a
// "blocked" note when it can't be reached directly). Empty when nothing is being followed.
std::string follow_ping();

// Per-frame (self-throttled): while a thing is under review, re-sound the ping the moment its route KIND
// changes (path becomes straight, becomes unreachable, ...) so the player hears the change without pressing ;.
// Only the kind triggers it; pan/volume move every frame and are left to the manual ping.
void reping_tick();
// dev: average microseconds for one reviewed_route() navmesh line probe (needs a target under review).
std::string probe_timing(int iters);

// ---- conversations (structured, from hooks on Conversation::GetText / GetSteps) ----
// The UI asks the conversation for each step's text as it draws the dialog; we keep what it asked for
// this frame: the steps, their type/availability and text, and the child lists it fetched. The screen
// reads this instead of crawling drawn text (which cannot tell a response from a line of speech).
struct ConvStep { void* step; int type; std::string type_tag; bool available, used; std::string text; void* parent; };
struct ConvState { void* conversation = nullptr; uint64_t frame = 0; std::vector<ConvStep> steps; std::vector<void*> children; void* children_of = nullptr; };
ConvState conversation_state();   // the most recent frame in which the UI fetched conversation text
bool in_conversation();           // text was fetched within the last few frames
std::string conversation_dump();  // /conv

// ---- targeting (measurement phase) ----
// Everything the game rendered last frame, nearest first: pointer, object id, record name, class, distance.
std::string entities_dump(float max_dist, bool frustum = false);
std::string los_dump(unsigned id);   // camera line of sight to an entity (dev; the exe's cursor-pick ray)   // frustum: Engine::GetEntitiesInPriorFrameFrustum (last frame's render set) instead of the sphere
bool set_target(unsigned id);    // ControllerPlayer::SetCombatEnemy + FaceTarget
void clear_target();
unsigned current_target();       // ControllerPlayer::GetCombatEnemy
std::string target_dump();       // combat enemy + Character::GetCurrentAttackTarget

// ---- the virtual cursor ----
// The exe clears and re-resolves the combat enemy/ally from the cursor every frame (read 2026-08-21), so a
// target of ours is expressed as the cursor parked over the entity on screen: the game then hovers, attacks
// and interacts with it natively. lock_target keeps the cursor override on the entity each frame (tick).
bool lock_target(unsigned id);
bool lock_point(const Vec3& world_point);   // the same, for a bare world point (a room exit); unlock_target releases it too
void unlock_target();
unsigned locked_target();
std::string lock_dump();   // dev: found / not found for N ms (grace) / point / none
// The free cursor (docs/controls.md "Advanced targeting"): Shift+W/A/S/D move the cursor's world point instead of
// the character, Z toggles grid / polar. The first step seeds the point from the current cursor (the reviewed
// thing, the lock point, or your own feet); any review landing (lock_target / lock_point) or unlock ends it.
// The reviewed thing itself is untouched: only where J / I / cursor-aimed skills land changes.
bool free_cursor_step(gd::core::CursorKey key);   // false when not in the world
bool free_cursor_active();
gd::core::CursorMode cursor_mode();
std::string toggle_cursor_mode();                 // the spoken line ("cursor mode polar"); persisted
std::string free_cursor_dump();                   // dev
void tick();                                   // per frame while in the world
bool entity_screen_pos(unsigned id, float& x, float& y);  // client-area pixels via WorldCamera::Project
std::string project_dump(unsigned id);
}  // namespace gd::world
