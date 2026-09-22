#include "combat.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <mutex>
#include <vector>
#include "app.h"
#include "core/combat_coalesce.h"
#include "core/combat_text.h"
#include "core/message_builder.h"
#include "core/strings.h"
#include "core/threshold_watcher.h"
#include "gameapi.h"
#include "gd_names.h"
#include "hooks.h"
#include "log.h"
#include "msvc_string.h"
#include "settings.h"
#include "speech.h"
#include "voice.h"
#include "world.h"

namespace gd::combat {
namespace {
using namespace gd::names;
using gd::core::MessageBuilder;
constexpr unsigned kCombatTextEvent = 0x1b;

// GameEvent for type 0x1b, from CombatManager::TakeAttack's stack record (static RE 2026-08-22, Game.dll
// +0x10abd4..: type, id, the victim's HeadEffect position, the style variable name, the drawn text, the
// text class 0x46 / 0x85 crit, the scale). Read-only, SEH-guarded, confirmed live via /combat?raw=.
struct CombatTextEvent {
  uint32_t type;            // +0x00 = 0x1b
  uint32_t id;              // +0x04 attacker/entity id (0 on the miss path)
  unsigned char wv[0x18];   // +0x08 WorldVec3 { Region* +0, Vec3 +8 }
  MsvcStringA style;        // +0x20 "missStyle" / "hitStyle" / "petHitStyle" ...
  MsvcStringW text;         // +0x40 as drawn: "Miss" / "123" / "456 (x1.50)"
  uint32_t text_class;      // +0x60 0x46 normal, 0x85 crit
  float scale;              // +0x64
};
static_assert(offsetof(CombatTextEvent, style) == 0x20);
static_assert(offsetof(CombatTextEvent, text) == 0x40);
static_assert(offsetof(CombatTextEvent, text_class) == 0x60);
static_assert(sizeof(CombatTextEvent) == 0x68);

// What the hook keeps: plain data only (SEH rule), handed to tick() on the same thread.
struct RawEvent {
  unsigned id = 0, text_class = 0;
  float scale = 0;
  char style[32] = {};
  char16_t text[64] = {};
  unsigned char wv[0x18] = {};
  bool has_region = false;
  double t = 0;
};

// A debuff application caught by the Character::DebufTarget hook: caster, victim, and the buff's record path
// (SkillBuffTransfer+0x00 -- its identity; the struct holds no skill id, +0x48 is the caster). Names/positions
// are resolved on the game thread in tick(), not in the hook.
struct RawDebuff {
  unsigned caster = 0, victim = 0; double t = 0;
  char record[160] = {};
};

std::vector<gd::hooks::Hook> g_hooks;
std::deque<RawEvent> g_pending;          // game thread only
std::deque<RawDebuff> g_pending_debuff;  // game thread only
std::atomic<uint64_t> g_seen{0}, g_parsed{0}, g_spoken{0}, g_bad{0};
std::atomic<uint64_t> g_debuffs{0};
std::atomic<uint64_t> g_exp_pending{0}, g_exp_total{0};   // XP (polled delta) attributed to kills, this burst / lifetime
constexpr double kExpWindow = 0.5;                         // coalesce a kill burst / capture its XP within this window
std::atomic<int> g_raw_log{0};
int g_outgoing = 2; bool g_incoming = true;   // the T overlay's switches (loaded from settings in install): outgoing 0 off / 1 brief / 2 full
// Incoming hit announcements (a T switch): say "hit" for every attack that reaches the player, from the victim-side
// resolver CombatManager::TakeAttack (runs before mitigation, so invincibility does not hide it). Counted per tick
// and voiced once per tick. /hitsay prints the counters.
bool g_say_hit = true;
std::atomic<int> g_hit_true{0}, g_hit_false{0};   // TakeAttack results on the player this tick
std::atomic<uint64_t> g_hits_total{0};
unsigned (*g_get_object_id)(const void*) = nullptr;
gd::core::CombatCoalescer g_coalescer;
gd::core::ThresholdWatcher g_health(0.10);
enum class PlayerLifeState { Unknown, Alive, Dead };
PlayerLifeState g_player_life = PlayerLifeState::Unknown;
unsigned g_life_player_id = 0;
std::atomic<uint64_t> g_deaths{0};
std::deque<std::string> g_recent;        // last parsed lines, for /combat
std::mutex g_recent_mu;

void note(std::string line) {
  std::lock_guard lk(g_recent_mu);
  g_recent.push_back(std::move(line));
  while (g_recent.size() > 20) g_recent.pop_front();
}

bool bad_ptr(const void* p, size_t n) { return IsBadReadPtr(p, n) != 0; }

// Bounded copies out of the event; the strings may be heap-backed (the crit text is longer than the SSO).
bool read_event_body(const void* ev, RawEvent& out) {
  const CombatTextEvent* e = (const CombatTextEvent*)ev;
  out.id = e->id; out.text_class = e->text_class; out.scale = e->scale;
  memcpy(out.wv, e->wv, sizeof out.wv);
  void* region; memcpy(&region, e->wv, sizeof region);
  out.has_region = region != nullptr;
  size_t n = e->style.size < 31 ? e->style.size : 31;
  const char* sd = e->style.data();
  if (n && bad_ptr(sd, n)) return false;
  memcpy(out.style, sd, n); out.style[n] = 0;
  size_t m = e->text.size < 63 ? e->text.size : 63;
  const char16_t* td = e->text.data();
  if (m && bad_ptr(td, m * 2)) return false;
  memcpy(out.text, td, m * 2); out.text[m] = 0;
  return true;
}
bool read_event(const void* ev, RawEvent& out) {
  __try {
    if (bad_ptr(ev, sizeof(CombatTextEvent))) return false;
    return read_event_body(ev, out);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
bool copy_raw(const void* ev, unsigned char* b, size_t n) {
  __try { memcpy(b, ev, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void dump_raw(const void* ev) {
  unsigned char b[0x80] = {};
  if (!copy_raw(ev, b, sizeof b)) { log::write("combat: raw event unreadable"); return; }
  std::string hex;
  for (size_t i = 0; i < sizeof b; ++i) { if (i && i % 16 == 0) hex += " | "; hex += std::format("{:02x}", b[i]); }
  log::writef("combat: raw 0x1b at {}: {}", ev, hex);
}

typedef void (*EventManagerSend_t)(void*, const void*, unsigned);
static EventManagerSend_t EventManagerSend_hook_orig;
static void EventManagerSend_hook(void* self, const void* ev, unsigned type) {
  EventManagerSend_hook_orig(self, ev, type);  // the game's behaviour first, always
  if (type != kCombatTextEvent || !ev) return;
  ++g_seen;
  if (g_raw_log > 0) { --g_raw_log; dump_raw(ev); }
  RawEvent r;
  if (!read_event(ev, r)) { ++g_bad; return; }
  r.t = app::now();
  if (g_pending.size() < 256) g_pending.push_back(r);
}

// Brief outgoing: one word per coalesced event -- crit / hit for a number, miss for the game's Miss and Dodge, blocked
// for Block; an effect-only token (no number, no word) says nothing.
std::string brief_line(const gd::core::CombatCoalescer::Out& o) {
  if (o.is_number) return std::string(o.crit ? strings::kCrit : strings::kHit);
  if (o.word == "Miss" || o.word == "Dodge") return std::string(strings::kMiss);
  if (o.word == "Block") return std::string(strings::kBlockedHit);
  return std::string();
}
std::string hit_line(const gd::core::CombatCoalescer::Out& o) {
  MessageBuilder m;
  if (o.is_number) strings::push_combat_hit(m, std::format("{:.0f}", o.amount), o.crit);
  else if (!o.word.empty()) strings::push_combat_word(m, o.word);
  for (const std::string& t : o.tags) m.fragment(t);   // "12 frozen" / "456 crit frozen" / just "frozen"
  return m.build();
}

// Character::DebufTarget(Character& victim, bool notify, SkillBuffTransfer const&, ...): this = caster, arg1 =
// victim, arg3 = the transfer whose +0x48 is the buff's skill id (RE 2026-08-25). We record ids only and resolve
// names/positions in tick(); a debuff a monster applies to the player is spoken in Zira panned to the caster,
// and one the player applies to an enemy rides that enemy's damage number ("12 frozen") via the coalescer.
bool read_debuff(void* self, void* victim, const void* transfer, RawDebuff& out) {
  __try {
    out.caster = g_get_object_id ? g_get_object_id(self) : 0;
    out.victim = g_get_object_id ? g_get_object_id(victim) : 0;
    const MsvcStringA* rec = (const MsvcStringA*)transfer;   // record path at +0x00 (the buff's identity)
    size_t len = rec->size < sizeof(out.record) - 1 ? rec->size : sizeof(out.record) - 1;
    const char* d = rec->data();
    if (len && !bad_ptr(d, len)) { memcpy(out.record, d, len); out.record[len] = 0; }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
typedef void (*DebufTarget_t)(void*, void*, bool, const void*, const void*, int);
static DebufTarget_t DebufTarget_hook_orig;
static void DebufTarget_hook(void* self, void* victim, bool notify, const void* transfer, const void* weapons, int hand) {
  DebufTarget_hook_orig(self, victim, notify, transfer, weapons, hand);   // the game's behaviour first, always
  if (!self || !victim || !transfer) return;
  ++g_debuffs;
  RawDebuff d;
  if (!read_debuff(self, victim, transfer, d)) return;
  d.t = app::now();
  if (g_pending_debuff.size() < 256) g_pending_debuff.push_back(d);
}

// Kill feedback: PlayStats::IncrementKills(a1, a2, MonsterClassification, bool) fires once per player-credited
// kill (confirmed live 2026-08-25 -- HandleExperienceNotification / SkillManager::OnEnemyDeath don't fire/credit
// in single-player). We only count here; XP has no event that fires, so it is read by polling
// GetExperiencePoints in tick(), attributed to a kill only when it lands within kExpWindow of one.
std::atomic<int> g_kill_count{0};
std::atomic<uint64_t> g_kills_total{0};
double g_last_kill_time = -1e9;            // game thread: app::now() of the last kill (burst coalescing / XP window)
long long g_last_xp = -1;                  // game thread: previous GetExperiencePoints sample (-1 = uninitialised)
world::Vec3 g_last_hit_pos;                // game thread: last combat-text position, to pan the kill line
typedef void (*IncrKills_t)(void*, unsigned, unsigned, int, bool);
static IncrKills_t IncrKills_hook_orig;
static void IncrKills_hook(void* self, unsigned a1, unsigned a2, int classification, bool b) {
  IncrKills_hook_orig(self, a1, a2, classification, b);
  ++g_kill_count; ++g_kills_total; g_last_kill_time = app::now();
}
static bool victim_is_player(void* cm) {   // CombatManager+8 = the Character it belongs to (GetCharacter is that one load)
  __try {
    if (!cm || IsBadReadPtr(cm, 16)) return false;
    void* ch; memcpy(&ch, (char*)cm + 8, sizeof ch);
    unsigned id = ch && g_get_object_id ? g_get_object_id(ch) : 0;
    return id && id == world::player_id();
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
typedef bool (*TakeAttack_t)(void*, void*, void*, void*);
static TakeAttack_t TakeAttack_hook_orig;
static bool TakeAttack_hook(void* self, void* params, void* skills, void* bio) {
  bool r = TakeAttack_hook_orig(self, params, skills, bio);   // the game's behaviour first, always
  if (g_say_hit && victim_is_player(self)) { if (r) ++g_hit_true; else ++g_hit_false; ++g_hits_total; }
  return r;
}
}  // namespace

bool install() {
  g_outgoing = settings::get_int("announce.outgoing.mode", 2);
  if (g_outgoing < 0 || g_outgoing > 2) g_outgoing = 2;
  g_incoming = settings::get_bool("announce.incoming", true);
  g_say_hit = settings::get_bool("announce.incoming.hits", true);
  g_get_object_id = (unsigned (*)(const void*))GetProcAddress(GetModuleHandleA(names::Object_GetObjectId_DLL), names::Object_GetObjectId);
  g_hooks = {GD_HOOK(EventManager_Send, EventManagerSend_hook), GD_HOOK(Character_DebufTarget, DebufTarget_hook),
             GD_HOOK(PlayStats_IncrementKills, IncrKills_hook), GD_HOOK(CombatManager_TakeAttack, TakeAttack_hook)};
  return gd::hooks::attach_hooks(g_hooks) == 0;
}
void remove() { gd::hooks::detach_hooks(g_hooks); g_pending.clear(); g_pending_debuff.clear(); g_exp_pending = 0; g_kill_count = 0; g_last_xp = -1; g_player_life = PlayerLifeState::Unknown; g_life_player_id = 0; }

// pan/gain of a world point from the player: the shared rule for the positioned voices.
static void positioned(const world::Vec3& p, float& pan, float& gain) {
  world::ear_frame(p, pan, gain);
  world::Vec3 me; float dist = 0.0f;
  if (world::player_position(me)) { float dx = p.x - me.x, dz = p.z - me.z; dist = std::sqrt(dx * dx + dz * dz); }
  gain = world::voice_gain(dist);
}

void tick() {
  if (!world::in_world()) { g_health.reset(); g_pending.clear(); g_pending_debuff.clear(); g_coalescer.clear(); g_exp_pending = 0; g_kill_count = 0; g_last_xp = -1; g_player_life = PlayerLifeState::Unknown; g_life_player_id = 0; return; }
  double now = app::now();
  while (!g_pending.empty()) {
    RawEvent r = std::move(g_pending.front()); g_pending.pop_front();
    ++g_parsed;
    std::string drawn = log::utf8(r.text);
    gd::core::CombatText ct = gd::core::parse_combat_text(drawn, r.text_class == 0x85);
    world::Vec3 p{}; float pan = 0.0f, gain = 1.0f;
    bool placed = r.has_region && world::world_point(r.wv, p);
    if (placed) { positioned(p, pan, gain); g_last_hit_pos = p; }   // remember where the last hit landed (to pan the kill line)
    note(std::format("{} '{}' class={:#x} style={} id={} pos=({:.1f},{:.1f}) pan={:+.2f} gain={:.2f}", ct.is_number ? (ct.crit ? "crit" : "hit") : "word",
                     drawn, r.text_class, r.style, r.id, p.x, p.z, pan, gain));
    g_coalescer.push({ct.is_number, ct.amount, ct.crit, ct.word, {}, p.x, p.z, pan, gain, r.t});
  }
  // Debuffs caught this tick: only those applied TO the player are announced (Zira, panned to the caster).
  // Announcing the debuffs the PLAYER applies to enemies is intentionally dropped for now -- naming them by the
  // full skill ("Olexra's Flash Freeze") per hit is unusably verbose in a fight, and a terse effect lexicon
  // ("frozen", "stunned", ...) is a proper research project (cross-referencing the DB's CC parameters / the
  // wiki). The DebufTarget hook and the CombatCoalescer's tag support stay in place for when that lands.
  unsigned pid = world::player_id();
  std::vector<voice::Say> zira;   // player-received effects, emitted after the Mark lines (staggered together)
  while (!g_pending_debuff.empty()) {
    RawDebuff d = std::move(g_pending_debuff.front()); g_pending_debuff.pop_front();
    if (!pid || d.victim != pid) continue;   // only debuffs on the player (enemy-debuff announcement deferred)
    // The entry carries no skill id (+0x48 is the caster); name it from the record on the player's SkillManager
    // (where the buff is registered). See docs/combat-feedback.md (RE 2026-08-25).
    std::string name = world::buff_name(d.victim, d.record);
    note(std::format("debuff on-me '{}' rec='{}' caster={}", name, d.record, d.caster));
    if (name.empty()) continue;
    world::Vec3 p{}; float pan = 0.0f, gain = 1.0f;
    if (world::entity_position(d.caster, p)) positioned(p, pan, gain);   // panned to whoever cast it
    zira.push_back({voice::Which::Zira, name, pan, gain, voice::Policy::Overlap, voice::kGroupSelfEffect});
  }
  // Emit, staggering co-timed lines by a little leading silence so identical text does not phase-lock into one.
  int emit = 0;
  auto stagger = [&emit] { return std::min((float)emit++ * 30.0f, 100.0f); };
  for (const auto& o : g_coalescer.flush(now)) {
    ++g_spoken;
    if (g_outgoing == 2) voice::say({voice::Which::Mark, hit_line(o), o.pan, o.gain, voice::Policy::Overlap, voice::kGroupEnemy, stagger()});
    else if (g_outgoing == 1) { std::string b = brief_line(o); if (!b.empty()) voice::say({voice::Which::Mark, b, o.pan, o.gain, voice::Policy::Overlap, voice::kGroupEnemy, stagger()}); }
  }
  if (g_incoming) for (voice::Say& s : zira) { s.predelay_ms = stagger(); voice::say(std::move(s)); }
  // IsAlive is authoritative for a death.  A player id and a positive life cap gate the read so a
  // loading frame (or an unresolved player) cannot turn its default zero into a false death.
  float mx = world::life_max();
  unsigned life_player_id = world::player_id();
  if (!life_player_id || mx <= 0.0f) {
    g_player_life = PlayerLifeState::Unknown;
    g_life_player_id = 0;
  } else {
    if (life_player_id != g_life_player_id) {
      g_life_player_id = life_player_id;
      g_player_life = PlayerLifeState::Unknown;
    }
    PlayerLifeState next = world::player_alive() ? PlayerLifeState::Alive : PlayerLifeState::Dead;
    if (g_player_life == PlayerLifeState::Alive && next == PlayerLifeState::Dead) {
      ++g_deaths;
      speech::speak(strings::kYouDied, true);
    }
    g_player_life = next;
  }
  if (life_player_id && mx > 0) {
    int pct = 0;
    if (g_health.update(world::life() / mx, pct)) {
      MessageBuilder m;
      strings::push_health_percent(m, pct);
      if (g_incoming) voice::say({voice::Which::Zira, m.build(), 0.0f, 1.0f, voice::Policy::Replace, voice::kGroupSelf, stagger()});
  }
  {
    int t = g_hit_true.exchange(0), f = g_hit_false.exchange(0);
    if (g_say_hit && t > 0) voice::say({voice::Which::Zira, std::string(strings::kHit), 0.0f, 1.0f, voice::Policy::Replace, voice::kGroupSelfEffect, 0.0f});
    (void)f;
    }
  }
  // XP has no event that fires in single-player, so poll GetExperiencePoints each tick and attribute a positive
  // delta to a kill only when it lands within kExpWindow of one (so quest / non-kill XP is not announced). A
  // negative delta is a level-up (the bar reset); just re-baseline.
  long long cur_xp = (long long)gameapi::experience();
  if (g_last_xp >= 0 && cur_xp > g_last_xp && now - g_last_kill_time < kExpWindow) {
    uint64_t d = (uint64_t)(cur_xp - g_last_xp);
    g_exp_pending += d; g_exp_total += d;
  }
  g_last_xp = cur_xp;
  // Kills: coalesce a burst -- flush kExpWindow after the LAST kill (so the burst, and its XP, have landed) into
  // one terse Zira line ("killed" / "3 killed", + ", N exp"), panned toward where the last hit landed.
  if (g_kill_count.load() > 0 && now - g_last_kill_time >= kExpWindow) {
    int kills = g_kill_count.exchange(0);
    uint64_t xp = g_exp_pending.exchange(0);
    float pan = 0.0f, gain = 1.0f;
    world::ear_frame(g_last_hit_pos, pan, gain);   // direction only; kills read at full level
    MessageBuilder m;
    strings::push_kills(m, kills, xp);
    if (g_outgoing > 0) voice::say({voice::Which::Zira, m.build(), pan, 1.0f, voice::Policy::Overlap, voice::kGroupSelfEffect, stagger()});
  }
}

// The H key is a screen-reader readout like every other key (the positional voices are only for things that
// happen in the world, not for what the player asked for).
void speak_vitals() {
  if (!world::in_world()) { speech::speak(strings::kNotInWorld, true); return; }
  MessageBuilder m;
  strings::push_vitals(m, world::life(), world::life_max(), world::energy(), world::energy_max());
  speech::speak(m.build(), true);
}

std::string status() {
  const char* life_state = g_player_life == PlayerLifeState::Alive ? "alive" : g_player_life == PlayerLifeState::Dead ? "dead" : "unknown";
  std::string out = std::format("events_seen={} parsed={} spoken={} unreadable={} pending={} debuffs={} deaths={} player={} kills={} exp_total={} exp_pending={} coalesce={} window={:.3f} merged={} dropped={} health_bucket={}\n",
                                g_seen.load(), g_parsed.load(), g_spoken.load(), g_bad.load(), g_pending.size(), g_debuffs.load(), g_deaths.load(), life_state, g_kills_total.load(), g_exp_total.load(), g_exp_pending.load(), g_coalescer.enabled(), g_coalescer.window(),
                                g_coalescer.merged(), g_coalescer.dropped(), g_health.bucket());
  std::lock_guard lk(g_recent_mu);
  for (const std::string& l : g_recent) out += "  " + l + "\n";
  return out;
}
void arm_raw_log(int n) { g_raw_log = n; }
std::string hit_status() { return std::format("hitsay: on={} attacks_on_player={} (true so far this tick {} false {})\n", g_say_hit, g_hits_total.load(), g_hit_true.load(), g_hit_false.load()); }
int outgoing_mode() { return g_outgoing; }
void set_outgoing_mode(int mode) { g_outgoing = mode < 0 ? 0 : mode > 2 ? 2 : mode; settings::set_int("announce.outgoing.mode", g_outgoing); }
bool incoming_enabled() { return g_incoming; }
void set_incoming(bool on) { g_incoming = on; settings::set_bool("announce.incoming", on); }
bool incoming_hits_enabled() { return g_say_hit; }
void set_incoming_hits(bool on) { g_say_hit = on; settings::set_bool("announce.incoming.hits", on); }
void set_coalesce(bool on) { g_coalescer.set_enabled(on); }
void set_window(double seconds) { g_coalescer.set_window(seconds); }
void set_cap(int per_flush) { g_coalescer.set_max_per_flush(per_flush); }
}  // namespace gd::combat
