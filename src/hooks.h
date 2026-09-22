#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "core/input.h"

namespace gd::hooks {
bool install();
void remove();

// A detour on an exported function (looked up by decorated name from src/gd_names.h). Other modules keep
// their own tables (src/world.cpp) and attach them through attach_hooks; GD_HOOK(ID, detour) builds an entry
// from an ID in gd_names.h and a detour whose original-pointer variable is named <detour>_orig.
struct Hook { const char* dll; const char* name; void* detour; void** orig; const char* label; bool ok; };
#define GD_HOOK(ID, DETOUR) gd::hooks::Hook{ID##_DLL, ID, (void*)&DETOUR, (void**)&DETOUR##_orig, #ID, false}
long attach_hooks(std::vector<Hook>& hooks);  // one Detours transaction; failures are logged per entry
void detach_hooks(std::vector<Hook>& hooks);

// ---- game-thread job pump (jobs run inside Display::Update) ----
// Blocks the calling thread until the job ran, or returns false on timeout.
bool run_on_game_thread(std::function<void()> fn, unsigned timeout_ms = 5000);

// ---- synthetic keyboard input, delivered through the game's own input poll ----
struct SynthKey { int code; bool released; bool shift, ctrl, alt; char16_t ch; };
void push_key_event(const SynthKey& k);                  // one event, its own frame
void push_key(int code, bool shift, bool ctrl, bool alt, char16_t ch);  // press frame + release frame
// Deliver a key to the game without also feeding it back into Grimdark's own bindings.  Lifted native
// commands use this so Ctrl+V opens Achievements rather than becoming the mod's plain-V review command.
void push_game_key(int code, bool shift, bool ctrl, bool alt, char16_t ch);
void set_game_keys_muted(bool m);                        // the game sees no physical key events ...
bool game_keys_muted();
void set_game_key_filter(std::function<bool(int code, bool released, bool shift, bool ctrl)> pass);  // ... except events this says to pass through (game thread); the modifiers are the EVENT's own flags

// ---- synthetic mouse input (same mechanism as keys; client-area coordinates) ----
// MouseEvent (28 bytes, measured 2026-08-21 from DirectInputDevice::Update): +0 int type: 0 = idle/position,
// 1 = left down, 9 = left up, 2 = right down, 10 = right up, 3..8 = buttons 3..8 down, 11..16 = their ups,
// 17/18 = wheel up/down; +4 float x, +8 float y; +0x10 left held, +0x11 right held, +0x12..0x17 buttons 3..8 held;
// +0x18 window-active flag, +0x19 shift, +0x1a alt, +0x1b ctrl.
struct SynthMouse { int type; float x, y; uint8_t left, right; uint8_t held[6]; bool shift, ctrl, alt; };
void push_mouse_event(const SynthMouse& m);              // one event, its own frame
void click(float x, float y, int button = 1);            // down + up over two frames, cursor overridden meanwhile (1 left, 2 right, 3 middle)
// A real hold: the down transition is injected once, then every real event the game polls reports the button
// held (the game re-issues the command each tick -- move, attack, skill -- and tears it down on the first
// event with both buttons up), and the up transition is injected on release. Position = the cursor override
// when one is on, else the real cursor. Game thread.
// The transition is delivered at (x, y), client coordinates, which the caller must keep INSIDE the window: the
// exe's mouse handler ignores off-window events, and a lost transition leaves its own "button held" byte set
// (world screen +0x88/+0x89), which mutes WASD until a transition inside the window clears it (2026-09-01).
void set_mouse_hold(int button, bool held, float x, float y);   // 1 left, 2 right
bool real_cursor_in_window(float& x, float& y);         // the OS cursor in client coordinates; false when outside the client area
bool mouse_held(int button);
void set_cursor_override(bool on, float x, float y);
void set_fake_active(bool on);                           // dev: make the game believe its window is active     // DirectInputDevice::GetCursorPosition returns this
std::string button_query_stats();                        // which Button values the game polls via IsButtonDown

uint64_t frame();
std::string counters();
// Tutorial tips the game fetched recently (tag, text with colour codes stripped, frame), oldest first.
struct Tip { std::string tag, text; uint64_t frame; };
std::vector<Tip> recent_tips();
std::string localize(const char* tag);  // a localization tag -> the game's text (empty until the game has localized anything)
void* engine_object();  // the GAME::Engine instance, captured from its input poll (null before the first frame)
// Keyboard state as seen in the game's device poll this frame (real AND synthetic events), for the mod's
// own InputManager. Valid between the poll and the end of Display::Update; cleared per frame.
const gd::core::KeySource& key_source();
std::u16string_view typed_chars();  // printable characters typed this frame (for type-ahead)
std::string button_names(int max_code);  // the game's own names for Button codes 0..max (via InputDevice::GetDefaultButtonName)  // input-path instrumentation for /health
}  // namespace gd::hooks
