// See exe_ui.h. Every RVA/offset below names the instruction that proved it in docs/exe-ui-layout.md.
#include "exe_ui.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <format>
#include <set>
#include "gameapi.h"
#include "gd_names.h"
#include "hooks.h"
#include "log.h"
#include "msvc_string.h"
#include "world.h"

namespace gd::exe_ui {
namespace {
namespace rva {
constexpr uintptr_t kMainObj = 0x3ceef8;        // the exe's application object global (stored at exe+0x86cff)
constexpr uintptr_t kButtonA1 = 0x30c018;       // framework A button vtable, HandleMouseEvent exe+0xa01d0
constexpr uintptr_t kButtonA2 = 0x30bf40;       // framework A button vtable (text-width variant), exe+0xa22d0
constexpr uintptr_t kMenuManagerVt = 0x30cca0;  // main-menu manager (ctor exe+0xd4fc0)
constexpr uintptr_t kTextA = 0x30c5e8;          // framework A static text (draw exe+0xb1ba0: string +0xc0, wrap width +0xe0)
constexpr uintptr_t kEditA = 0x30d588;          // framework A edit box (draw exe+0xee730: string +0x238, caret +0x21c; keys exe+0xedc80)
constexpr uintptr_t kSliderA = 0x30d4c0;        // options slider (ctor exe+0xeac40; thumb math exe+0xebb46)
constexpr uintptr_t kComboA = 0x30c278;         // drop-down (commit exe+0xab951)
constexpr uintptr_t kListA = 0x30c530;          // key-binding table (ctor exe+0xaf8f0, rows exe+0xafe20)
constexpr uintptr_t kOptionsScreenVt = 0x30cad0, kOptionsPanelVt = 0x30d650, kOptionsPageVt = 0x30cbb8;  // measured live 2026-08-22
constexpr uintptr_t kConvWindowVt = 0x3157a8, kConvRowVt = 0x315710;  // ctors exe+0x16e9a0 / exe+0x16d9b0
constexpr uintptr_t kPopupLayerVt = 0x30bd80;   // modal layer at the root holding a popup window (measured live: the name-exists box)
constexpr uintptr_t kConfirmLayerVt = 0x30bc88; // Yes/No confirmation layer (cloud save operations, Options discard)
constexpr uintptr_t kPopupWindowVt = 0x30d650;  // the popup window itself (text widgets + buttons)
constexpr uintptr_t kButtonB = 0x313e78;        // framework B button vtable (ctor exe+0x124d60, size 0x388)
constexpr uintptr_t kTextButtonB = 0x313ce8;    // framework B TextButton vtable (ctor exe+0x126fe0, size 0x3b0; caption +0x358)
constexpr uintptr_t kTextB = 0x31c7c0;          // framework B text element vtable (draw exe+0x25b700)
constexpr uintptr_t kTitleTextB = 0x31c2b0;     // framework B title/caption text element (the shrine windows' title +0x540; u16 at +0x40; set through vt+0x18 exe+0x1adb30)
constexpr uintptr_t kNumberTextB = 0x31c4d0;    // framework B number/name text element (same ctor exe+0x2595c0 as the title text: string at +0x40; the Inventor's cost / dynamite numbers and NPC name)
constexpr uintptr_t kEnchanterBoxSalvage = 0x316c28, kEnchanterBoxDismantle = 0x3169f8, kEnchanterBoxResult = 0x316878;   // the Inventor's item boxes (ctor exe+0x1ad7b0; item id +0xa0, SetItem = vt+0xa8)
constexpr uintptr_t kTextBlockB = 0x31b830;     // framework B multi-line text block (the shrine windows' info +0x638; u16 at +0x38; set through vt+0xa0 exe+0x2401e0)
}  // namespace rva
namespace off {
constexpr size_t kMainObj_UiRoot = 0x88;        // MenuManager (DisplayWidget) -- exe+0xa02f6
constexpr size_t kMainObj_WorldScreen = 0x90;   // the world screen object -- exe+0x1099f0
constexpr size_t kMainObj_App = 0x250;          // the exe's Display subclass -- main loop exe+0xeeb8
constexpr size_t kApp_State = 0x260;            // current app state -- exe+0xbe3d0
constexpr size_t kApp_MainMenu = 0x298;         // main-menu manager -- exe+0xbe4b5
constexpr size_t kWorldScreen_InGameUI = 0x2f0; // exe+0x200dc
constexpr size_t kMI_Node = 8;                  // DisplayWidget at 0, tree node at +8 (thunks at exe+0xc324c)
constexpr size_t kMenu_CurrentSub = 0xf8;       // exe+0xd88b2
// framework A tree node (base class "B"): GetRect = exe+0xa2b70 (lea rax,[rcx+0x18]); child walk exe+0xa30a0
constexpr size_t kA_Parent = 0x08, kA_Rect = 0x18, kA_ChildBegin = 0x38, kA_ChildEnd = 0x40, kA_Active = 0x50, kA_Visible = 0x51;
// framework A button (exe+0xa01d0): listeners +0x230/+0x238, hovered +0x248, pressed +0x249, caption +0x268 (exe+0x9f870)
constexpr size_t kA_ListBegin = 0x230, kA_ListEnd = 0x238, kA_Hovered = 0x248, kA_Pressed = 0x249, kA_Toggle = 0x24a, kA_Caption = 0x268;
constexpr size_t kA_TextString = 0xc0;          // static text widget -- exe+0xb1c5f (size at +0xd0)
constexpr size_t kA_EditString = 0x238, kA_EditCaret = 0x21c, kA_EditFocus = 0x218;  // exe+0xeea41, +0xeebf2, +0xeebe5 (focus byte inferred)
// framework B
constexpr size_t kB_Text = 0x40;                // text element's u16 string -- exe+0x25b7cc
constexpr size_t kB_Visible = 0x28, kB_Disabled = 0x281, kB_Pressed = 0x282;  // PromptBox::Update writes / TextButton render
constexpr size_t kTB_Caption = 0x358;           // TextButton's localized caption -- exe+0x127510
constexpr size_t kVt_HostClick = 0x80;          // listener registry: PressChild(control, playSound) -- exe+0x211c05, exe+0x12ac80
constexpr size_t kVt_Show = 0xb0, kVt_IsVisible = 0xb8;  // InGameUI::CloseAllWindows / IsAnyWindowOpen
constexpr size_t kPrompt_Showing = 0x99;        // PromptBox "a prompt is on screen" -- exe+0x190581
// GAME::DialogManager::Dialog (copy ctor exe+0x1905b0)
constexpr size_t kDialog_Text = 0x00, kDialog_Party = 0x60, kDialog_Type = 0x64;
}  // namespace off

// Code bytes at sites the layout depends on (from the unpacked image, 2026-08-22).
struct Signature { uintptr_t rva; const char* what; const char* bytes; };
const Signature kSignatures[] = {
  {0x281640, "Illusionist selection", "\x88\x54\x24\x10\x55\x53\x56\x57\x41\x54"},
  {0x2823e0, "Illusionist appearance pages", "\x48\x8b\xc4\x57\x41\x54\x41\x55\x41\x56"},
  {0xc324c, "node->DisplayWidget thunk", "\x48\x83\xe9\x08\xe9"},
  {0xa30a0, "tree HandleMouseEvent", "\x48\x89\x5c\x24\x08\x48\x89\x74\x24\x10\x57\x48\x83\xec\x20\x80"},
  {0xbb2c0, "App::RequestState", "\x40\x53\x48\x83\xec\x20\x8d\x42\xfa\x89\x91\x58"},
  {0xa01d0, "button A1 HandleMouseEvent", "\x48\x89\x5c\x24\x18\x57\x48\x83\xec\x40\x80\x79"},
  {0xa22d0, "button A2 HandleMouseEvent", "\x48\x89\x5c\x24\x10\x57\x48\x83\xec\x50\x80\x79"},
  {0x1903b0, "PromptBox::Update", "\x40\x57\x48\x81\xec\xc0\x00\x00\x00\x48\xc7\x44"},
  {0xbe320, "App::ApplyPendingState", "\x48\x8b\xc4\x57\x48\x83\xec\x60\x48\xc7\x40\xc8"},
  {0x213840, "InGameUI::Init", "\x48\x8b\xc4\x55\x53\x56\x57\x41\x54\x41\x55\x41"},
  {0x211980, "InGameUI::HandleKeyAction", "\x48\x8b\xc4\x57\x41\x54\x41\x55\x41\x56\x41\x57\x48\x83\xec\x40"},
  {0x27c580, "SkillsWindow::SetPane", "\x40\x57\x48\x83\xec\x30\x48\xc7\x44\x24\x20\xfe\xff\xff\xff\x48"},
  {0x21be20, "riftgate map open", "\x40\x53\x48\x83\xec\x20\x48\x8d\x99\x60\x22\x04\x00\x48\x8b\x03"},
  {0x25d890, "caravan tab list rebuild", "\x48\x89\x5c\x24\x18\x55\x56\x41\x56\x48\x83\xec\x20\x48\x8b\xd9"},
  {0x12ec70, "caravan sack dims", "\x40\x57\x48\x83\xec\x20\x0f\x57\xc0\x48\x8b\xf9\x0f\x2e\x81\x28"},
  {0x291520, "WorldMapWindow travel", "\x48\x89\x5c\x24\x08\x48\x89\x74\x24\x10\x57\x48\x81\xec\xa0\x00"},
  {0x28ed20, "WorldMap Icon ctor", "\x48\x89\x5c\x24\x08\x48\x89\x6c\x24\x10\x48\x89\x74\x24\x18\x48"},
  {0x8a040, "CharacterPicker::HandleMouseEvent", "\x40\x53\x56\x57\x48\x83\xec\x50\x0f\x29\x74\x24\x40\x48\x8b\xd9"},
  {0x185640, "DevotionWindow ctor", "\x48\x89\x4c\x24\x08\x55\x56\x57\x41\x54\x41\x55\x41\x56\x41\x57"},
  {0x17ea10, "Star::HandleMouseEvent", "\x48\x8b\xc4\x55\x56\x57\x41\x54\x41\x55\x41\x56\x41\x57\x48\x81"},
};
const size_t kSignatureLens[] = {10, 10, 5, 16, 12, 12, 12, 12, 12, 12, 16, 16, 16, 16, 16, 16, 16, 16};   // each <= kSignatureMax
static_assert(std::size(kSignatureLens) == std::size(kSignatures));
constexpr size_t kSignatureMax = 16;

uintptr_t g_base = 0;
size_t g_image_size = 0;
bool g_available = false;
std::string g_version;

struct {
  void* (*GetDialogManager)(void*) = nullptr;
  int (*GetNumDialog)(const void*) = nullptr;
  const void* (*PeekTopDialog)(void*) = nullptr;
  void (*AddResponse)(void*, const void*) = nullptr;
  void (*RemoveTopDialog)(void*) = nullptr;
  void (*SetLastUsedTeleportId)(void*, const void*) = nullptr;   // GameEngine, UniqueId const&
} g_dm;

// ---- guarded memory access (SEH: the functions hold no C++ objects) ----
bool read_mem(const void* src, void* dst, size_t n) {
  __try { memcpy(dst, src, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template <class T> bool rd(const void* base, size_t off, T& out) { return base && read_mem((const char*)base + off, &out, sizeof out); }
template <class T> T rd_or(const void* base, size_t off, T def) { T v; return rd(base, off, v) ? v : def; }
void* rdp(const void* base, size_t off) { return rd_or<void*>(base, off, nullptr); }
bool write_byte(void* base, size_t off, uint8_t v) {
  __try { *((uint8_t*)base + off) = v; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool write_int(void* base, size_t off, int v) {
  __try { *(int*)((char*)base + off) = v; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool in_exe(const void* p) { uintptr_t a = (uintptr_t)p; return a >= g_base && a < g_base + g_image_size; }
uintptr_t exe_rva(const void* p) { return in_exe(p) ? (uintptr_t)p - g_base : 0; }
uintptr_t vtable_rva_of(const void* obj) { return exe_rva(rdp(obj, 0)); }

std::string read_u16(const void* base, size_t off) {
  MsvcStringW s{};
  if (!rd(base, off, s) || s.size > 4096) return {};
  if (s.capacity < 8) return log::utf8(std::u16string_view(s.u.buf, s.size));
  std::u16string buf(s.size, u'\0');
  if (!s.u.ptr || !read_mem(s.u.ptr, buf.data(), s.size * 2)) return {};
  return log::utf8(buf);
}

typedef void (*ListenerFn)(void*, void*, int);
typedef bool (*BoolThis)(void*);
typedef void (*VoidThisBool)(void*, bool);
typedef void (*HostClickFn)(void*, void*, bool);
bool call_listener(void* l, void* w, int code) {
  __try { ((ListenerFn)(*(void***)l)[0])(l, w, code); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool call_bool(void* obj, size_t vt_off, bool& out) {
  __try { out = ((BoolThis)(*(void***)obj)[vt_off / 8])(obj); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool call_void_bool(void* obj, size_t vt_off, bool arg) {
  __try { ((VoidThisBool)(*(void***)obj)[vt_off / 8])(obj, arg); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool call_host_click(void* host, void* child, bool sound = true) {
  __try { ((HostClickFn)(*(void***)host)[off::kVt_HostClick / 8])(host, child, sound); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void* main_obj() { return g_base ? rdp((void*)(g_base + rva::kMainObj), 0) : nullptr; }
// The main-menu manager is the tree node directly under the root whose vtable is the manager's (its primary
// vtable IS the tree-node vtable: the node pointer is the object). Measured live 2026-08-22.
void* menu_manager_obj() {
  void* mm = rdp(main_obj(), off::kMainObj_UiRoot);
  if (!mm) return nullptr;
  WidgetA r{(char*)mm + off::kMI_Node};
  for (WidgetA c : r.children()) if (c.vtable_rva() == rva::kMenuManagerVt) return c.p;
  return nullptr;
}
// The app object: reached through the manager's back-pointer (+0x118, exe+0xd888b) and validated by its own
// manager slot (+0x298) pointing back. [main_obj+0x250] is NOT it (measured live: garbage).
void* g_app = nullptr;  // lives as long as the process (the exe's Display subclass); remembered once validated
void* app() {
  if (g_app) return g_app;
  void* mm = menu_manager_obj();
  void* a = rdp(mm, 0x118);
  if (a && rdp(a, off::kApp_MainMenu) == mm) g_app = a;
  return g_app;
}
void* dialog_manager() { void* ge = world::game_engine(); return ge && g_dm.GetDialogManager ? g_dm.GetDialogManager(ge) : nullptr; }
}  // namespace

// The DLL is injected before the SteamStub unpacks the exe, so the code bytes cannot be checked at install
// time; the check runs on first use, from the game thread (the first tick / dev route), when the image is plain.
namespace { bool g_checked = false; bool check_layout(); }
bool install() {
  g_base = (uintptr_t)GetModuleHandleW(nullptr);
  auto* dos = (const IMAGE_DOS_HEADER*)g_base;
  auto* nt = (const IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew);
  g_image_size = nt->OptionalHeader.SizeOfImage;
  HMODULE game = GetModuleHandleA("Game.dll");
  bool ok = true;
#define LOAD(f, ID) g_dm.f = game ? (decltype(g_dm.f))GetProcAddress(game, names::ID) : nullptr; if (!g_dm.f) { ok = false; log::writef("exe_ui: export {} missing", #ID); }
  LOAD(GetDialogManager, GameEngine_GetDialogManager);
  LOAD(GetNumDialog, DialogManager_GetNumDialog);
  LOAD(PeekTopDialog, DialogManager_PeekTopDialog);
  LOAD(AddResponse, DialogManager_AddResponse);
  LOAD(RemoveTopDialog, DialogManager_RemoveTopDialog);
  LOAD(SetLastUsedTeleportId, GameEngine_SetLastUsedTeleportId);
#undef LOAD
  g_version = std::format("exe timestamp={:#x} image={:#x} layout=unchecked", nt->FileHeader.TimeDateStamp, g_image_size);
  g_checked = false;
  return ok;
}
namespace {
bool check_layout() {
  if (g_checked) return g_available;
  g_checked = true;
  bool ok = g_dm.GetDialogManager && g_dm.GetNumDialog && g_dm.PeekTopDialog && g_dm.AddResponse && g_dm.RemoveTopDialog;
  for (size_t i = 0; i < std::size(kSignatures); ++i) {
    const Signature& s = kSignatures[i];
    uint8_t buf[kSignatureMax] = {};
    size_t n = kSignatureLens[i] <= kSignatureMax ? kSignatureLens[i] : kSignatureMax;   // a longer signature overflowed this buffer once (2026-08-22)
    bool match = s.rva + n <= g_image_size && read_mem((void*)(g_base + s.rva), buf, n) && memcmp(buf, s.bytes, n) == 0;
    if (!match) { ok = false; log::writef("exe_ui: signature MISMATCH at exe+{:#x} ({})", s.rva, s.what); }
  }
  // The button vtables must dispatch HandleMouseEvent (+0x20) to the functions the signatures cover.
  if (exe_rva(rdp((void*)(g_base + rva::kButtonA1), 0x20)) != 0xa01d0) { ok = false; log::write("exe_ui: button A1 vtable mismatch"); }
  if (exe_rva(rdp((void*)(g_base + rva::kButtonA2), 0x20)) != 0xa22d0) { ok = false; log::write("exe_ui: button A2 vtable mismatch"); }
  g_available = ok;
  g_version = g_version.substr(0, g_version.find(" layout=")) + (ok ? " layout=ok" : " layout=MISMATCH");
  log::writef("exe_ui: {}", g_version);
  return ok;
}
}  // namespace
bool available() { return check_layout(); }
std::string version_line() { return g_version; }

// ---- framework A ----
WidgetA WidgetA::parent() const { return {rdp(p, off::kA_Parent)}; }
std::vector<WidgetA> WidgetA::children() const {
  std::vector<WidgetA> out;
  void* b = rdp(p, off::kA_ChildBegin); void* e = rdp(p, off::kA_ChildEnd);
  if (!b || !e || e < b) return out;
  size_t n = ((char*)e - (char*)b) / sizeof(void*);
  if (n > 512) return out;
  for (size_t i = 0; i < n; ++i) { void* c = rdp(b, i * sizeof(void*)); if (c) out.push_back({c}); }
  return out;
}
Rect WidgetA::rect() const { return rd_or<Rect>(p, off::kA_Rect, Rect{}); }
Rect WidgetA::abs_rect() const {
  Rect r = rect();
  WidgetA q = parent();
  for (int depth = 0; q && depth < 32; ++depth, q = q.parent()) { Rect pr = q.rect(); r.x += pr.x; r.y += pr.y; }
  return r;
}
bool WidgetA::active() const { return rd_or<uint8_t>(p, off::kA_Active, 0) != 0; }
bool WidgetA::enabled() const { return rd_or<uint8_t>(p, off::kA_Visible, 0) != 0; }
uintptr_t WidgetA::vtable_rva() const { return vtable_rva_of(p); }
bool WidgetA::is_button() const { uintptr_t v = vtable_rva(); return v == rva::kButtonA1 || v == rva::kButtonA2; }
bool WidgetA::is_text() const { return vtable_rva() == rva::kTextA; }
bool WidgetA::is_edit() const { return vtable_rva() == rva::kEditA; }
std::string WidgetA::text() const {
  if (is_button()) return read_u16(p, off::kA_Caption);
  if (is_text()) return read_u16(p, off::kA_TextString);
  if (is_edit()) return read_u16(p, off::kA_EditString);
  return {};
}
std::string WidgetA::tooltip_tag() const {
  if (!is_button()) return {};
  MsvcStringA s{};
  if (!rd(p, 0x78, s) || s.size > 256) return {};
  if (s.capacity < 16) return std::string(s.u.buf, s.size);
  std::string out(s.size, '\0');
  return s.u.ptr && read_mem(s.u.ptr, out.data(), s.size) ? out : std::string();
}
bool WidgetA::hovered() const { return rd_or<uint8_t>(p, off::kA_Hovered, 0) != 0; }
bool WidgetA::pressed() const { return rd_or<uint8_t>(p, off::kA_Pressed, 0) != 0; }
std::string WidgetA::edit_state() const {
  return std::format("caret={} +0x218={} +0x230={} +0x231={} +0x258={}", rd_or<int>(p, off::kA_EditCaret, -1), rd_or<uint8_t>(p, off::kA_EditFocus, 0xff), rd_or<uint8_t>(p, 0x230, 0xff),
                     rd_or<uint8_t>(p, 0x231, 0xff), rd_or<uint8_t>(p, 0x258, 0xff));
}
// The A2 class (exe+0xa22d0) always toggles: left-down flips pressed and fires the listeners once, there is no
// release path; the A1 class (exe+0xa01d0) toggles only with its +0x24a flag set.
bool WidgetA::is_toggle() const { return vtable_rva() == rva::kButtonA2 || rd_or<uint8_t>(p, off::kA_Toggle, 0) != 0; }
bool WidgetA::activate() const {
  if (!is_button()) return false;
  void* b = rdp(p, off::kA_ListBegin); void* e = rdp(p, off::kA_ListEnd);
  if (!b || !e || e < b) return false;
  size_t n = ((char*)e - (char*)b) / sizeof(void*);
  if (n > 64) return false;
  std::vector<void*> ls;
  for (size_t i = 0; i < n; ++i) { void* l = rdp(b, i * sizeof(void*)); if (l) ls.push_back(l); }
  bool ok = true;
  write_byte(p, off::kA_Hovered, 1);
  if (is_toggle()) {  // exe+0xa01d0 left-down: pressed = !pressed; listeners(0); (no release path for toggles)
    write_byte(p, off::kA_Pressed, pressed() ? 0 : 1);
    for (void* l : ls) ok = call_listener(l, p, 0) && ok;
  } else {
    write_byte(p, off::kA_Pressed, 1);
    for (void* l : ls) ok = call_listener(l, p, 0) && ok;   // pressed
    for (void* l : ls) ok = call_listener(l, p, 2) && ok;   // released inside = clicked
    write_byte(p, off::kA_Pressed, 0);
  }
  log::writef("exe_ui: activated {} {} '{}' listeners={} ok={}", is_toggle() ? "toggle" : "button", p, caption(), ls.size(), ok);
  return ok;
}
std::vector<WidgetA> WidgetA::buttons() const { std::vector<WidgetA> v; for (WidgetA c : children()) if (c.is_button()) v.push_back(c); return v; }
std::vector<WidgetA> WidgetA::texts() const { std::vector<WidgetA> v; for (WidgetA c : children()) if (c.is_text()) v.push_back(c); return v; }
std::vector<WidgetA> WidgetA::edits() const { std::vector<WidgetA> v; for (WidgetA c : children()) if (c.is_edit()) v.push_back(c); return v; }

Popup popup() {
  auto in_layer = [](WidgetA layer) -> Popup {
    uintptr_t vt = layer.vtable_rva();
    if ((vt != rva::kPopupLayerVt && vt != rva::kConfirmLayerVt) || !layer.active()) return {};
    for (WidgetA w : layer.children()) {
      if (w.vtable_rva() != rva::kPopupWindowVt || !w.active()) continue;
      // The Network page briefly creates an empty layer while probing UPnP. It is not a modal prompt and can
      // disappear during the same update, so require the visible prompt payload rather than the layer alone.
      bool has_text = false, has_button = false;
      for (WidgetA t : w.texts()) if (t.active() && !t.text().empty()) { has_text = true; break; }
      for (WidgetA b : w.buttons()) if (b.active() && !b.caption().empty()) { has_button = true; break; }
      if (has_text && has_button) return {w};
    }
    return {};
  };
  // Ordinary menu popups are root children. Options owns its confirmation layer beneath the screen object.
  for (WidgetA top : root().children()) {
    if (Popup p = in_layer(top)) return p;
    for (WidgetA layer : top.children()) if (Popup p = in_layer(layer)) return p;
  }
  return {};
}
std::string Popup::text() const {
  std::string out;
  for (WidgetA t : window.texts()) { if (!t.active()) continue; std::string s = t.text(); if (s.empty()) continue; if (!out.empty()) out += ' '; out += s; }
  return out;
}

int app_state() { return rd_or<int>(app(), off::kApp_State, 0); }
WidgetA root() { void* mm = rdp(main_obj(), off::kMainObj_UiRoot); return mm ? WidgetA{(char*)mm + off::kMI_Node} : WidgetA{}; }
MainMenu main_menu() { return {menu_manager_obj()}; }
WidgetA MainMenu::button(unsigned slot_off) const { return {rdp(p, slot_off)}; }
// CharacterPicker (static RE 2026-08-22, class size 0x610, vtable exe+0x30af80, HandleMouseEvent exe+0x8a040):
// +0xc0 selected index, +0xc4 first visible, +0xc8/+0xd0 vector<Entry> (stride 0x90), +0x118 rows per page.
// Entry: +0x20 u16 name, +0x60 std::string class tag (empty = no mastery), +0x80 level, +0x84 hardcore,
// +0x85 female, +0x88 the preview Player's object id. A left-up on a row writes +0xc0 and nothing else.
namespace {
constexpr size_t kPk_Selected = 0xc0, kPk_First = 0xc4, kPk_Begin = 0xc8, kPk_End = 0xd0, kPk_PerPage = 0x118;
constexpr size_t kEntry_Size = 0x90, kEntry_Name = 0x20, kEntry_Class = 0x60, kEntry_Level = 0x80, kEntry_Hardcore = 0x84, kEntry_Female = 0x85, kEntry_Preview = 0x88;
std::string read_ascii(const void* base, size_t off) {   // a std::string (char) field
  MsvcString<char> s{};
  if (!rd(base, off, s) || s.size > 4096) return {};
  if (s.capacity < 16) return std::string(s.u.buf, s.size);
  std::string buf(s.size, '\0');
  if (!s.u.ptr || !read_mem(s.u.ptr, buf.data(), s.size)) return {};
  return buf;
}
}  // namespace
std::vector<MainMenu::Character> MainMenu::characters() const {
  std::vector<Character> out;
  void* pk = rdp(p, kPicker);
  if (!pk) return out;
  char* b = (char*)rdp(pk, kPk_Begin); char* e = (char*)rdp(pk, kPk_End);
  if (!b || !e || e < b || (size_t)(e - b) > 64 * kEntry_Size) return out;
  for (char* it = b; it + kEntry_Size <= e; it += kEntry_Size) {
    Character c;
    c.name = read_u16(it, kEntry_Name);
    c.class_tag = read_ascii(it, kEntry_Class);
    c.level = rd_or<uint32_t>(it, kEntry_Level, 0);
    c.hardcore = rd_or<uint8_t>(it, kEntry_Hardcore, 0) != 0;
    c.female = rd_or<uint8_t>(it, kEntry_Female, 0) != 0;
    c.preview_id = rd_or<uint32_t>(it, kEntry_Preview, 0);
    out.push_back(std::move(c));
  }
  return out;
}
int MainMenu::selected_character() const { void* pk = rdp(p, kPicker); return pk ? rd_or<int>(pk, kPk_Selected, -1) : -1; }
bool MainMenu::select_character(int index) const {
  void* pk = rdp(p, kPicker);
  if (!pk) return false;
  char* b = (char*)rdp(pk, kPk_Begin); char* e = (char*)rdp(pk, kPk_End);
  int n = (b && e && e >= b) ? (int)((e - b) / kEntry_Size) : 0;
  if (index < 0 || index >= n) return false;
  int per = rd_or<int>(pk, kPk_PerPage, 1); if (per < 1) per = 1;
  int first = rd_or<int>(pk, kPk_First, 0);
  if (!write_int(pk, kPk_Selected, index)) return false;
  if (index < first) write_int(pk, kPk_First, index);
  else if (index >= first + per) write_int(pk, kPk_First, index - per + 1);
  return true;
}
void* MainMenu::sub_window(unsigned slot_off) const { return rdp(p, slot_off); }
void* MainMenu::current_sub_window() const { return rdp(p, off::kMenu_CurrentSub); }
WidgetA window_node(void* window) { return {window}; }
bool window_hidden_flag(void* window, unsigned flag_off) { return rd_or<uint8_t>(window, flag_off, 1) != 0; }

// ---- framework B ----
void* ingame_ui() { return rdp(rdp(main_obj(), off::kMainObj_WorldScreen), off::kWorldScreen_InGameUI); }
bool WindowB::visible() const { bool v = false; return p && call_bool(p, off::kVt_IsVisible, v) && v; }

// ---- the riftgate travel map (static RE 2026-08-22, docs/exe-ui-layout.md "Riftgate travel") ----
namespace {
// MiniMap (InGameUI+0x42260): +0x68 shown, +0x418 mode (1 = the local map, 0 = the riftgate world map); the
// WorldMapWindow is its sub-object at +0x7940: +0x118 std::list of sections (node+0x30 = section; section
// +0x08/+0x10 = vector<Icon*>), +0x200 = the object id of the gate being used (0 from the L key).
// Icon (ctor exe+0x28ed20): +0x00 state (1 = current), +0x128 int[3] world position, +0x134 owner player id,
// +0x138 the gate's object id, +0x140 u16string name, +0x160 UniqueId (4 ints).
constexpr size_t kMM_Shown = 0x68, kMM_Mode = 0x418, kMM_WorldMap = 0x7940;
constexpr size_t kWM_Sections = 0x118, kWM_Here = 0x200;
constexpr size_t kSec_Begin = 0x08, kSec_End = 0x10, kNode_Section = 0x30;
constexpr size_t kIcon_State = 0x00, kIcon_Pos = 0x128, kIcon_Owner = 0x134, kIcon_ObjId = 0x138, kIcon_Name = 0x140, kIcon_Uid = 0x160;
constexpr uintptr_t kWorldMap_Travel = 0x291520;   // (unused this, const int pos[3]): the distance guard + InitiatePlayerTeleport
typedef void (*TravelFn)(void*, const int*);
void* worldmap() { void* ui = ingame_ui(); return ui ? (char*)ui + ingame::kMiniMap + kMM_WorldMap : nullptr; }
}  // namespace
bool riftgate_map_open() {
  void* ui = ingame_ui();
  if (!ui || !available()) return false;
  WindowB mm{(char*)ui + ingame::kMiniMap};
  return mm.visible() && rd_or<uint8_t>(mm.p, kMM_Shown, 0) != 0 && rd_or<uint8_t>(mm.p, kMM_Mode, 1) == 0;
}
// The local aerial map (the M / Ctrl+M window): MiniMap shown with mode byte 1. This is the map whose nugget
// cache (aerial_nugget_span) the marker picker reads.
bool aerial_map_open() {
  void* ui = ingame_ui();
  if (!ui || !available()) return false;
  WindowB mm{(char*)ui + ingame::kMiniMap};
  return mm.visible() && rd_or<uint8_t>(mm.p, kMM_Shown, 0) != 0 && rd_or<uint8_t>(mm.p, kMM_Mode, 0) == 1;
}
void aerial_map_close() {
  WindowB mm = ingame_window(ingame::kMiniMap);
  if (mm) mm.show(false);
}
// Zoom fields: the map-update method exe+0x174e80 runs on the aerial sub-object at MiniMap+0xba0 (its nugget vector
// at +0x210 = MiniMap+0xdb0, the one aerial_nugget_span reads); it lerps [+0xacc] toward [+0xad0] and builds the
// icon frustum from the current value. Both written so the next update gathers at the new reach.
constexpr size_t kMM_Zoom = 0xba0 + 0xacc, kMM_ZoomTarget = 0xba0 + 0xad0;
float aerial_zoom() {
  void* ui = ingame_ui();
  if (!ui || !available()) return 0.0f;
  return rd_or<float>((char*)ui + ingame::kMiniMap, kMM_Zoom, 0.0f);
}
bool aerial_zoom_set(float zoom) {
  void* ui = ingame_ui();
  if (!ui || !available()) return false;
  if (zoom < kAerialZoomMin) zoom = kAerialZoomMin;
  if (zoom > kAerialZoomMax) zoom = kAerialZoomMax;
  char* mm = (char*)ui + ingame::kMiniMap;
  float cur = rd_or<float>(mm, kMM_Zoom, -1.0f);
  if (cur < kAerialZoomMin - 1.0f || cur > kAerialZoomMax + 1.0f) return false;   // not the field we think (layout drift): leave it alone
  __try {
    *(float*)(mm + kMM_Zoom) = zoom;
    *(float*)(mm + kMM_ZoomTarget) = zoom;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
std::vector<Riftgate> riftgates() {
  std::vector<Riftgate> out;
  void* wm = worldmap();
  if (!wm || !available()) return out;
  void* head = rdp(wm, kWM_Sections);        // the std::list's sentinel
  size_t count = rd_or<size_t>(wm, kWM_Sections + 8, 0);
  if (!head || count > 256) return out;
  unsigned here = (unsigned)rd_or<int>(wm, kWM_Here, 0);
  void* node = rdp(head, 0);
  for (size_t i = 0; node && node != head && i < count; ++i, node = rdp(node, 0)) {
    void* sec = rdp(node, kNode_Section);
    if (!sec) continue;
    char* b = (char*)rdp(sec, kSec_Begin); char* e = (char*)rdp(sec, kSec_End);
    if (!b || !e || e < b || (size_t)(e - b) > 64 * sizeof(void*)) continue;
    for (char* it = b; it < e; it += sizeof(void*)) {
      void* icon = rdp(it, 0);
      if (!icon) continue;
      Riftgate g;
      g.name = read_u16(icon, kIcon_Name);
      for (int k = 0; k < 3; ++k) g.pos[k] = rd_or<int>(icon, kIcon_Pos + k * 4, 0);
      g.owner = (unsigned)rd_or<int>(icon, kIcon_Owner, 0);
      g.object_id = (unsigned)rd_or<int>(icon, kIcon_ObjId, 0);
      for (int k = 0; k < 4; ++k) g.uid[k] = rd_or<int>(icon, kIcon_Uid + k * 4, 0);
      g.current = rd_or<int>(icon, kIcon_State, 0) == 1 || (g.object_id && g.object_id == here);
      // The map pre-builds an icon for every gate of the world (27 in the campaign, measured 2026-08-22); the
      // undiscovered ones sit at (0, 0, 0) with a zero UniqueId. Discovered = keyed (or a personal gate).
      bool discovered = g.uid[0] || g.uid[1] || g.uid[2] || g.uid[3] || g.owner;
      if (discovered) out.push_back(std::move(g));
    }
  }
  return out;
}
bool riftgate_travel(const Riftgate& g) {
  if (!available()) return false;
  void* ge = gd::world::game_engine();
  bool has_uid = g.uid[0] || g.uid[1] || g.uid[2] || g.uid[3];
  if (ge && has_uid && g_dm.SetLastUsedTeleportId) {
    __try { g_dm.SetLastUsedTeleportId(ge, g.uid); } __except (EXCEPTION_EXECUTE_HANDLER) { log::write("exe_ui: SetLastUsedTeleportId faulted"); return false; }
  }
  TravelFn f = (TravelFn)(g_base + kWorldMap_Travel);
  __try { f(nullptr, g.pos); } __except (EXCEPTION_EXECUTE_HANDLER) { log::write("exe_ui: riftgate travel faulted"); return false; }
  log::writef("exe_ui: riftgate travel to '{}' ({}, {}, {})", g.name, g.pos[0], g.pos[1], g.pos[2]);
  return true;
}
void riftgate_map_close() { WindowB mm = ingame_window(ingame::kMiniMap); if (mm) mm.show(false); }

// dev: the aerial map's cached MinimapGameNugget vector (built by the map-update method exe+0x174e80 via
// GameEngine::GetDetailMapData; struct = 0xA0 stride, type/class int at +0x08, WorldVec3 position at +0x58).
// Tries both candidate object bases (the MiniMap itself and its aerialMap sub at +0xb08).
// POD, SEH-guarded: read region-relative fields (no world_point on unvalidated memory). WorldVec3 at
// nugget+0x58 = Region*(+0x58) then Vec3(+0x60). Returns elems read (<= maxn), -1 on fault.
struct NugRaw { int type; void* region; float x, y, z; };
static int read_nuggets_raw(void* begin, size_t n, NugRaw* out, int maxn) {
  int got = 0;
  __try {
    for (size_t i = 0; i < n && got < maxn; ++i) {
      char* nug = (char*)begin + i * 0xA0;
      NugRaw r;
      r.type = *(int*)(nug + 0x08);
      r.region = *(void**)(nug + 0x58);
      r.x = *(float*)(nug + 0x60);
      r.y = *(float*)(nug + 0x64);
      r.z = *(float*)(nug + 0x68);
      out[got++] = r;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
  return got;
}
static bool finite_coord(float v) { return v == v && v > -2e4f && v < 2e4f; }
static bool heap_ptr(void* p) { uintptr_t a = (uintptr_t)p; return (a & 7) == 0 && a >= 0x10000000000ull && a < 0x20000000000000ull; }
static bool good_nug(const NugRaw& r) { return heap_ptr(r.region) && finite_coord(r.x) && finite_coord(r.y) && finite_coord(r.z) && !(r.x == 0 && r.z == 0); }
// Scan one object's fields for a mem::vector<0xA0> that looks like the nugget list (finite region-relative coords).
static std::string scan_for_nuggets(const char* who, void* obj, int maxn) {
  if (!obj) return std::format("{}: null\n", who);
  std::string out;
  for (size_t off = 0x10; off <= 0x1200; off += 8) {
    void* begin = rdp(obj, off);
    void* end = rdp(obj, off + 8);
    if (!begin || !end || (uintptr_t)end <= (uintptr_t)begin) continue;
    size_t bytes = (char*)end - (char*)begin;
    if (bytes % 0xA0 != 0 || bytes == 0 || bytes > 0xA0 * 8192) continue;
    size_t n = bytes / 0xA0;
    static NugRaw buf[256];
    int sample = (int)(n < 8 ? n : 8);
    NugRaw sbuf[8];
    int got = read_nuggets_raw(begin, n, sbuf, sample);
    if (got < sample) continue;
    int good = 0;
    for (int i = 0; i < got; ++i) if (good_nug(sbuf[i])) ++good;
    if (good < got) continue;   // require ALL sampled elements valid
    got = read_nuggets_raw(begin, n, buf, maxn < 256 ? maxn : 256);
    if (got <= 0) continue;
    out += std::format("{} +{:#x}: {} nuggets\n", who, off, n);
    for (int i = 0; i < got; ++i)
      out += std::format("  [{}] type={} region={} relpos=({:.1f},{:.1f},{:.1f})\n", i, buf[i].type, buf[i].region, buf[i].x, buf[i].y, buf[i].z);
  }
  if (out.empty()) return std::format("{}: no plausible 0xA0-stride vector\n", who);
  return out;
}
static bool hexdump_nugget(void* nug, char* buf16[10], std::string& out) {
  (void)buf16;
  unsigned char b[0xA0];
  bool ok = read_mem(nug, b, sizeof b);
  if (!ok) return false;
  for (int r = 0; r < 0xA0; r += 16) {
    out += std::format("  +{:#04x}:", r);
    for (int c = 0; c < 16; ++c) out += std::format(" {:02x}", b[r + c]);
    // also interpret this row as ints/floats
    out += "   |";
    for (int c = 0; c < 16; c += 4) { int iv; memcpy(&iv, b + r + c, 4); float fv; memcpy(&fv, b + r + c, 4); out += std::format(" {}={}/{:.1f}", r + c, iv, fv); }
    out += "\n";
  }
  return true;
}
// The aerial map's live nugget vector: MiniMap+0xdb0 = {begin, end} of MinimapGameNugget (0xA0 stride),
// filled by the map-update method (exe+0x174e80 via GameEngine::GetDetailMapData) while the map is open.
bool aerial_nugget_span(void*& begin, size_t& count) {
  begin = nullptr;
  count = 0;
  void* ui = ingame_ui();
  if (!ui || !g_available) return false;
  void* mm = (char*)ui + ingame::kMiniMap;
  void* b = rdp(mm, 0xdb0);
  void* e = rdp(mm, 0xdb8);
  if (!b || !e || (uintptr_t)e <= (uintptr_t)b) return false;
  size_t bytes = (char*)e - (char*)b;
  if (bytes % 0xA0 != 0 || bytes > 0xA0 * 8192) return false;
  begin = b;
  count = bytes / 0xA0;
  return true;
}
std::string map_nuggets_dump(int maxn) {
  void* ui = ingame_ui();
  if (!ui) return "no InGameUI\n";
  void* mm = (char*)ui + ingame::kMiniMap;
  // The live cache is at MiniMap+0xdb0 (found by scan). If maxn is negative, hexdump nugget[-maxn-1].
  void* begin = rdp(mm, 0xdb0);
  void* end = rdp(mm, 0xdb8);
  if (maxn < 0 && begin && end) {
    int idx = -maxn - 1;
    size_t n = ((char*)end - (char*)begin) / 0xA0;
    if (idx < 0 || (size_t)idx >= n) return std::format("index out of range (n={})\n", n);
    std::string out = std::format("nugget[{}] hexdump:\n", idx);
    char* dummy[10] = {};
    hexdump_nugget((char*)begin + (size_t)idx * 0xA0, dummy, out);
    return out;
  }
  std::string out;
  out += scan_for_nuggets("MiniMap(inline)", mm, maxn);
  return out;
}
void WindowB::show(bool on) const { if (p) call_void_bool(p, off::kVt_Show, on); }
WindowB ingame_window(unsigned o) { void* ui = ingame_ui(); return ui ? WindowB{(char*)ui + o} : WindowB{}; }

// ---- loot filter window (static RE 2026-08-29, docs/re_lootfilter_exe.md; map + bytes verified live) ----
namespace {
constexpr size_t kLF_Boxes = 0xd58;                 // std::map<CheckBox*, int option>: {head*, size}; MSVC node = {left, parent, right, color, isnil, key +0x20, value +0x28}
constexpr size_t kWorldScreen_ActorCapture = 0x110; // InGameUIActorCapture -- exe+0x20fd8
constexpr size_t kCapture_ShowAllItems = 0x129;     // Alt held (key action 0x23) -- exe+0x28ac00; read by exe+0x20ff8
bool write_byte(void* p, unsigned char v) {
  __try { *(unsigned char*)p = v; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void walk_boxes(void* node, void* head, std::vector<LootFilterBox>& out, int depth) {
  if (!node || node == head || depth > 64 || out.size() > 64) return;
  unsigned char nil = 1;
  if (!rd(node, 0x19, nil) || nil) return;
  walk_boxes(rdp(node, 0x00), head, out, depth + 1);
  LootFilterBox b;
  b.ctrl = rdp(node, 0x20);
  int opt = -1; unsigned char checked = 0;
  if (rd(node, 0x28, opt) && b.ctrl && rd(b.ctrl, off::kB_Pressed, checked)) { b.option = opt; b.checked = checked != 0; out.push_back(b); }
  walk_boxes(rdp(node, 0x10), head, out, depth + 1);
}
}  // namespace
std::vector<LootFilterBox> loot_filter_boxes() {
  std::vector<LootFilterBox> out;
  WindowB w = ingame_window(ingame::kLootFilter);
  if (!w) return out;
  void* head = rdp(w.p, kLF_Boxes);
  size_t size = rd_or<size_t>(w.p, kLF_Boxes + 8, 0);
  if (!head || size == 0 || size > 64) return out;
  walk_boxes(rdp(head, 0x08), head, out, 0);   // head->parent = root
  return out;
}
bool loot_filter_mirror(int option, bool on) {
  for (const LootFilterBox& b : loot_filter_boxes())
    if (b.option == option) return write_byte((char*)b.ctrl + off::kB_Pressed, on ? 1 : 0);
  return false;
}
bool set_show_all_items(bool on) {
  void* cap = rdp(rdp(main_obj(), off::kMainObj_WorldScreen), kWorldScreen_ActorCapture);
  return cap && write_byte((char*)cap + kCapture_ShowAllItems, on ? 1 : 0);
}

// ---- crafting window (static RE 2026-08-29, docs/re_crafting_exe.md; ids, category, rows, flags verified live) ----
namespace {
constexpr size_t kCW_Category = 0x19a8, kCW_TabRegistry = 0x4a0, kCW_NameText = 0x200 /* text element +0x1c0, u16 at +0x40 */, kCW_Panel = 0x1e40;
constexpr size_t kCW_Tabs[kCraftingTabs] = {0x4e0, 0x818, 0xb50, 0xe88, 0x11c0};   // screen order; categories 6, 2, 3, 1, 4
constexpr int kCW_Categories[kCraftingTabs] = {6, 2, 3, 1, 4};
constexpr const char* kCW_TabTags[kCraftingTabs] = {"tagCraftTabArtifactA", "tagCraftTabMeleeA", "tagCraftTabRangedA", "tagCraftTabArmorA", "tagCraftTabMiscA"};
constexpr size_t kCP_Selected = 0x60, kCP_Registry = 0x1e60, kCP_Combine = 0x1ea8, kCP_CombineDisabled = 0x2129, kCP_ListBox = 0x2fa0, kCP_RowsBegin = 0x3070, kCP_RowsEnd = 0x3078;
constexpr size_t kRow_Stride = 0xd0, kRow_Text = 0x00, kRow_Selected = 0x58, kRow_New = 0x5e, kRow_Formula = 0x64;
constexpr uintptr_t kListBox_SelectByData = 0x1f9f00;   // (listbox, int data) -> bool; signature-checked below
typedef bool (*SelectByDataFn)(void*, int);
char* crafting_window() { WindowB w = ingame_window(ingame::kCrafting); return w && w.visible() ? (char*)w.p : nullptr; }
char* crafting_panel() { char* w = crafting_window(); return w ? w + kCW_Panel : nullptr; }
bool crafting_sig_ok() {
  static int ok = -1;
  if (ok < 0) {
    static const unsigned char sig[16] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x7c, 0x24, 0x10, 0x44, 0x8b, 0xd2, 0x4c, 0x8b, 0xc9};
    unsigned char buf[16] = {};
    ok = (kListBox_SelectByData + 16 <= g_image_size && read_mem((void*)(g_base + kListBox_SelectByData), buf, 16) && memcmp(buf, sig, 16) == 0) ? 1 : 0;
    if (!ok) log::write("exe_ui: ListBox::SelectByData signature MISMATCH -- crafting selection disabled");
  }
  return ok == 1;
}
}  // namespace
std::vector<CraftingRow> crafting_rows() {
  std::vector<CraftingRow> out;
  char* p = crafting_panel();
  if (!p) return out;
  char* b = (char*)rdp(p, kCP_RowsBegin); char* e = (char*)rdp(p, kCP_RowsEnd);
  if (!b || !e || e < b || (size_t)(e - b) / kRow_Stride > 2000) return out;
  for (char* r = b; r + kRow_Stride <= e; r += kRow_Stride) {
    CraftingRow row;
    row.text = read_u16(r, kRow_Text);
    row.formula = rd_or<unsigned>(r, kRow_Formula, 0);
    row.selected = rd_or<uint8_t>(r, kRow_Selected, 0) != 0;
    row.is_new = rd_or<uint8_t>(r, kRow_New, 0) != 0;
    out.push_back(std::move(row));
  }
  return out;
}
int crafting_tab() {
  char* w = crafting_window();
  if (!w) return -1;
  int cat = rd_or<int>(w, kCW_Category, -1);
  for (int i = 0; i < kCraftingTabs; ++i) if (kCW_Categories[i] == cat) return i;
  return -1;
}
bool crafting_press_tab(int index) {
  char* w = crafting_window();
  if (!w || index < 0 || index >= kCraftingTabs) return false;
  return WidgetB{w + kCW_Tabs[index]}.press(w + kCW_TabRegistry);
}
const char* crafting_tab_tag(int index) { return index >= 0 && index < kCraftingTabs ? kCW_TabTags[index] : ""; }
std::string crafting_npc_name() { char* w = crafting_window(); return w ? read_u16(w, kCW_NameText) : std::string(); }
unsigned crafting_npc_id() { char* w = crafting_window(); return w ? rd_or<unsigned>(w, 0x9c, 0) : 0; }
unsigned crafting_selected() { char* p = crafting_panel(); return p ? rd_or<unsigned>(p, kCP_Selected, 0) : 0; }
bool crafting_select(unsigned formula_id) {
  char* p = crafting_panel();
  if (!p || !crafting_sig_ok()) return false;
  // Clear the old selection byte ourselves (the list box only sets the new one), then the game's SelectByData.
  char* b = (char*)rdp(p, kCP_RowsBegin); char* e = (char*)rdp(p, kCP_RowsEnd);
  if (b && e && e > b && (size_t)(e - b) / kRow_Stride <= 2000)
    for (char* r = b; r + kRow_Stride <= e; r += kRow_Stride) if (rd_or<uint8_t>(r, kRow_Selected, 0)) write_byte(r + kRow_Selected, 0);
  bool ok = false;
  __try { ok = ((SelectByDataFn)(g_base + kListBox_SelectByData))(p + kCP_ListBox, (int)formula_id); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
  return ok;
}
bool crafting_combine_enabled() { char* p = crafting_panel(); return p && rd_or<uint8_t>(p, kCP_CombineDisabled, 1) == 0; }
bool crafting_craft(unsigned formula_id) {
  char* p = crafting_panel();
  if (!p || !crafting_select(formula_id)) return false;
  unsigned id = formula_id;
  __try { memcpy(p + kCP_Selected, &id, sizeof id); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
  if (!write_byte(p + kCP_CombineDisabled, 0)) return false;
  return WidgetB{p + kCP_Combine}.press(p + kCP_Registry, false);   // no button click: the game's craft sound follows anyway
}
bool crafting_combine() {
  char* p = crafting_panel();
  if (!p || !crafting_combine_enabled()) return false;
  return WidgetB{p + kCP_Combine}.press(p + kCP_Registry, false);
}
std::string crafting_dump() {
  char* w = crafting_window();
  if (!w) return "crafting window not open\n";
  std::string out = std::format("crafting window {} npc='{}' tab={} category={} selected={} combine_enabled={} sig_ok={}\n", (void*)w, crafting_npc_name(), crafting_tab(),
                                rd_or<int>(w, kCW_Category, -1), crafting_selected(), crafting_combine_enabled(), crafting_sig_ok());
  for (const CraftingRow& r : crafting_rows()) out += std::format("  {} formula={} sel={} new={} '{}'\n", r.header() ? "HEAD" : "row ", r.formula, r.selected, r.is_new, r.text);
  return out;
}

// ---- the Inventor's window (static RE 2026-08-30, docs/re_inventor_exe.md; ids, tab, flags and texts verified live) ----
namespace {
// Window (ctor exe+0x26bd00, vtable exe+0x31d050): the frame's loader exe+0x26cd80 binds the record fields in this order.
constexpr size_t kEW_NameText = 0x2e0 /*enchanterNameText, the NPC*/, kEW_HeadingText = 0x3d8 /*enchanterHeadingText "Inventor"*/, kEW_NpcId = 0x9c;
constexpr size_t kEW_Tab = 0x8bc0, kEW_TabRegistry = 0x8bc8;
constexpr size_t kEW_TabButtons[kInventorTabs] = {0x8c08, 0x8f40, 0x9278, 0x95b0};   // Salvage, Dismantle, Convert, Reroll (the listener exe+0x26cad0 maps them to tab 0..3)
constexpr size_t kEW_Panels[kInventorTabs] = {0xa38, 0x1af8, 0x2d98, 0x5970};
constexpr const char* kEW_TabTags[kInventorTabs] = {"tagDividerTab01", "tagDividerTab02", "tagConvertTab", "tagRerollTab"};
constexpr const char* kEW_TabInfoTags[kInventorTabs] = {"tagDividerRoll01", "tagDividerRoll02B", "tagConvertInfoB", "tagRerollInfoB"};
// Salvage ("recover") panel, ctor exe+0x1b1a40, vtable exe+0x316b90, size 0x10c0: loader exe+0x1b2700.
constexpr size_t kSP_Visible = 0x38, kSP_Box = 0x40, kSP_Registry = 0x268, kSP_KeepAddon = 0x2b0 /*recoverRelicButton*/, kSP_KeepItem = 0x660 /*recoverItemButton*/,
                 kSP_RemoveAugment = 0xa10, kSP_CostNumber = 0xdc0, kSP_DialogPending = 0x10a8, kSP_TooExpensive = 0x10a9;
// Dismantle panel, ctor exe+0x1a97b0, vtable exe+0x316950, size 0x12a0.
constexpr size_t kDP_Visible = 0x40, kDP_Box = 0x108, kDP_Result1 = 0x330, kDP_Result2 = 0x558, kDP_Registry = 0x780, kDP_Button = 0x7c8, kDP_CostNumber = 0xb78,
                 kDP_DynamiteNumber = 0xd68, kDP_DialogPending = 0x1148, kDP_Cannot = 0x1149, kDP_NoMoney = 0x114a, kDP_NoDynamite = 0x114b;
// The item box (ctor exe+0x1ad7b0, size 0x228): item id +0xa0, "holds an item detached from the inventory" +0x61,
// "item is invalid" +0x220; SetItem(id) = vt+0xa8 (exe+0x1afc30), HasItem = vt+0xa0.
constexpr size_t kBox_ItemId = 0xa0, kBox_Detached = 0x61, kBox_Invalid = 0x220, kBox_SetItemSlot = 0xa8;
constexpr size_t kBtn_Disabled = 0x281;
typedef void (*BoxSetItemFn)(void*, unsigned);
char* inventor_window() { WindowB w = ingame_window(ingame::kEnchanter); return w && w.visible() ? (char*)w.p : nullptr; }
char* inventor_panel(int tab) { char* w = inventor_window(); return w && tab >= 0 && tab < kInventorTabs ? w + kEW_Panels[tab] : nullptr; }
bool is_enchanter_box(const void* box) {
  uintptr_t v = vtable_rva_of(box);
  return v == rva::kEnchanterBoxSalvage || v == rva::kEnchanterBoxDismantle || v == rva::kEnchanterBoxResult;
}
unsigned box_item(const void* box) { return box && is_enchanter_box(box) ? rd_or<unsigned>(box, kBox_ItemId, 0) : 0; }
bool box_set_item(void* box, unsigned id) {   // POD only: SEH cannot unwind C++ objects
  if (!box || !is_enchanter_box(box)) return false;
  void** vt = (void**)rdp(box, 0);
  void* f = vt ? rdp(vt, kBox_SetItemSlot) : nullptr;
  if (!f) return false;
  __try { ((BoxSetItemFn)f)(box, id); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
InventorButton read_button(char* p, const char* warning_tag) {
  InventorButton b;
  b.p = p;
  WidgetB w{p};
  b.caption = w.text();
  b.enabled = w.enabled();
  b.warning_tag = warning_tag;
  return b;
}
}  // namespace
bool inventor_open() { return inventor_window() != nullptr; }
std::string inventor_npc_name() { char* w = inventor_window(); return w ? WidgetB{w + kEW_NameText}.text() : std::string(); }
unsigned inventor_npc_id() { char* w = inventor_window(); return w ? rd_or<unsigned>(w, kEW_NpcId, 0) : 0; }
int inventor_tab() { char* w = inventor_window(); return w ? rd_or<int>(w, kEW_Tab, -1) : -1; }
std::vector<InventorTab> inventor_tabs() {
  std::vector<InventorTab> out;
  char* w = inventor_window();
  if (!w) return out;
  for (int i = 0; i < kInventorTabs; ++i) {
    InventorTab t;
    WidgetB b{w + kEW_TabButtons[i]};
    // All four buttons are constructed (ctor exe+0x10aee0, a bitmap button class of its own); the two the master
    // table always has are present, the expansions' Convert / Reroll count as present only once the game enables
    // them (Show(true) greys them unless MainPlayerCanUseConvert/Reroll, which need the expansion loaded).
    t.enabled = b.enabled();
    t.present = i < 2 || t.enabled;
    t.label = kEW_TabTags[i];
    t.info_tag = kEW_TabInfoTags[i];
    out.push_back(t);
  }
  return out;
}
bool inventor_press_tab(int index) {
  char* w = inventor_window();
  if (!w || index < 0 || index >= kInventorTabs) return false;
  if (inventor_tab() == index) return true;
  return WidgetB{w + kEW_TabButtons[index]}.press(w + kEW_TabRegistry);
}
InventorSalvage inventor_salvage() {
  InventorSalvage s;
  char* p = inventor_panel(0);
  if (!p) return s;
  s.item = box_item(p + kSP_Box);
  s.cost = s.item ? WidgetB{p + kSP_CostNumber}.text() : std::string();
  s.too_expensive = rd_or<uint8_t>(p, kSP_TooExpensive, 0) != 0;
  s.dialog_pending = rd_or<uint8_t>(p, kSP_DialogPending, 0) != 0;
  s.keep_item = read_button(p + kSP_KeepItem, "tagDividingKeepItemWarning");
  s.keep_addon = read_button(p + kSP_KeepAddon, "tagDividingKeepComponentWarning");
  s.remove_augment = read_button(p + kSP_RemoveAugment, "tagDividingRemoveAugmentWarning");
  return s;
}
InventorDismantle inventor_dismantle() {
  InventorDismantle d;
  char* p = inventor_panel(1);
  if (!p) return d;
  d.item = box_item(p + kDP_Box);
  d.result1 = box_item(p + kDP_Result1);
  d.result2 = box_item(p + kDP_Result2);
  d.cost = d.item ? WidgetB{p + kDP_CostNumber}.text() : std::string();
  d.dynamite = WidgetB{p + kDP_DynamiteNumber}.text();
  d.no_money = rd_or<uint8_t>(p, kDP_NoMoney, 0) != 0;
  d.no_dynamite = rd_or<uint8_t>(p, kDP_NoDynamite, 0) != 0;
  d.dialog_pending = rd_or<uint8_t>(p, kDP_DialogPending, 0) != 0;
  d.dismantle = read_button(p + kDP_Button, "tagDismantleButton");
  return d;
}
// The exe's drop handlers (salvage exe+0x1afc70, dismantle exe+0x1b0190) after their own accept test: box
// SetItem(id), PlayerInventoryCtrl::RemoveItem(id, true), box+0x220 = 0, box+0x61 = 1, then the cursor is cleared.
// The accept test is the caller's (gameapi::inventor_accepts); here only "one item per chamber".
bool inventor_put(unsigned item_id) {
  int tab = inventor_tab();
  char* p = inventor_panel(tab);
  if (!p || !item_id || (tab != 0 && tab != 1)) return false;
  char* box = p + (tab == 0 ? kSP_Box : kDP_Box);
  if (box_item(box)) { log::writef("exe_ui: inventor chamber already holds {}", box_item(box)); return false; }
  if (!box_set_item(box, item_id)) return false;
  if (!gameapi::inventory_detach(item_id)) { box_set_item(box, 0); return false; }
  write_byte(box + kBox_Invalid, 0);
  write_byte(box + kBox_Detached, 1);
  log::writef("exe_ui: inventor chamber <- {} (tab {})", item_id, tab);
  return true;
}
// The exe's return helper exe+0x1ae9b0 (a Shift-click on a box, and every panel's Hide): SetItem(0), +0x61 = 0,
// ControllerPlayer::GiveItemToPlayer(id, false) -- SendDropItemRandom if that fails (a full bag), which we leave
// to the game's own Hide.
bool inventor_take(int which) {
  int tab = inventor_tab();
  char* p = inventor_panel(tab);
  if (!p || (tab != 0 && tab != 1)) return false;
  char* box = nullptr;
  if (which == 0) box = p + (tab == 0 ? kSP_Box : kDP_Box);
  else if (tab == 1 && which == 1) box = p + kDP_Result1;
  else if (tab == 1 && which == 2) box = p + kDP_Result2;
  unsigned id = box_item(box);
  if (!id) return false;
  if (!box_set_item(box, 0)) return false;
  write_byte(box + kBox_Detached, 0);
  bool ok = gameapi::give_item_to_player(id);
  log::writef("exe_ui: inventor chamber {} -> bag ok={}", id, ok);
  return ok;
}
bool inventor_press(InventorAction a) {
  int tab = inventor_tab();
  char* p = inventor_panel(tab);
  if (!p) return false;
  if (a == InventorAction::Dismantle) return tab == 1 && WidgetB{p + kDP_Button}.press(p + kDP_Registry);
  if (tab != 0) return false;
  size_t off = a == InventorAction::KeepItem ? kSP_KeepItem : a == InventorAction::KeepAddon ? kSP_KeepAddon : kSP_RemoveAugment;
  return WidgetB{p + off}.press(p + kSP_Registry);
}
std::string inventor_dump() {
  char* w = inventor_window();
  if (!w) return "inventor window not open\n";
  std::string out = std::format("inventor window {} npc='{}' id={} heading='{}' tab={}\n", (void*)w, inventor_npc_name(), inventor_npc_id(), WidgetB{w + kEW_HeadingText}.text(), inventor_tab());
  std::vector<InventorTab> tabs = inventor_tabs();
  for (int i = 0; i < (int)tabs.size(); ++i) out += std::format("  tab {} {} present={} enabled={} {}\n", i, tabs[(size_t)i].label, tabs[(size_t)i].present, tabs[(size_t)i].enabled, WidgetB{w + kEW_TabButtons[i]}.state_bytes());
  InventorSalvage s = inventor_salvage();
  out += std::format("  salvage: item={} cost='{}' too_expensive={} dialog_pending={} visible={}\n", s.item, s.cost, s.too_expensive, s.dialog_pending, rd_or<uint8_t>(w + kEW_Panels[0], kSP_Visible, 0));
  for (const InventorButton* b : {&s.keep_item, &s.keep_addon, &s.remove_augment}) out += std::format("    button {} '{}' enabled={} {}\n", b->p, b->caption, b->enabled, WidgetB{b->p}.state_bytes());
  InventorDismantle d = inventor_dismantle();
  out += std::format("  dismantle: item={} results={},{} cost='{}' dynamite='{}' no_money={} no_dynamite={} cannot={} dialog_pending={} visible={}\n", d.item, d.result1, d.result2, d.cost, d.dynamite,
                     d.no_money, d.no_dynamite, rd_or<uint8_t>(w + kEW_Panels[1], kDP_Cannot, 0), d.dialog_pending, rd_or<uint8_t>(w + kEW_Panels[1], kDP_Visible, 0));
  out += std::format("    button {} '{}' enabled={} {}\n", d.dismantle.p, d.dismantle.caption, d.dismantle.enabled, WidgetB{d.dismantle.p}.state_bytes());
  return out;
}
uintptr_t WidgetB::vtable_rva() const { return vtable_rva_of(p); }
bool WidgetB::is_button() const { return vtable_rva() == rva::kButtonB; }
bool WidgetB::is_text_button() const { return vtable_rva() == rva::kTextButtonB; }
bool WidgetB::is_text() const { return vtable_rva() == rva::kTextB; }
std::string WidgetB::text() const {
  if (is_text()) return read_u16(p, off::kB_Text);
  if (is_text_button()) return read_u16(p, off::kTB_Caption);
  if (vtable_rva() == rva::kTitleTextB) return read_u16(p, 0x40);   // measured live on the desecrated shrine window 2026-08-28
  if (vtable_rva() == rva::kNumberTextB) return read_u16(p, 0x40);
  if (vtable_rva() == rva::kTextBlockB) return read_u16(p, 0x38);
  return {};
}
bool WidgetB::visible() const { return rd_or<uint8_t>(p, off::kB_Visible, 0) != 0; }
bool WidgetB::enabled() const { return rd_or<uint8_t>(p, off::kB_Disabled, 1) == 0; }
bool WidgetB::pressed() const { return rd_or<uint8_t>(p, off::kB_Pressed, 0) != 0; }
std::string WidgetB::state_bytes() const {
  return std::format("visible={} disabled={} pressed={} rollover={} +0x284={}", rd_or<uint8_t>(p, 0x28, 0xff), rd_or<uint8_t>(p, 0x281, 0xff), rd_or<uint8_t>(p, 0x282, 0xff),
                     rd_or<uint8_t>(p, 0x283, 0xff), rd_or<uint8_t>(p, 0x284, 0xff));
}
bool WidgetB::press(void* registry, bool sound) const {
  if (!registry || !p) return false;
  if (!enabled()) { log::writef("exe_ui: press {} refused: disabled", p); return false; }
  bool ok = call_host_click(registry, p, sound);
  log::writef("exe_ui: pressed {} '{}' via registry {} ok={}", p, text(), registry, ok);
  return ok;
}
ExitWindow exit_window() { return {ingame_window(ingame::kExit).p}; }
// InGameUI::HandleKeyAction(this, action, bool, bool, bool) -- exe+0x211980, what the game's key bindings call
// (docs/ingame-ui-survey.md has the action ids: 1 character, 2 skills, 3 codex, 0x36 interact, 0x37 pickup ...).
typedef bool (*KeyActionFn)(void*, int, bool, bool, bool);
constexpr uintptr_t kInGameUI_HandleKeyAction = 0x211980;
// The skills window (InGameUI+0x3fc20; 2026-08-22 readout): SetPane = exe+0x27c580(window, tab, paneIndex) puts a
// mastery's skill tree (paneIndex = mastery enumeration 0..) or the class-selection pane (0x50) on a tab; the
// current tab index sits at +0x2630. Choosing a class this way is exactly the pane's own click path; it
// becomes permanent when the mastery skill takes its first point.
typedef void (*SetPaneFn)(void*, int, int);
constexpr uintptr_t kSkillsWindow_SetPane = 0x27c580;
constexpr size_t kSkillsWindow_Tab = 0x2630;
constexpr size_t kSkillsWindow_Reclaim = 0x1f4c;   // nonzero when a spirit guide opened the window in reclaim mode.
                                                   // The click handler exe+0x248380 branches on [controller+0x1e1c]
                                                   // where its `this` is the embedded controller at window+0x130, so
                                                   // the flag is window+0x1f4c (verified live: only DisplaySkill-
                                                   // ReallocationWindow flips window+0x1f49/+0x1f4c 0->1).
bool skills_set_pane(int tab, int pane_index) {
  void* ui = ingame_ui();
  if (!ui || !g_available) return false;
  void* w = (char*)ui + ingame::kSkills;
  SetPaneFn f = (SetPaneFn)(g_base + kSkillsWindow_SetPane);
  __try { f(w, tab, pane_index); } __except (EXCEPTION_EXECUTE_HANDLER) { log::writef("exe_ui: SetPane({}, {}) faulted", tab, pane_index); return false; }
  log::writef("exe_ui: SkillsWindow::SetPane(tab {}, pane {})", tab, pane_index);
  return true;
}
// The market id IS the merchant's object id (NpcMerchant::OnPlayerInteract sends event 0x1a with GetObjectId(this);
// verified live 2026-08-23: market_stock(Kerrick's id) = 4 stocked tabs). The window stores it inline at +0x245c
// (and again at +0xa4); the earlier read through +0x2410 as a pointer hit a static exe object and gave 340.
unsigned vendor_market_id(const WindowB& w) {
  if (!w) return 0;
  unsigned id = rd_or<unsigned>(w.p, 0x245c, 0);
  return id ? id : rd_or<unsigned>(w.p, 0xa4, 0);
}
namespace {
constexpr size_t kVendor_TabMap = 0x26f8, kVendor_SelectedType = 0x25e0, kVendor_ButtonDisabled = 0x281;
constexpr size_t kVendor_TabButtons[5] = {0x550, 0x888, 0xbc0, 0xef8, 0x1230};
// In-order walk of an MSVC red-black map (head->left = leftmost; node: left +0, parent +8, right +0x10, isnil +0x19).
std::vector<void*> vendor_map_nodes(const void* map, size_t max_nodes) {
  std::vector<void*> out;
  void* head = rdp(map, 0);
  if (!head) return out;
  void* n = rdp(head, 0);
  size_t guard = 0;
  while (n && n != head && rd_or<uint8_t>(n, 0x19, 1) == 0 && guard++ < max_nodes) {
    out.push_back(n);
    void* r = rdp(n, 0x10);
    if (r && rd_or<uint8_t>(r, 0x19, 1) == 0) {
      n = r;
      void* l;
      while ((l = rdp(n, 0)) && rd_or<uint8_t>(l, 0x19, 1) == 0) n = l;
    } else {
      void* p = rdp(n, 8);
      while (p && rd_or<uint8_t>(p, 0x19, 1) == 0 && rdp(p, 0x10) == n) { n = p; p = rdp(n, 8); }
      n = p;
    }
  }
  return out;
}
}  // namespace
std::vector<VendorTab> vendor_tabs(const WindowB& w) {
  std::vector<VendorTab> out;
  if (!w) return out;
  for (void* node : vendor_map_nodes((char*)w.p + kVendor_TabMap, 16)) {
    VendorTab t;
    t.type = rd_or<int>(node, 0x20, 0);
    t.button = rdp(node, 0x28);
    if (!t.button) continue;
    for (int i = 0; i < 5; ++i) if ((char*)t.button == (char*)w.p + kVendor_TabButtons[i]) t.index = i;
    t.disabled = rd_or<uint8_t>(t.button, kVendor_ButtonDisabled, 0) != 0;
    out.push_back(t);
  }
  std::sort(out.begin(), out.end(), [](const VendorTab& a, const VendorTab& b) { return a.index < b.index; });
  return out;
}
int vendor_selected_type(const WindowB& w) { return w ? rd_or<int>(w.p, kVendor_SelectedType, -1) : -1; }
namespace {
constexpr size_t kCaravan_PrivatePanel = 0x13c8, kCaravan_TransferPanel = 0x13d0, kPanel_Costs = 0x378, kPanel_TabList = 0x98;
constexpr uintptr_t kCaravan_RebuildTabs = 0x25d890, kCaravan_SackDims = 0x12ec70;
}  // namespace
namespace {
constexpr size_t kIngameUI_HudToolbar = 0xb748, kIngameUI_HudStatus = 0xb758, kIngameUI_HudTopLeft = 0xb768;
bool plausible_rect(const Rect& r, float w, float h) { return r.w > 0 && r.h > 0 && r.x >= 0 && r.y >= 0 && r.x + r.w <= w + 1 && r.y + r.h <= h + 1; }
}  // namespace
std::vector<Rect> hud_rects() {
  std::vector<Rect> out;
  void* ui = ingame_ui();
  if (!ui || !available()) return out;
  RECT rc{};
  HWND hw = FindWindowA("Grim Dawn", nullptr);
  if (!hw || !GetClientRect(hw, &rc)) return out;
  float w = (float)rc.right, h = (float)rc.bottom;
  Rect toolbar = rd_or<Rect>(ui, kIngameUI_HudToolbar, Rect{}), status = rd_or<Rect>(ui, kIngameUI_HudStatus, Rect{});
  float tlx = rd_or<float>(ui, kIngameUI_HudTopLeft, -1.f), tly = rd_or<float>(ui, kIngameUI_HudTopLeft + 4, -1.f);
  if (plausible_rect(toolbar, w, h) && toolbar.y > h / 2) out.push_back(toolbar);
  if (plausible_rect(status, w, h) && status.y > h / 2) out.push_back(status);
  // The whole block from the HUD's top-left down to the toolbar's bottom edge (the compass strip above the status bars).
  if (!out.empty() && tlx >= 0 && tly > h / 2 && tly < toolbar.y) {
    Rect block{tlx, tly, toolbar.x + toolbar.w - tlx, toolbar.y + toolbar.h - tly};
    if (plausible_rect(block, w, h)) out.push_back(block);
  }
  return out;
}
bool point_over_hud(float x, float y) {
  for (const Rect& r : hud_rects()) if (x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h) return true;
  return false;
}
CaravanPanel caravan_panel(bool shared) {
  CaravanPanel out;
  WindowB w = ingame_window(ingame::kCaravan);
  if (!w || !available()) return out;
  void* panel = rdp(w.p, shared ? kCaravan_TransferPanel : kCaravan_PrivatePanel);
  if (!panel || !in_exe(rdp(panel, 0))) return out;   // a framework-B object: its vtable is in the exe
  const int* b = (const int*)rdp(panel, kPanel_Costs); const int* e = (const int*)rdp(panel, kPanel_Costs + 8);
  if (!b || !e || e < b || (size_t)(e - b) > 16) return out;
  for (const int* it = b; it < e; ++it) out.costs.push_back(rd_or<int>(it, 0, 0));
  // Self-check against the record shape (the first tab is free, prices rise): a wrong pointer will not look like this.
  if (out.costs.size() < 2 || out.costs[0] != 0 || out.costs[1] <= 0) { out.costs.clear(); return out; }
  out.panel = panel;
  return out;
}
namespace {
bool caravan_refresh_guarded(void* list, const void* sack_vector) {   // no C++ objects here: SEH and unwinding do not mix
  typedef void (*RebuildFn)(void*, const void*);
  typedef void (*DimsFn)(void*);
  __try {
    ((RebuildFn)(g_base + kCaravan_RebuildTabs))(list, sack_vector);
    ((DimsFn)(g_base + kCaravan_SackDims))(list);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
  return true;
}
}  // namespace
bool caravan_refresh(bool shared, const void* sack_vector) {
  CaravanPanel p = caravan_panel(shared);
  if (!p.panel || !sack_vector) return false;
  bool ok = caravan_refresh_guarded((char*)p.panel + kPanel_TabList, sack_vector);
  if (!ok) log::write("exe_ui: caravan refresh faulted");
  return ok;
}
int quickbar_page() { void* ui = ingame_ui(); int p = ui ? rd_or<int>(ui, 0x72f0, -1) : -1; return p >= 0 && p < 4 ? p : (ui ? 0 : -1); }
// Toggle UI's byte (InGameUI+0xac990: the ctor exe+0x206dca sets 1, key action 0x3c at exe+0x211f5e flips it, the
// render pass exe+0x20a860 skips every window but the pause menu / options host / prompt box while it is 0).
int ui_visible() { void* ui = ingame_ui(); if (!ui) return -1; unsigned char b = rd_or<unsigned char>(ui, 0xac990, 1); return b ? 1 : 0; }
int skills_tab() { void* ui = ingame_ui(); return ui ? rd_or<int>((char*)ui + ingame::kSkills, kSkillsWindow_Tab, -1) : -1; }
// The skills window's mastery panes (UISkillPane, ctor exe+0x243510, 0x1ea8 bytes, vtable exe+0x31bd18; RE
// 2026-08-27): heap objects at window+0x100 (tab 0) / +0x108 (tab 1), replaced by the 0x3d0-byte class-selection
// pane (vtable exe+0x31a1d8) while no class is chosen -- so the vtable is checked before any offset is used.
// +0x80 the pane's own listener registry; +0x820 "Undo Class Selection" and +0xbd0 "Undo Points" (TextButtons;
// Undo Points is enabled by Update exe+0x247a71 exactly while +0x1e45 "pending changes" is set, and its press
// runs UISkillPane::UndoPoints exe+0x2494d0: every pending delta reverted + Character::AddToSkillPoints);
// +0x1e4c reclaim mode (the spirit guide's flag, per pane).
namespace {
constexpr uintptr_t kSkillPaneVt = 0x31bd18;
constexpr size_t kSkillsWindow_Pane0 = 0x100, kSkillsWindow_Pane1 = 0x108;
constexpr size_t kPane_Registry = 0x80, kPane_UndoPoints = 0xbd0, kPane_ReclaimMode = 0x1e4c;
// The WINDOW's own reclaim byte (RE 2026-09-20, exe+0x21a6f0 = the target of GameEngine::DisplaySkillReallocationWindow):
// with any mastery active it writes window+0x2639 = 1, the npc id to +0x2634, calls slot vt+0xa8(1) on BOTH pane
// slots (UISkillPane: `mov [this+0x1e4c], dl`; the class-selection pane: a bare `ret`), then Show(true). The window
// reads +0x2639 itself (exe+0x27a155/+0x27a1d0/+0x27c8d1) and clears it with the panes on teardown (exe+0x2795a2)
// and through its set(bl) path (exe+0x27a0b2). So it is the authoritative "opened by a spirit guide" state.
constexpr size_t kSkillsWindow_ReclaimByte = 0x2639;
char* skills_window() { void* ui = ingame_ui(); return ui && g_available ? (char*)ui + ingame::kSkills : nullptr; }
// The mastery pane in tab slot 0 / 1, or null when that slot holds the class-selection pane (or nothing).
void* skills_pane_at(int tab) {
  char* w = skills_window();
  if (!w || (tab != 0 && tab != 1)) return nullptr;
  void* pane = rdp(w, tab == 1 ? kSkillsWindow_Pane1 : kSkillsWindow_Pane0);
  return pane && vtable_rva_of(pane) == kSkillPaneVt ? pane : nullptr;
}
// The pane of the tab the GAME shows (2 = the Devotion tab: none). Dev dumps only: the screen's own tab can differ
// from the game's (the game rests a one-class character on tab 1 = "select a class"; verified live 2026-09-20).
void* skills_pane() { char* w = skills_window(); return w ? skills_pane_at(rd_or<int>(w, kSkillsWindow_Tab, -1)) : nullptr; }
}  // namespace
// Reclaim mode = the window's own byte, else any mastery pane's flag, else the old Devotion-button proxy (+0x1f4c).
// Before 2026-09-20 only the pane of the game's CURRENT tab was read: a one-class character's window rests on tab 1,
// the class-selection pane, which carries no flag, and the proxy read 0 -- so a spirit guide opened a plain skills
// window for them (the user's report; the log showed the rows without costs and no hint row).
bool skills_reclaim_mode() {
  char* w = skills_window();
  if (!w) return false;
  if (rd_or<uint8_t>(w, kSkillsWindow_ReclaimByte, 0) != 0) return true;
  for (int t = 0; t < 2; ++t) if (void* pane = skills_pane_at(t)) if (rd_or<uint8_t>(pane, kPane_ReclaimMode, 0) != 0) return true;
  return rd_or<uint8_t>(w, kSkillsWindow_Reclaim, 0) != 0;
}
bool ingame_key_action(int action) {
  void* ui = ingame_ui();
  if (!ui || !g_available) return false;
  KeyActionFn f = (KeyActionFn)(g_base + kInGameUI_HandleKeyAction);
  bool r = false;
  __try { r = f(ui, action, true, false, false); } __except (EXCEPTION_EXECUTE_HANDLER) { log::writef("exe_ui: HandleKeyAction({}) faulted", action); return false; }
  log::writef("exe_ui: HandleKeyAction({}) -> {}", action, r);
  return true;
}
bool ExitWindow::visible() const { return WindowB{p}.visible(); }
// The pane's skill icons: vector<SkillEntry> at pane+0x68/+0x70, stride 0x78 -- entry+0x00 the icon control,
// +0x10 the pending undo delta (learn = -1, reclaim = +1), +0x50 the skill object id (RE 2026-08-27). Pressing an
// icon through the pane's registry is the game's own learn / reclaim click (exe+0x248380, event 0): its gates, its
// sound, and the pending delta that makes Undo Points work.
namespace {
constexpr size_t kPane_EntriesBegin = 0x68, kPane_EntriesEnd = 0x70, kEntry_Stride = 0x78, kEntry_Control = 0x00, kEntry_Delta = 0x10, kEntry_SkillId = 0x50;
std::vector<char*> pane_entries(void* pane) {
  std::vector<char*> out;
  char* b = (char*)rdp(pane, kPane_EntriesBegin); char* e = (char*)rdp(pane, kPane_EntriesEnd);
  if (!b || !e || e < b || (size_t)(e - b) / kEntry_Stride > 64) return out;
  for (char* p = b; p + kEntry_Stride <= e; p += kEntry_Stride) out.push_back(p);
  return out;
}
}  // namespace
bool skills_press_skill(unsigned skill_id) {
  // A skill sits on exactly one mastery's pane: search both slots, not the pane of the tab the game happens to show
  // (which can be the class-selection pane while our screen is on the other tab).
  for (int t = 0; t < 2; ++t) {
    void* pane = skills_pane_at(t);
    if (!pane) continue;
    for (char* en : pane_entries(pane)) {
      if (rd_or<unsigned>(en, kEntry_SkillId, 0) != skill_id) continue;
      WidgetB icon{rdp(en, kEntry_Control)};
      if (!icon) return false;
      bool ok = icon.press((char*)pane + kPane_Registry);
      log::writef("exe_ui: skills icon press skill {} on pane {} ok={}", skill_id, t, ok);
      return ok;
    }
  }
  return false;
}
std::string skills_pane_dump() {
  void* pane = skills_pane();
  if (!pane) return "no mastery pane on the current tab\n";
  std::string out = std::format("pane {} dirty={} reclaim={} undo_points enabled={}\n", pane, rd_or<uint8_t>(pane, 0x1e45, 0xff), rd_or<uint8_t>(pane, kPane_ReclaimMode, 0xff), WidgetB{(char*)pane + kPane_UndoPoints}.enabled());
  for (char* en : pane_entries(pane)) {
    WidgetB icon{rdp(en, kEntry_Control)};
    out += std::format("  entry {} control={} vt=exe+{:#x} delta={} skill={} {}\n", (void*)en, icon.p, icon ? icon.vtable_rva() : 0, rd_or<int>(en, kEntry_Delta, 0), rd_or<unsigned>(en, kEntry_SkillId, 0), icon ? icon.state_bytes() : std::string());
  }
  return out;
}
bool skills_undo_points_enabled(int tab) { void* p = skills_pane_at(tab); return p && WidgetB{(char*)p + kPane_UndoPoints}.enabled(); }
bool skills_undo_points(int tab) { void* p = skills_pane_at(tab); return p && WidgetB{(char*)p + kPane_UndoPoints}.press((char*)p + kPane_Registry); }

// ---- the devotion window's constellation graph (docs/re_devotion_exe.md sections 1.2-1.4) ----
// window+0xa8 = std::vector<Constellation*>; Constellation: +0x38 name tag, +0x58 info tag (std::string), +0x78 stars
// (std::vector<Star*>), +0x90 / +0xa8 required / given affinity pairs (mem::vector<{int type, unsigned amount}>);
// Star: +0x108 skill id, +0x10c bound host skill id, +0x118 links (mem::vector<int>, 1-based).
namespace {
constexpr size_t kDev_Constellations = 0xa8, kCon_NameTag = 0x38, kCon_InfoTag = 0x58, kCon_Stars = 0x78, kCon_Required = 0x90, kCon_Given = 0xa8,
                 kStar_SkillId = 0x108, kStar_HostId = 0x10c, kStar_Links = 0x118;
std::string read_a(const void* base, size_t off) {
  MsvcStringA s{};
  if (!rd(base, off, s) || s.size > 512) return {};
  if (s.capacity < 16) return std::string(s.u.buf, s.size);
  std::string out(s.size, '\0');
  return s.u.ptr && read_mem(s.u.ptr, out.data(), s.size) ? out : std::string();
}
template <class T> std::vector<T> read_vec(const void* base, size_t off, size_t max_items) {
  std::vector<T> out;
  void* b = rdp(base, off); void* e = rdp(base, off + 8);
  if (!b || !e || e < b) return out;
  size_t n = ((char*)e - (char*)b) / sizeof(T);
  if (n > max_items) return out;
  out.resize(n);
  if (n && !read_mem(b, out.data(), n * sizeof(T))) out.clear();
  return out;
}
struct AffPair { int type; unsigned amount; };
}  // namespace
std::vector<DevotionConstellationB> devotion_constellations() {
  std::vector<DevotionConstellationB> out;
  void* ui = ingame_ui();
  if (!ui || !g_available) return out;
  void* w = (char*)ui + ingame::kDevotion;
  for (void* c : read_vec<void*>(w, kDev_Constellations, 110)) {
    if (!c) continue;
    DevotionConstellationB con{c};
    con.name_tag = read_a(c, kCon_NameTag);
    con.info_tag = read_a(c, kCon_InfoTag);
    for (const AffPair& a : read_vec<AffPair>(c, kCon_Required, 3)) if (a.type >= 0 && a.type < kAffinityCount) con.required.push_back({a.type, a.amount});
    for (const AffPair& a : read_vec<AffPair>(c, kCon_Given, 3)) if (a.type >= 0 && a.type < kAffinityCount) con.given.push_back({a.type, a.amount});
    unsigned i = 0;
    for (void* st : read_vec<void*>(c, kCon_Stars, 10)) {
      ++i;
      if (!st) continue;
      DevotionStarB s{st, i, rd_or<unsigned>(st, kStar_SkillId, 0), rd_or<unsigned>(st, kStar_HostId, 0), read_vec<int>(st, kStar_Links, 10)};
      con.stars.push_back(std::move(s));
    }
    if (con.stars.empty()) continue;   // constellation87 is background art with no stars
    out.push_back(std::move(con));
  }
  return out;
}
bool devotion_set_star_host(void* star, unsigned host_id) { return star && write_int(star, kStar_HostId, (int)host_id); }

// ---- options value controls ----
namespace {
constexpr size_t kSl_Max = 0x31c, kSl_Min = 0x320, kSl_Value = 0x324, kSl_ListBegin = 0x300, kSl_ListEnd = 0x308;  // exe+0xebb46.., exe+0xc4dd0
constexpr size_t kCb_ItemsBegin = 0xd0, kCb_ItemsEnd = 0xd8, kCb_ItemStride = 0x30, kCb_Selected = 0xec, kCb_ListBegin = 0xb8, kCb_ListEnd = 0xc0;  // exe+0xab951
constexpr size_t kLs_RowsBegin = 0x260, kLs_RowsEnd = 0x268;  // vector<vector<u16string>*> -- exe+0xafe20
constexpr size_t kOpt_TabIndex = 0x280;                       // exe+0xcd300
bool write_float(void* base, size_t off, float v) {
  __try { *(float*)((char*)base + off) = v; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
std::vector<void*> listeners_of(const void* p, size_t begin_off, size_t end_off) {
  std::vector<void*> ls;
  void* b = rdp(p, begin_off); void* e = rdp(p, end_off);
  if (!b || !e || e < b) return ls;
  size_t n = ((char*)e - (char*)b) / sizeof(void*);
  for (size_t i = 0; i < n && i < 64; ++i) { void* l = rdp(b, i * sizeof(void*)); if (l) ls.push_back(l); }
  return ls;
}
}  // namespace
bool WidgetA::is_slider() const { return vtable_rva() == rva::kSliderA; }
float WidgetA::slider_value() const {
  float lo = rd_or<float>(p, kSl_Min, 0), hi = rd_or<float>(p, kSl_Max, 1), v = rd_or<float>(p, kSl_Value, 0);
  return hi > lo ? (v - lo) / (hi - lo) : 0;
}
bool WidgetA::set_slider(float v01) const {
  if (!is_slider()) return false;
  float lo = rd_or<float>(p, kSl_Min, 0), hi = rd_or<float>(p, kSl_Max, 1);
  if (v01 < 0) v01 = 0; if (v01 > 1) v01 = 1;
  if (!write_float(p, kSl_Value, lo + v01 * (hi - lo))) return false;
  bool ok = true;
  for (void* l : listeners_of(p, kSl_ListBegin, kSl_ListEnd)) ok = call_listener(l, p, 0) && ok;  // L->vt[0](L, slider)
  return ok;
}
bool WidgetA::is_combo() const { return vtable_rva() == rva::kComboA; }
std::vector<std::string> WidgetA::combo_items() const {
  std::vector<std::string> out;
  void* b = rdp(p, kCb_ItemsBegin); void* e = rdp(p, kCb_ItemsEnd);
  if (!b || !e || e < b) return out;
  size_t n = ((char*)e - (char*)b) / kCb_ItemStride;
  for (size_t i = 0; i < n && i < 256; ++i) out.push_back(read_u16(b, i * kCb_ItemStride));
  return out;
}
int WidgetA::combo_index() const { return rd_or<int>(p, kCb_Selected, -1); }
bool WidgetA::set_combo(int index) const {
  if (!is_combo() || index < 0) return false;
  if (!write_int(p, kCb_Selected, index)) return false;
  bool ok = true;
  for (void* l : listeners_of(p, kCb_ListBegin, kCb_ListEnd)) ok = call_listener(l, p, 0) && ok;
  return ok;
}
bool WidgetA::is_list() const { return vtable_rva() == rva::kListA; }
std::vector<std::vector<std::string>> WidgetA::list_rows() const {
  std::vector<std::vector<std::string>> out;
  void* b = rdp(p, kLs_RowsBegin); void* e = rdp(p, kLs_RowsEnd);
  if (!b || !e || e < b) return out;
  size_t n = ((char*)e - (char*)b) / sizeof(void*);
  for (size_t i = 0; i < n && i < 256; ++i) {
    void* row = rdp(b, i * sizeof(void*));  // a vector<u16string>
    std::vector<std::string> cells;
    void* cb = rdp(row, 0); void* ce = rdp(row, 8);
    if (cb && ce && ce >= cb) { size_t m = ((char*)ce - (char*)cb) / 32; for (size_t j = 0; j < m && j < 8; ++j) cells.push_back(read_u16(cb, j * 32)); }
    out.push_back(cells);
  }
  return out;
}

OptionsScreen options_screen() {
  OptionsScreen o;
  if (app_state() == 5) {
    for (WidgetA c : root().children()) if (c.vtable_rva() == rva::kOptionsScreenVt && c.active()) { o.screen = c; break; }
  } else if (void* ui = ingame_ui()) {
    // From the pause menu the very same screen class is owned by a small host window instead of the menu tree.
    void* host = rdp(ui, ingame::kOptionsHostPtr);
    if (host && rd_or<uint8_t>(host, ingame::kOptionsHost_Visible, 0)) {
      WidgetA c{rdp(host, ingame::kOptionsHost_Screen)};
      if (c && c.vtable_rva() == rva::kOptionsScreenVt && c.active()) o.screen = c;
    }
  }
  if (!o.screen) return o;
  for (WidgetA c : o.screen.children()) {
    if (c.vtable_rva() == rva::kOptionsPanelVt && !o.panel) o.panel = c;
    else if (c.is_button() && c.is_toggle() && c.caption().empty()) o.tabs.push_back(c);
  }
  if (!o.panel) return o;
  WidgetA page_candidate;
  for (WidgetA c : o.panel.children()) {
    if (c.vtable_rva() == rva::kOptionsPageVt && c.active() && !o.page) o.page = c;
    else if (c.is_button()) o.buttons.push_back(c);
    else if (c.active() && !page_candidate && !c.children().empty()) page_candidate = c;
  }
  if (!o.page) o.page = page_candidate;
  // "Default" is the first plain button in every page. Other pages (notably Network) also contain
  // Help/Test/cloud-save buttons; those already belong to the page stop and must not be repeated in the
  // bottom Apply/Default/Close row.
  if (o.page) for (WidgetA c : o.page.children()) if (c.is_button() && !c.is_toggle() && !c.caption().empty()) { o.buttons.push_back(c); break; }
  return o;
}
int OptionsScreen::tab_index() const { return rd_or<int>(screen.p, kOpt_TabIndex, -1); }

// ---- conversation window ----
namespace {
constexpr size_t kInGame_ConvWindow = 0x8efd0;  // exe+0x21a1e0 / IsOpen exe+0x21a630
constexpr size_t kCw_Visible = 0x28, kCw_Speaker = 0x2a0, kCw_Page = 0x378, kCw_Speech = 0x1ac0, kCw_Fade = 0x1ab8, kCw_RowsBegin = 0x1a60, kCw_RowsEnd = 0x1a68;
constexpr size_t kCr_Step = 0x48, kCr_Text = 0x1c8, kCr_Rect = 0x38;
constexpr size_t kCw_Rect = 0x40;         // the window's absolute rect (measured live 2026-08-22: 543,626 548x146 -- a different base than exe+0x123390's)
constexpr size_t kCwText_String = 0x38;   // the speaker/page text class (vtable exe+0x31b830) keeps its u16 string at +0x38 (measured live)
}  // namespace
ConvWindow conv_window() {
  void* w = rdp(ingame_ui(), kInGame_ConvWindow);
  return {w && vtable_rva_of(w) == rva::kConvWindowVt ? w : nullptr};
}
bool ConvWindow::open() const { return p && rd_or<uint8_t>(p, kCw_Visible, 0) != 0 && rd_or<int>(p, kCw_Fade, 3) != 3; }
std::string ConvWindow::speaker() const { return read_u16((char*)p + kCw_Speaker, kCwText_String); }
std::string ConvWindow::speech() const { return read_u16(p, kCw_Speech); }
std::string ConvWindow::page_text() const { return read_u16((char*)p + kCw_Page, kCwText_String); }
std::vector<ConvRow> ConvWindow::rows() const {
  std::vector<ConvRow> out;
  void* b = rdp(p, kCw_RowsBegin); void* e = rdp(p, kCw_RowsEnd);
  if (!b || !e || e < b) return out;
  size_t n = ((char*)e - (char*)b) / sizeof(void*);
  for (size_t i = 0; i < n && i < 32; ++i) { void* r = rdp(b, i * sizeof(void*)); if (r && vtable_rva_of(r) == rva::kConvRowVt) out.push_back({r}); }
  return out;
}
Rect ConvWindow::rect() const { return rd_or<Rect>(p, kCw_Rect, Rect{}); }
std::string ConvRow::text() const { return read_u16(p, kCr_Text); }
void* ConvRow::step() const { return rdp(p, kCr_Step); }
Rect ConvRow::rect() const { return rd_or<Rect>(p, kCr_Rect, Rect{}); }
std::string conv_elements_dump() {
  ConvWindow w = conv_window();
  if (!w) return {};
  std::string out;
  for (size_t off : {kCw_Speaker, kCw_Page}) {
    void* e = (char*)w.p + off;
    out += std::format("  element +{:#x} vt=exe+{:#x} text(+0x40)='{}' u16@+0x40 size={}\n", off, vtable_rva_of(e), read_u16(e, off::kB_Text), rd_or<size_t>(e, off::kB_Text + 16, 0));
  }
  return out;
}
bool ConvWindow::choose(const ConvRow& r) const {
  Rect w = rect(), rr = r.rect();
  if (rr.w <= 0 || rr.h <= 0) return false;
  float x = w.x + rr.x + rr.w / 2, y = w.y + rr.y + rr.h / 2;
  log::writef("exe_ui: choosing conversation row {} '{}' at ({:.0f},{:.0f})", r.p, r.text(), x, y);
  gd::hooks::click(x, y, 1);
  return true;
}

// ---- tips ----
namespace {
constexpr size_t kMainObj_TipManager = 0xbe0;                    // exe+0x262167
constexpr size_t kTipMgr_Deque = 0x08;                           // std::deque<Tip*>: map +0x10, mapsize +0x18, off +0x20, size +0x28
constexpr size_t kTip_Lines = 0x00, kTip_State = 0x18, kTip_Timer = 0x1c, kTip_Kind = 0xd98, kTip_Page = 0xda0;  // ctor exe+0x109070, exe+0x1099f0
}  // namespace
std::vector<Tip> tips() {
  std::vector<Tip> out;
  void* mgr = rdp(main_obj(), kMainObj_TipManager);
  if (!mgr) return out;
  char* dq = (char*)mgr + kTipMgr_Deque;
  void* map = rdp(dq, 0x10);
  size_t mapsize = rd_or<size_t>(dq, 0x18, 0), off = rd_or<size_t>(dq, 0x20, 0), size = rd_or<size_t>(dq, 0x28, 0);
  if (!map || !mapsize || size > 64) return out;
  for (size_t i = 0; i < size; ++i) {   // MSVC deque of 8-byte elements: 2 per block
    size_t idx = off + i, block = (idx / 2) % mapsize;
    void* blk = rdp(map, block * sizeof(void*));
    void* tip = blk ? rdp(blk, (idx % 2) * sizeof(void*)) : nullptr;
    if (tip) out.push_back({tip});
  }
  return out;
}
std::vector<std::string> Tip::lines() const {
  std::vector<std::string> out;
  void* b = rdp(p, kTip_Lines); void* e = rdp(p, kTip_Lines + 8);
  if (!b || !e || e < b) return out;
  size_t n = ((char*)e - (char*)b) / 32;
  for (size_t i = 0; i < n && i < 64; ++i) out.push_back(read_u16(b, i * 32));
  return out;
}
int Tip::state() const { return rd_or<int>(p, kTip_State, 3); }
int Tip::kind() const { return rd_or<int>(p, kTip_Kind, -1); }
int Tip::page() const { return rd_or<int>(p, kTip_Page, -1); }
void Tip::dismiss() const { write_int(p, kTip_Timer, 0x1f4); write_int(p, kTip_State, 3); log::writef("exe_ui: dismissed tip {}", p); }

// ---- dialogs ----
bool dialog_open() { void* dm = dialog_manager(); return dm && g_dm.GetNumDialog(dm) > 0; }
std::string dialog_text() { void* dm = dialog_manager(); if (!dm || g_dm.GetNumDialog(dm) <= 0) return {}; return read_u16(g_dm.PeekTopDialog(dm), off::kDialog_Text); }
int dialog_type() { void* dm = dialog_manager(); if (!dm || g_dm.GetNumDialog(dm) <= 0) return -1; return rd_or<int>(g_dm.PeekTopDialog(dm), off::kDialog_Type, -1); }
namespace { DialogAnswer g_last_answer; }
DialogAnswer last_dialog_answer() { return g_last_answer; }
bool answer_dialog(bool yes) {
  void* dm = dialog_manager();
  if (!dm || g_dm.GetNumDialog(dm) <= 0) return false;
  const void* d = g_dm.PeekTopDialog(dm);
  int type = rd_or<int>(d, off::kDialog_Type, -1);
  struct { int party; bool yes; } r{rd_or<int>(d, off::kDialog_Party, 0), yes};
  if (type == 1) g_dm.AddResponse(dm, &r);  // Okay boxes (0) take no response; the exe's Escape path does the same
  g_dm.RemoveTopDialog(dm);
  g_last_answer = DialogAnswer{hooks::frame(), type, r.party, yes};
  log::writef("exe_ui: answered dialog type={} party={} yes={}", type, r.party, yes);
  return true;
}

// ---- dev dumps ----
namespace {
void dump_tree(std::string& out, WidgetA w, int depth, int& budget, std::set<void*>& seen) {
  if (!w || budget-- <= 0 || depth > 16 || !seen.insert(w.p).second) return;
  Rect r = w.rect(), a = w.abs_rect();
  out += std::string(depth * 2, ' ');
  out += std::format("{} vt=exe+{:#x} rect=({:.0f},{:.0f} {:.0f}x{:.0f}) abs=({:.0f},{:.0f}) active={} enabled={} children={}", w.p, w.vtable_rva(), r.x, r.y, r.w, r.h, a.x, a.y,
                     (int)w.active(), (int)w.enabled(), w.children().size());
  if (w.is_button()) out += std::format(" BUTTON '{}' hovered={} pressed={} toggle={} tip='{}'", w.caption(), (int)w.hovered(), (int)w.pressed(), (int)w.is_toggle(), w.tooltip_tag());
  else if (w.is_text()) out += std::format(" TEXT '{}'", w.text());
  else if (w.is_edit()) out += std::format(" EDIT '{}' {}", w.text(), w.edit_state());
  out += '\n';
  for (WidgetA c : w.children()) dump_tree(out, c, depth + 1, budget, seen);
}
bool find_in_tree(WidgetA w, void* target, int depth = 0) {
  if (!w || depth > 16) return false;
  if (w.p == target) return true;
  for (WidgetA c : w.children()) if (find_in_tree(c, target, depth + 1)) return true;
  return false;
}
}  // namespace

std::string ui_dump() {
  std::string out = std::format("available={} {}\nmain_obj={} app={} app_state={} ui_root={} world_screen={}\n", available(), g_version, main_obj(), app(), app_state(),
                                rdp(main_obj(), off::kMainObj_UiRoot), rdp(main_obj(), off::kMainObj_WorldScreen));
  MainMenu mm = main_menu();
  out += std::format("main_menu={} (raw {} vt=exe+{:#x})\n", mm.p, rdp(app(), off::kApp_MainMenu), vtable_rva_of(rdp(app(), off::kApp_MainMenu)));
  if (mm) {
    struct { const char* name; unsigned off; } btns[] = {{"Create", MainMenu::kBtnCreate}, {"slot2a8", MainMenu::kBtnSlotA}, {"Delete", MainMenu::kBtnDelete}, {"Options", MainMenu::kBtnOptions},
                                                          {"Credits", MainMenu::kBtnCredits}, {"Exit", MainMenu::kBtnExit}, {"DLC", MainMenu::kBtnDLC}, {"GameGuide", MainMenu::kBtnGameGuide},
                                                          {"Resume?", MainMenu::kBtnResume}, {"Community", MainMenu::kBtnCommunity}, {"Multiplayer", MainMenu::kBtnMultiplayer},
                                                          {"Start", MainMenu::kBtnStart}, {"Difficulty", MainMenu::kBtnDifficulty}, {"GameMode", MainMenu::kBtnGameMode}};
    for (auto& b : btns) {
      WidgetA w = mm.button(b.off);
      out += std::format("  btn {:<11} +{:#x} = {} vt=exe+{:#x} button={} '{}' active={} enabled={}\n", b.name, b.off, w.p, w.vtable_rva(), (int)w.is_button(), w.caption(), (int)w.active(), (int)w.enabled());
    }
    struct { const char* name; unsigned off; unsigned flag; } wins[] = {{"CreateCharacter", MainMenu::kWinCreateCharacter, 0x288}, {"DeleteCharacter", MainMenu::kWinDeleteCharacter, 0}, {"Difficulty", MainMenu::kWinDifficulty, 0x350},
                                                                        {"4th", MainMenu::kWin4th, 0x29e}, {"hiding", MainMenu::kWinHiding, 0}};
    for (auto& w : wins) {
      void* win = mm.sub_window(w.off);
      out += std::format("  win {:<15} +{:#x} = {} vt=exe+{:#x} hidden_flag={} in_tree={}\n", w.name, w.off, win, vtable_rva_of(win), w.flag ? (int)window_hidden_flag(win, w.flag) : -1,
                         win ? (int)find_in_tree(root(), win) : -1);
    }
    out += std::format("  current_sub_window={}\n", mm.current_sub_window());
  }
  Popup pu = popup();
  out += std::format("popup={} text='{}' buttons={}\n", pu.window.p, pu ? pu.text() : "", pu ? pu.buttons().size() : 0);
  out += "tree:\n";
  int budget = 3000;
  std::set<void*> seen;
  dump_tree(out, root(), 0, budget, seen);
  if (budget <= 0) out += "(truncated)\n";
  return out;
}

std::string ingame_dump() {
  void* ui = ingame_ui();
  std::string out = std::format("ingame_ui={} app_state={}\n", ui, app_state());
  if (!ui) return out;
  struct { const char* name; unsigned off; } wins[] = {{"PromptBox", ingame::kPromptBox}, {"Inventory", ingame::kInventory}, {"Inspect", ingame::kCharacter},
                                                        {"Quest", ingame::kQuest}, {"Skills", ingame::kSkills}, {"MiniMap", ingame::kMiniMap}, {"Exit", ingame::kExit}, {"Party", ingame::kParty},
                                                        {"Factions", ingame::kFactions}, {"Achievements", ingame::kAchievements}, {"Devotion", ingame::kDevotion}, {"Stack", ingame::kStack},
                                                        {"Potions", ingame::kPotions}, {"QuestReward", ingame::kQuestReward}, {"Objective", ingame::kObjective}, {"LootFilter", ingame::kLootFilter},
                                                        {"Trade", ingame::kTrade}, {"Market", ingame::kMarket}, {"Enchanter", ingame::kEnchanter}, {"Transmuter", ingame::kTransmuter}, {"Altar", ingame::kAltar}, {"Shrine", ingame::kShrine}, {"ShrineCorrupt", ingame::kShrineCorrupted}};
  for (auto& w : wins) {
    WindowB win = ingame_window(w.off);
    out += std::format("  {:<14} +{:#x} vt=exe+{:#x} visible={}\n", w.name, w.off, vtable_rva_of(win.p), (int)win.visible());
  }
  void* pb = (char*)ui + ingame::kPromptBox;
  out += std::format("  prompt showing(+0x99)={} buttons:", rd_or<uint8_t>(pb, off::kPrompt_Showing, 0xff));
  for (unsigned b : {0x430u, 0x7e0u, 0xb90u, 0xf40u}) { WidgetB w{(char*)pb + b}; out += std::format(" [+{:#x} vt=exe+{:#x} '{}' {}]", b, w.vtable_rva(), w.text(), w.state_bytes()); }
  out += std::format("\n  host +{:#x} vt=exe+{:#x}\n", ingame::kHost, vtable_rva_of((char*)ui + ingame::kHost));
  ExitWindow ew = exit_window();
  out += std::format("  exit window {} visible={} title='{}' registry vt=exe+{:#x}\n", ew.p, (int)ew.visible(), WidgetB{(char*)ew.p + ExitWindow::kTitle}.text(), vtable_rva_of((char*)ew.p + ExitWindow::kRegistry));
  for (WidgetB b : ew.buttons()) out += std::format("    button {} vt=exe+{:#x} '{}' {}\n", b.p, b.vtable_rva(), b.text(), b.state_bytes());
  return out;
}

std::string dialog_dump() {
  void* dm = dialog_manager();
  if (!dm) return "no dialog manager (not in the world?)\n";
  int n = g_dm.GetNumDialog(dm);
  std::string out = std::format("dialogs={}\n", n);
  if (n > 0) out += std::format("  type={} party={} text='{}'\n", dialog_type(), rd_or<int>(g_dm.PeekTopDialog(dm), off::kDialog_Party, -1), dialog_text());
  return out;
}

bool peek_u32(const void* p, unsigned& out) {
  __try { out = *(const unsigned*)p; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
std::string peek(uintptr_t ptr, int n) {
  std::string out;
  if (n > 4096) n = 4096;
  for (int o = 0; o < n; o += 16) {
    uint8_t b[16] = {};
    int m = n - o < 16 ? n - o : 16;
    if (!read_mem((char*)ptr + o, b, m)) { out += std::format("+{:#06x}: (unreadable)\n", o); break; }
    out += std::format("+{:#06x}:", o);
    for (int i = 0; i < m; ++i) out += std::format(" {:02x}", b[i]);
    float f[4]; memcpy(f, b, sizeof f);
    out += std::format("   f=({:.1f},{:.1f},{:.1f},{:.1f})", f[0], f[1], f[2], f[3]);
    uint64_t q0, q1; memcpy(&q0, b, 8); memcpy(&q1, b + 8, 8);
    if (in_exe((void*)q0)) out += std::format(" [exe+{:#x}]", q0 - g_base);
    if (in_exe((void*)q1)) out += std::format(" [+8: exe+{:#x}]", q1 - g_base);
    out += '\n';
  }
  return out;
}

bool activate_ptr(uintptr_t p) {
  WidgetA w{(void*)p};
  if (!find_in_tree(root(), w.p)) { log::writef("exe_ui: {:#x} is not in the current tree", p); return false; }
  return w.activate();
}
namespace {
char* illusionist_window() {
  WindowB w = ingame_window(ingame::kTransmuter);
  return w && w.visible() && vtable_rva_of(w.p) == 0x31da48 ? (char*)w.p : nullptr;
}
size_t illusionist_span(const void* begin, const void* end, size_t stride, size_t max) {
  uintptr_t b = (uintptr_t)begin, e = (uintptr_t)end;
  return b && e >= b && (e - b) % stride == 0 && (e - b) / stride <= max ? (e - b) / stride : 0;
}
unsigned illusionist_box_id(void* box) {
  if (!box) return 0;
  __try {
    void* item = ((void* (*)(void*))(*(void***)box)[0xa0 / 8])(box);
    return item ? *(unsigned*)((char*)item + 0x30) : 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
bool illusionist_refresh(char* w) {
  // Native slot/appearance selection handler, exe+0x28069d / +0x280713.
  __try { ((void (*)(void*, bool))(g_base + 0x281640))(w, true); return true; }
  __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}
IllusionistState illusionist_state() {
  IllusionistState s;
  char* w = illusionist_window();
  if (!w) return s;
  s.open = true;
  s.equipment = rd_or<unsigned>(w, 0x108, 0);
  s.appearance = rd_or<unsigned>(w, 0x114, 0);
  char* boxes = (char*)rdp(w, 0xa50);
  size_t n = illusionist_span(boxes, rdp(w, 0xa58), 8, 32);
  for (size_t i = 0; i < n; ++i) {
    unsigned id = illusionist_box_id(rdp(boxes, i * 8));
    if (id) s.equipment_ids.push_back(id);
  }
  // vector<vector<unsigned>> (24-byte pages), used by native page refresh +0x2823e0.
  char* pages = (char*)rdp(w, 0x998);
  n = illusionist_span(pages, rdp(w, 0x9a0), 24, 512);
  for (size_t p = 0; p < n; ++p) {
    char* ids = (char*)rdp(pages, p * 24);
    size_t count = illusionist_span(ids, rdp(pages, p * 24 + 8), 4, 1024);
    for (size_t i = 0; i < count; ++i) {
      unsigned id = rd_or<unsigned>(ids, i * 4, 0);
      if (id && std::find(s.appearance_ids.begin(), s.appearance_ids.end(), id) == s.appearance_ids.end()) s.appearance_ids.push_back(id);
    }
  }
  // Native pending-preview map: sentinel +0x9c8; nodes have equipment id +0x10,
  // appearance object +0x18. Read only, never modify its container.
  void* head = rdp(w, 0x9c8);
  void* node = rdp(head, 0);
  for (size_t i = 0; node && node != head && i < 32; ++i) {
    s.pending.emplace_back(rd_or<unsigned>(node, 0x10, 0), rd_or<unsigned>(rdp(node, 0x18), 0x30, 0));
    node = rdp(node, 0);
  }
  s.cost = WidgetB{w + 0x1940}.text();
  s.money = WidgetB{w + 0x1848}.text();
  s.can_apply = !s.pending.empty() && WidgetB{w + 0x598}.enabled();
  return s;
}
bool illusionist_select(unsigned id, bool appearance) {
  IllusionistState s = illusionist_state();
  const auto& ids = appearance ? s.appearance_ids : s.equipment_ids;
  if (!s.open || !id || std::find(ids.begin(), ids.end(), id) == ids.end()) return false;
  char* w = illusionist_window();
  return w && write_int(w, appearance ? 0x114 : 0x108, (int)id) && illusionist_refresh(w);
}
bool illusionist_apply(const IllusionistState& expected) {
  IllusionistState s = illusionist_state();
  if (!s.open || !s.can_apply || s.pending != expected.pending || s.cost != expected.cost || s.money != expected.money) return false;
  char* w = illusionist_window();
  return w && WidgetB{w + 0x598}.press(w + 0x948);
}
std::string illusionist_dump() {
  auto s = illusionist_state();
  std::string out = std::format("open={} equipment={} appearance={} cost='{}' money='{}' can_apply={} pending={}\n", s.open, s.equipment, s.appearance, s.cost, s.money, s.can_apply, s.pending.size());
  for (unsigned id : s.equipment_ids) out += std::format("equipment {} '{}'\n", id, gameapi::item_name(gameapi::object_by_id(id)));
  for (unsigned id : s.appearance_ids) out += std::format("appearance {} '{}'\n", id, gameapi::item_name(gameapi::object_by_id(id)));
  return out;
}
}  // namespace gd::exe_ui
