#include "devserver.h"
#include "hooks.h"
#include "app.h"
#include "log.h"
#include "speech.h"
#include "textcap.h"
#include "world.h"
#include "travel.h"
#include "notify.h"
#include "quest_rewards.h"
#include "exe_ui.h"
#include "audio.h"
#include "audio_mute.h"
#include "casts.h"
#include "telegraph.h"
#include "combat.h"
#include "hazard.h"
#include "rooms.h"
#include "sonar.h"
#include "voice.h"
#include "screens/in_game.h"
#include "screens/inventory.h"
#include "screens/quickbar.h"
#include "shrine_table.h"
#include "gameapi.h"
#include "core/strings.h"
#include <cmath>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>

namespace gd::dev {
static std::atomic<bool> g_run{false};
static SOCKET g_listen = INVALID_SOCKET;
static std::thread g_thread;

static std::string url_decode(std::string_view s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') o += ' ';
    else if (s[i] == '%' && i + 2 < s.size()) { o += (char)strtol(std::string(s.substr(i + 1, 2)).c_str(), nullptr, 16); i += 2; }
    else o += s[i];
  }
  return o;
}
static std::map<std::string, std::string> parse_query(std::string_view q) {
  std::map<std::string, std::string> m;
  size_t p = 0;
  while (p <= q.size()) {
    size_t e = q.find('&', p); if (e == std::string_view::npos) e = q.size();
    std::string_view kv = q.substr(p, e - p);
    size_t eq = kv.find('=');
    if (!kv.empty()) m[url_decode(kv.substr(0, eq))] = eq == std::string_view::npos ? "1" : url_decode(kv.substr(eq + 1));
    p = e + 1;
  }
  return m;
}
static int parse_int(const std::string& s, int def = 0) {
  if (s.empty()) return def;
  return (int)strtol(s.c_str(), nullptr, s.rfind("0x", 0) == 0 ? 16 : 10);
}
static bool truthy(const std::string& s) { return s == "1" || s == "true" || s == "on" || s == "yes"; }

// Key names -> the game's Button enum (DIK scancodes for plain keys; extended keys measured from the game).
static int key_code(const std::string& name) {
  static const std::map<std::string, int> k = {
    {"escape", 0x01}, {"esc", 0x01}, {"1", 0x02}, {"2", 0x03}, {"3", 0x04}, {"4", 0x05}, {"5", 0x06}, {"6", 0x07}, {"7", 0x08}, {"8", 0x09}, {"9", 0x0a}, {"0", 0x0b},
    {"minus", 0x0c}, {"equals", 0x0d}, {"backspace", 0x0e}, {"tab", 0x0f},
    {"q", 0x10}, {"w", 0x11}, {"e", 0x12}, {"r", 0x13}, {"t", 0x14}, {"y", 0x15}, {"u", 0x16}, {"i", 0x17}, {"o", 0x18}, {"p", 0x19},
    {"enter", 0x1c}, {"return", 0x1c}, {"ctrl", 0x1d}, {"lctrl", 0x1d}, {"rctrl", 0x6b}, {"insert", 0x80}, {"delete", 0x81},
    {"a", 0x1e}, {"s", 0x1f}, {"d", 0x20}, {"f", 0x21}, {"g", 0x22}, {"h", 0x23}, {"j", 0x24}, {"k", 0x25}, {"l", 0x26},
    {"lshift", 0x2a}, {"z", 0x2c}, {"x", 0x2d}, {"c", 0x2e}, {"v", 0x2f}, {"b", 0x30}, {"n", 0x31}, {"m", 0x32},
    {"rshift", 0x36}, {"lalt", 0x38}, {"alt", 0x38}, {"ralt", 0x76}, {"space", 0x39}, {"capslock", 0x3a},
    {"f1", 0x3b}, {"f2", 0x3c}, {"f3", 0x3d}, {"f4", 0x3e}, {"f5", 0x3f}, {"f6", 0x40}, {"f7", 0x41}, {"f8", 0x42}, {"f9", 0x43}, {"f10", 0x44},
    {"f11", 0x57}, {"f12", 0x58},
    {"home", 0x78}, {"up", 0x79}, {"pageup", 0x7a}, {"left", 0x7b}, {"right", 0x7c}, {"end", 0x7d}, {"down", 0x7e}, {"pagedown", 0x7f},  // from the game's own key names
    {"lbracket", 0x1a}, {"rbracket", 0x1b}, {"semicolon", 0x27}, {"apostrophe", 0x28}, {"grave", 0x29}, {"backslash", 0x2b},
    {"comma", 0x33}, {"period", 0x34}, {"slash", 0x35}, {"numpadplus", 0x4e}, {"numpadminus", 0x4a},
  };
  auto it = k.find(name);
  return it == k.end() ? -1 : it->second;
}

static std::string handle(const std::string& path, const std::map<std::string, std::string>& q, const std::string& body, int& status) {
  status = 200;
  if (path == "/health")
    return std::format("ok pid={} frame={} backend={} speech_muted={} game_keys_muted={} keyboard={}\n", GetCurrentProcessId(), hooks::frame(),
                       speech::backend_name(), speech::muted(), hooks::game_keys_muted(), app::owns_keyboard() ? "mod" : "game") + hooks::counters() + "\n";
  if (path == "/voices") return voice::status() + "rolloff: " + world::voice_rolloff() + "\n";  // the positional voices: state, bound voices, counters, the installed list
  if (path == "/voice") {  // /voice?since=N the spoken-line ring; ?say=..&voice=mark|zira&pan=&gain=&replace=1 a test line; knobs enable= vol=
    if (q.count("enable")) voice::set_enabled(truthy(q.at("enable")));
    if (q.count("vol")) voice::set_gain((float)atof(q.at("vol").c_str()));
    if (q.count("coalesce")) combat::set_coalesce(truthy(q.at("coalesce")));
    if (q.count("window")) combat::set_window(atof(q.at("window").c_str()));
    if (q.count("cap")) combat::set_cap(parse_int(q.at("cap"), 4));
    if (q.count("near") || q.count("far") || q.count("floor"))
      world::set_voice_rolloff(q.count("near") ? (float)atof(q.at("near").c_str()) : -1.0f, q.count("far") ? (float)atof(q.at("far").c_str()) : -1.0f, q.count("floor") ? (float)atof(q.at("floor").c_str()) : -1.0f);
    if (q.count("max")) voice::set_max_concurrent(q.count("voice") && q.at("voice") == "zira" ? voice::Which::Zira : voice::Which::Mark, parse_int(q.at("max"), 4));
    if (q.count("say")) {
      voice::Say s;
      s.voice = q.count("voice") && q.at("voice") == "zira" ? voice::Which::Zira : voice::Which::Mark;
      s.text = q.at("say");
      s.pan = q.count("pan") ? (float)atof(q.at("pan").c_str()) : 0.0f;
      s.gain = q.count("gain") ? (float)atof(q.at("gain").c_str()) : 1.0f;
      s.policy = q.count("replace") && truthy(q.at("replace")) ? voice::Policy::Replace : voice::Policy::Overlap;
      s.group = s.voice == voice::Which::Zira ? voice::kGroupSelf : voice::kGroupEnemy;
      voice::say(s);
      return "queued\n";
    }
  }
  if (path == "/hitsay") { if (q.count("on")) combat::set_incoming_hits(true); if (q.count("off")) combat::set_incoming_hits(false); return combat::hit_status(); }
  if (path == "/note") { rooms::note_place(); return "noted\n"; }   // the old T key: append this place to untagged_rooms.txt
  if (path == "/telegraph") {  // ?mode=0..3 (off / your target / highest tier / all) &shape=<name>,0|1 &vol= &ms=100|200 &test=<shape> -- attack telegraph cues (src/telegraph.cpp)
    if (q.count("mode")) telegraph::set_mode((telegraph::Mode)parse_int(q.at("mode"), 3));
    if (q.count("shape")) { std::string v = q.at("shape"); size_t c = v.find(','); for (int i = 0; i < telegraph::kShapes; ++i) if (v.substr(0, c) == telegraph::kShapeNames[i]) telegraph::set_shape_enabled(i, c == std::string::npos || v.substr(c + 1) != "0"); }
    if (q.count("vol")) telegraph::set_volume((float)atof(q.at("vol").c_str()));
    if (q.count("ms")) telegraph::set_variant(parse_int(q.at("ms"), 100));
    if (q.count("test")) telegraph::test(q.at("test"));
    if (q.count("foe")) { unsigned id = (unsigned)parse_int(q.at("foe"), 0); return std::format("is_foe({}) = {} (player id {})\n", id, world::is_foe(id), world::player_id()); }
    return telegraph::status();
  }
  if (path == "/casts") {  // ?lines=N (default 200) | ?clear=1 | ?cb=1 -- skill activations, hits, animation callbacks (src/casts.cpp)
    if (q.count("clear")) casts::clear();
    return casts::dump(q.count("lines") ? parse_int(q.at("lines"), 200) : 200, q.count("cb") > 0);   // ?cb=1: every animation callback (footsteps included)
  }
  if (path == "/combat") {  // /combat?raw=N arms a hex dump of the next N 0x1b events into /log
    if (q.count("raw")) combat::arm_raw_log(parse_int(q.at("raw"), 10));
    return combat::status();
  }
  if (path == "/notify") return notify::status();   // banners + error popups the game showed, and dedupe count
  if (path == "/rewards") return quest_rewards::status();   // the last quest completions and what they handed out (src/quest_rewards.cpp)
  if (path == "/inspect") { std::string s = world::inspect_target(); return s.empty() ? "no target\n" : s + "\n"; }
  if (path == "/findskill") {  // ?id=<character>&record=<records/skills/...> -> the buff/skill name via FindSkillId
    unsigned oid = (unsigned)parse_int(q.count("id") ? q.at("id") : "0", 0);
    std::string rec = q.count("record") ? q.at("record") : "";
    std::string nm = world::buff_name(oid, rec.c_str());
    return nm.empty() ? "not found\n" : nm + "\n";
  }
  if (path == "/effects") return world::effects_dump((unsigned)parse_int(q.count("id") ? q.at("id") : "0", 0));   // ?id=<character> raw + resolved buff list
  if (path == "/speech" || path == "/log" || path == "/voice") {
    auto& ring = path == "/speech" ? speech::history() : path == "/voice" ? voice::history() : log::ring();
    uint64_t since = q.count("since") ? strtoull(q.at("since").c_str(), nullptr, 10) : 0;
    auto [lines, cur] = ring.since(since);
    std::string out = std::format("cursor: {}\n", cur);
    for (auto& [i, l] : lines) out += std::format("{}: {}\n", i, l);
    return out;
  }
  if (path == "/text") {
    std::string out;
    for (auto& it : textcap::snapshot())
      out += std::format("{}\t{}\t{}\t{}\t{:08x}\t{}\t{}\n", it.x, it.y, it.xalign, it.yalign, it.rgba, it.variant, textcap::speakable(it.text));
    return out.empty() ? "(no text)\n" : out;
  }
  if (path == "/say") { std::string t = q.count("text") ? q.at("text") : body; speech::speak(t, truthy(q.count("interrupt") ? q.at("interrupt") : "1")); return "ok\n"; }
  if (path == "/mute") {
    bool on = truthy(q.count("on") ? q.at("on") : "1");
    speech::set_muted(on);
    if (!on) SetEnvironmentVariableW(L"GRIMDARK_MUTE", nullptr);  // or the next hot reload re-mutes (dllmain reads it)
    return std::format("speech_muted={}\n", speech::muted());
  }
  if (path == "/audiomute") {  // the process's WASAPI session (game sounds AND our tones): /audiomute?on=0 to hear it
    bool on = truthy(q.count("on") ? q.at("on") : "1");
    if (!on) SetEnvironmentVariableW(L"GRIMDARK_MUTE", nullptr);
    return std::format("audio_muted={} ok={}\n", on, audio::mute_process(on));
  }
  if (path == "/gamekeys") { hooks::set_game_keys_muted(truthy(q.count("on") ? q.at("on") : "1")); return std::format("game_keys_muted={}\n", hooks::game_keys_muted()); }
  if (path == "/key") {
    int code = q.count("name") ? key_code(q.at("name")) : parse_int(q.count("code") ? q.at("code") : "", -1);
    if (code < 0) { status = 400; return "unknown key\n"; }
    char16_t ch = 0;
    if (q.count("ch") && !q.at("ch").empty()) { std::string c = q.at("ch"); wchar_t w[4]; MultiByteToWideChar(CP_UTF8, 0, c.c_str(), -1, w, 4); ch = (char16_t)w[0]; }
    bool shift = q.count("shift") && truthy(q.at("shift")), ctrl = q.count("ctrl") && truthy(q.at("ctrl")), alt = q.count("alt") && truthy(q.at("alt"));
    hooks::push_key(code, shift, ctrl, alt, ch);
    return std::format("queued key {:#x}\n", code);
  }
  if (path == "/keys") {
    std::string t = q.count("text") ? q.at("text") : body;
    int n = 0;
    for (char c : t) {
      std::string name(1, (char)tolower((unsigned char)c));
      if (c == ' ') name = "space";
      int code = key_code(name);
      if (code < 0) continue;
      hooks::push_key(code, isupper((unsigned char)c) != 0, false, false, (char16_t)c);
      ++n;
    }
    return std::format("queued {} keys\n", n);
  }
  if (path == "/cursor") {
    if (q.count("clear")) { hooks::set_cursor_override(false, 0, 0); return "cursor override cleared\n"; }
    hooks::set_cursor_override(true, (float)atof(q.at("x").c_str()), (float)atof(q.at("y").c_str()));
    return "cursor override set\n";
  }
  if (path == "/buttons") return hooks::button_query_stats();
  if (path == "/travel") {
    if (q.count("preview")) return travel::preview();
    if (q.count("background") && !travel::background_test(q.at("background") == "1")) return "requires GRIMDARK_NOFOCUS=1\n";
    if (q.count("stop")) travel::stop();
    return travel::status();
  }
  if (path == "/gui") return app::gui_dump();
  if (path == "/actions") return app::action_keys();
  if (path == "/action") { std::string k = q.count("key") ? q.at("key") : ""; if (!app::fire_action(k)) { status = 400; return "unknown action; see /actions\n"; } return "fired\n"; }
  if (path == "/keynames") return hooks::button_names(q.count("max") ? parse_int(q.at("max"), 0x120) : 0x120);
  if (path == "/fakeactive") { hooks::set_fake_active(truthy(q.count("on") ? q.at("on") : "1")); return "ok\n"; }
  if (path == "/click") {
    if (!q.count("x") || !q.count("y")) { status = 400; return "need x and y\n"; }
    int button = q.count("button") ? (q.at("button") == "right" ? 2 : q.at("button") == "middle" ? 3 : parse_int(q.at("button"), 1)) : 1;
    hooks::click((float)atof(q.at("x").c_str()), (float)atof(q.at("y").c_str()), button);
    return "queued click\n";
  }
  if (path == "/lootfilter") {   // ?set=<opt>&on=0|1 | ?defaults=<col|-1> | ?all=0|1 (the O latch) | (none) the options + the window's boxes
    if (q.count("set")) {
      int o = parse_int(q.at("set"), -1);
      bool on = parse_int(q.count("on") ? q.at("on") : "1", 1) != 0;
      bool ok = gameapi::set_loot_filter(o, on);
      if (ok) exe_ui::loot_filter_mirror(o, on);
      return ok ? "ok\n" : "failed\n";
    }
    if (q.count("defaults")) return gameapi::loot_filter_defaults(parse_int(q.at("defaults"), -1)) ? "ok\n" : "failed\n";
    if (q.count("all")) {
      if ((parse_int(q.at("all"), 0) != 0) != world::show_all_items()) world::toggle_show_all_items();
      return std::format("show_all={}\n", world::show_all_items());
    }
    std::string out = std::format("show_all={}\n", world::show_all_items()) + gameapi::dump_loot_filter();
    for (const exe_ui::LootFilterBox& b : exe_ui::loot_filter_boxes()) out += std::format("  box opt={:2} checked={} ctrl={}\n", b.option, b.checked, b.ctrl);
    return out;
  }
  if (path == "/inventor") {   // ?tab=0..3 | ?put=<id> | ?take=0|1|2 | ?press=keepitem|keepaddon|removeaugment|dismantle | (none) the window's state
    if (q.count("tab")) return exe_ui::inventor_press_tab(parse_int(q.at("tab"), 0)) ? "ok\n" : "failed\n";
    if (q.count("put")) return exe_ui::inventor_put((unsigned)parse_int(q.at("put"), 0)) ? "ok\n" : "failed\n";
    if (q.count("take")) return exe_ui::inventor_take(parse_int(q.at("take"), 0)) ? "ok\n" : "failed\n";
    if (q.count("press")) {
      const std::string& a = q.at("press");
      exe_ui::InventorAction act = a == "keepitem" ? exe_ui::InventorAction::KeepItem : a == "keepaddon" ? exe_ui::InventorAction::KeepAddon
                                 : a == "removeaugment" ? exe_ui::InventorAction::RemoveAugment : exe_ui::InventorAction::Dismantle;
      return exe_ui::inventor_press(act) ? "ok\n" : "refused\n";
    }
    std::string out = exe_ui::inventor_dump();
    out += std::format("dynamite={} dismantle_unlocked={} money={}\n", gameapi::dynamite_count(), gameapi::dismantle_unlocked(), gameapi::money());
    for (const gameapi::Bag& bag : gameapi::bags())
      for (const gameapi::BagItem& it : bag.items)
        out += std::format("  item {} '{}' equipment={} component={} augment={} class={} salvage={} dismantle={}\n", it.id, it.name, gameapi::is_equipment(it.p), gameapi::has_component(it.p),
                           gameapi::has_augment(it.p), gameapi::item_classification(it.p), gameapi::salvage_cost(it.p), gameapi::dismantle_cost(it.p));
    return out;
  }
  if (path == "/crafting") {   // ?formula=<id> details | ?select=<id> | ?tab=0..4 | ?combine=1 | (none) the window's rows
    if (q.count("formula")) return gameapi::dump_formula((unsigned)parse_int(q.at("formula"), 0));
    if (q.count("select")) return exe_ui::crafting_select((unsigned)parse_int(q.at("select"), 0)) ? "ok\n" : "failed\n";
    if (q.count("tab")) return exe_ui::crafting_press_tab(parse_int(q.at("tab"), 0)) ? "ok\n" : "failed\n";
    if (q.count("combine")) return exe_ui::crafting_combine() ? "ok\n" : "refused\n";
    return exe_ui::crafting_dump();
  }
  if (path == "/pets") {   // ?stance=<pet id>&type=0|1|2 | ?attack=<pet id>&target=<id> | ?move=<pet id>&x=&y=&z= | ?release=<pet id> | (none) list
    if (q.count("stance")) return gameapi::set_pet_stance((unsigned)parse_int(q.at("stance"), 0), parse_int(q.count("type") ? q.at("type") : "1", 1)) ? "ok\n" : "failed\n";
    if (q.count("attack")) return gameapi::pet_attack((unsigned)parse_int(q.at("attack"), 0), (unsigned)parse_int(q.count("target") ? q.at("target") : "0", 0)) ? "ok\n" : "failed\n";
    if (q.count("move")) { world::Vec3 v{(float)atof(q.count("x") ? q.at("x").c_str() : "0"), (float)atof(q.count("y") ? q.at("y").c_str() : "0"), (float)atof(q.count("z") ? q.at("z").c_str() : "0")}; return gameapi::pet_move((unsigned)parse_int(q.at("move"), 0), v) ? "ok\n" : "failed\n"; }
    if (q.count("release")) return gameapi::release_pet((unsigned)parse_int(q.at("release"), 0)) ? "ok\n" : "failed\n";
    return gameapi::dump_pets();
  }
  if (path == "/player") return world::debug_dump();
  if (path == "/classinfo") return world::classinfo_dump();
  if (path == "/scan") {  // /scan?group=0..3&max=40 -- the review cursor's list for a group (enemies, people, bystanders, objects)
    int g = q.count("group") ? parse_int(q.at("group"), 3) : 3;
    float r = q.count("max") ? (float)atof(q.at("max").c_str()) : 40.0f;
    std::string out;
    for (const world::ScanItem& it : world::scan((world::ScanGroup)g, r)) out += std::format("{:6.1f} at ({:.1f},{:.1f}) hour {} id={} {} '{}' {} {}\n", it.dist, it.pos.x, it.pos.z, world::clock_hour(it.pos), it.id, it.cls, it.label, it.record, it.note);
    return out.empty() ? "nothing\n" : out;
  }
  if (path == "/keydown" || path == "/keyup") {  // hold a key across frames: /keydown?name=w ... /keyup?name=w
    int code = q.count("name") ? key_code(q.at("name")) : parse_int(q.count("code") ? q.at("code") : "", -1);
    if (code < 0) { status = 400; return "unknown key\n"; }
    hooks::push_key_event({code, path == "/keyup", false, false, false, 0});
    return std::format("queued {} {:#x}\n", path == "/keyup" ? "up" : "down", code);
  }
  if (path == "/hazard") {   // painted damage ground (src/hazard.cpp): status + knobs; ?time=N times the probes
    if (q.count("time")) return hazard::probe_timing(parse_int(q.at("time"), 10));
    if (q.count("at")) {   // ?at=x,z -> the sector lookup + mesh containment at a world point
      float x = 0, z = 0;
      if (sscanf_s(q.at("at").c_str(), "%f,%f", &x, &z) != 2) return std::string("at=x,z\n");
      world::Vec3 p; world::player_position(p); p.x = x; p.z = z;
      float rate = 0; int type = 0; bool painted = world::hazard_at(p, &rate, &type);
      return std::format("({:.1f}, {:.1f}): painted={} rate={:.2f} type={} on_mesh={}\n", x, z, painted, rate, type, world::mesh_contains(p));
    }
    std::string bad;
    for (const auto& [k, v] : q) if (!hazard::set_param(k, v)) bad += k + " ";
    return (bad.empty() ? std::string() : "unknown: " + bad + "\n") + hazard::status();
  }
  if (path == "/walltones") {
    if (q.count("on")) screens::walltones::set_enabled(truthy(q.at("on")));
    if (q.count("range")) screens::walltones::set_range((float)atof(q.at("range").c_str()));  // world units
    if (q.count("vol")) screens::walltones::set_gain((float)atof(q.at("vol").c_str()));        // 0..1
    if (q.count("lanes")) screens::walltones::set_lanes(parse_int(q.at("lanes"), 2));            // side lanes each way (0 = single ray)
    if (q.count("trim")) {   // loudness trims: trim=off | trim=default | trim=<n|e|s|w>,<dB>
      const std::string& t = q.at("trim");
      if (t == "off") screens::walltones::set_trim(-1, 0);
      else if (t == "default") screens::walltones::set_trim(-2, 0);
      else {
        int dir = -1; float db = 0; char d = 0;
        if (sscanf_s(t.c_str(), "%c,%f", &d, 1, &db) == 2) {
          dir = d == 'n' ? 0 : d == 'e' ? 1 : d == 's' ? 2 : d == 'w' ? 3 : -1;
          if (dir >= 0) screens::walltones::set_trim(dir, db); else { status = 400; return "trim=<n|e|s|w>,<dB>\n"; }
        } else { status = 400; return "trim=off | trim=default | trim=<n|e|s|w>,<dB>\n"; }
      }
    }
    if (q.count("time")) return screens::walltones::probe_timing(parse_int(q.at("time"), 500));  // dev: time the tick's probing
    return screens::walltones::status();
  }
  if (path == "/jkey") { world::mouse_key(1, q.count("down") ? parse_int(q.at("down"), 1) != 0 : true); return "ok\n"; }   // dev: J (left button at the reviewed thing) down/up without the game seeing a J key
  if (path == "/riftgates") {   // the riftgate travel map's rows (dev)
    std::string out = std::format("map_open={}\n", exe_ui::riftgate_map_open());
    for (const exe_ui::Riftgate& g : exe_ui::riftgates())
      out += std::format("  '{}' at ({}, {}, {}) obj={} owner={} uid={:#x}:{:#x}:{:#x}:{:#x} current={}\n", g.name, g.pos[0], g.pos[1], g.pos[2], g.object_id, g.owner, (unsigned)g.uid[0], (unsigned)g.uid[1], (unsigned)g.uid[2], (unsigned)g.uid[3], g.current);
    return out;
  }
  if (path == "/shrines") {   // dev: the character's shrine UIDs vs the offline shrine table (src/shrine_table.h) -- is the UID the chunk GUID?
    gameapi::ShrineUids u = gameapi::shrine_uids();
    auto hex = [](const gameapi::UniqueId& id) { std::string h; for (int i = 0; i < 16; ++i) h += std::format("{:02x}", id.b[i]); return h; };
    std::string out = std::format("discovered={} restored={}\n", u.discovered.size(), u.restored.size());
    for (const gameapi::UniqueId& id : u.discovered) out += "  discovered " + hex(id) + "\n";
    for (const gameapi::UniqueId& id : u.restored) out += "  restored   " + hex(id) + "\n";
    out += "table:\n";
    for (const shrine_table::Shrine& s : shrine_table::kShrines) {
      gameapi::UniqueId id; std::memcpy(id.b, s.guid, 16);
      bool d = std::find(u.discovered.begin(), u.discovered.end(), id) != u.discovered.end(), r = std::find(u.restored.begin(), u.restored.end(), id) != u.restored.end();
      out += std::format("  {} {:28} '{}' chunk {} at ({}, {}, {}) corrupted={} ruined={} discovered={} restored={}\n", hex(id), s.icon, s.area, s.chunk, s.x, s.y, s.z, s.corrupted, s.ruined, d, r);
    }
    return out;
  }
  if (path == "/hud") {   // dev: the HUD rectangles the mouse keys refuse (exe_ui::hud_rects) + a point test ?x=&y=
    std::string out;
    for (const exe_ui::Rect& r : exe_ui::hud_rects()) out += std::format("  rect x={:.1f} y={:.1f} w={:.1f} h={:.1f}\n", r.x, r.y, r.w, r.h);
    if (q.count("x") && q.count("y")) out += std::format("  point ({}, {}) over_hud={}\n", q.at("x"), q.at("y"), exe_ui::point_over_hud((float)atof(q.at("x").c_str()), (float)atof(q.at("y").c_str())));
    return out.empty() ? "no hud rects\n" : out;
  }
  if (path == "/mapnuggets") return exe_ui::map_nuggets_dump(parse_int(q.count("max") ? q.at("max") : "80", 80));   // the aerial map's cached icon nuggets (dev)
  if (path == "/mapmarkers") return world::map_markers_dump();   // the aerial map's icons, named + nearest-first (dev)
  if (path == "/sonar") {   // the sonar field: ?on=0|1 &radius= &vol= &ref= &floor= &pnear= &pfar= &dnear= &dfar= &force= &window= &hash= &grace=
    if (q.count("on")) sonar::set_enabled(truthy(q.at("on")));
    for (const char* k : {"radius", "vol", "ref", "floor", "pnear", "pfar", "dnear", "dfar", "force", "window", "hash", "grace"}) if (q.count(k)) sonar::set_knob(k, (float)atof(q.at(k).c_str()));
    if (q.count("trim")) { std::string t = q.at("trim"); size_t c = t.find(','); if (!sonar::set_trim(t.substr(0, c), c == std::string::npos ? 0.0f : (float)atof(t.c_str() + c + 1))) return "unknown kind\n"; }   // ?trim=<kind>,<dB> | ?trim=all
    return sonar::status();
  }
  if (path == "/pingtime") {  // dev: time the reping navmesh line probe; ?group=0..5 first lands the review cursor
    if (q.count("group")) world::cycle_review((world::ScanGroup)parse_int(q.at("group"), 0), 0, true);
    return world::probe_timing(q.count("n") ? parse_int(q.at("n"), 200) : 200);
  }
  if (path == "/wallcmp")   // dev: A/B flat vs terrain-following wall probe -- /wallcmp?dirs=16&max=15&step=0.5
    return world::wall_compare(q.count("dirs") ? parse_int(q.at("dirs"), 16) : 16,
                               q.count("max") ? (float)atof(q.at("max").c_str()) : 15.0f,
                               q.count("step") ? (float)atof(q.at("step").c_str()) : 0.5f);
  if (path == "/vwindow")   // dev: PutOnFloor vertical window at the feet -- /vwindow?span=20&step=0.5
    return world::nav_vwindow(q.count("span") ? (float)atof(q.at("span").c_str()) : 20.0f,
                              q.count("step") ? (float)atof(q.at("step").c_str()) : 0.5f);
  if (path == "/where") { screens::speak_where(); return "ok\n"; }
  if (path == "/blocks") return world::blocks_dump();
  if (path == "/room") {      // the rooms feature: current place + exits; ?dwell=ms ?untitled=0|1 ?say=1 (re-announce) ?reload=1
    if (q.count("dwell")) rooms::set_dwell_ms(parse_int(q.at("dwell"), 400));
    if (q.count("settle")) rooms::set_settle_ms(parse_int(q.at("settle"), 1000));
    if (q.count("untitled")) rooms::set_say_untitled(truthy(q.at("untitled")));
    if (q.count("reload")) rooms::reload();
    if (q.count("say")) rooms::announce_now();
    return rooms::status();
  }
  if (path == "/settle") return world::settle_status(q.count("radius") ? (float)atof(q.at("radius").c_str()) : 160.f);   // loader idle / chunks loading near the player; ?radius=
  if (path == "/regions") return world::regions_dump(parse_int(q.count("max") ? q.at("max") : "40", 40));   // engine Regions (chunks): name, offset, loaded, portals; ?max=
  if (path == "/portals") return world::portals_dump();   // the player's chunk's portals
  if (path == "/markers") return world::markers_dump();   // Player::GetMarkerUIDs (quest-marker UID list)
  if (path == "/pause") return world::set_paused(q.count("set") ? atoi(q.at("set").c_str()) : -1);   // /pause[?set=0|1]: game time
  if (path == "/teleport") {  // /teleport?x=&z= -- the player, floored (authoring; docs/rooms.md M4)
    if (!q.count("x") || !q.count("z")) { status = 400; return "need x and z\n"; }
    return world::teleport((float)atof(q.at("x").c_str()), (float)atof(q.at("z").c_str()), q.count("check") > 0);   // &check=1: report only
  }
  if (path == "/project" && q.count("pts")) {  // /project?pts=x,z;x,z;... -- ground points to screen (or POST the list)
    std::vector<world::Vec3> pts;
    std::string s = q.at("pts");
    size_t i = 0;
    while (i < s.size()) {
      size_t e = s.find(';', i); if (e == std::string::npos) e = s.size();
      std::string pair = s.substr(i, e - i); size_t c = pair.find(',');
      if (c != std::string::npos) pts.push_back({(float)atof(pair.substr(0, c).c_str()), 0.f, (float)atof(pair.substr(c + 1).c_str())});
      i = e + 1;
    }
    return world::project_points(pts);
  }
  if (path == "/fog") {       // /fog?x=&z=&radius=N -- reveal around a point (the dev character's map)
    if (!q.count("x") || !q.count("z")) { status = 400; return "need x and z\n"; }
    return world::fog_reveal((float)atof(q.at("x").c_str()), (float)atof(q.at("z").c_str()), parse_int(q.count("radius") ? q.at("radius") : "20", 20));
  }
  if (path == "/navprobe") {  // /navprobe?x0=&z0=&x1=&z1=&step=0.5 -- IsPointOnPathMesh over a grid, '#' walkable, rows = z ascending
    auto f = [&](const char* k, float d) { return q.count(k) ? (float)atof(q.at(k).c_str()) : d; };
    return world::navprobe(f("x0", 0), f("z0", 0), f("x1", 0), f("z1", 0), f("step", 0.5f));
  }
  if (path == "/findpath") {  // /findpath?x=&z=[&y=][&f1=][&f2=] -- Player::FindPath from the player to (x,z); raw PathResult + endpoint
    auto f = [&](const char* k, float d) { return q.count(k) ? (float)atof(q.at(k).c_str()) : d; };
    if (!q.count("x") || !q.count("z")) { status = 400; return "need x and z\n"; }
    world::Vec3 me{}; world::player_position(me);
    world::Vec3 out{};
    world::Vec3 target{f("x", 0), f("y", me.y), f("z", 0)};
    int r = world::find_path(target, f("f1", 0.f), f("f2", 0.f), &out);
    std::string s = std::format("findpath to ({:.1f},{:.1f}) f1={} f2={}: result={} endpoint=({:.1f},{:.1f},{:.1f}) dist_to_target={:.1f}\n",
                                f("x", 0), f("z", 0), f("f1", 0.f), f("f2", 0.f), r, out.x, out.y, out.z,
                                std::hypot(out.x - f("x", 0), out.z - f("z", 0)));
    if (q.count("corridor")) {  // &corridor=1 -- the NavManager straight-path corridor used by the direct-exit test
      std::vector<world::Vec3> c;
      bool ok = world::find_path_corridor(target, c);
      s += std::format("corridor: {} ({} points)\n", ok ? "found" : "empty", c.size());
      for (size_t i = 0; i < c.size(); ++i) s += std::format("  [{}] ({:.1f}, {:.1f}, {:.1f})\n", i, c[i].x, c[i].y, c[i].z);
    }
    return s;
  }
  if (path == "/conv") return world::conversation_dump();
  if (path == "/ui") {                                    // the exe's menu widget tree (framework A); ?char=<index> selects a main-menu character
    if (q.count("char")) { exe_ui::MainMenu mm = exe_ui::main_menu(); return mm && mm.select_character(parse_int(q.at("char"), -1)) ? "ok\n" : "failed\n"; }
    if (q.count("chars")) {   // the picker's characters: index, name, level, class tag, hardcore, and which one is selected
      exe_ui::MainMenu mm = exe_ui::main_menu();
      if (!mm) return "no main menu\n";
      std::string out; int sel = mm.selected_character(); size_t i = 0;
      for (const exe_ui::MainMenu::Character& c : mm.characters()) { out += std::format("{} {}'{}' level {} class={} hardcore={}\n", i, (int)i == sel ? "* " : "", c.name, c.level, c.class_tag, (int)c.hardcore); ++i; }
      return out.empty() ? "no characters\n" : out;
    }
    return exe_ui::ui_dump();
  }
  if (path == "/ui/activate") {                           // /ui/activate?ptr=0x... presses a framework A button through its listeners
    uintptr_t p = q.count("ptr") ? (uintptr_t)strtoull(q.at("ptr").c_str(), nullptr, 0) : 0;
    return exe_ui::activate_ptr(p) ? "activated\n" : "not a button in the current tree (see /ui)\n";
  }
  // ---- the in-world windows' model (src/gameapi.h) ----
  if (path == "/quests") { if (q.count("complete")) return gameapi::complete_quest_task((void*)strtoull(q.at("complete").c_str(), nullptr, 0), parse_int(q.count("task") ? q.at("task") : "0", 0)) ? "ok\n" : "failed\n"; if (q.count("track")) return gameapi::set_quest_tracked((void*)strtoull(q.at("track").c_str(), nullptr, 0), !q.count("off")) ? "ok\n" : "failed\n"; return gameapi::dump_quests(parse_int(q.count("filter") ? q.at("filter") : "0", 0)); }
  if (path == "/objectives") { std::string out; for (const std::string& l : gameapi::objectives()) out += l + "\n"; return out.empty() ? "no objectives\n" : out; }
  if (path == "/factions") return gameapi::dump_factions();
  if (path == "/illusionist") return exe_ui::illusionist_dump();
  if (path == "/hotbar") {   // ?assign=<slot index>&skill=<id> | ?primary=<id> | ?secondary=<id> | ?activate=<index> | ?tip=<index> | ?base=&stride= (quickbar layout knob)
    if (q.count("base")) screens::set_quickbar_base((unsigned)parse_int(q.at("base"), 0), (unsigned)parse_int(q.count("stride") ? q.at("stride") : "10", 10));
    if (q.count("assign")) return gameapi::assign_skill_to_slot((unsigned)parse_int(q.at("assign"), 0), (unsigned)parse_int(q.count("skill") ? q.at("skill") : "0", 0)) ? "ok\n" : "failed\n";
    if (q.count("primary")) return gameapi::set_primary_skill((unsigned)parse_int(q.at("primary"), 0)) ? "ok\n" : "failed\n";
    if (q.count("secondary")) return gameapi::set_secondary_skill((unsigned)parse_int(q.at("secondary"), 0)) ? "ok\n" : "failed\n";
    if (q.count("activate")) return gameapi::activate_hotslot((unsigned)parse_int(q.at("activate"), 0)) ? "ok\n" : "failed\n";
    if (q.count("tip")) { std::string out; for (const std::string& l : gameapi::hotslot_tooltip((unsigned)parse_int(q.at("tip"), 0))) out += l + "\n"; return out.empty() ? "no text\n" : out; }
    return gameapi::dump_hotslots();
  }
  if (path == "/lore") { if (q.count("read")) { std::string out; for (const std::string& l : gameapi::note_text(gameapi::object_by_id((unsigned)parse_int(q.at("read"), 0)))) out += l + "\n"; return out.empty() ? "no text\n" : out; } return gameapi::dump_lore(); }
  if (path == "/shrine") {   // dev: both shrine windows (ruined +0x7da50, desecrated +0x7f6f8): the shrine object + offering names
    std::string out;
    for (auto [label, off] : {std::pair{"ruined", exe_ui::ingame::kShrine}, std::pair{"desecrated", exe_ui::ingame::kShrineCorrupted}}) {
      exe_ui::WindowB w = exe_ui::ingame_window(off);
      unsigned id = 0; if (w) exe_ui::peek_u32((char*)w.p + 0xa4, id);
      out += std::format("{} window={} visible={} shrine_id={} restored={}\n", label, w.p, w ? w.visible() : false, id, world::shrine_restored(id));
      for (const std::string& s : gameapi::shrine_offerings(id)) out += "  offering: " + s + "\n";
    }
    return out;
  }
  if (path == "/vendor") {   // ?id=<market id candidate>: the engine's market map + market_stock(id) (dev)
    std::string v;
    for (unsigned off : {exe_ui::ingame::kMarket, exe_ui::ingame::kFactionVendor}) {
      exe_ui::WindowB w = exe_ui::ingame_window(off);
      v += std::format("window +{:#x}: visible={} market_id={} selected_type={} merchant_faction={}\n", off, w.visible(), exe_ui::vendor_market_id(w), exe_ui::vendor_selected_type(w), gameapi::merchant_faction(exe_ui::vendor_market_id(w)));
      for (const exe_ui::VendorTab& t : exe_ui::vendor_tabs(w)) v += std::format("  tab index={} type={} button={} disabled={}\n", t.index, t.type, t.button, t.disabled);
    }
    for (const char* tag : {"tagFactionStateFriend1", "tagFactionStateFriend2", "tagFactionStateFriend4", "tagFactionStateFriend5"}) v += std::format("  {} -> {}\n", tag, gameapi::faction_level_value(tag));
    return v + gameapi::vendor_dump(q.count("id") ? (unsigned)parse_int(q.at("id"), 0) : 0u);
  }
  if (path == "/inv") {      // ?tip=<id>[&simple=1] | ?use=<id>[&source=N] | ?drop=<id> | ?unequip=<loc> | ?equip=<id>&loc=<loc> | ?bag=<n> | ?source=N (the UseItem ItemSource knob)
    if (q.count("source")) screens::set_bag_item_source(parse_int(q.at("source"), 0));
    if (q.count("tip")) { std::string out; for (const std::string& l : gameapi::item_tooltip(gameapi::object_by_id((unsigned)parse_int(q.at("tip"), 0)), q.count("simple"), q.count("details"))) out += l + "\n"; return out.empty() ? "no text\n" : out; }
    if (q.count("use")) return gameapi::use_item((unsigned)parse_int(q.at("use"), 0), screens::bag_item_source()) ? "ok\n" : "failed\n";
    if (q.count("drop")) return gameapi::drop_item((unsigned)parse_int(q.at("drop"), 0)) ? "ok\n" : "failed\n";
    if (q.count("unequip")) return gameapi::unequip(parse_int(q.at("unequip"), 0)) ? "ok\n" : "failed\n";
    if (q.count("equip")) return gameapi::equip((unsigned)parse_int(q.at("equip"), 0), parse_int(q.count("loc") ? q.at("loc") : "0", 0)) ? "ok\n" : "failed\n";
    if (q.count("bag")) return gameapi::select_bag(parse_int(q.at("bag"), 0)) ? "ok\n" : "failed\n";
    if (q.count("compat")) {   // ?compat=<component id> -> is it a component + the item ids it can attach to (with names)
      unsigned cid = (unsigned)parse_int(q.at("compat"), 0);
      std::string out = std::format("component {} is_component={}\n", cid, gameapi::is_component(cid));
      for (unsigned tid : gameapi::compatible_items(cid)) out += std::format("  target {} '{}'\n", tid, gameapi::item_name(gameapi::object_by_id(tid)));
      return out;
    }
    if (q.count("attach")) return gameapi::attach_component((unsigned)parse_int(q.at("attach"), 0), (unsigned)parse_int(q.count("target") ? q.at("target") : "0", 0), screens::bag_item_source()) ? "ok\n" : "failed\n";   // ?attach=<component>&target=<item>
    return std::format("item source knob {}\n", screens::bag_item_source()) + gameapi::dump_bags() + gameapi::dump_equipment();
  }
  if (path == "/skills") {   // ?tip=<id> | ?learn=<id> | ?refund=<id> | ?attr=1..3 (spend an attribute point) | ?pane=<mastery enum|80>&tab=0|1
    if (q.count("attr")) return gameapi::spend_attribute_point(parse_int(q.at("attr"), 0)) ? "ok\n" : "failed\n";
    if (q.count("pane")) return exe_ui::skills_set_pane(parse_int(q.count("tab") ? q.at("tab") : "0", 0), parse_int(q.at("pane"), 0)) ? "ok\n" : "failed\n";
    if (q.count("pickup")) return gameapi::pickup_item((unsigned)parse_int(q.at("pickup"), 0)) ? "ok\n" : "failed\n";
    if (q.count("tip")) { std::string out; for (const std::string& l : gameapi::skill_tooltip(gameapi::object_by_id((unsigned)parse_int(q.at("tip"), 0)))) out += l + "\n"; return out.empty() ? "no text\n" : out; }
    if (q.count("learn")) return gameapi::learn_skill(gameapi::object_by_id((unsigned)parse_int(q.at("learn"), 0))) ? "ok\n" : "failed\n";
    if (q.count("refund")) return gameapi::refund_skill(gameapi::object_by_id((unsigned)parse_int(q.at("refund"), 0))) ? "ok\n" : "failed\n";
    if (q.count("itemskills")) return gameapi::dump_item_skills();
    if (q.count("panedump")) return exe_ui::skills_pane_dump();   // the current mastery pane's icon entries
    if (q.count("press")) return exe_ui::skills_press_skill((unsigned)parse_int(q.at("press"), 0)) ? "ok\n" : "failed\n";
    return gameapi::dump_skills();
  }
  if (path == "/sheet") return gameapi::dump_sheet();
  if (path == "/masteries") {   // every mastery's tree, tooltips at level 0 and max + aim (tools/gen_masteries_doc.py)
    return gameapi::dump_masteries([](const void* s) -> std::string {
      std::string_view word;
      switch (world::skill_aim(s)) {
        case world::SkillAim::SelfCast: word = strings::kAimSelf; break;
        case world::SkillAim::AroundYou: word = strings::kAimAround; break;
        case world::SkillAim::AtPoint: word = strings::kAimPoint; break;
        case world::SkillAim::AtTarget: word = strings::kAimTarget; break;
        default: break;
      }
      return std::format("{}|{}|{}", word, world::skill_target_type(s), world::object_class_name(s));
    });
  }
  if (path == "/devotion") {   // ?take=<star skill id> | ?tip=<star skill id> | ?hosts=<power id> | ?bind=<power id>&host=<skill id|0>
    if (q.count("take")) { bool done = false; bool ok = gameapi::take_star((unsigned)parse_int(q.at("take"), 0), done); return std::format("{} completed={}\n", ok ? "ok" : "failed", done); }
    if (q.count("why")) { unsigned id = (unsigned)parse_int(q.at("why"), 0); std::vector<gameapi::DevotionConstellation> all = gameapi::constellations(); for (const auto& c : all) for (const auto& s : c.stars) if (s.skill_id == id) return std::format("take: '{}'  reclaim: '{}'\n", gameapi::can_take_star(c, s), gameapi::can_reclaim_star(c, s, all)); return "no such star\n"; }
    if (q.count("reclaim")) { bool un = false; bool ok = gameapi::reclaim_star((unsigned)parse_int(q.at("reclaim"), 0), un); return std::format("{} uncompleted={}\n", ok ? "ok" : "failed", un); }
    if (q.count("tip")) { std::string out; for (const std::string& l : gameapi::star_tooltip((unsigned)parse_int(q.at("tip"), 0))) out += l + "\n"; return out.empty() ? "no text\n" : out; }
    if (q.count("hosts")) { std::string out; for (const gameapi::SkillInfo& s : gameapi::power_host_candidates((unsigned)parse_int(q.at("hosts"), 0))) out += std::format("  id={} '{}' lvl {}\n", s.id, s.name, s.level); return out.empty() ? "no candidates\n" : out; }
    if (q.count("bind")) { std::string replaced; bool ok = gameapi::bind_power((unsigned)parse_int(q.at("bind"), 0), (unsigned)parse_int(q.count("host") ? q.at("host") : "0", 0), &replaced); return std::format("{} replaced='{}'\n", ok ? "ok" : "failed", replaced); }
    return gameapi::dump_devotion();
  }
  if (path == "/lua") { std::string code = q.count("code") ? q.at("code") : body; if (code.empty()) return "?code= or POST body\n"; return gameapi::lua_run(code) ? "ok\n" : "failed (see /log)\n"; }   // dev: a chunk in the game's Lua state
  if (path == "/cheat") { if (q.count("xp")) return gameapi::dev_add_experience((unsigned)parse_int(q.at("xp"), 0)) ? "ok\n" : "failed\n"; if (q.count("bits")) return gameapi::dev_add_money((unsigned)parse_int(q.at("bits"), 0)) ? "ok\n" : "failed\n"; if (q.count("devotion")) return gameapi::dev_add_devotion((unsigned)parse_int(q.at("devotion"), 0)) ? "ok\n" : "failed\n"; if (q.count("aether")) return gameapi::dev_add_aether((unsigned)parse_int(q.at("aether"), 0)) ? "ok\n" : "failed\n"; return std::string("?xp=N | ?bits=N | ?devotion=N | ?aether=N\n"); }
  if (path == "/reclaim") return gameapi::dev_open_skill_reclaim() ? "ok\n" : "failed\n";   // open skills in spirit-guide reclaim mode
  if (path == "/obj" && q.count("find")) return gameapi::find_objects(q.at("find"), q.count("max") ? (size_t)parse_int(q.at("max"), 50) : 50);
  if (path == "/obj") return q.count("id") ? gameapi::dump_object((unsigned)parse_int(q.at("id"), 0)) : gameapi::dump_objects_stats();
  if (path == "/loc2") return q.count("tag") ? gameapi::localize(q.at("tag")) + "\n" : std::string("?tag=\n");
  if (path == "/ingame") { if (q.count("action")) return exe_ui::ingame_key_action(parse_int(q.at("action"), 0)) ? "ok\n" : "failed\n"; return exe_ui::ingame_dump(); }    // InGameUI's windows and the prompt box (framework B)
  if (path == "/peek") {                                 // /peek?ptr=0x...&n=256 -- hex dump (dev; SEH-guarded)
    uintptr_t p = q.count("ptr") ? (uintptr_t)strtoull(q.at("ptr").c_str(), nullptr, 0) : 0;
    return exe_ui::peek(p, q.count("n") ? parse_int(q.at("n"), 256) : 256);
  }
  if (path == "/convwin") {                              // the game's conversation window: speaker/speech elements, rows with their steps
    exe_ui::ConvWindow w = exe_ui::conv_window();
    if (!w) return "no conversation window\n";
    std::string out = std::format("window {} open={} rect=({:.0f},{:.0f} {:.0f}x{:.0f}) speaker='{}' page='{}'\nspeech='{}'\n", w.p, (int)w.open(), w.rect().x, w.rect().y, w.rect().w, w.rect().h, w.speaker(), w.page_text(), w.speech());
    for (exe_ui::ConvRow r : w.rows()) out += std::format("  row {} step={} rect=({:.0f},{:.0f} {:.0f}x{:.0f}) '{}'\n", r.p, r.step(), r.rect().x, r.rect().y, r.rect().w, r.rect().h, r.text());
    out += exe_ui::conv_elements_dump();
    return out;
  }
  if (path == "/tips") {                                 // the game's tip manager: live tips with kind/state/lines; /tips?dismiss=1 closes tutorial tips
    std::string out;
    for (exe_ui::Tip t : exe_ui::tips()) {
      out += std::format("tip {} kind={} state={} page={}\n", t.p, t.kind(), t.state(), t.page());
      for (const std::string& l : t.lines()) out += "  | " + l + "\n";
      if (q.count("dismiss") && t.kind() == 1) t.dismiss();
    }
    return out.empty() ? "no tips\n" : out;
  }
  if (path == "/loc") return hooks::localize(q.count("tag") ? q.at("tag").c_str() : "") + "\n";  // /loc?tag=tagVideoTitle
  if (path == "/dialog") {                                // /dialog?answer=yes|no|okay answers the game's message box through DialogManager
    if (q.count("answer")) { std::string a = q.at("answer"); return exe_ui::answer_dialog(a == "yes" || a == "okay") ? "answered\n" : "no dialog open\n"; }
    return exe_ui::dialog_dump();
  }
  if (path == "/los") return q.count("id") ? world::los_dump((unsigned)strtoul(q.at("id").c_str(), nullptr, 10)) : std::string("need id\n");   // camera LOS test
  if (path == "/entities") return world::entities_dump(q.count("max") ? (float)atof(q.at("max").c_str()) : 40.0f, q.count("src") && q.at("src") == "frustum");   // ?src=frustum: last frame's render set
  if (path == "/lock") {  // /lock?id=N parks the cursor over the entity each frame; /lock?off=1 releases; no args: report
    if (q.count("off")) world::unlock_target();
    else if (q.count("id")) { if (!world::lock_target((unsigned)strtoul(q.at("id").c_str(), nullptr, 10))) return "entity not found near the player\n"; }
    return std::format("locked={}\n", world::locked_target()) + world::lock_dump() + world::target_dump();
  }
  if (path == "/freecursor") {  // ?key=w|a|s|d[&n=N] steps the free cursor; ?mode=toggle flips grid/polar; ?off=1 unlocks; no args: report
    if (q.count("off")) world::unlock_target();
    if (q.count("mode")) return world::toggle_cursor_mode() + "\n" + world::free_cursor_dump();
    if (q.count("key")) {
      std::string k = q.at("key");
      gd::core::CursorKey dir = k == "w" ? gd::core::CursorKey::Forward : k == "s" ? gd::core::CursorKey::Back : k == "a" ? gd::core::CursorKey::Left : gd::core::CursorKey::Right;
      int n = q.count("n") ? parse_int(q.at("n"), 1) : 1;
      for (int i = 0; i < n; ++i) if (!world::free_cursor_step(dir)) return "not in the world\n";
    }
    return world::free_cursor_dump() + world::lock_dump();
  }
  if (path == "/project") return world::project_dump(q.count("id") ? (unsigned)strtoul(q.at("id").c_str(), nullptr, 10) : 0);
  if (path == "/target") {  // /target?id=N sets the combat enemy; /target?clear=1; no args: report
    if (q.count("clear")) world::clear_target();
    else if (q.count("id")) { if (!world::set_target((unsigned)strtoul(q.at("id").c_str(), nullptr, 10))) return "no controller\n"; }
    return world::target_dump();
  }
  if (path == "/nav") {  // free distance along a direction: /nav?dx=1&dz=0&max=15&step=0.5
    float dx = q.count("dx") ? (float)atof(q.at("dx").c_str()) : 1, dz = q.count("dz") ? (float)atof(q.at("dz").c_str()) : 0;
    float len = std::sqrt(dx * dx + dz * dz); if (len > 0) { dx /= len; dz /= len; }
    float mx = q.count("max") ? (float)atof(q.at("max").c_str()) : 15.0f, st = q.count("step") ? (float)atof(q.at("step").c_str()) : 0.5f;
    return std::format("free={:.2f}\n", world::free_distance(dx, dz, mx, st));
  }
  if (path == "/tone") {  // /tone?freq=440&ms=300&vol=0.5&pan=0  (ms=0: continuous tone id=99 until /tone?stop=1)
    if (q.count("stop")) { audio::stop_tone(99); return "stopped\n"; }
    float f = q.count("freq") ? (float)atof(q.at("freq").c_str()) : 440, v = q.count("vol") ? (float)atof(q.at("vol").c_str()) : 0.5f, pn = q.count("pan") ? (float)atof(q.at("pan").c_str()) : 0;
    int ms = q.count("ms") ? parse_int(q.at("ms"), 300) : 300;
    if (ms > 0) audio::beep(f, ms, v, pn); else audio::set_tone(99, f, v, pn);
    return std::format("audio_ready={}\n", audio::ready());
  }
  if (path == "/mouse") {
    hooks::SynthMouse m{};
    m.type = parse_int(q.count("type") ? q.at("type") : "9", 9);
    m.x = q.count("x") ? (float)atof(q.at("x").c_str()) : 0; m.y = q.count("y") ? (float)atof(q.at("y").c_str()) : 0;
    hooks::push_mouse_event(m);
    return "queued mouse event\n";
  }
  status = 404;
  return "unknown route\n";
}

struct JobCtx { const std::string* path; const std::map<std::string, std::string>* q; const std::string* body; int* status; std::string* out; };
static void run_job(JobCtx* c) { *c->out = handle(*c->path, *c->q, *c->body, *c->status); }
static int seh_filter(EXCEPTION_POINTERS* ep, void** addr, DWORD* code) {
  *addr = ep->ExceptionRecord->ExceptionAddress;
  *code = ep->ExceptionRecord->ExceptionCode;
  return EXCEPTION_EXECUTE_HANDLER;
}
static DWORD run_job_seh(JobCtx* c, void** addr) {  // no C++ objects with destructors in here (SEH rule)
  DWORD code = 0;
  __try { run_job(c); } __except (seh_filter(GetExceptionInformation(), addr, &code)) {}
  return code;
}
static std::string module_offset(void* addr) {
  HMODULE m = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)addr, &m);
  if (!m) return std::format("{}", addr);
  char path[MAX_PATH]; GetModuleFileNameA(m, path, MAX_PATH);
  const char* base = strrchr(path, '\\'); base = base ? base + 1 : path;
  return std::format("{}+{:#x}", base, (uintptr_t)addr - (uintptr_t)m);
}

static void serve(SOCKET c) {
  std::string req;
  char buf[4096];
  // read headers (and whatever body arrived with them)
  while (req.find("\r\n\r\n") == std::string::npos) {
    int n = recv(c, buf, sizeof buf, 0);
    if (n <= 0) return;
    req.append(buf, n);
    if (req.size() > 1 << 20) return;
  }
  size_t hdr_end = req.find("\r\n\r\n");
  std::string head = req.substr(0, hdr_end), body = req.substr(hdr_end + 4);
  size_t cl = head.find("Content-Length:");
  if (cl != std::string::npos) {
    size_t want = strtoul(head.c_str() + cl + 15, nullptr, 10);
    while (body.size() < want) { int n = recv(c, buf, sizeof buf, 0); if (n <= 0) break; body.append(buf, n); }
  }
  size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1);
  std::string target = head.substr(sp1 + 1, sp2 - sp1 - 1);
  size_t qm = target.find('?');
  std::string path = target.substr(0, qm);
  auto query = parse_query(qm == std::string::npos ? "" : target.substr(qm + 1));
  // Routes poke at game objects with guessed layouts; a fault inside one must not take the game down.
  // The SEH frame lives in a function without C++ objects (run_job_seh); the report names the module.
  // The job owns everything it touches (shared with this frame): a route that outlives the 8 s timeout
  // still runs to completion later on the game thread and must not write into a dead stack frame.
  struct Owned { std::string path, body; std::map<std::string, std::string> query; int status = 500; std::string out; };
  auto owned = std::make_shared<Owned>();
  owned->path = path; owned->body = body; owned->query = query;
  bool ran = hooks::run_on_game_thread([owned] {
    JobCtx ctx{&owned->path, &owned->query, &owned->body, &owned->status, &owned->out};
    void* addr = nullptr;
    DWORD code = run_job_seh(&ctx, &addr);
    if (code) {
      owned->status = 500;
      owned->out = std::format("route faulted: exception {:#x} at {}\n", code, module_offset(addr));
      log::write(owned->out);
    }
  }, 8000);
  int status = 504;
  std::string out = "game thread did not run the job within 8 s (still queued/running; not ticking? unfocused with inactiveUpdateRate=0?)\n";
  if (ran) { status = owned->status; out = owned->out; }   // not touched otherwise: the job may still be writing it
  std::string resp = std::format("HTTP/1.1 {} {}\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                                 status, status == 200 ? "OK" : "ERR", out.size()) + out;
  send(c, resp.data(), (int)resp.size(), 0);
}

static void loop(int port) {
  WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
  g_listen = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  BOOL yes = TRUE; setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);
  if (bind(g_listen, (sockaddr*)&a, sizeof a) != 0 || listen(g_listen, 8) != 0) { log::writef("devserver: bind/listen on {} failed: {}", port, WSAGetLastError()); return; }
  log::writef("devserver: listening on 127.0.0.1:{}", port);
  while (g_run) {
    SOCKET c = accept(g_listen, nullptr, nullptr);
    if (c == INVALID_SOCKET) break;
    DWORD to = 10000; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);
    serve(c);
    closesocket(c);
  }
}

static int g_port = 8791;

void start(int port) {
  if (g_run) return;
  if (g_thread.joinable()) g_thread.join();   // a previous stop(false) left the finished thread to collect
  g_port = port;
  g_run = true;
  g_thread = std::thread(loop, port);
}
void stop(bool wait) {
  g_run = false;
  if (g_listen != INVALID_SOCKET) { closesocket(g_listen); g_listen = INVALID_SOCKET; }
  // The game thread must not join: a request in flight may be waiting for it to run its job (the next start()
  // collects the thread instead; the loop leaves as soon as that request completes).
  if (wait && g_thread.joinable()) g_thread.join();
}
bool running() { return g_run; }
int port() { return g_port; }
}  // namespace gd::dev
