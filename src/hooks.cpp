#include "hooks.h"
#include "gd_names.h"
#include "log.h"
#include "msvc_string.h"
#include "speech.h"
#include "textcap.h"
#include "app.h"
#include "core/message_builder.h"
#include "core/strings.h"
#include <windows.h>
#include <detours.h>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <intrin.h>
#include <set>
#include <tlhelp32.h>

namespace {
// Every Detours transaction must update EVERY thread that may be executing detoured code, not just the caller:
// Detours moves a suspended thread's instruction pointer out of a trampoline/prologue being rewritten, and
// the game thread runs our hooks on every frame. Hot reloads unload on a remote thread; with only the current
// thread updated, the game thread sat inside a freed trampoline (two crashes on reload, 2026-08-21/22).
struct ThreadUpdater {
  std::vector<HANDLE> handles;
  ThreadUpdater() {
    DetourUpdateThread(GetCurrentThread());
    DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{}; te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
      if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
      HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
      if (!h) continue;
      if (DetourUpdateThread(h) == NO_ERROR) handles.push_back(h); else CloseHandle(h);
    }
    CloseHandle(snap);
  }
  // Destroy after DetourTransactionCommit (which resumes the threads).
  ~ThreadUpdater() { for (HANDLE h : handles) CloseHandle(h); }
};
}  // namespace

namespace gd::hooks {
using namespace gd::names;
static std::string where(void* ret) {
  HMODULE m = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)ret, &m);
  char name[MAX_PATH] = "?"; if (m) GetModuleFileNameA(m, name, MAX_PATH);
  const char* base = strrchr(name, '\\'); base = base ? base + 1 : name;
  return std::format("{}+0x{:x}", base, (uintptr_t)ret - (uintptr_t)m);
}
static void log_caller(const char* what, void* ret) {
  static std::mutex mu; static std::set<std::string> seen;
  std::string w = std::string(what) + " <- " + where(ret);
  std::lock_guard lk(mu);
  if (seen.insert(w).second) log::writef("caller: {}", w);
}
static uint64_t g_c_numkey, g_c_nummouse, g_c_synthkey, g_c_synthmouse, g_c_cursor_client, g_c_cursor_screen, g_c_dispmouse, g_c_mouseevent_real;

// ---- engine value types we only read ----
struct Color { float r, g, b, a; };  // assumption: 4 floats; verify against logged values
struct Rect  { float x, y, w, h; };  // assumption; a by-value Rect arrives as a pointer to a copy on x64

static uint32_t pack(const Color* c) {
  if (!c) return 0;
  auto q = [](float f) { int v = (int)(f * 255.0f + 0.5f); return (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v); };
  return q(c->r) << 24 | q(c->g) << 16 | q(c->b) << 8 | q(c->a);
}
static std::u16string u16(const uint16_t* s) { return s ? std::u16string((const char16_t*)s) : std::u16string(); }

// ---- hook table ----
static std::vector<Hook> g_hooks;
#define HOOK(ID, DETOUR) Hook{ID##_DLL, ID, (void*)&DETOUR, (void**)&DETOUR##_orig, #ID, false}

static std::set<void*> g_attached_targets;
// A function whose whole body is `ret` / `ret 0` / `xor al,al; ret` / `xor eax,eax; ret` / `mov al,1; ret`.
static bool is_bare_stub(void* target) {
  const unsigned char* b = (const unsigned char*)target;
  if (b[0] == 0xC3) return true;
  if (b[0] == 0xC2 && b[1] == 0 && b[2] == 0) return true;
  if ((b[0] == 0x32 || b[0] == 0x30 || b[0] == 0x33 || b[0] == 0x31) && b[1] == 0xC0 && b[2] == 0xC3) return true;
  if (b[0] == 0xB0 && b[2] == 0xC3) return true;
  return false;
}
static LONG attach_all(std::vector<Hook>& hooks) {
  DetourTransactionBegin();
  ThreadUpdater threads;
  size_t ok = 0;
  for (auto& h : hooks) {
    if (h.ok) continue;
    HMODULE m = GetModuleHandleA(h.dll);
    void* target = m ? (void*)GetProcAddress(m, h.name) : nullptr;
    if (!target) { log::writef("hook {}: export not found in {}", h.label, h.dll); continue; }
    // MSVC folds identical bodies: in Game.dll one `ret` is the export address of 1,574 empty virtuals and one
    // `xor al,al; ret` of 525 (2026-09-01: detouring SkillActivated::HitAction/ActivateNow/StartAction, which
    // ARE those stubs, hooked every empty virtual in the game -- it exited silently at start). Refuse a target
    // that is a bare stub, and never attach the same address twice.
    if (is_bare_stub(target)) { log::writef("hook {}: REFUSED, target {} is a shared stub (folded empty body)", h.label, target); continue; }
    if (g_attached_targets.count(target)) { log::writef("hook {}: skipped, target {} already detoured", h.label, target); continue; }
    *h.orig = target;
    LONG r = DetourAttach(h.orig, h.detour);
    if (r != NO_ERROR) { log::writef("hook {}: DetourAttach failed {}", h.label, r); *h.orig = nullptr; continue; }
    h.ok = true; ++ok; g_attached_targets.insert(target);
  }
  LONG r = DetourTransactionCommit();
  log::writef("hooks: {} attached this pass, commit={}", ok, r);
  return r;
}
long attach_hooks(std::vector<Hook>& hooks) { return attach_all(hooks); }
void detach_hooks(std::vector<Hook>& hooks) {
  DetourTransactionBegin();
  ThreadUpdater threads;
  for (auto& h : hooks) if (h.ok && *h.orig) { DetourDetach(h.orig, h.detour); h.ok = false; g_attached_targets.erase((void*)h.orig[0]); }
  DetourTransactionCommit();
}

// ======================= game-thread job pump =======================
static std::mutex g_jobs_mu;
static std::deque<std::function<void()>> g_jobs;
static uint64_t g_frame;
uint64_t frame() { return g_frame; }
// The job owns its state: a caller that times out returns while the job may still run later on the game
// thread (a /regions dump that loaded the whole world did exactly that on 2026-08-22 and the stack-captured
// state produced a bad_function_call crash). The caller's result buffer must then also outlive the call,
// which devserver's routes guarantee by building their strings inside the job (the job holds the only ref).
bool run_on_game_thread(std::function<void()> fn, unsigned timeout_ms) {
  struct State { std::mutex m; std::condition_variable cv; bool done = false; std::function<void()> fn; };
  auto st = std::make_shared<State>();
  st->fn = std::move(fn);
  {
    std::lock_guard lk(g_jobs_mu);
    g_jobs.push_back([st] { if (st->fn) st->fn(); { std::lock_guard l2(st->m); st->done = true; } st->cv.notify_all(); });
  }
  std::unique_lock lk(st->m);
  return st->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return st->done; });
}
static void drain_jobs() {
  for (;;) {
    std::function<void()> j;
    { std::lock_guard lk(g_jobs_mu); if (g_jobs.empty()) return; j = std::move(g_jobs.front()); g_jobs.pop_front(); }
    j();
  }
}

// ======================= text capture: GraphicsCanvas::RenderText2d* =======================
// All GraphicsCanvas methods are member functions: `this` is the first argument on x64. The Rect overloads
// call the (x,y) overloads internally, so only the (x,y) family is captured.
typedef void (*RT_XY_C_t)(void*, int, int, const Color*, const uint16_t*, const void*, int, int, int, int, int);
static RT_XY_C_t RT_XY_C_orig;
static void RT_XY_C(void* self, int x, int y, const Color* c, const uint16_t* text, const void* font, int size, int xa, int ya, int sf, int fl) {
  textcap::on_text({x, y, xa, ya, pack(c), u16(text), "xy_c"});
  RT_XY_C_orig(self, x, y, c, text, font, size, xa, ya, sf, fl);
}
typedef void (*RT_XY_CC_t)(void*, int, int, const Color*, const Color*, const uint16_t*, const void*, int, int, int, int, int);
static RT_XY_CC_t RT_XY_CC_orig;
static void RT_XY_CC(void* self, int x, int y, const Color* c1, const Color* c2, const uint16_t* text, const void* font, int size, int xa, int ya, int sf, int fl) {
  textcap::on_text({x, y, xa, ya, pack(c1), u16(text), "xy_cc"});
  RT_XY_CC_orig(self, x, y, c1, c2, text, font, size, xa, ya, sf, fl);
}
typedef void (*RT_XY_FN_t)(void*, int, int, const uint16_t*, const MsvcStringA*, float, int, int, int);
static RT_XY_FN_t RT_XY_FN_orig;
static void RT_XY_FN(void* self, int x, int y, const uint16_t* text, const MsvcStringA* fn, float size, int xa, int ya, int fl) {
  textcap::on_text({x, y, xa, ya, 0, u16(text), "xy_fontname"});
  RT_XY_FN_orig(self, x, y, text, fn, size, xa, ya, fl);
}
typedef void (*RT_XY_FN2_t)(void*, int, int, const uint16_t*, const MsvcStringA*, float, float, int, int, int);
static RT_XY_FN2_t RT_XY_FN2_orig;
static void RT_XY_FN2(void* self, int x, int y, const uint16_t* text, const MsvcStringA* fn, float a, float b, int xa, int ya, int fl) {
  textcap::on_text({x, y, xa, ya, 0, u16(text), "xy_fontname2"});
  RT_XY_FN2_orig(self, x, y, text, fn, a, b, xa, ya, fl);
}
typedef void (*RTB_F_t)(void*, int, int, const Color*, const uint16_t*, const void*, int, bool, const Color*, int, int);
static RTB_F_t RTB_F_orig;
static void RTB_F(void* self, int x, int y, const Color* c, const uint16_t* text, const void* font, int size, bool b, const Color* bg, int sf, int fl) {
  textcap::on_text({x, y, -1, -1, pack(c), u16(text), "box_font"});
  RTB_F_orig(self, x, y, c, text, font, size, b, bg, sf, fl);
}
typedef void (*RTB_FN_t)(void*, int, int, const Color*, const uint16_t*, const MsvcStringA*, float, bool, const Color*);
static RTB_FN_t RTB_FN_orig;
static void RTB_FN(void* self, int x, int y, const Color* c, const uint16_t* text, const MsvcStringA* fn, float size, bool b, const Color* bg) {
  textcap::on_text({x, y, -1, -1, pack(c), u16(text), "box_fontname"});
  RTB_FN_orig(self, x, y, c, text, fn, size, b, bg);
}
typedef void (*RCT_t)(void*, int, int, const MsvcStringW*, const MsvcStringA*, const Color*, int);
static RCT_t RCT_orig;
static void RCT(void* self, int x, int y, const MsvcStringW* text, const MsvcStringA* fn, const Color* c, int fl) {
  textcap::on_text({x, y, -1, -1, pack(c), std::u16string(text->view()), "colored"});
  RCT_orig(self, x, y, text, fn, c, fl);
}
typedef void (*RTP_t)(void*, int, int, const Color*, const Color*, const void*, const MsvcStringA*, bool);
static RTP_t RTP_orig;
static void RTP(void* self, int x, int y, const Color* c1, const Color* c2, const void* lines, const MsvcStringA* fn, bool b) {
  textcap::on_text({x, y, -1, -1, pack(c1), u"<paragraph>", "paragraph"});
  RTP_orig(self, x, y, c1, c2, lines, fn, b);
}

// ======================= localization =======================
typedef MsvcStringW* (*LocGetText_t)(void*, MsvcStringW*, const char*);
static LocGetText_t LocGetText_orig;
// Tutorial tips: the game fetches `tagQuickTip*` when it is about to show one; we keep the recent ones so a
// mod-owned screen can recognise the popup on screen (by its title line) and read it.
static std::mutex g_tips_mu;
static std::vector<Tip> g_tips;
std::vector<Tip> recent_tips() { std::lock_guard lk(g_tips_mu); return g_tips; }
// The LocalizationManager instance, captured from the game's own fetches; localize() resolves a tag the way
// the widgets do (LocalizeWithoutParams), for controls that carry only a tag (the options tabs' tooltips).
static void* g_loc_manager;
std::string localize(const char* tag) {
  typedef const char16_t* (*Localize_t)(void*, const char*);
  static Localize_t fn = [] { HMODULE e = GetModuleHandleA("Engine.dll"); return e ? (Localize_t)GetProcAddress(e, names::LocalizationManager_LocalizeWithoutParams) : nullptr; }();
  if (!g_loc_manager || !fn || !tag || !*tag) return {};
  const char16_t* s = fn(g_loc_manager, tag);
  return s ? log::utf8(s) : std::string();
}
static MsvcStringW* LocGetText(void* self, MsvcStringW* out, const char* tag) {
  g_loc_manager = self;
  MsvcStringW* r = LocGetText_orig(self, out, tag);
  static std::vector<std::string> seen;  // log each tag once
  std::string t = tag ? tag : "(null)";
  if (r && t.rfind("tagQuickTip", 0) == 0) {
    Tip tip{t, textcap::speakable(r->view()), g_frame};
    std::lock_guard lk(g_tips_mu);
    std::erase_if(g_tips, [&](const Tip& x) { return x.tag == t; });
    g_tips.push_back(tip);
    if (g_tips.size() > 16) g_tips.erase(g_tips.begin());
  }
  bool dup = false;
  for (auto& s : seen) if (s == t) { dup = true; break; }
  if (!dup) {
    if (seen.size() < 5000) seen.push_back(t);
    log::writef("loc: {} = \"{}\"", t, r ? log::utf8(r->view()) : std::string("?"));
  }
  return r;
}

// ======================= key state for the mod's InputManager =======================
struct GameKeys : gd::core::KeySource {
  bool down[512] = {}, pressed[512] = {}, up[512] = {};
  std::u16string typed;  // printable characters pressed this frame, in order
  void record(int code, bool released) {
    if (code < 0 || code >= 512) return;
    if (released) { down[code] = false; up[code] = true; }
    else { if (!down[code]) pressed[code] = true; down[code] = true; }
  }
  void end_frame() { memset(pressed, 0, sizeof pressed); memset(up, 0, sizeof up); typed.clear(); }
  // Modifier state comes from the game's own per-event flags (it reads the real keyboard state), NOT from
  // our down[] tracking: alt-tabbing into the game delivers an Alt key-down whose key-up goes to the task
  // switcher, which left Alt "held" forever and failed every plain binding (measured 2026-08-21).
  void record_mods(bool s, bool a, bool c) {
    mshift = s; malt = a; mctrl = c;
    down[0x2a] = down[0x36] = s; down[0x38] = down[0x76] = a; down[0x1d] = down[0x6b] = c;
  }
  // Keys cannot be trusted across a focus change: releases go to whoever has the focus then.
  void reset() { memset(down, 0, sizeof down); memset(pressed, 0, sizeof pressed); memset(up, 0, sizeof up); typed.clear(); mshift = malt = mctrl = false; }
  bool any(std::initializer_list<int> codes) const { for (int c : codes) if (down[c]) return true; return false; }
  bool just_pressed(int k) const override { return k >= 0 && k < 512 && pressed[k]; }
  bool held(int k) const override { return k >= 0 && k < 512 && down[k]; }
  bool released(int k) const override { return k >= 0 && k < 512 && up[k]; }
  bool ctrl() const override { return mctrl; }
  bool shift() const override { return mshift; }
  bool alt() const override { return malt; }
  bool mshift = false, malt = false, mctrl = false;
};
static GameKeys g_keys;
static void* g_input_device;  // the DirectInputDevice instance, captured from the game's key poll
const gd::core::KeySource& key_source() { return g_keys; }
std::string button_names(int max_code) {
  typedef bool (*GetDefaultButtonName_t)(void*, int, MsvcStringW*);
  // The device's own virtual knows keyboard keys (scancode -> OS key name); the base class only names mouse/gamepad.
  HMODULE di = GetModuleHandleA("DirectInput.dll");
  auto fn = di ? (GetDefaultButtonName_t)GetProcAddress(di, DirectInputDevice_GetButtonName) : nullptr;
  if (!fn) { HMODULE eng = GetModuleHandleA("Engine.dll"); fn = eng ? (GetDefaultButtonName_t)GetProcAddress(eng, InputDevice_GetDefaultButtonName) : nullptr; }
  if (!fn || !g_input_device) return "input device or export not available\n";
  std::string out;
  for (int code = 0; code < max_code; ++code) {
    alignas(16) unsigned char buf[64] = {};
    MsvcStringW* s = (MsvcStringW*)buf;
    s->capacity = 7;  // an empty SSO string: the callee assigns into it
    bool ok = fn(g_input_device, code, s);
    std::string name = ok ? log::utf8(s->view()) : std::string();
    if (s->capacity > 7 && s->u.ptr) free(s->u.ptr);  // heap-allocated by the game's CRT (same MSVC CRT)
    if (!name.empty()) out += std::format("{:#x}\t{}\n", code, name);
  }
  return out;
}
std::u16string_view typed_chars() { return g_keys.typed; }

// ======================= input =======================
// The game polls its input device each frame: DirectInputDevice::GetNumKeyEvents() then GetKeyEvent(i).
// ButtonEvent layout (measured 2026-08-21): +0 vtable, +8 int button (game's Button enum ~ DIK scancodes,
// extended keys remapped), +12 int released, +16 flag bytes {valid?, shift, alt, ctrl}, +20 char16 character.
static bool g_swallow_keys = false;
static int g_real_keys = 0;                 // physical events exposed this poll
static std::mutex g_synth_mu;
struct QueuedSynthKey { SynthKey key; bool report_to_mod; };
static std::deque<std::vector<QueuedSynthKey>> g_synth_pending;  // one group per frame
static std::vector<QueuedSynthKey> g_synth_active;               // the group visible this frame
typedef int (*GetNumKeyEvents_t)(void*);
static GetNumKeyEvents_t GetNumKeyEvents_hook_orig;
typedef void* (*GetKeyEvent_t)(void*, void*, int);  // ButtonEvent returned by value -> hidden pointer
static GetKeyEvent_t GetKeyEvent_hook_orig;
typedef const uint16_t* (*ButtonEvent_GetText_t)(const void*);
static ButtonEvent_GetText_t g_ButtonEvent_GetText;
typedef void (*ButtonEvent_ctor_t)(void*);
static ButtonEvent_ctor_t g_ButtonEvent_ctor, g_ButtonEvent_dtor;

static std::function<bool(int, bool, bool, bool)> g_key_pass;  // while muted: real key events (code, released, shift, ctrl) still delivered to the game
static std::vector<int> g_pass_idx;          // real event indices delivered to the game this poll
static int GetNumKeyEvents_hook(void* self) {
  ++g_c_numkey;
  g_input_device = self;
  log_caller("GetNumKeyEvents", _ReturnAddress());
  int n = GetNumKeyEvents_hook_orig(self);
  static uint64_t logged_frame = ~0ull;
  bool first = n > 0 && logged_frame != g_frame;  // the game may poll more than once per frame; record/log once
  if (first) logged_frame = g_frame;
  g_pass_idx.clear();
  for (int i = 0; i < n && i < 64; ++i) {
    alignas(16) unsigned char buf[256] = {};
    GetKeyEvent_hook_orig(self, buf, i);
    int code = *(int*)(buf + 8), rel = *(int*)(buf + 12);
    // The filter gets the event's OWN modifier flags: key_source() is only updated below, so it would still show the
    // previous event's modifiers (a plain W right after Shift came up would read as Shift+W and be swallowed).
    if (!g_swallow_keys || (g_key_pass && g_key_pass(code, rel != 0, buf[17] != 0, buf[19] != 0))) g_pass_idx.push_back(i);
    if (first) {
      g_keys.record(code, rel != 0);
      g_keys.record_mods(buf[17] != 0, buf[18] != 0, buf[19] != 0);  // ButtonEvent +16: {valid, shift, alt, ctrl}
      if (!rel) { uint16_t ch = *(uint16_t*)(buf + 20); if (ch >= 0x20 && ch != 0x7f) g_keys.typed.push_back((char16_t)ch); }
      std::string name = g_ButtonEvent_GetText ? log::utf8((const char16_t*)g_ButtonEvent_GetText(buf)) : std::string("?");
      log::writef("key: code={:#x} {} flags={:02x}{:02x}{:02x}{:02x} ch={:#x} text=\"{}\"", code, rel ? "up" : "down",
                  buf[16], buf[17], buf[18], buf[19], *(uint16_t*)(buf + 20), name);
    }
    if (g_ButtonEvent_dtor) g_ButtonEvent_dtor(buf);
  }
  g_real_keys = (int)g_pass_idx.size();
  std::lock_guard lk(g_synth_mu);
  return g_real_keys + (int)g_synth_active.size();
}
static void* GetKeyEvent_hook(void* self, void* out, int i) {
  if (i < g_real_keys) return GetKeyEvent_hook_orig(self, out, g_pass_idx[(size_t)i]);
  QueuedSynthKey queued{};
  {
    std::lock_guard lk(g_synth_mu);
    size_t j = (size_t)(i - g_real_keys);
    if (j >= g_synth_active.size()) return GetKeyEvent_hook_orig(self, out, 0);  // defensive: never index past the real queue
    queued = g_synth_active[j];
  }
  const SynthKey& k = queued.key;
  ++g_c_synthkey;
  unsigned char* b = (unsigned char*)out;
  memset(b, 0, 32);
  if (g_ButtonEvent_ctor) g_ButtonEvent_ctor(out);
  *(int*)(b + 8) = k.code;
  *(int*)(b + 12) = k.released ? 1 : 0;
  b[16] = 1; b[17] = k.shift; b[18] = k.alt; b[19] = k.ctrl;
  *(uint16_t*)(b + 20) = (uint16_t)k.ch;
  if (queued.report_to_mod) {
    g_keys.record(k.code, k.released);
    g_keys.record_mods(k.shift, k.alt, k.ctrl);
    if (!k.released && k.ch >= 0x20 && k.ch != 0x7f) g_keys.typed.push_back(k.ch);
  }
  return out;
}
static void push_synth_key(const SynthKey& k, bool report_to_mod) { std::lock_guard lk(g_synth_mu); g_synth_pending.push_back({{k, report_to_mod}}); }
void push_key_event(const SynthKey& k) { push_synth_key(k, true); }
void push_key(int code, bool shift, bool ctrl, bool alt, char16_t ch) {
  push_key_event({code, false, shift, ctrl, alt, ch});
  push_key_event({code, true, shift, ctrl, alt, ch});
}
void push_game_key(int code, bool shift, bool ctrl, bool alt, char16_t ch) {
  push_synth_key({code, false, shift, ctrl, alt, ch}, false);
  push_synth_key({code, true, shift, ctrl, alt, ch}, false);
}
void set_game_keys_muted(bool m) { g_swallow_keys = m; }
bool game_keys_muted() { return g_swallow_keys; }
void set_game_key_filter(std::function<bool(int, bool, bool, bool)> pass) { g_key_pass = std::move(pass); }
static void advance_synth_frame();

// --- synthetic mouse events through DirectInputDevice::GetNumMouseEvents/GetMouseEvent ---
static int g_real_mouse = 0;
static std::deque<std::vector<SynthMouse>> g_mouse_pending;
static std::vector<SynthMouse> g_mouse_active;
static int g_cursor_override_frames = 0;
static bool g_hold_left = false, g_hold_right = false;  // set_mouse_hold state
static bool g_cursor_override = false;
static float g_cursor_x = 0, g_cursor_y = 0;
typedef int (*GetNumMouseEvents_t)(void*);
static GetNumMouseEvents_t GetNumMouseEvents_hook_orig;
typedef void* (*GetMouseEvent_t)(void*, void*, int);  // MouseEvent returned by value -> hidden pointer
static GetMouseEvent_t GetMouseEvent_hook_orig;
typedef void (*MouseEvent_ctor_t)(void*);
static MouseEvent_ctor_t g_MouseEvent_ctor;
static bool g_force_active = false;
static int GetNumMouseEvents_hook(void* self) {
  ++g_c_nummouse;
  if (g_force_active) ((unsigned char*)self)[0x2d0] = 1;  // the device's "window active" byte, copied into every event's +0x18
  log_caller("GetNumMouseEvents", _ReturnAddress());
  g_real_mouse = GetNumMouseEvents_hook_orig(self);
  std::lock_guard lk(g_synth_mu);
  return g_real_mouse + (int)g_mouse_active.size();
}
static void* GetMouseEvent_hook(void* self, void* out, int i) {
  log_caller("GetMouseEvent", _ReturnAddress());
  if (i < g_real_mouse) {
    ++g_c_mouseevent_real;
    void* r = GetMouseEvent_hook_orig(self, out, i);
    // A mouse hold in progress: every real event reports the button held (and sits at the override position)
    // so the game's per-tick repeat keeps running; without this its idle event says "both up" and tears the
    // command down each frame.
    if (r && (g_hold_left || g_hold_right)) {
      unsigned char* hb = (unsigned char*)out;
      if (g_hold_left) hb[0x10] = 1;
      if (g_hold_right) hb[0x11] = 1;
      hb[0x18] = 1;  // "window active": the game drops inactive events, and the held state is ours to assert
      if (g_cursor_override) { memcpy(hb + 4, &g_cursor_x, 4); memcpy(hb + 8, &g_cursor_y, 4); }
    }
    static unsigned char last[28];
    if (memcmp(last, out, 28) != 0) {  // log real events whenever their bytes change
      memcpy(last, out, 28);
      const unsigned char* b = (const unsigned char*)out; float x, y; memcpy(&x, b + 4, 4); memcpy(&y, b + 8, 4);
      std::string hex; for (int k = 0; k < 28; ++k) hex += std::format("{:02x}{}", b[k], (k % 4 == 3) ? " " : "");
      log::writef("mouse(real): type={} x={} y={} raw={}", *(int*)b, x, y, hex);
    }
    return r;
  }
  ++g_c_synthmouse;
  SynthMouse m{};
  {
    std::lock_guard lk(g_synth_mu);
    size_t j = (size_t)(i - g_real_mouse);
    if (j >= g_mouse_active.size()) return GetMouseEvent_hook_orig(self, out, 0);
    m = g_mouse_active[j];
  }
  unsigned char* b = (unsigned char*)out;
  memset(b, 0, 28);
  if (g_MouseEvent_ctor) g_MouseEvent_ctor(out);
  *(int*)(b + 0) = m.type;
  memcpy(b + 4, &m.x, 4); memcpy(b + 8, &m.y, 4);
  b[0x10] = m.left; b[0x11] = m.right;
  for (int k = 0; k < 6; ++k) b[0x12 + k] = m.held[k];
  b[0x18] = 1; b[0x19] = m.shift; b[0x1a] = m.alt; b[0x1b] = m.ctrl;
  { std::string hex; for (int k = 0; k < 28; ++k) hex += std::format("{:02x}{}", b[k], (k % 4 == 3) ? " " : ""); log::writef("mouse(synth): type={} x={} y={} raw={}", m.type, m.x, m.y, hex); }
  return out;
}
void push_mouse_event(const SynthMouse& m) { std::lock_guard lk(g_synth_mu); g_mouse_pending.push_back({m}); }
static void set_cursor_override_frames(float x, float y, int frames);
void click(float x, float y, int button) {
  if (button < 1 || button > 6) button = 1;
  SynthMouse dn{}; dn.x = x; dn.y = y;
  SynthMouse up = dn;
  if (button == 2)      { dn.type = 2; dn.right = 1; up.type = 10; }
  else if (button >= 3) { dn.type = button; dn.held[button - 3] = 1; up.type = button + 8; }
  else                  { dn.type = 1; dn.left = 1; up.type = 9; }
  set_cursor_override_frames(x, y, 10);
  push_mouse_event(dn); push_mouse_event(up);
  log::writef("click: button {} at {},{}", button, x, y);
}
HWND game_window();
// The OS cursor in client space (GetCursorPos is detoured below; with no override it reports the real
// position). False when it is outside the client area -- a transition there is invisible to the game.
bool real_cursor_in_window(float& x, float& y) {
  POINT p{}; RECT rc{};
  HWND w = game_window();
  if (!w || !GetCursorPos(&p) || !ScreenToClient(w, &p) || !GetClientRect(w, &rc)) return false;
  x = (float)p.x; y = (float)p.y;
  return p.x >= 0 && p.y >= 0 && p.x < rc.right && p.y < rc.bottom;
}
void set_mouse_hold(int button, bool held, float x, float y) {
  bool& flag = button == 2 ? g_hold_right : g_hold_left;
  if (flag == held) return;
  flag = held;
  SynthMouse ev{};
  ev.x = x; ev.y = y;
  if (button == 2) { ev.type = held ? 2 : 10; ev.right = held; ev.left = g_hold_left; }
  else             { ev.type = held ? 1 : 9;  ev.left = held;  ev.right = g_hold_right; }
  push_mouse_event(ev);
  log::writef("mouse hold: button {} {} at {},{}", button, held ? "down" : "up", ev.x, ev.y);
}
bool mouse_held(int button) { return button == 2 ? g_hold_right : g_hold_left; }

// --- cursor position override (the game reads the OS cursor through DirectInputDevice::GetCursorPosition) ---
static void set_cursor_override_frames(float x, float y, int frames) { g_cursor_x = x; g_cursor_y = y; g_cursor_override = true; g_cursor_override_frames = frames; }
static std::mutex g_btn_mu;
static std::map<int, uint64_t> g_btn_queries;
typedef bool (*IsButtonDown_t)(void*, int);
static IsButtonDown_t IsButtonDown_hook_orig;
static bool IsButtonDown_hook(void* self, int button) {
  { std::lock_guard lk(g_btn_mu); ++g_btn_queries[button]; }
  return IsButtonDown_hook_orig(self, button);
}
// Vec2 has constructors, so MSVC returns it through a hidden pointer (2nd argument), not in RAX (measured:
// treating it as RAX-returned produced garbage).
struct Vec2 { float x, y; };
typedef Vec2* (*GetCursorPosition_t)(void*, Vec2*, bool);
static GetCursorPosition_t GetCursorPosition_hook_orig;
static HWND g_game_hwnd;
static BOOL CALLBACK find_game_window(HWND h, LPARAM) {
  DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
  if (pid == GetCurrentProcessId() && GetWindow(h, GW_OWNER) == nullptr && IsWindowVisible(h)) { g_game_hwnd = h; return FALSE; }
  return TRUE;
}
HWND game_window() { if (!g_game_hwnd || !IsWindow(g_game_hwnd)) { g_game_hwnd = nullptr; EnumWindows(find_game_window, 0); } return g_game_hwnd; }
// The override is given in client coordinates; the game mostly asks for the screen variant (b=false).
static Vec2* GetCursorPosition_hook(void* self, Vec2* out, bool b) {
  ++(b ? g_c_cursor_client : g_c_cursor_screen);
  log_caller(b ? "GetCursorPosition(client)" : "GetCursorPosition(screen)", _ReturnAddress());
  Vec2* r = GetCursorPosition_hook_orig(self, out, b);
  static Vec2 last{-1, -1};
  if (r && (r->x != last.x || r->y != last.y)) { last = *r; log::writef("cursor({}): {},{}", b, r->x, r->y); }
  if (g_cursor_override && r) {
    POINT p{(LONG)g_cursor_x, (LONG)g_cursor_y};
    if (!b) { HWND w = game_window(); if (w) ClientToScreen(w, &p); }
    r->x = (float)p.x; r->y = (float)p.y;
  }
  return r;
}
typedef BOOL (WINAPI* GetCursorPos_t)(LPPOINT);
static GetCursorPos_t GetCursorPos_orig;
static BOOL WINAPI GetCursorPos_hook(LPPOINT p) {
  log_caller("user32!GetCursorPos", _ReturnAddress());
  BOOL r = GetCursorPos_orig(p);
  if (g_cursor_override && p) { p->x = (LONG)g_cursor_x; p->y = (LONG)g_cursor_y; HWND w = game_window(); if (w) ClientToScreen(w, p); }
  return r;
}
void set_fake_active(bool on) {
  g_force_active = on;
  HWND w = game_window();
  if (on && w) {
    // Tell the game it is active the way Windows would, without actually activating anything.
    SendMessageW(w, WM_ACTIVATEAPP, TRUE, 0);
    SendMessageW(w, WM_ACTIVATE, WA_ACTIVE, 0);
    SendMessageW(w, WM_SETFOCUS, 0, 0);
    log::write("fakeactive: posted WM_ACTIVATEAPP/WM_ACTIVATE/WM_SETFOCUS");
  }
}
void set_cursor_override(bool on, float x, float y) { g_cursor_override = on; g_cursor_x = x; g_cursor_y = y; g_cursor_override_frames = 0; }
static void advance_synth_frame() {
  std::lock_guard lk(g_synth_mu);
  g_synth_active.clear();
  if (!g_synth_pending.empty()) { g_synth_active = std::move(g_synth_pending.front()); g_synth_pending.pop_front(); }
  g_mouse_active.clear();
  if (!g_mouse_pending.empty()) { g_mouse_active = std::move(g_mouse_pending.front()); g_mouse_pending.pop_front(); }
  if (g_cursor_override_frames > 0 && --g_cursor_override_frames == 0) g_cursor_override = false;
}
std::string button_query_stats() {
  std::lock_guard lk(g_btn_mu);
  std::string s;
  for (auto& [b, n] : g_btn_queries) s += std::format("button {:#x}: {} queries\n", b, n);
  return s;
}

// Does the engine route mouse events through Display::HandleMouseEvent? (counter only)
typedef void (*DisplayMouse_t)(void*, const void*);
static DisplayMouse_t DisplayMouse_orig;
static void DisplayMouse(void* self, const void* ev) { ++g_c_dispmouse; DisplayMouse_orig(self, ev); }

std::string counters() {
  return std::format("numkey_polls={} nummouse_polls={} synth_keys_served={} synth_mouse_served={} real_mouse_served={} cursor_reads_client={} cursor_reads_screen={} display_mouse_events={}",
                     g_c_numkey, g_c_nummouse, g_c_synthkey, g_c_synthmouse, g_c_mouseevent_real, g_c_cursor_client, g_c_cursor_screen, g_c_dispmouse);
}

// ======================= the engine's input handler list (Engine::AddWidget registrants) =======================
// Engine::ProcessUserInput polls the device and hands every event to each handler in the vector at
// Engine+0x3e8..0x3f0 through vtable slots +0x10 (key, ButtonEvent&) and +0x18 (mouse, MouseEvent&), stopping at
// the first that returns true. The exe's UI lives there. We detour those slots to see what it does with our events.
typedef void (*ProcessUserInput_t)(void*);
static ProcessUserInput_t ProcessUserInput_hook_orig;
static void* g_engine;
void* engine_object() { return g_engine; }
typedef bool (*HandlerFn_t)(void*, void*);
// One distinct detour per target function (several widgets share one implementation, and a function may only be
// detoured once). Slot i remembers its original; the detours are template instantiations so each has its own address.
static const int kMaxSlots = 16;
static HandlerFn_t g_slot_orig[kMaxSlots];
static const char* g_slot_kind[kMaxSlots];
static std::map<void*, int> g_slot_of_fn;
static bool handler_common(int slot, void* w, void* ev) {
  bool r = g_slot_orig[slot](w, ev);
  const unsigned char* b = (const unsigned char*)ev;
  if (g_slot_kind[slot][0] == 'k') {
    log::writef("widget {} key: code={:#x} {} -> {}", where(*(void**)w), *(int*)(b + 8), *(int*)(b + 12) ? "up" : "down", r);
  } else {
    int type = *(int*)b;
    if (type != 0) { float x, y; memcpy(&x, b + 4, 4); memcpy(&y, b + 8, 4); log::writef("widget {} mouse: type={} at {},{} inside={} active={} -> {}", where(*(void**)w), type, x, y, b[0x10], b[0x18], r); }
  }
  return r;
}
template <int N> static bool handler_slot(void* w, void* ev) { return handler_common(N, w, ev); }
static HandlerFn_t g_slot_detour[kMaxSlots] = {
  handler_slot<0>, handler_slot<1>, handler_slot<2>, handler_slot<3>, handler_slot<4>, handler_slot<5>, handler_slot<6>, handler_slot<7>,
  handler_slot<8>, handler_slot<9>, handler_slot<10>, handler_slot<11>, handler_slot<12>, handler_slot<13>, handler_slot<14>, handler_slot<15> };
static int g_slots_used = 0;
static void hook_handlers() {
  if (!g_engine) return;
  static int enabled = -1;
  if (enabled < 0) { wchar_t v[4]; enabled = GetEnvironmentVariableW(L"GRIMDARK_HOOK_WIDGETS", v, 4) > 0 && v[0] == L'1'; }
  if (!enabled) return;  // experimental: detouring the exe's widget handlers has crashed the game; opt-in only
  unsigned char* e = (unsigned char*)g_engine;
  void** begin = *(void***)(e + 0x3e8); void** end = *(void***)(e + 0x3f0);
  if (!begin || !end || end < begin || end - begin > 64) return;
  static std::set<void*> seen_vtables;
  std::vector<std::pair<void*, const char*>> todo;
  for (void** p = begin; p < end; ++p) {
    void* w = *p; if (!w) continue;
    void* vt = *(void**)w;
    if (!seen_vtables.insert(vt).second) continue;
    void* key = ((void**)vt)[2]; void* mouse = ((void**)vt)[3];
    log::writef("input handler: widget {} vtable {} key={} mouse={}", where(w), where(vt), where(key), where(mouse));
    // only the exe's handlers are interesting (the engine ones are defaults / engine UI)
    HMODULE exe = GetModuleHandleW(nullptr);
    auto in_exe = [&](void* f) { HMODULE m = nullptr; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)f, &m); return m == exe; };
    // Skip trivial stubs (COMDAT-folded "return false": xor al,al / ret, at odd addresses) -- too short to detour.
    auto is_stub = [](void* f) {
      const unsigned char* b = (const unsigned char*)f;
      return ((uintptr_t)f & 1) || b[0] == 0xc3 || (b[0] == 0x32 && b[1] == 0xc0 && b[2] == 0xc3) || (b[0] == 0x30 && b[1] == 0xc0 && b[2] == 0xc3) || (b[0] == 0xb0 && b[2] == 0xc3);
    };
    static std::set<void*> queued;
    if (in_exe(key) && !is_stub(key) && queued.insert(key).second) todo.push_back({key, "key"});
    if (in_exe(mouse) && !is_stub(mouse) && queued.insert(mouse).second) todo.push_back({mouse, "mouse"});
  }
  if (todo.empty()) return;
  DetourTransactionBegin(); ThreadUpdater threads;
  for (auto& [fn, kind] : todo) {
    if (g_slots_used >= kMaxSlots) break;
    int i = g_slots_used++;
    g_slot_orig[i] = (HandlerFn_t)fn; g_slot_kind[i] = kind; g_slot_of_fn[fn] = i;
    LONG r = DetourAttach((void**)&g_slot_orig[i], (void*)g_slot_detour[i]);
    log::writef("detour slot {} ({}) on {}: {}", i, kind, where(fn), r);
  }
  LONG r = DetourTransactionCommit();
  log::writef("input handler hooks committed: {}", r);
}
static void ProcessUserInput_hook(void* self) {
  if (!g_engine) { g_engine = self; log::writef("engine object captured: {:#x}", (uintptr_t)self); }
  hook_handlers();
  ProcessUserInput_hook_orig(self);
}

// ======================= per-frame tick: Engine::Update() on the game thread =======================
// The exe's main loop (exe+0xee4d..0xef91, read 2026-08-21) runs per iteration: display->Update(dt) (render),
// the input device poll, SoundManager::Update, PresentSurface, Steamworks::Update, Engine::Update(0,0,0,0).
// The display object is the engine's Display in the menus but the exe's own subclass in the world, whose
// Update override never reaches the exported Display::Update -- so the tick rides Engine::Update, which both
// states call after the poll and the render. Display::Update is only counted.
typedef void (*DisplayUpdate_t)(void*);
static DisplayUpdate_t DisplayUpdate_orig;
typedef void (*EngineUpdate_t)(void*, const void*, const void*, bool, const void*);
static EngineUpdate_t EngineUpdate_orig;
static uint64_t g_c_display_update = 0, g_c_engine_update = 0;
static std::vector<Hook> g_late;  // hooks into DLLs the game loads after startup (DirectInput.dll)
static void install_late();
static void frame_tick();
static void DisplayUpdate(void* self) { DisplayUpdate_orig(self); ++g_c_display_update; }
static void EngineUpdate(void* self, const void* sphere, const void* frustum, bool b, const void* frustum2) {
  EngineUpdate_orig(self, sphere, frustum, b, frustum2);
  ++g_c_engine_update;
  frame_tick();
}
static void frame_tick() {
  ++g_frame;
  install_late();
  advance_synth_frame();
  textcap::on_frame_end();
  drain_jobs();
  gd::app::tick();
  g_keys.end_frame();
  { static HWND last_fg = nullptr; HWND now_fg = GetForegroundWindow(); if (now_fg != last_fg) { last_fg = now_fg; g_keys.reset(); } }
  // (The F10-F12 GetAsyncKeyState dev hotkeys lived here until 2026-09-13; /health, /text and /textcap do their jobs.)
}

// ======================= dev mode: never let the game take the foreground =======================
// The dev loop runs the game visible but unfocused (the developer's screen reader must not be interrupted).
// The game activates itself on restore/startup, so under GRIMDARK_NOFOCUS=1 its activating calls are defanged.
static bool g_nofocus = false;
HWND game_window();
// Under GRIMDARK_NOFOCUS the game's OWN code (exe, Engine.dll, Game.dll, Crate's DirectInput.dll wrapper) is told
// its window is active -- its UI ignores mouse events otherwise -- but Microsoft's dinput8.dll and everything
// else see the truth, or a foreground-mode keyboard would keep delivering keystrokes to the unfocused game
// (measured 2026-08-21: the game processed keys typed into other windows).
static bool caller_is_game(void* ret) {
  HMODULE m = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)ret, &m);
  if (!m) return false;
  static HMODULE exe = GetModuleHandleW(nullptr), eng = GetModuleHandleA("Engine.dll"), game = GetModuleHandleA("Game.dll");
  static HMODULE di = nullptr;
  if (!di) di = GetModuleHandleA("DirectInput.dll");
  return m == exe || m == eng || m == game || (di && m == di);
}
typedef HWND (WINAPI* GetForegroundWindow_t)(); typedef HWND (WINAPI* GetActiveWindow_t)(); typedef HWND (WINAPI* GetFocus_t)();
static GetForegroundWindow_t GetForegroundWindow_orig; static GetActiveWindow_t GetActiveWindow_orig; static GetFocus_t GetFocus_orig;
static HWND WINAPI GetForegroundWindow_hook() { HWND r = GetForegroundWindow_orig(); if (g_nofocus && caller_is_game(_ReturnAddress())) { HWND g = game_window(); if (g) return g; } return r; }
static HWND WINAPI GetActiveWindow_hook() { HWND r = GetActiveWindow_orig(); if (g_nofocus && caller_is_game(_ReturnAddress())) { HWND g = game_window(); if (g) return g; } return r; }
static HWND WINAPI GetFocus_hook() { HWND r = GetFocus_orig(); if (g_nofocus && caller_is_game(_ReturnAddress())) { HWND g = game_window(); if (g) return g; } return r; }
typedef BOOL (WINAPI* SetForegroundWindow_t)(HWND);
typedef BOOL (WINAPI* ShowWindow_t)(HWND, int);
typedef BOOL (WINAPI* SetWindowPos_t)(HWND, HWND, int, int, int, int, UINT);
typedef HWND (WINAPI* SetActiveWindow_t)(HWND);
typedef BOOL (WINAPI* BringWindowToTop_t)(HWND);
static SetForegroundWindow_t SetForegroundWindow_orig; static ShowWindow_t ShowWindow_orig; static SetWindowPos_t SetWindowPos_orig;
static SetActiveWindow_t SetActiveWindow_orig; static BringWindowToTop_t BringWindowToTop_orig;
static BOOL WINAPI SetForegroundWindow_hook(HWND h) { if (g_nofocus) { log::write("nofocus: blocked SetForegroundWindow"); return TRUE; } return SetForegroundWindow_orig(h); }
static HWND WINAPI SetActiveWindow_hook(HWND h) { if (g_nofocus) { log::write("nofocus: blocked SetActiveWindow"); return h; } return SetActiveWindow_orig(h); }
static BOOL WINAPI BringWindowToTop_hook(HWND h) { if (g_nofocus) { log::write("nofocus: blocked BringWindowToTop"); return TRUE; } return BringWindowToTop_orig(h); }
static BOOL WINAPI ShowWindow_hook(HWND h, int cmd) {
  if (g_nofocus && (cmd == SW_SHOW || cmd == SW_SHOWNORMAL || cmd == SW_RESTORE || cmd == SW_SHOWMAXIMIZED || cmd == SW_SHOWDEFAULT)) {
    log::writef("nofocus: ShowWindow({}) -> SW_SHOWNOACTIVATE", cmd); cmd = SW_SHOWNOACTIVATE;
  }
  return ShowWindow_orig(h, cmd);
}
static BOOL WINAPI SetWindowPos_hook(HWND h, HWND after, int x, int y, int cx, int cy, UINT f) {
  if (g_nofocus && !(f & SWP_NOACTIVATE)) { f |= SWP_NOACTIVATE; if (after == HWND_TOPMOST || after == HWND_TOP) f |= SWP_NOZORDER; }
  return SetWindowPos_orig(h, after, x, y, cx, cy, f);
}

// ======================= neutralize the game's legacy "Disabling Shortcut Keys in Games" code =======================
// GAME::DirectInputDevice installs a WH_KEYBOARD_LL hook on every focus gain (its callback is a pure pass-through)
// and toggles StickyKeys/ToggleKeys/FilterKeys via SystemParametersInfo on every focus change. The hook churn
// during the busy focus transition gets other processes' low-level hooks (NVDA's) timed out and silently removed
// by the system. None of it does anything useful for the game, so we refuse the hook and swallow the SPI sets.
static const HHOOK kFakeHook = (HHOOK)(uintptr_t)0x6D6F6E6B686F6F6BULL;
typedef HHOOK (WINAPI* SetWindowsHookExA_t)(int, HOOKPROC, HINSTANCE, DWORD);
typedef HHOOK (WINAPI* SetWindowsHookExW_t)(int, HOOKPROC, HINSTANCE, DWORD);
typedef BOOL (WINAPI* UnhookWindowsHookEx_t)(HHOOK);
typedef BOOL (WINAPI* SystemParametersInfoA_t)(UINT, UINT, PVOID, UINT);
typedef BOOL (WINAPI* SystemParametersInfoW_t)(UINT, UINT, PVOID, UINT);
static SetWindowsHookExA_t SetWindowsHookExA_orig;
static SetWindowsHookExW_t SetWindowsHookExW_orig;
static UnhookWindowsHookEx_t UnhookWindowsHookEx_orig;
static SystemParametersInfoA_t SystemParametersInfoA_orig;
static SystemParametersInfoW_t SystemParametersInfoW_orig;
static HHOOK WINAPI SetWindowsHookExA_hook(int id, HOOKPROC fn, HINSTANCE mod, DWORD tid) {
  if (id == WH_KEYBOARD_LL) { log::write("blocked: game tried to install a WH_KEYBOARD_LL hook (A)"); return kFakeHook; }
  return SetWindowsHookExA_orig(id, fn, mod, tid);
}
static HHOOK WINAPI SetWindowsHookExW_hook(int id, HOOKPROC fn, HINSTANCE mod, DWORD tid) {
  if (id == WH_KEYBOARD_LL) { log::write("blocked: game tried to install a WH_KEYBOARD_LL hook (W)"); return kFakeHook; }
  return SetWindowsHookExW_orig(id, fn, mod, tid);
}
static BOOL WINAPI UnhookWindowsHookEx_hook(HHOOK h) {
  if (h == kFakeHook) return TRUE;
  return UnhookWindowsHookEx_orig(h);
}
static bool spi_blocked(UINT action) {
  return action == SPI_SETSTICKYKEYS || action == SPI_SETTOGGLEKEYS || action == SPI_SETFILTERKEYS;
}
static BOOL WINAPI SystemParametersInfoA_hook(UINT action, UINT p, PVOID pv, UINT f) {
  if (spi_blocked(action)) { log::writef("blocked: SystemParametersInfoA({:#x})", action); return TRUE; }
  return SystemParametersInfoA_orig(action, p, pv, f);
}
static BOOL WINAPI SystemParametersInfoW_hook(UINT action, UINT p, PVOID pv, UINT f) {
  if (spi_blocked(action)) { log::writef("blocked: SystemParametersInfoW({:#x})", action); return TRUE; }
  return SystemParametersInfoW_orig(action, p, pv, f);
}
static void remove_existing_game_hook(HMODULE di) {
  HHOOK* slot = (HHOOK*)GetProcAddress(di, DirectInputDevice_keyboardHook);
  if (!slot) { log::write("DirectInputDevice::keyboardHook export not found"); return; }
  if (*slot && *slot != kFakeHook) {
    BOOL ok = UnhookWindowsHookEx_orig ? UnhookWindowsHookEx_orig(*slot) : UnhookWindowsHookEx(*slot);
    log::writef("removed the game's existing WH_KEYBOARD_LL hook {:#x}: {}", (uintptr_t)*slot, ok ? "ok" : "failed");
  } else {
    log::write("game has no WH_KEYBOARD_LL hook installed right now");
  }
  *slot = kFakeHook;
}

// Hooks into DirectInput.dll, which the game loads after startup. Called every frame until it is present.
static void install_late() {
  static bool done = false;
  if (done) return;
  HMODULE di = GetModuleHandleA("DirectInput.dll");
  if (!di) return;
  done = true;
  log::write("DirectInput.dll is loaded; installing input hooks");
  remove_existing_game_hook(di);
  g_late = {
    HOOK(DirectInputDevice_GetNumKeyEvents, GetNumKeyEvents_hook),
    HOOK(DirectInputDevice_GetKeyEvent, GetKeyEvent_hook),
    HOOK(DirectInputDevice_IsButtonDown, IsButtonDown_hook),
    HOOK(DirectInputDevice_GetCursorPosition, GetCursorPosition_hook),
    HOOK(DirectInputDevice_GetNumMouseEvents, GetNumMouseEvents_hook),
    HOOK(DirectInputDevice_GetMouseEvent, GetMouseEvent_hook),
  };
  attach_all(g_late);
}

bool install() {
  { wchar_t v[4]; g_nofocus = GetEnvironmentVariableW(L"GRIMDARK_NOFOCUS", v, 4) > 0 && v[0] == L'1'; }
  g_hooks = {
    Hook{"user32.dll", "SetForegroundWindow", (void*)&SetForegroundWindow_hook, (void**)&SetForegroundWindow_orig, "SetForegroundWindow", false},
    Hook{"user32.dll", "SetActiveWindow", (void*)&SetActiveWindow_hook, (void**)&SetActiveWindow_orig, "SetActiveWindow", false},
    Hook{"user32.dll", "BringWindowToTop", (void*)&BringWindowToTop_hook, (void**)&BringWindowToTop_orig, "BringWindowToTop", false},
    Hook{"user32.dll", "ShowWindow", (void*)&ShowWindow_hook, (void**)&ShowWindow_orig, "ShowWindow", false},
    Hook{"user32.dll", "SetWindowPos", (void*)&SetWindowPos_hook, (void**)&SetWindowPos_orig, "SetWindowPos", false},
    Hook{"user32.dll", "GetForegroundWindow", (void*)&GetForegroundWindow_hook, (void**)&GetForegroundWindow_orig, "GetForegroundWindow", false},
    Hook{"user32.dll", "GetActiveWindow", (void*)&GetActiveWindow_hook, (void**)&GetActiveWindow_orig, "GetActiveWindow", false},
    Hook{"user32.dll", "GetFocus", (void*)&GetFocus_hook, (void**)&GetFocus_orig, "GetFocus", false},
    Hook{"user32.dll", "GetCursorPos", (void*)&GetCursorPos_hook, (void**)&GetCursorPos_orig, "GetCursorPos", false},
    Hook{"user32.dll", "SetWindowsHookExA", (void*)&SetWindowsHookExA_hook, (void**)&SetWindowsHookExA_orig, "SetWindowsHookExA", false},
    Hook{"user32.dll", "SetWindowsHookExW", (void*)&SetWindowsHookExW_hook, (void**)&SetWindowsHookExW_orig, "SetWindowsHookExW", false},
    Hook{"user32.dll", "UnhookWindowsHookEx", (void*)&UnhookWindowsHookEx_hook, (void**)&UnhookWindowsHookEx_orig, "UnhookWindowsHookEx", false},
    Hook{"user32.dll", "SystemParametersInfoA", (void*)&SystemParametersInfoA_hook, (void**)&SystemParametersInfoA_orig, "SystemParametersInfoA", false},
    Hook{"user32.dll", "SystemParametersInfoW", (void*)&SystemParametersInfoW_hook, (void**)&SystemParametersInfoW_orig, "SystemParametersInfoW", false},
    HOOK(RenderText2d_XY_C_U16, RT_XY_C), HOOK(RenderText2d_XY_CC_U16, RT_XY_CC),
    HOOK(RenderText2d_XY_U16_FontName, RT_XY_FN), HOOK(RenderText2d_XY_U16_FontName2, RT_XY_FN2),
    HOOK(RenderText2dBox_XY_C_U16_Font, RTB_F), HOOK(RenderText2dBox_XY_C_U16_FontName, RTB_FN),
    HOOK(RenderColoredText2d_U16, RCT), HOOK(RenderText2dParagraph, RTP),
    HOOK(LocalizationManager_GetText, LocGetText),
    HOOK(Display_Update, DisplayUpdate), HOOK(Engine_Update, EngineUpdate), HOOK(Display_HandleMouseEvent, DisplayMouse),
    HOOK(Engine_ProcessUserInput, ProcessUserInput_hook),
  };
  HMODULE eng = GetModuleHandleA("Engine.dll");
  g_ButtonEvent_GetText = eng ? (ButtonEvent_GetText_t)GetProcAddress(eng, ButtonEvent_GetText) : nullptr;
  g_ButtonEvent_dtor = eng ? (ButtonEvent_ctor_t)GetProcAddress(eng, ButtonEvent_dtor) : nullptr;
  g_ButtonEvent_ctor = eng ? (ButtonEvent_ctor_t)GetProcAddress(eng, ButtonEvent_ctor) : nullptr;
  g_MouseEvent_ctor = eng ? (MouseEvent_ctor_t)GetProcAddress(eng, MouseEvent_ctor) : nullptr;
  LONG r = attach_all(g_hooks);
  install_late();  // no-op unless DirectInput.dll is already loaded (late injection)
  return r == NO_ERROR;
}

void remove() {
  DetourTransactionBegin();
  ThreadUpdater threads;
  for (auto* v : {&g_hooks, &g_late})
    for (auto& h : *v) if (h.ok && *h.orig) DetourDetach(h.orig, h.detour);
  DetourTransactionCommit();
}
}  // namespace gd::hooks
