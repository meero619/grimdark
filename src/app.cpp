#include "app.h"
#include <windows.h>
#include <format>
#include <memory>
#include "core/graph_announcer.h"
#include "core/message_builder.h"
#include "core/navigator.h"
#include "core/strings.h"
#include "combat.h"
#include "rooms.h"
#include "hooks.h"
#include "log.h"
#include "screens/conversation.h"
#include "screens/create_character.h"
#include "screens/delete_character.h"
#include "screens/options.h"
#include "screens/loot_filter.h"
#include "screens/crafting.h"
#include "screens/inventor.h"
#include "screens/illusionist.h"
#include "screens/difficulty_select.h"
#include "screens/in_game.h"
#include "screens/loading.h"
#include "screens/main_menu.h"
#include "screens/map_markers.h"
#include "screens/message_box.h"
#include "screens/pause_menu.h"
#include "screens/tip.h"
#include "screens/codex.h"
#include "screens/factions.h"
#include "screens/achievements.h"
#include "screens/riftgate.h"
#include "sonar.h"
#include "screens/inventory.h"
#include "screens/skills.h"
#include "screens/quickbar.h"
#include "screens/modals.h"
#include "screens/vendor.h"
#include "screens/list_picker.h"
#include "screens/reward_list.h"
#include "screens/count_prompt.h"
#include "screens/hotbar_manager.h"
#include "screens/announcements.h"
#include "screens/cue_settings.h"
#include "screens/mod_menu.h"
#include "screens/mod_options.h"
#include "screens/sound_glossary.h"
#include "screens/pets.h"
#include "gameapi.h"
#include "speech.h"
#include "world.h"
#include "travel.h"

namespace gd::app {
using namespace gd::core;

static InputManager g_input;
static ScreenManager g_screens;
static std::unique_ptr<GraphNavigator> g_nav;
static bool g_owns_keyboard = false;  // last decision applied to the key hook; changes are logged
static bool g_inited = false;
static double g_last_tick = 0;

double now() {
  static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
  LARGE_INTEGER c; QueryPerformanceCounter(&c);
  return (double)c.QuadPart / (double)freq.QuadPart;
}

bool owns_keyboard() { return g_owns_keyboard; }
InputManager& input() { return g_input; }
ScreenManager& screens() { return g_screens; }
GraphNavigator* navigator() { return g_nav.get(); }

// Typematic at the user's own OS keyboard settings, so nav key-repeat feels like the rest of the system.
static InputManager::Typematic os_typematic() {
  InputManager::Typematic t;
  int delay = 0, speed = 0;
  if (SystemParametersInfoW(SPI_GETKEYBOARDDELAY, 0, &delay, 0) && SystemParametersInfoW(SPI_GETKEYBOARDSPEED, 0, &speed, 0)) {
    delay = delay < 0 ? 0 : delay > 3 ? 3 : delay;           // 0..3 -> 250..1000 ms
    speed = speed < 0 ? 0 : speed > 31 ? 31 : speed;         // 0..31 -> 2.5..30 cps
    t.initial_delay = (delay + 1) * 0.25;
    t.repeat_interval = 1.0 / (2.5 + (speed / 31.0) * 27.5);
    log::writef("OS key repeat: delay={:.3f}s interval={:.3f}s (raw delay={} speed={})", t.initial_delay, t.repeat_interval, delay, speed);
  } else {
    log::write("OS key repeat query failed; using defaults");
  }
  return t;
}

// Key codes in the game's Button enum (DIK for plain keys; extended keys measured -- see CLAUDE.md).
namespace keys {
constexpr int Escape = 0x01, Tab = 0x0f, Enter = 0x1c, Backspace = 0x0e, Space = 0x39, F1 = 0x3b, Backslash = 0x2b;
constexpr int Home = 0x78, Up = 0x79, PageUp = 0x7a, Left = 0x7b, Right = 0x7c, End = 0x7d, Down = 0x7e, PageDown = 0x7f;  // from the game's own key names (tools/exports/keynames.txt)
constexpr int P = 0x19;
}  // namespace keys

static void register_actions() {
  auto& m = g_input;
  struct Ui { std::string_view id; const char* label; int key; bool shift, ctrl; bool repeat; };
  const Ui ui[] = {
    {ui_actions::Up, "Previous item", keys::Up, false, false, true}, {ui_actions::Down, "Next item", keys::Down, false, false, true},
    {ui_actions::Left, "Left", keys::Left, false, false, true}, {ui_actions::Right, "Right", keys::Right, false, false, true},
    {ui_actions::Next, "Next panel", keys::Tab, false, false, true}, {ui_actions::Prev, "Previous panel", keys::Tab, true, false, true},
    {ui_actions::Home, "First", keys::Home, false, false, false}, {ui_actions::End, "Last", keys::End, false, false, false},
    {ui_actions::RegionPrev, "Previous region", keys::Up, false, true, false}, {ui_actions::RegionNext, "Next region", keys::Down, false, true, false},
    {ui_actions::Activate, "Activate", keys::Enter, false, false, false}, {ui_actions::Secondary, "Secondary action", keys::Backspace, false, false, false},
    {ui_actions::Back, "Back", keys::Escape, false, false, false}, {ui_actions::Tooltip, "Tooltip", keys::Space, false, false, false},
    {ui_actions::TooltipDetail, "Detailed tooltip", keys::Space, false, true, false},
    {ui_actions::Compare, "Compare with equipped", keys::Backslash, false, false, false},
  };
  for (const Ui& u : ui) {
    auto& a = m.register_action(std::string(u.id), u.label, InputCategory::UI).bind(u.key, u.ctrl, u.shift, false);
    if (u.repeat) a.repeating();
  }
  // F1 = the mod's own menu, live everywhere (Global); tooltips stay on Space (2026-09-01, F1 used to double as tooltip).
  m.register_action("mod.menu", "Grimdark menu", InputCategory::Global, [] { screens::open_mod_menu(); }).bind(keys::F1);
  m.register_action("travel.menu", "Travel menu", InputCategory::InGame, [] { travel::open_menu(); }).bind(0x27, true, true, false);
  m.register_action("travel.reviewed", "Walk to selected target or stop", InputCategory::InGame, [] { travel::reviewed(); }).bind(0x27, true, false, false);
  m.register_action("travel.followed", "Walk to followed destination", InputCategory::InGame, [] { travel::followed(); }).bind(0x28, true, false, false);
  m.register_action("travel.town", "Return to town", InputCategory::InGame, [] { travel::return_to_town(); }).bind(0x26, true, true, false);
  m.register_action("travel.stop", "Stop travel", InputCategory::Global, [] { travel::stop(); }).bind(0x42); // F8; Ctrl+Escape belongs to Windows
  // Ctrl+Tab / Ctrl+Shift+Tab: the current screen's tabs (tab list across the top; the page is one column).
  m.register_action("ui.tabNext", "Next tab", InputCategory::UI, [] { Screen* s = g_screens.current(); if (s) s->switch_tab(1); }).bind(keys::Tab, true, false, false);
  m.register_action("ui.tabPrev", "Previous tab", InputCategory::UI, [] { Screen* s = g_screens.current(); if (s) s->switch_tab(-1); }).bind(keys::Tab, true, true, false);
  // In-world readouts: Q = the objective tracker. Bare Y is the game's own Quickbar Switch (it passes
  // straight through -- see screens/in_game.cpp; the switch is announced by screens::quickbar_tick).
  m.register_action("ingame.objectives", "Objectives", InputCategory::InGame, [] { screens::speak_objectives(); }).bind(0x10);
  // G = the game's own Pickup action (nearest item within 10 units; the game leaves it unbound).
  m.register_action("ingame.pickup", "Pick up nearest item", InputCategory::InGame, [] { screens::pickup_nearest(); }).bind(0x22);
  // O = show all items: the review groups and the sonar ignore the loot filter and the game draws every label (its
  // Alt modifier, latched); the window itself is Ctrl+O (the lift below). docs/loot-filter.md.
  m.register_action("ingame.showAllItems", "Show all items on or off", InputCategory::InGame, [] { world::toggle_show_all_items(); }).bind(0x18);
  // F = swap the active weapon set (the game's "Switch Weapons" is unbound); announces the new set + hands.
  m.register_action("ingame.swapWeapons", "Swap weapon set", InputCategory::InGame, [] { screens::swap_weapons(); }).bind(0x21);
  // Ctrl+` = the hotbar manager (assign skills to the number bars).
  m.register_action("ingame.hotbarManager", "Hotbar manager", InputCategory::InGame, [] { screens::open_hotbar_manager(); }).bind(0x29, true, false, false);
  // Reading the quickbar in the world: Ctrl+1..0 read slot 1..10 of the displayed bar, each saying the skill
  // and how it aims (world::skill_aim); Ctrl+- / Ctrl+= read the left / right mouse skill.
  for (int k = 1; k <= 10; ++k) m.register_action(std::format("read.slot{}", k), std::format("Read quickbar slot {}", k % 10), InputCategory::InGame, [k] { screens::speak_slot(k); }).bind(k == 10 ? 0x0b : 0x01 + k, true, false, false);
  m.register_action("read.leftMouse", "Read left mouse skill", InputCategory::InGame, [] { screens::speak_mouse(true); }).bind(0x0c, true, false, false);   // Ctrl+-
  m.register_action("read.rightMouse", "Read right mouse skill", InputCategory::InGame, [] { screens::speak_mouse(false); }).bind(0x0d, true, false, false);  // Ctrl+=
  // Quickbar assignment from a window (the skills window's focused skill): Ctrl+1..0 -> slot, Ctrl+J / Ctrl+I -> mouse.
  for (int k = 1; k <= 10; ++k) m.register_action(std::format("assign.slot{}", k), std::format("Assign to quickbar slot {}", k % 10), InputCategory::Windows, [k] { screens::assign_focused(k); }).bind(k == 10 ? 0x0b : 0x01 + k, true, false, false);
  m.register_action("assign.primary", "Assign to left mouse", InputCategory::Windows, [] { screens::assign_focused(0); }).bind(0x24, true, false, false);
  // Ctrl+Enter on a stack in the vendor's Sell tab: sell part of it (a count prompt).
  // Ctrl+Enter in a window: the vendor's Sell tab sells part of a stack; the inventory's bag tab makes that bag the
  // receiving one. Each is silent when its screen is not the current one.
  m.register_action("window.ctrlEnter", "Sell part of a stack / receiving bag", InputCategory::Windows, [] { screens::sell_partial_focused(); screens::set_receiving_bag_focused(); }).bind(keys::Enter, true, false, false);
  m.register_action("assign.secondary", "Assign to right mouse", InputCategory::Windows, [] { screens::assign_focused(-1); }).bind(0x17, true, false, false);
  m.register_action("ingame.where", "Where am I", InputCategory::InGame, [] { screens::speak_where(); }).bind(keys::P, true, true).bind(0x25);  // K
  // Health and energy in full, in the player's own voice (bare H; Ctrl+H stays the game's Help window).
  m.register_action("ingame.vitals", "Health and energy", InputCategory::InGame, [] { combat::speak_vitals(); }).bind(0x23);
  // The review cursor (wotr's scanner keys): Period enemies, N people, B bystanders, M objects; Shift
  // cycles backward. Each landing speaks name / distance / clock bearing and parks the virtual cursor on it.
  struct Cycle { const char* id; const char* label; int key; world::ScanGroup group; int dir; bool shift; };
  const Cycle cycles[] = {
    {"scan.enemyNext", "Next enemy", 0x34, world::ScanGroup::Enemies, 1, false}, {"scan.enemyPrev", "Previous enemy", 0x34, world::ScanGroup::Enemies, -1, true},
    {"scan.neutralNext", "Next person or object", 0x31, world::ScanGroup::Neutrals, 1, false}, {"scan.neutralPrev", "Previous person or object", 0x31, world::ScanGroup::Neutrals, -1, true},
    {"scan.bystanderNext", "Next bystander", 0x30, world::ScanGroup::Bystanders, 1, false}, {"scan.bystanderPrev", "Previous bystander", 0x30, world::ScanGroup::Bystanders, -1, true},
    {"scan.objectNext", "Next loot", 0x32, world::ScanGroup::Objects, 1, false}, {"scan.objectPrev", "Previous loot", 0x32, world::ScanGroup::Objects, -1, true},
    {"scan.exitNext", "Next exit", 0x2f, world::ScanGroup::Exits, 1, false}, {"scan.exitPrev", "Previous exit", 0x2f, world::ScanGroup::Exits, -1, true},   // V: the current room's exits (docs/rooms.md)
    {"scan.playerNext", "Next character", 0x2e, world::ScanGroup::Players, 1, false}, {"scan.playerPrev", "Previous character", 0x2e, world::ScanGroup::Players, -1, true},   // C: player characters (you; party members one day) -- locking yourself aims cursor-placed skills at your feet
  };
  for (const Cycle& c : cycles) {
    world::ScanGroup g = c.group; int dir = c.dir;
    m.register_action(c.id, c.label, InputCategory::InGame, [g, dir] { speech::speak(world::cycle_review(g, dir), true); }).bind(c.key, false, c.shift, false);
    // Alt + the key: the NEAREST of the group, whatever is reviewed now (the thing that just ran up to you).
    if (!c.shift) {
      std::string id = std::string(c.id).substr(0, std::string(c.id).size() - 4) + "Nearest";   // scan.enemyNext -> scan.enemyNearest
      std::string label = std::string("Nearest ") + (c.label + 5);                                 // "Next enemy" -> "Nearest enemy"
      m.register_action(id, label, InputCategory::InGame, [g] { speech::speak(world::cycle_review(g, 1, true), true); }).bind(c.key, false, false, true);
    }
  }
  // Pets (docs/pets.md): [ / ] cycle the review cursor through the player's own pets (Alt+] = nearest); Backspace
  // = the pet overlay (stance, disband, selection, commands); F2..F6 / F7 = our own selection toggles (the game's
  // are swallowed: they had a 5-pet ceiling and made the next click a hidden pet command); Shift+Backspace = the
  // selected (or all) pets attack the locked target.
  m.register_action("scan.petNext", "Next pet", InputCategory::InGame, [] { speech::speak(world::cycle_review(world::ScanGroup::Pets, 1), true); }).bind(0x1b);
  m.register_action("scan.petPrev", "Previous pet", InputCategory::InGame, [] { speech::speak(world::cycle_review(world::ScanGroup::Pets, -1), true); }).bind(0x1a);
  m.register_action("scan.petNearest", "Nearest pet", InputCategory::InGame, [] { speech::speak(world::cycle_review(world::ScanGroup::Pets, 1, true), true); }).bind(0x1b, false, false, true);
  m.register_action("ingame.pets", "Pets", InputCategory::InGame, [] { screens::open_pet_overlay(); }).bind(0x0e);
  m.register_action("ingame.petsAttack", "Pets attack locked target", InputCategory::InGame, [] { screens::pets_attack_locked(); }).bind(0x0e, false, true, false);
  for (int i = 0; i < 5; ++i) m.register_action(std::format("ingame.selectPet{}", i + 1), std::format("Select pet {}", i + 1), InputCategory::InGame, [i] { screens::toggle_pet_selected(i); }).bind(0x3c + i);
  m.register_action("ingame.selectAllPets", "Select all pets", InputCategory::InGame, [] { screens::select_all_pets(); }).bind(0x41);
  // The free cursor (docs/controls.md "Advanced targeting"): Shift+W/A/S/D move the cursor's world point instead of
  // the character (app's key filter keeps the presses from the game), Z toggles grid / polar and is the only one
  // that speaks. Repeating: a held Shift+W walks the point at the OS typematic rate.
  struct CursorStep { const char* id; const char* label; int key; gd::core::CursorKey dir; };
  const CursorStep steps[] = {{"cursor.forward", "Cursor up", 0x11, gd::core::CursorKey::Forward}, {"cursor.back", "Cursor down", 0x1f, gd::core::CursorKey::Back},
                              {"cursor.left", "Cursor left", 0x1e, gd::core::CursorKey::Left}, {"cursor.right", "Cursor right", 0x20, gd::core::CursorKey::Right}};
  for (const CursorStep& c : steps) {
    gd::core::CursorKey dir = c.dir;
    m.register_action(c.id, c.label, InputCategory::InGame, [dir] { world::free_cursor_step(dir); }).bind(c.key, false, true, false).repeating();
  }
  m.register_action("cursor.mode", "Cursor mode grid or polar", InputCategory::InGame, [] { speech::speak(world::toggle_cursor_mode(), true); }).bind(0x2c);
  // Backslash: the sonar sweep on / off (the game's Toggle Party Display is lifted to Ctrl+Backslash).
  m.register_action("sonar.toggle", "Sonar on or off", InputCategory::InGame, [] {
    sonar::set_enabled(!sonar::enabled());
    speech::speak(sonar::enabled() ? strings::kSonarOn : strings::kSonarOff, true);
  }).bind(keys::Backslash);
  // (F12 = 0x56 in the game's Button enum, NOT the DIK 0x58 -- tools/exports/keynames.txt. It was the sonar dev A/B key
  // on 2026-09-20 and is free.)
  m.register_action("scan.ping", "Ping the reviewed thing", InputCategory::InGame,
                    [] { if (world::ping_reviewed().empty()) speech::speak(strings::kNoTarget, true); }).bind(0x27);  // Semicolon
  // The follow key: ping the map marker picked in the Ctrl+M window, with its distance and heading.
  m.register_action("follow.ping", "Follow the quest marker", InputCategory::InGame, [] {
    std::string line = world::follow_ping();
    speech::speak(line.empty() ? std::string(strings::kNotFollowing) : line, true);
  }).bind(0x28);  // Apostrophe
  // Inspect the current target (/ ): "<pct> percent health, <effects>" with no name; silent for a non-enemy.
  m.register_action("scan.inspect", "Inspect target", InputCategory::InGame, [] {
    std::string s = world::inspect_target();
    if (!s.empty()) speech::speak(s, true);
  }).bind(0x35);  // Slash
  // Comma cycles only the highest-rarity enemies nearby (find the boss / a summoner's adds); an inert camera key.
  m.register_action("scan.topClassNext", "Cycle highest-rarity enemy", InputCategory::InGame,
                    [] { speech::speak(world::cycle_highest_classification(1), true); }).bind(0x33);          // ,
  m.register_action("scan.topClassPrev", "Previous highest-rarity enemy", InputCategory::InGame,
                    [] { speech::speak(world::cycle_highest_classification(-1), true); }).bind(0x33, false, true, false);  // Shift+,
  // Rooms (docs/rooms.md): X = the current room's title and description; its exits are the scanner's Exits
  // group (V above). Place changes are announced automatically in the player's voice.
  m.register_action("rooms.describe", "Describe the room", InputCategory::InGame, [] { rooms::speak_description(); }).bind(0x2d);   // X
  m.register_action("rooms.note", "Note this place for authoring", InputCategory::InGame, [] { rooms::note_place(); });   // unbound since 2026-09-01 (dev route /note); T = announcements
  m.register_action("ingame.announcements", "Announcement toggles", InputCategory::InGame, [] { screens::open_announcements(); }).bind(0x14);   // T
  m.register_action("ingame.cueSettings", "Sound cue settings", InputCategory::InGame, [] { screens::open_cue_settings(); }).bind(0x14, true, false, false);   // Ctrl+T, next to T's announcement settings (Ctrl+Backslash was the first pick; 1Password owns that chord system-wide)
  // The mouse buttons (J left, I right, Enter = left; hold to hold) are polled per frame by the in-game screen,
  // not dispatched as actions: a hold needs the key's held state, not a press.
  // The camera is locked (far zoom, yaw 0 = grid up; the game's dialogue north is 50 deg clockwise of it, docs/compass.md) by the in-game screen; no zoom/rotate keys.
  // The game's less frequent functions, lifted to Ctrl + their default key (docs/controls.md): the chord is
  // ours, the plain key is injected into the game's poll, so the game's own map stays untouched and the
  // plain letters are free for the mod. Frequent keys (WASD, 1-0, Space, E, R, U, Escape) pass through
  // directly in screens/in_game.cpp.
  struct Lift { const char* id; const char* label; int code; char16_t ch; };
  const Lift lifted[] = {
    {"game.character", "Character window", 0x2e, u'c'}, {"game.skills", "Skill window", 0x31, u'n'}, {"game.codex", "Codex window", 0x10, u'q'},
    {"game.map", "Map window", 0x32, u'm'}, {"game.lootFilter", "Loot filter window", 0x18, u'o'}, {"game.group", "Group window", 0x25, u'k'},
    {"game.gameMenu", "Game menu", 0x22, u'g'}, {"game.help", "Help window", 0x23, u'h'}, {"game.factions", "Factions window", 0x24, u'j'},
    {"game.achievements", "Achievements window", 0x2f, u'v'}, {"game.riftgate", "Personal riftgate", 0x26, u'l'}, {"game.drop", "Drop item", 0x30, u'b'},
    {"game.tooltips", "Show item tooltips", 0x2d, u'x'}, {"game.showItems", "Show items (filter common)", 0x2c, u'z'},
    {"game.pause", "Pause game", 0x19, u'p'}, {"game.partyDisplay", "Toggle party display", 0x2b, u'\\'},
    {"game.toggleUi", "Toggle UI", 0x1b, u']'}, {"game.cameraLeft", "Camera rotate left", 0x33, u','}, {"game.cameraRight", "Camera rotate right", 0x34, u'.'},
    {"game.chat", "Chat window", 0x1c, 0}, {"game.pushToTalk", "Push to talk", 0x0f, 0},
  };
  for (const Lift& l : lifted) {
    int code = l.code; char16_t ch = l.ch;
    std::string key = l.id;
    m.register_action(l.id, l.label, InputCategory::Lifted, [code, ch, key] {
      if (key == "game.drop") {
        Screen* screen = g_screens.current();
        if (screen && screen->invoke_action(key)) return;
      }
      hooks::push_game_key(code, false, false, false, ch);
    }).bind(l.code, true, false, false);
  }
  // The game opens the character/inventory window from C or I; both lifts (Ctrl+I injects the plain C).
  m.find("game.character")->bind(0x17, true, false, false);
}

void init() {
  if (g_inited) return;
  g_inited = true;
  g_input.set_typematic(os_typematic());
  // Announcer wording comes from the strings module.
  GraphAnnouncer::position_text = [](int i, int n) { MessageBuilder m; strings::push_position(m, i, n); return m.build(); };
  GraphAnnouncer::expanded_state_text = [](bool e) { return std::string(e ? strings::kExpanded : strings::kCollapsed); };
  g_nav = std::make_unique<GraphNavigator>(NavigatorHost{
      [](std::string_view t, bool interrupt) { speech::speak(t, interrupt); },
      {},  // hover sound: none yet
      [] { return g_screens.owns_keyboard(); },  // live: the navigator attaches from inside the screen tick
      [] { return (int)hooks::frame(); }});
  Screen::set_host([](std::string_view s) { speech::speak(s, false); }, [](Screen* s) { if (g_nav) g_nav->screen_closed(s); });
  g_screens.set_navigator([](Screen* s) { if (g_nav) g_nav->attach(s); }, [] { if (g_nav) g_nav->ensure_focus(); });
  register_actions();
  hooks::set_game_key_filter([](int code, bool released, bool shift, bool ctrl) {
    Screen* s = g_screens.current();
    if (!s || !s->passes_key(code)) return false;
    // Shift+W/A/S/D move the free cursor (cursor.*), not the character: the press is ours. The RELEASE always
    // passes: a W held before Shift went down is the game's own move, and swallowing its release would leave the
    // exe's held byte set (the character runs on) -- a spurious up for a key the game never saw down is harmless.
    if ((code == 0x11 || code == 0x1e || code == 0x1f || code == 0x20) && !released && shift) return false;
    // Ctrl+<digit> is the mod's quickbar-read chord (read.slot1..10), never the game's -- the game binds
    // single buttons only, so it never wants a modified digit. The passthrough is by code alone (modifier
    // blind), so without this a real Ctrl+1 would ALSO activate the game's slot 1 you only meant to read.
    if ((code >= 0x02 && code <= 0x0b) && ctrl) return false;
    return true;
  });
  g_screens.register_screen(screens::make_unsupported());
  g_screens.register_screen(screens::make_main_menu());
  g_screens.register_screen(screens::make_create_character());
  g_screens.register_screen(screens::make_delete_character());
  g_screens.register_screen(screens::make_options());
  g_screens.register_screen(screens::make_difficulty_select());
  g_screens.register_screen(screens::make_game_mode_select());
  g_screens.register_screen(screens::make_in_game());
  g_screens.register_screen(screens::make_message_box());
  g_screens.register_screen(screens::make_pause_menu());
  g_screens.register_screen(screens::make_loading());
  g_screens.register_screen(screens::make_tip());
  g_screens.register_screen(screens::make_conversation());
  g_screens.register_screen(screens::make_codex());
  g_screens.register_screen(screens::make_factions());
  g_screens.register_screen(screens::make_achievements());
  g_screens.register_screen(screens::make_loot_filter());
  g_screens.register_screen(screens::make_crafting());
  g_screens.register_screen(screens::make_inventor());
  g_screens.register_screen(screens::make_illusionist());
  g_screens.register_screen(screens::make_riftgate());
  g_screens.register_screen(screens::make_map_markers());
  g_screens.register_screen(screens::make_inventory());
  g_screens.register_screen(screens::make_skills());
  g_screens.register_screen(screens::make_quest_reward());
  g_screens.register_screen(screens::make_shrine());
  g_screens.register_screen(screens::make_corrupted_shrine());
  g_screens.register_screen(screens::make_vendor());
  g_screens.register_screen(screens::make_stash());
  g_screens.register_screen(screens::make_hotbar_manager());
  g_screens.register_screen(screens::make_pet_overlay());
  g_screens.register_screen(screens::make_announcements_overlay());
  g_screens.register_screen(screens::make_cue_settings_overlay());
  g_screens.register_screen(screens::make_mod_menu());
  g_screens.register_screen(screens::make_sound_glossary());
  g_screens.register_screen(screens::make_mod_options());
  g_screens.register_screen(screens::make_list_picker());
  g_screens.register_screen(screens::make_count_prompt());
  g_screens.register_screen(screens::make_reward_notice());
  gameapi::load();
  g_last_tick = now();
  log::writef("app: initialized with {} actions", g_input.actions().size());
}

void tick() {
  if (!g_inited) return;
  double t = now();
  double dt = g_last_tick > 0 ? t - g_last_tick : 0;
  g_last_tick = t;
  // A screen capturing raw input this frame keeps the keys it read in on_update from also reaching the
  // navigator (the Enter that ends a text edit must not re-activate the field).
  Screen* before = g_screens.current();
  bool raw = before && before->captures_raw_input();
  g_screens.tick();
  travel::tick();
  // The screen stack decides who gets the keyboard; the hook is only touched when the decision changes so a
  // dev override (/gamekeys) holds until the next screen change.
  bool owns = g_screens.owns_keyboard();
  if (owns != g_owns_keyboard) {
    g_owns_keyboard = owns;
    hooks::set_game_keys_muted(owns);
    Screen* s = g_screens.current();
    log::writef("keyboard: {} ({})", owns ? "mod" : "game", s ? s->key() : "no screen");
  }
  g_input.set_live_categories(g_screens.live_categories());
  Screen* cur = g_screens.current();
  if (raw || (cur && cur->captures_raw_input())) return;
  // The keys of the frame a screen became current belong to whoever had the keyboard before it: the Escape
  // that the game turned into its pause menu must not reach the pause menu screen as Back (structured
  // detection is immediate, so the new screen would otherwise close itself with the key that opened it).
  if (cur != before) return;
  const KeySource& ks = hooks::key_source();
  g_input.tick(t, ks, [](InputAction& a) { return g_nav && g_nav->on_action(a.key()); });
  if (g_nav) {
    TypeaheadInput in;
    in.typed = hooks::typed_chars();
    in.ctrl = ks.ctrl(); in.alt = ks.alt(); in.shift = ks.shift();
    in.escape_pressed = ks.just_pressed(keys::Escape);
    in.up_held = ks.held(keys::Up); in.down_held = ks.held(keys::Down);
    in.dt = dt;
    g_nav->tick_typeahead(in);
  }
}

bool fire_action(std::string_view key) {
  InputAction* a = g_input.find(key);
  if (!a) return false;
  bool consumed = a->category() == InputCategory::UI && g_nav && g_nav->on_action(a->key());
  if (!consumed) a->invoke_performed();
  return true;
}

std::string action_keys() {
  std::string s;
  for (auto& a : g_input.actions()) s += a->key() + "\n";
  return s;
}

std::string gui_dump() { return g_nav ? g_nav->dump() : "(no navigator)\n"; }

void shutdown() {
  g_inited = false;
  g_nav.reset();
  hooks::set_game_key_filter({});
  if (g_owns_keyboard) hooks::set_game_keys_muted(false);
  g_owns_keyboard = false;
}
}  // namespace gd::app
