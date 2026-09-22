#include "travel.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <format>
#include <vector>
#include "app.h"
#include "core/travel_path.h"
#include "exe_ui.h"
#include "gameapi.h"
#include "hooks.h"
#include "log.h"
#include "rooms.h"
#include "screens/list_picker.h"
#include "speech.h"

namespace gd::travel {
namespace {
namespace path = core::travel_path;
enum class Mode { Idle, Arming, Walking, TownArming, Portal, PortalMap };
Mode mode = Mode::Idle;
world::TravelTarget target;
std::vector<path::Point> route;
size_t corner = 0;
path::Progress progress;
double deadline = 0, last_plan = 0, last_report = 0, last_probe = 0;
unsigned owner = 0;
world::Vec3 planned{};
std::string last_status = "No travel in progress";
bool map_request = false;
bool quest_request = false;
bool test_background = false;
world::Vec3 last_position{};
double last_motion = 0, last_life = 0;
std::vector<unsigned> old_portals;
std::vector<world::Vec3> itinerary;
size_t leg = 0;
std::string selected_quest_area;
path::Point point(world::Vec3 p) { return {p.x, p.y, p.z}; }
world::Vec3 vec(path::Point p) { return {p.x, p.y, p.z}; }
void say(const std::string& s) { last_status = s; log::write("travel: " + s); speech::speak(s, true); }
bool in_game() { auto* s = app::screens().current(); return s && s->key() == "in_game"; }
bool foreground() { HWND game = FindWindowA("Grim Dawn", nullptr); return game && (test_background || GetForegroundWindow() == game); }
bool arrived(world::Vec3 me, float radius) {
  float d = path::distance(point(me), point(target.pos));
  if (d >= radius || std::abs(me.y - target.pos.y) >= 3) return false;
  if (d < 0.2f) return true;
  return world::free_distance_ray((target.pos.x-me.x)/d, (target.pos.z-me.z)/d, 0, d, nullptr) >= d - 0.2f;
}
bool manual() {
  const auto& ks = hooks::key_source();
  // A fresh Enter/J may start combat; carry-over Enter from a destination picker is ignored while arming.
  const int keys[] = {0x11,0x1e,0x1f,0x20,0x39,0x01,0x24,0x17,0x1c,0x16,0x3b};
  for (int k : keys) if (ks.just_pressed(k)) return true;
  for (int k : {0x11,0x1e,0x1f,0x20}) if (ks.held(k)) return true;
  return false;
}
bool plan(world::Vec3 me) {
  std::vector<world::Vec3> result;
  if (!world::find_path_corridor(target.pos, result)) return false;
  route.clear();
  for (auto p : result) route.push_back(point(p));
  if (!path::reaches(route, point(target.pos))) return false;
  corner = 0;
  while (corner + 1 < route.size() && path::distance(point(me), route[corner]) < 0.9f) ++corner;
  progress.reset(app::now(), path::remaining(point(me), route, corner));
  planned = target.pos; last_plan = app::now();
  return true;
}
void town_picker() {
  // The game's discovered rift list is the authority; no hard-coded coordinates or unlocks.
  auto gates = exe_ui::riftgates();
  std::vector<screens::PickerItem> rows;
  for (size_t i = 0; i < gates.size(); ++i) {
    auto& g = gates[i];
    if (g.owner) continue;
    rows.push_back({static_cast<unsigned>(i + 1), g.name, g.current ? "You are here" : "Unlocked riftgate"});
  }
  mode = Mode::Idle;
  if (rows.empty()) { say("No unlocked town or riftgate destinations are available"); return; }
  screens::open_picker("Return to town: choose an unlocked riftgate", std::move(rows), [gates](unsigned id) {
    if (!exe_ui::riftgate_map_open() || !id || id > gates.size()) { say("Riftgate travel is no longer available"); return; }
    const auto& g = gates[id - 1];
    if (g.current) { say("You are already here"); return; }
    if (exe_ui::riftgate_travel(g)) say("Travelling through the rift to " + g.name);
    else say("The game could not use that riftgate");
  });
}
}
bool active() { return mode != Mode::Idle; }
bool background_test(bool on) {
  wchar_t v[4]{};
  if (on && !(GetEnvironmentVariableW(L"GRIMDARK_NOFOCUS",v,4) && v[0] == L'1')) return false;
  test_background = on; return true;
}
std::string status() { return last_status + "\n" + std::format("active={} corners={} next={} target={}\n", active(), route.size(), corner, target.id); }
void stop(const std::string& reason) {
  bool walking = mode == Mode::Walking || mode == Mode::PortalMap;
  bool was_active = active() || map_request || quest_request;
  mode = Mode::Idle; map_request = false; quest_request = false; route.clear();
  if (walking && world::player_id() == owner) world::stop_movement();
  if (was_active && !reason.empty()) say(reason);
}
void start(world::TravelTarget selected) {
  stop("");
  itinerary.clear(); leg=0;
  if (!world::in_world() || !path::finite(point(selected.pos))) { say("No destination available"); return; }
  target = std::move(selected); owner = world::player_id();
  if (target.id) world::lock_target(target.id); else world::lock_point(target.pos);
  mode = Mode::Arming; deadline = app::now() + 15;
  say("Preparing to walk to " + target.label);
}
void named_place(const std::string& area) {
  auto legs=rooms::travel_route(area);
  if (legs.empty()) { say("No connected room route to " + area + " is known"); return; }
  world::TravelTarget t; t.label=area; t.pos=legs.front();
  start(std::move(t));
  itinerary=std::move(legs); selected_quest_area=area; leg=0;
  world::set_follow_target(0,itinerary.back(),area);
}
void quest_places() {
  std::vector<screens::PickerItem> rows;
  std::vector<std::string> places;
  for (const auto& q:gameapi::quests(gameapi::kQuestsInProgress)) {
    for (const auto& task:q.tasks) {
      if (task.state!=2) continue;
      std::string objectives;
      for (const auto& ob:task.objectives) if (!ob.done()) objectives += ob.text + " ";
      auto names=rooms::places_in_text(objectives);
      if (names.empty()) names=rooms::places_in_text(task.description);
      for (const auto& name:names) {
        if (std::find(places.begin(),places.end(),name)!=places.end()) continue;
        places.push_back(name);
        rows.push_back({static_cast<unsigned>(places.size()),name,q.name});
      }
    }
  }
  if (rows.empty()) { say("No named place could be resolved from your active quest. Choose a map marker or a nearby exit."); return; }
  screens::open_picker("Active quest destinations",std::move(rows),[places](unsigned id) {
    if (id && id<=places.size()) named_place(places[id-1]);
  });
}
void reviewed() {
  if (active()) { stop(); return; }
  world::TravelTarget t;
  if (!world::reviewed_destination(t)) { say("Select a target with the tracker first"); return; }
  start(std::move(t));
}
void followed() {
  if (!selected_quest_area.empty() && world::follow_target_label()==selected_quest_area) {
    std::string area=selected_quest_area; named_place(area); return;
  }
  world::TravelTarget t;
  if (!world::followed_destination(t)) { say("Choose a destination from the map first"); return; }
  start(std::move(t));
}
void open_map() {
  stop(""); map_request = true; deadline = app::now() + 15;
  say("Opening quest and map destinations. Choose a marker, then press Backspace to walk there. Enter sets a direction beacon.");
}
void return_to_town() {
  stop("");
  if (!world::in_world()) { say("Enter the game first"); return; }
  owner = world::player_id(); mode = Mode::TownArming; deadline = app::now() + 15;
  say("Preparing your personal rift for return travel");
}
void open_menu() {
  stop();
  screens::open_picker("Travel", {
    {1,"Walk to selected tracker target","Control semicolon"},
    {6,"Active quest destinations","Walk through successive room exits"},
    {2,"Map destinations","Choose a marker; Backspace walks there"},
    {3,"Walk to followed map destination","Control apostrophe"},
    {4,"Return to town or unlocked riftgate","Control Shift L"},
    {5,"Stop travel",""}
  }, [](unsigned id) {
    if (id == 1) reviewed(); else if (id == 6) { quest_request=true; deadline=app::now()+15; }
    else if (id == 2) open_map(); else if (id == 3) followed();
    else if (id == 4) return_to_town(); else stop();
  });
}
void tick() {
  if (!active() && !map_request && !quest_request) return;
  const double now = app::now();
  if (quest_request) {
    if (now > deadline || hooks::key_source().just_pressed(0x01)) { stop("Quest selection cancelled"); return; }
    if (in_game()) { quest_request=false; quest_places(); }
    return;
  }
  if (map_request) {
    if (now > deadline) { stop("Map opening cancelled"); return; }
    if (hooks::key_source().just_pressed(0x01)) { stop("Map opening cancelled"); return; }
    if (in_game()) { map_request = false; app::fire_action("game.map"); }
    return;
  }
  if (!world::in_world() || world::player_id() != owner || world::life() <= 0) { stop("Travel stopped: character unavailable"); return; }
  const auto& ks = hooks::key_source();
  if (mode == Mode::Arming || mode == Mode::TownArming) {
    if (now > deadline) { stop("Travel cancelled before starting"); return; }
    if (ks.just_pressed(0x01) || ks.just_pressed(0x11) || ks.just_pressed(0x1e) || ks.just_pressed(0x1f) || ks.just_pressed(0x20)) { stop(); return; }
    if (!in_game()) {
      auto* s = app::screens().current();
      // Only tolerate the closing frame of the menu/map that requested travel. Never leave a latent
      // movement request waiting underneath an unrelated window or across a return to the main menu.
      if (now > deadline - 14 || !s || (s->key() != "list_picker" && s->key() != "mapmarkers" && s->key() != "mod_menu"))
        stop("Travel cancelled: another screen is open");
      return;
    }
    if (world::game_paused() || !foreground()) { stop("Travel cancelled: game paused or focus changed"); return; }
    for (int k : {0x1c,0x24,0x17,0x11,0x1e,0x1f,0x20}) if (ks.held(k)) return;
    if (mode == Mode::TownArming) {
      old_portals.clear();
      for (const auto& it : world::scan(world::ScanGroup::Neutrals, 12)) old_portals.push_back(it.id);
      app::fire_action("game.riftgate"); mode = Mode::Portal; deadline = now + 8; last_probe = 0;
      say("Opening your personal rift"); return;
    }
    world::Vec3 me;
    if (!world::player_position(me)) { stop("Position unavailable"); return; }
    if (target.id && !world::entity_position(target.id, target.pos)) { stop("Target lost"); return; }
    if (arrived(me, target.enemy ? 2.4f : 1.4f)) { stop("Already near " + target.label); return; }
    if (!plan(me)) { stop("No complete walkable route to " + target.label + ". A door, blocked exit, or unloaded area may be in the way."); return; }
    mode = Mode::Walking; last_report = now; last_position = me; last_motion = now; last_life = world::life();
    say("Walking to " + target.label + ". Press a movement key to stop.");
  }
  if (manual()) { stop("Travel stopped: manual control"); return; }
  if (!foreground() || world::game_paused()) { stop("Travel stopped: game paused or focus changed"); return; }
  if (mode == Mode::Portal || mode == Mode::PortalMap) {
    if (exe_ui::riftgate_map_open()) { town_picker(); return; }
    if (!in_game()) { stop("Return travel stopped: another window opened"); return; }
    if (now > deadline) { stop("The personal rift could not be opened here. Use Control L and select the rift with N to check it."); return; }
    if (mode == Mode::Portal && now - last_probe > 0.4) {
      last_probe = now;
      for (const auto& it : world::scan(world::ScanGroup::Neutrals, 12)) {
        if (it.cls.find("Teleporter") == std::string::npos) continue;
        if (std::find(old_portals.begin(), old_portals.end(), it.id) != old_portals.end()) continue;
        world::lock_target(it.id); world::mouse_key(1,true); world::mouse_key(1,false);
        mode = Mode::PortalMap; deadline = now + 6; return;
      }
    }
    return;
  }
  if (!in_game()) { stop("Travel stopped: menu or conversation opened"); return; }
  world::Vec3 me;
  if (!world::player_position(me)) { stop("Travel stopped: position unavailable"); return; }
  if (!target.enemy && world::life() < last_life - 0.5) { stop("Travel stopped: you are taking damage"); return; }
  last_life = world::life();
  if (path::distance(point(me), point(last_position)) > 0.3f) { last_position = me; last_motion = now; }
  if (now - last_motion > 4) { stop("Travel blocked: unable to move"); return; }
  if (target.id) {
    if (!world::entity_position(target.id, target.pos)) { stop("Target lost: " + target.label); return; }
    if (target.enemy) { float hp; int level, cls; if (!world::enemy_vitals(target.id,hp,level,cls) || hp <= 0) { stop("Target is no longer alive"); return; } }
  }
  float d = path::distance(point(me), point(target.pos));
  const float arrival = target.enemy ? 2.4f : 1.4f;
  if (arrived(me, arrival) || (!itinerary.empty() && corner+1==route.size() && path::distance(point(me),route.back())<0.9f)) {
    if (leg+1<itinerary.size()) {
      target.pos=itinerary[++leg]; world::lock_point(target.pos);
      if (!plan(me)) { stop("Route to " + target.label + " is blocked at the next room. Check nearby doors with N; Control apostrophe retries the journey."); return; }
    } else {
      stop("Arrived near " + target.label + (target.id ? ". Press J to interact or attack." : ". Use Control M for the entrance marker, or N for nearby entrances.")); return;
    }
  }
  if (target.id && path::distance(point(planned), point(target.pos)) > 2 && now - last_plan > 0.8) {
    if (!plan(me)) { stop("The target moved beyond a reachable route"); return; }
  }
  while (corner + 1 < route.size() && path::distance(point(me),route[corner]) < 0.8f) ++corner;
  if (corner >= route.size() || progress.stalled(now,path::remaining(point(me),route,corner))) { stop("Travel blocked. Select an exit, door, or another destination."); return; }
  if (corner + 1 == route.size() && path::distance(point(me), route[corner]) < 0.9f) { stop("Reached the nearest walkable point to " + target.label + ". Press J if interaction is available."); return; }
  auto next = path::toward(point(me), route[corner], 1.5f);
  float rate = 0; int type = 0;
  if (world::hazard_at(vec(next), &rate, &type)) { stop("Travel stopped before harmful ground"); return; }
  world::movement_step(vec(next));  // rejection while stunned/animating is handled by the progress timeout
  if (now - last_report > 8) { say(std::format("Walking to {}, {:.0f} away", target.label, d)); last_report = now; }
}
}
