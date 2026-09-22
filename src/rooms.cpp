#include "rooms.h"
#include <cstdio>
#include "audio.h"
#include "audio.h"
#include "db.h"
#include "log.h"
#include "speech.h"
#include "voice.h"
#include "world.h"
#include "core/message_builder.h"
#include "core/rooms_model.h"
#include "core/travel_path.h"
#include <cctype>
#include "core/strings.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace gd::rooms {
namespace {
using gd::core::MessageBuilder;
namespace strings = gd::strings;

struct Room { std::string key, title, body, subregion, area; std::string cls; float ax = 0, az = 0; bool island = false; };
struct Exit {
  int a = -1, b = -1; float x = 0, z = 0, width = 0; bool cut = false;
  std::string foreign;   // set when the far side is another region's room ("lowercrossing:43:-262"): b = -1
};
struct Region {
  std::string key, name;
  core::rooms::LabelGrid grid;
  std::vector<Room> rooms;                 // index = grid label
  std::vector<Exit> exits;
  std::map<std::string, std::string> subregion_names;
  bool loaded = false;
};

std::unique_ptr<db::Db> g_db;
std::map<std::string, std::string> g_chunk_region;   // lvl path -> region key
std::map<std::string, std::unique_ptr<Region>> g_regions;
Region* g_current = nullptr;
std::string g_announced_region, g_announced_subregion;
core::rooms::Hysteresis g_hyst;
bool g_say_untitled = true;
std::string g_last_line;
constexpr int kLookupRing = 8;           // cells (2 units) of ring search around the player's cell

int now_ms() { return (int)(GetTickCount64() & 0x7fffffff); }

// The chunks column is a JSON array of strings; pull the quoted strings out.
std::vector<std::string> json_strings(const std::string& s) {
  std::vector<std::string> out;
  size_t p = 0;
  while ((p = s.find('"', p)) != std::string::npos) {
    size_t e = s.find('"', p + 1);
    if (e == std::string::npos) break;
    std::string v = s.substr(p + 1, e - p - 1);
    for (size_t k = v.find("\\\\"); k != std::string::npos; k = v.find("\\\\", k + 1)) v.erase(k, 1);
    out.push_back(v);
    p = e + 1;
  }
  return out;
}

// The db must match the world the game mounts (docs/rooms.md "Expansion maps", 2026-09-14): each expansion ships a
// complete replacement world001.map, so assets/rooms.db is built from the Forgotten Gods map and rooms_base.db
// from the base game's. Which one the game runs is decided by what is installed under the install root (the exe
// is <root>\x64\Grim Dawn.exe; Steam removes a disabled DLC's folder). An Ashes-only install has no db of its own.
std::string db_file_for_install() {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::filesystem::path root = std::filesystem::path(exe).parent_path().parent_path();
  std::error_code ec;
  bool gdx2 = std::filesystem::exists(root / "gdx2" / "resources" / "Levels.arc", ec);
  bool gdx1 = std::filesystem::exists(root / "gdx1" / "resources" / "Levels.arc", ec);
  if (gdx2) return "rooms.db";
  if (gdx1) log::writef("rooms: Ashes of Malmouth without Forgotten Gods -- no rooms db for that map, using the base game's");
  return "rooms_base.db";
}

bool open_db() {
  g_db = std::make_unique<db::Db>();
  std::string path = audio::module_dir() + "assets\\" + db_file_for_install();
  if (!g_db->open_readonly(path)) { g_db.reset(); log::writef("rooms: no database at {}", path); return false; }
  {
    db::Stmt st(*g_db, "SELECT value FROM meta WHERE key='map'");
    log::writef("rooms: db map={}", st.ok() && st.step() ? st.text(0) : std::string("base"));
  }
  db::Stmt st(*g_db, "SELECT key, chunks FROM regions");
  while (st.step()) {
    std::string key = st.text(0);
    for (const std::string& c : json_strings(st.text(1))) {
      std::string n = c;
      std::replace(n.begin(), n.end(), '\\', '/');
      g_chunk_region[n] = key;
    }
  }
  log::writef("rooms: {} ({} chunks mapped, sqlite {})", path, g_chunk_region.size(), db::version());
  return true;
}

Region* load_region(const std::string& key) {
  auto it = g_regions.find(key);
  if (it != g_regions.end()) return it->second.get();
  auto r = std::make_unique<Region>();
  r->key = key;
  {
    db::Stmt st(*g_db, "SELECT name FROM regions WHERE key=?");
    st.bind(1, key);
    if (st.step()) r->name = st.text(0);
  }
  std::vector<std::string> label_keys;
  {
    db::Stmt st(*g_db, "SELECT x0, z0, w, h, cell, labels, label_keys FROM grids WHERE region_key=?");
    st.bind(1, key);
    if (!st.step()) { log::writef("rooms: region '{}' has no grid", key); return nullptr; }
    r->grid.x0 = st.real(0); r->grid.z0 = st.real(1); r->grid.cell = st.real(4);
    std::vector<uint8_t> blob = st.blob(5);
    if (!r->grid.decode_rle(blob.data(), blob.size(), (int)st.int64(2), (int)st.int64(3))) { log::writef("rooms: region '{}' grid does not decode", key); return nullptr; }
    label_keys = json_strings(st.text(6));
  }
  {
    // schema v2 height data; a v1 db just lacks the columns (statement fails to prepare) and lookups ignore y
    db::Stmt st(*g_db, "SELECT heights, overlays FROM grids WHERE region_key=?");
    if (st.ok()) {
      st.bind(1, key);
      if (st.step() && !st.is_null(0)) {
        std::vector<uint8_t> hb = st.blob(0), ob = st.blob(1);
        if (!r->grid.decode_heights(hb.data(), hb.size())) log::writef("rooms: region '{}' heights do not decode", key);
        r->grid.decode_overlays(ob.data(), ob.size());
      }
    }
  }
  r->rooms.resize(label_keys.size());
  std::map<std::string, int> by_key;
  for (size_t i = 0; i < label_keys.size(); ++i) { r->rooms[i].key = label_keys[i]; by_key[label_keys[i]] = (int)i; }
  {
    db::Stmt st(*g_db, "SELECT key, title, body, subregion_key, cls, anchor_x, anchor_z, island FROM rooms WHERE region_key=?");
    st.bind(1, key);
    while (st.step()) {
      auto f = by_key.find(st.text(0));
      if (f == by_key.end()) continue;
      Room& rm = r->rooms[f->second];
      rm.title = st.text(1); rm.body = st.text(2); rm.subregion = st.text(3); rm.cls = st.text(4);
      rm.ax = (float)st.real(5); rm.az = (float)st.real(6); rm.island = st.int64(7) != 0;
    }
  }
  {
    // Painted HUD area names (schema v3, tools/rooms.py areas --write): the game's own per-cell area layer,
    // majority per room. An older db lacks the column (prepare fails) and the region name is spoken instead.
    db::Stmt st(*g_db, "SELECT key, area_name FROM rooms WHERE region_key=? AND area_name IS NOT NULL");
    if (st.ok()) {
      st.bind(1, key);
      while (st.step()) {
        auto f = by_key.find(st.text(0));
        if (f != by_key.end()) r->rooms[f->second].area = st.text(1);
      }
    }
  }
  {
    db::Stmt st(*g_db, "SELECT room_a, room_b, x, z, width, cut FROM exits WHERE region_key=?");
    st.bind(1, key);
    while (st.step()) {
      auto a = by_key.find(st.text(0)), b = by_key.find(st.text(1));
      if (a == by_key.end()) continue;   // room_a is always this region's room (the seam pass writes one row per side)
      if (b == by_key.end()) {           // a cross-region exit: keep the far room's key, resolve on use
        r->exits.push_back({a->second, -1, (float)st.real(2), (float)st.real(3), (float)st.real(4), st.int64(5) != 0, st.text(1)});
        continue;
      }
      r->exits.push_back({a->second, b->second, (float)st.real(2), (float)st.real(3), (float)st.real(4), st.int64(5) != 0});
    }
  }
  {
    db::Stmt st(*g_db, "SELECT key, name FROM subregions WHERE region_key=?");
    st.bind(1, key);
    while (st.step()) r->subregion_names[st.text(0)] = st.text(1);
  }
  r->loaded = true;
  log::writef("rooms: loaded region '{}' ({}): {}x{} cells, {} rooms, {} exits", key, r->name, r->grid.w, r->grid.h, r->rooms.size(), r->exits.size());
  Region* raw = r.get();
  g_regions[key] = std::move(r);
  return raw;
}

std::string room_label(const Region& r, int label) {
  if (label < 0 || label >= (int)r.rooms.size()) return {};
  const Room& rm = r.rooms[label];
  if (!rm.title.empty()) return rm.title;
  if (!g_say_untitled) return {};
  MessageBuilder m;
  m.fragment(strings::kRoom).fragment(std::to_string(label));
  return m.build();
}

void announce(bool force) {
  if (!g_current || g_hyst.current < 0) return;
  const Room& rm = g_current->rooms[g_hyst.current];
  // The top layer of the spoken place is the game's own painted HUD area name ("Lower Crossing",
  // "Burrwitch Slums", "Anguish"), not our location-record region -- the location partition spans several
  // named areas (decided with the user 2026-08-31). Unpainted rooms fall back to the region name.
  std::string region = !rm.area.empty() ? rm.area : (g_current->name.empty() ? g_current->key : g_current->name);
  auto sr = g_current->subregion_names.find(rm.subregion);
  std::string subregion = sr != g_current->subregion_names.end() ? sr->second : std::string();
  MessageBuilder m;
  strings::push_place(m, (force || region != g_announced_region) ? region : std::string_view(),
                      (force || subregion != g_announced_subregion) ? subregion : std::string_view(), room_label(*g_current, g_hyst.current));
  g_announced_region = region; g_announced_subregion = subregion;
  std::string line = m.build();
  if (line.empty()) return;
  g_last_line = line;
  voice::say({voice::Which::Zira, line, 0.0f, 1.0f, voice::Policy::Replace, voice::kGroupInfo});
}

Region* region_for_player() {
  std::string chunk = world::region_name();
  std::replace(chunk.begin(), chunk.end(), '\\', '/');
  auto it = g_chunk_region.find(chunk);
  if (it == g_chunk_region.end()) return nullptr;
  return load_region(it->second);
}
}  // namespace

std::vector<world::ScanItem> exit_items();   // below
void init() { open_db(); world::set_exit_provider(exit_items); }
void shutdown() { g_regions.clear(); g_current = nullptr; g_db.reset(); }
std::vector<std::string> places_in_text(const std::string& text) {
  std::vector<std::string> out;
  if (!g_db) return out;
  auto lower=[](std::string s) { for (char& c:s) c=(char)std::tolower((unsigned char)c); return s; };
  std::string hay=lower(text);
  db::Stmt st(*g_db,"SELECT DISTINCT area_name FROM rooms WHERE area_name IS NOT NULL AND status!='orphan'");
  while (st.step()) {
    std::string name=st.text(0), needle=lower(name);
    if (needle.size()<5) continue;
    size_t p=hay.find(needle);
    if (p!=std::string::npos && (p==0 || !std::isalnum((unsigned char)hay[p-1])) &&
        (p+needle.size()==hay.size() || !std::isalnum((unsigned char)hay[p+needle.size()]))) out.push_back(name);
  }
  std::sort(out.begin(),out.end());
  return out;
}
std::vector<world::Vec3> travel_route(const std::string& area) {
  std::vector<world::Vec3> out;
  world::Vec3 me;
  if (!g_db || !g_current || !world::player_position(me)) return out;
  int label=g_current->grid.label_at(me.x,me.z,me.y,kLookupRing);
  if (label<0 || (size_t)label>=g_current->rooms.size()) return out;
  std::string start_key=g_current->rooms[(size_t)label].key;
  std::vector<core::travel_path::Point> points;
  std::vector<std::string> regions;
  std::vector<bool> goals;
  std::map<std::string,size_t> ids;
  db::Stmt nodes(*g_db,"SELECT key,region_key,anchor_x,anchor_z,area_name FROM rooms WHERE status!='orphan'");
  while (nodes.step()) {
    ids[nodes.text(0)]=points.size();
    regions.push_back(nodes.text(1));
    points.push_back({(float)nodes.real(2),me.y,(float)nodes.real(3)});
    goals.push_back(nodes.text(4)==area);
  }
  auto start=ids.find(start_key);
  if (start==ids.end()) return out;
  std::vector<std::vector<size_t>> edges(points.size());
  db::Stmt links(*g_db,"SELECT room_a,room_b FROM exits");
  while (links.step()) {
    auto a=ids.find(links.text(0)), b=ids.find(links.text(1));
    if (a!=ids.end() && b!=ids.end()) { edges[a->second].push_back(b->second); edges[b->second].push_back(a->second); }
  }
  auto path=core::travel_path::shortest_route(points,edges,start->second,goals);
  for (size_t i=path.size()>1?1:0;i<path.size();++i) {
    size_t n=path[i]; auto p=points[n];
    Region* r=load_region(regions[n]); double y=me.y;
    if (r) r->grid.floor_y_at(p.x,p.z,me.y,y);
    out.push_back({p.x,(float)y,p.z});
  }
  return out;
}
void reset() { g_hyst.reset(); g_announced_region.clear(); g_announced_subregion.clear(); g_current = nullptr; }
void set_dwell_ms(int ms) { g_hyst.dwell_ms = ms; }
void set_settle_ms(int ms) { g_hyst.settle_ms = ms; }
void set_say_untitled(bool on) { g_say_untitled = on; }
void announce_now() { announce(true); }
void reload() { shutdown(); g_chunk_region.clear(); reset(); open_db(); }

unsigned g_ticks = 0;
void tick() {
  ++g_ticks;
  if (!g_db || !world::in_world()) return;
  world::Vec3 p;
  if (!world::player_position(p)) return;
  Region* r = region_for_player();
  if (r != g_current) { g_current = r; g_hyst.reset(); }
  if (!r) return;
  // The runtime mesh is up to ~1.5 units wider than the bake at edges (measured 2026-08-22 north of the
  // spawn: ours from x 62.45, the game from 61.0), so search 8 cells = 2 units around the player.
  int label = r->grid.label_at(p.x, p.z, p.y, kLookupRing);
  if (g_hyst.update(label, now_ms())) announce(false);
}

void note_place() {
  world::Vec3 p{};
  bool have = world::player_position(p);
  std::string area = world::area_name();
  std::string room = "no room";
  if (g_current && g_hyst.current >= 0) {
    const Room& rm = g_current->rooms[g_hyst.current];
    room = rm.key + (rm.title.empty() ? " untitled" : " '" + rm.title + "'");
  }
  SYSTEMTIME t; GetLocalTime(&t);
  std::string line = std::format("{:04}-{:02}-{:02} {:02}:{:02}  area='{}'  chunk={}  at ({:.1f}, {:.1f})  region={}  room={}\n",
                                 t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, area, world::region_name(), have ? p.x : 0.0f, have ? p.z : 0.0f,
                                 g_current ? g_current->key : std::string("none"), room);
  // build/ninja/grimdark.dll -> the repo root is two levels up; fall back to the log directory.
  std::string path = audio::module_dir() + "..\\..\\untagged_rooms.txt";
  FILE* f = fopen(path.c_str(), "ab");
  if (!f) { char la[MAX_PATH] = {}; GetEnvironmentVariableA("LOCALAPPDATA", la, MAX_PATH); path = std::string(la) + "\\Grimdark\\untagged_rooms.txt"; f = fopen(path.c_str(), "ab"); }
  if (f) { fputs(line.c_str(), f); fclose(f); }
  log::writef("rooms: noted {}", line);
  MessageBuilder m;
  m.list_item().fragment(f ? strings::kNoted : strings::kNoteFailed);
  if (!area.empty()) m.list_item().fragment(area);
  m.list_item().fragment(room);
  speech::speak(m.build(), true);
}

void speak_description() {
  if (!g_current || g_hyst.current < 0) { speech::speak(strings::kNoRoom, true); return; }
  const Room& rm = g_current->rooms[g_hyst.current];
  MessageBuilder m;
  m.list_item().fragment(room_label(*g_current, g_hyst.current));
  m.list_item().fragment(rm.body.empty() ? std::string(strings::kNoDescription) : rm.body);
  speech::speak(m.build(), true);
}

// An exit is "blocked" when the live mesh refuses the opening although the bake allows it (a runtime
// obstacle: door, barricade, gate). The live mesh is up to ~1.5 units narrower than the bake at edges, so one
// point is not enough: probe a cross and call it blocked only if every point fails.
bool exit_blocked(const world::Vec3& at) {
  const float d = 0.75f;
  const world::Vec3 pts[] = {at, {at.x + d, at.y, at.z}, {at.x - d, at.y, at.z}, {at.x, at.y, at.z + d}, {at.x, at.y, at.z - d}};
  for (const world::Vec3& p : pts) if (world::on_navmesh(p)) return false;
  return true;
}

// The far side of a cross-region exit: "<painted area name>, <room title>" (the far region loads, cached,
// on first use; the region name when the far room is unpainted, the area alone when it is untitled) -- the
// player hears where the opening LEADS.
std::string foreign_label(const std::string& key) {
  size_t colon = key.find(':');
  if (colon == std::string::npos) return {};
  Region* fr = load_region(key.substr(0, colon));
  if (!fr) return {};
  MessageBuilder m;
  const Room* dest = nullptr;
  for (const Room& rm : fr->rooms)
    if (rm.key == key) { dest = &rm; break; }
  m.list_item().fragment(dest && !dest->area.empty() ? dest->area : (fr->name.empty() ? fr->key : fr->name));
  if (dest && !dest->title.empty()) m.list_item().fragment(dest->title);
  return m.build();
}

// The scanner's exit group: the current room's exits as point items. id = kPointIdBase + the neighbour's
// label (stable, so the cycle continues from the reviewed exit); label = the destination's title or "room N".
// Exits are found LIVE from the runtime navmesh, not the stored (flat, seam-eroded, off-mesh-blind) exit
// table (2026-08-24): every OTHER room whose cells fall within kExitRadius and that the game's own pathfinder
// (world::find_path) can actually reach from here is an exit, pointed at that room's nearest cell. Correct by
// construction for reachability -- quest doors / destructibles / NavBlockers gate find_path exactly as they
// gate the player, and stacked/elevated floors the player cannot reach are dropped (find_path is height-aware).
// Line-of-sight is NOT required: the bearing to the nearest cell may point through a wall at a room you reach
// by going round (the same as the old adjacency exits -- decided with the user 2026-08-24). But the route must
// be DIRECT: a room whose only navmesh path from here runs THROUGH a third labelled room is that third room's
// exit, not ours, so each candidate's actual corridor (world::find_path_corridor = NavManager::FindPath) is
// classified against the label grid (LabelGrid::path_is_direct) and dropped if it lingers in a third room
// (2026-08-24, on the user's report that "in A, can directly reach B" -- not merely "B is near and reachable").
// The directness test is strictly additive: a candidate is dropped only on a positive detour finding; if the
// corridor cannot be computed we keep it (the reachability gate already passed). Cross-region (foreign)
// openings still come from the seam rows because the far room lives in another region's label grid.
// On-demand (V / room change), so the ~a-few pathfinder calls are not a per-frame cost.
constexpr double kExitRadius = 28.0;   // units; a room whose opening is farther than this shows once you near it
constexpr double kExitFloorTol = 1.5;  // units; reached point vs the destination cell's floor y (layers < 0.9 apart are one floor)
constexpr double kReachTol = 1.5;      // units; how far short of the destination cell a FindPath endpoint may stop and still count as arriving
std::vector<world::ScanItem> exit_items() {
  std::vector<world::ScanItem> out;
  world::Vec3 p;
  if (!g_current || g_hyst.current < 0 || !world::player_position(p)) return out;
  const int room = g_hyst.current;
  for (const auto& nb : g_current->grid.neighbors_within(p.x, p.z, p.y, room, kExitRadius)) {
    world::Vec3 at{(float)nb.x, p.y, (float)nb.z};
    world::Vec3 reached{};
    if (world::find_path(at, 0.f, 0.f, &reached) != 0) continue;   // not reachable at all -> not an exit
    // Detour SNAPS an unreachable target onto the nearest reachable polygon and reports a complete path to
    // THAT -- for a room across a wall / drop-off / on another floor the endpoint lands on our own side, at the
    // player (2026-08-25, prison cellar: "ruined tier hall 4 units south" through a wall) or at the edge of the
    // ledge we stand on (2026-08-30, Flooded Cellar: "ruined camp cavern 6 away" across a drop-off -- FindPath
    // result 0, endpoint 4.9 u short on the tongue's lip; the old "made progress" test passed it because the
    // cell was only 6 u away). A route is a route only if it ARRIVES: the reached point within kReachTol of
    // the destination cell (runtime edge erosion of the baked cells is under a unit) or already inside the
    // neighbour's own label. Anything shorter is the snap, not a way through.
    {
      const double end_to_target = std::hypot((double)at.x - reached.x, (double)at.z - reached.z);
      const bool in_neighbour = g_current->grid.label_at(reached.x, reached.z, reached.y, 0) == nb.label;
      if (end_to_target > kReachTol && !in_neighbour) continue;
    }
    // Same floor: the pathfinder SNAPS the target to the nearest polygon within its search radius and reports
    // a complete path to that -- for a cell on the tier above (the prison cellar: the corridor at y -4, the
    // cellblock rooms at y +1) the snap lands on the wall's foot below it, 4-5 units under the cell's floor.
    // The grid stores each cell's floor height, so a reached point more than kExitFloorTol below/above the
    // destination cell's floor is a route that never got to that floor (2026-08-25, "ruined tier hall 4 units
    // south" -- and "cobbled cellblock corridor", the same tier, had passed for the same reason). A corridor
    // that climbs a stair arrives at the tier's height and passes; a cell without height data is not gated.
    {
      double floor = 0;
      if (g_current->grid.floor_y_at(nb.x, nb.z, p.y, floor) && std::fabs((double)reached.y - floor) > kExitFloorTol) continue;
    }
    // Direct-exit test: an exit to `nb` exists only if the game's actual navmesh route to it does not detour
    // through a third labelled room (LOS is NOT required -- the route may curve round a wall, but it must run
    // from our room straight into the neighbour). Strictly additive: only DROP on a positive detour finding;
    // if the corridor can't be computed we keep the exit (the reachability gate above already passed).
    // The exit's POSITION is where the corridor first enters the neighbour (the opening), not the neighbour's
    // nearest cell -- that cell can sit across a wall from us, which made bearings point through walls
    // (2026-08-25). Without a corridor the nearest cell stays the fallback.
    float dist = (float)nb.dist;
    if (std::vector<world::Vec3> corridor; world::find_path_corridor(at, corridor)) {
      std::vector<std::array<double, 3>> pts;
      pts.reserve(corridor.size());
      for (const world::Vec3& c : corridor) pts.push_back({c.x, c.y, c.z});
      if (!g_current->grid.path_is_direct(pts, room, nb.label)) continue;
      if (std::array<double, 3> entry{}; g_current->grid.path_entry_point(pts, nb.label, entry)) {
        at = {(float)entry[0], (float)entry[1], (float)entry[2]};
        dist = (float)std::hypot(entry[0] - p.x, entry[2] - p.z);
      } else {
        // The corridor reached (within a unit of) the target cell yet never sampled inside the neighbour:
        // the cell is labelled for a floor the route did not enter -> not our exit. A corridor that stopped
        // short (snapped onto unlabelled cells beside the room) is kept with the nearest-cell position.
        const world::Vec3& last = corridor.back();
        if (std::hypot((double)last.x - at.x, (double)last.z - at.z) <= 1.0) continue;
      }
    }
    std::string dest = room_label(*g_current, nb.label);
    if (dest.empty()) dest = nb.label < (int)g_current->rooms.size() ? g_current->rooms[nb.label].cls : std::string(strings::kRoom);
    out.push_back({world::kPointIdBase + (unsigned)nb.label, "exit", dest, {}, at, dist, {}});
  }
  // Cross-region openings: the neighbour is in another region's grid, so keep them from the seam rows,
  // shown only when the live mesh still reaches the opening.
  for (int i = 0; i < (int)g_current->exits.size(); ++i) {
    const Exit& e = g_current->exits[i];
    if (e.a != room || e.foreign.empty()) continue;
    world::Vec3 at{e.x, p.y, e.z};
    if (!world::on_navmesh(at)) continue;
    out.push_back({world::kPointIdBase + 0x10000u + (unsigned)i, "exit", foreign_label(e.foreign), {}, at, std::hypot(e.x - p.x, e.z - p.z), {}});
  }
  return out;
}

std::string status() {
  if (!g_db) return "rooms: no database\n";
  std::string s = std::format("db open, {} chunks mapped, {} regions loaded, dwell {} ms, settle {} ms, untitled {}\n", g_chunk_region.size(), g_regions.size(), g_hyst.dwell_ms, g_hyst.settle_ms, g_say_untitled);
  for (const auto& [chunk, key] : g_chunk_region) s += std::format("  '{}' -> {}\n", chunk, key);
  s += std::format("ticks {}\n", g_ticks);
  world::Vec3 p;
  bool have = world::in_world() && world::player_position(p);
  s += std::format("chunk '{}' -> region {}\n", world::region_name(), g_current ? g_current->key : "none");
  if (!g_current || !have) return s;
  int label = g_current->grid.label_at(p.x, p.z, p.y, kLookupRing);
  s += std::format("player ({:.1f}, {:.1f}) -> label {} current {} candidate {}; last line '{}'\n", p.x, p.z, label, g_hyst.current, g_hyst.candidate, g_last_line);
  if (g_hyst.current >= 0) {
    const Room& rm = g_current->rooms[g_hyst.current];
    s += std::format("room key {} cls {} anchor ({:.0f}, {:.0f}) island {} title '{}' area '{}' subregion '{}' body '{}'\n", rm.key, rm.cls, rm.ax, rm.az, rm.island, rm.title, rm.area, rm.subregion, rm.body.substr(0, 80));
    for (const Exit& e : g_current->exits)
      if (e.a == g_hyst.current || e.b == g_hyst.current) {
        int other = e.a == g_hyst.current ? e.b : e.a;
        world::Vec3 at{e.x, p.y, e.z};
        s += std::format("  exit -> {} at ({:.1f}, {:.1f}) width {:.1f} cut {} dist {:.1f} hour {} walkable {}\n", other < 0 && !e.foreign.empty() ? e.foreign : std::to_string(other), e.x, e.z, e.width, e.cut, std::hypot(e.x - p.x, e.z - p.z), world::clock_hour(at), world::on_navmesh(at));
      }
  }
  return s;
}
}  // namespace gd::rooms
