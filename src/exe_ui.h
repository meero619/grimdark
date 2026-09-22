#pragma once
// The exe's private UI objects, reached by base-relative layout (static RE of Grim Dawn v1.3.0.8 x64, 2026-08-22;
// docs/exe-ui-layout.md has the evidence). Two widget frameworks live in the exe, neither exported:
//   A -- the menu tree (main menu and its dialogs, options, multiplayer): a parent/children widget tree under a
//        DisplayWidget root; buttons carry their caption and a listener list, and a click is a listener call.
//   B -- the in-world windows owned by InGameUI (character, quest, exit menu, NPC dialog, prompt box ...):
//        by-value members at fixed offsets; open = a virtual IsVisible(); buttons are pressed through the host
//        widget's "click this child" virtual, the same path the game's own key bindings use.
// Message boxes sit on the EXPORTED GAME::DialogManager and are read/answered through it.
// Rules: everything is exe_base()+rva or an offset off a live pointer (ASLR-safe); every read is SEH-guarded;
// every call happens on the game thread; no widget pointer is held across frames (resolve each frame).
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace gd::exe_ui {

// Installed once; false when the exe does not match the layout this module was written against (a game
// patch). Screens built on this module report inactive then, so the unsupported fallback takes over.
bool install();
bool available();
std::string version_line();   // the exe's PE timestamp / size, for the log and /health

struct Rect { float x = 0, y = 0, w = 0, h = 0; };

// ---- framework A: the menu tree ----
struct WidgetA {
  void* p = nullptr;  // the tree node (the "B" subobject as stored in child vectors)
  explicit operator bool() const { return p != nullptr; }
  bool operator==(const WidgetA& o) const { return p == o.p; }
  WidgetA parent() const;
  std::vector<WidgetA> children() const;
  Rect rect() const;       // parent-relative
  Rect abs_rect() const;   // summed up the parent chain (scroll offsets ignored)
  bool active() const;     // +0x50: gates rendering and all input
  bool enabled() const;    // +0x51: a disabled (greyed) button has 0 -- measured live on Create Character's Next
  uintptr_t vtable_rva() const;
  bool is_button() const;  // caption at +0x268; toggle buttons (radio/check) keep their state in pressed()
  bool is_text() const;    // a static text widget: string at +0xc0 (wrapped at draw time)
  bool is_edit() const;    // an edit box: string at +0x238, caret index +0x21c, focus byte +0x218
  std::string text() const;  // the widget's own string (caption / static text / edit contents), UTF-8
  std::string caption() const { return is_button() ? text() : std::string(); }
  std::string tooltip_tag() const;  // a button's rollover localization tag (+0x78, std::string); empty if none
  bool hovered() const;
  bool pressed() const;    // +0x249: pressed, or for a toggle button its checked/selected state
  bool is_toggle() const;  // +0x24a: a radio / check box (left-down flips pressed and fires the listeners once)
  std::string edit_state() const;  // dev: the edit box's state bytes
  // What the game's own mouse path does on a click, minus the sounds: a plain button gets press (0) then
  // release-inside (2) through its listeners; a toggle flips its state and fires once. Game thread. False if
  // this is not a button or a call faulted.
  bool activate() const;
  // Children of one kind, in tree (= draw) order.
  std::vector<WidgetA> buttons() const;
  std::vector<WidgetA> texts() const;
  std::vector<WidgetA> edits() const;
  // ---- the options screen's value controls (docs/exe-ui-layout.md, "Options") ----
  bool is_slider() const;          // exe+0x30d4c0: float value +0x324 in [min +0x320, max +0x31c]
  float slider_value() const;      // normalised to 0..1 (the options sliders are 0..1 already)
  bool set_slider(float v01) const;  // writes the value and fires the slider's listeners (what a drag does)
  bool is_combo() const;           // exe+0x30c278: items vector +0xd0 (0x30 stride, u16 text first), selected +0xec
  std::vector<std::string> combo_items() const;
  int combo_index() const;
  bool set_combo(int index) const;   // writes the index and fires the listeners (what choosing a row does)
  bool is_list() const;            // exe+0x30c530: the key-binding table, rows of u16 cells (action, key, key)
  std::vector<std::vector<std::string>> list_rows() const;
};

// A modal popup of the menu tree (e.g. "A character with that name already exists."): a layer widget at the
// root holding one window with text widgets and buttons. Null when none is up.
struct Popup {
  WidgetA window;
  explicit operator bool() const { return (bool)window; }
  std::string text() const;              // its text widgets, joined
  std::vector<WidgetA> buttons() const { return window.buttons(); }
};
Popup popup();

// App state: 3/4/8 main menu, 5 options, 6 multiplayer, 7 and 9 unnamed, 10 the world. 0 = unknown.
int app_state();
WidgetA root();             // the tree root (MenuManager+8); null outside the menus

// The main-menu manager (app state 3/4/8) and its named slots.
struct MainMenu {
  void* p = nullptr;
  explicit operator bool() const { return p != nullptr; }
  WidgetA button(unsigned slot_off) const;   // one of the kBtn* offsets below
  void* sub_window(unsigned slot_off) const; // kWinCreateCharacter ... (null when not open)
  void* current_sub_window() const;          // +0xf8
  // Slots measured live 2026-08-22 against the captions (click handler exe+0xd8f80 names the behaviours).
  static constexpr unsigned kBtnCreate = 0x2a0, kBtnDelete = 0x2b0, kBtnOptions = 0x2f8 /*icon, no caption*/, kBtnCredits = 0x300,
                            kBtnExit = 0x308 /*icon, no caption*/, kBtnDLC = 0x310, kBtnGameGuide = 0x318, kBtnResume = 0x320 /*null so far*/,
                            kBtnCommunity = 0x328, kBtnMultiplayer = 0x330, kBtnStart = 0x338, kBtnDifficulty = 0x340 /*"Normal Difficulty"*/,
                            kBtnGameMode = 0x348 /*"Main Campaign"*/, kBtnSlotA = 0x2a8;
  static constexpr unsigned kWinCreateCharacter = 0x2b8, kWinDeleteCharacter = 0x2c0, kWinDifficulty = 0x2d0,
                            kWin4th = 0x2d8, kWinHiding = 0x2e0;
  // The character list on the left (docs/exe-ui-layout.md "Character picker"): the CharacterPicker at +0x370 (not
  // in the widget tree; the menu renders and hit-tests it itself) and +0x378 = the preview Player's object id of
  // the committed selection (0 = none; the Start/Delete buttons follow it).
  struct Character { std::string name, class_tag; unsigned level = 0; bool female = false, hardcore = false; unsigned preview_id = 0; };
  std::vector<Character> characters() const;   // the playable saves in the picker's order
  int selected_character() const;              // the picker's selected index (-1 = none)
  bool select_character(int index) const;      // write the index (and keep it on screen); the game commits it next frame
  static constexpr unsigned kPicker = 0x370, kSelectedPreviewId = 0x378;
  // A sub-window object stays allocated while hidden behind the next dialog; these bytes say so.
  static constexpr unsigned kCreateCharacterHidden = 0x288, kDifficultyHidden = 0x350, k4thHidden = 0x29e;
};
MainMenu main_menu();
// A sub-window object's tree node: the window object IS its node (measured live 2026-08-22: the Create
// Character window pointer appears in the manager's child vector as is).
WidgetA window_node(void* window);
bool window_hidden_flag(void* window, unsigned flag_off);  // CreateCharacter +0x288, Difficulty +0x350

// The options screen (app state 5): seven tab toggles (their rollover tags name them), the active page's
// controls in declaration order (a TEXT label immediately precedes the slider/combo it names), and the
// Default / Apply / Close buttons. Page switch = the tab toggle's listeners (exe+0xcd300).
struct OptionsScreen {
  WidgetA screen, panel, page;
  std::vector<WidgetA> tabs, buttons;  // buttons: in tree order (Apply, Close, Default)
  explicit operator bool() const { return screen && page && tabs.size() == 7; }
  int tab_index() const;               // screen+0x280
};
OptionsScreen options_screen();

// ---- framework B: InGameUI's embedded windows ----
void* ingame_ui();  // null outside the world
struct WindowB {
  void* p = nullptr;
  explicit operator bool() const { return p != nullptr; }
  bool visible() const;    // vtable +0xb8
  void show(bool on) const;  // vtable +0xb0
};
WindowB ingame_window(unsigned off);   // InGameUI + off (kWin* below)
// ---- the loot filter window (docs/re_lootfilter_exe.md): its check boxes are heap objects in a
// std::map<CheckBox*, LootFilterOption> at window+0xd58; a box's checked byte is the TextButton pressed byte ----
struct LootFilterBox { void* ctrl = nullptr; int option = -1; bool checked = false; };
std::vector<LootFilterBox> loot_filter_boxes();      // empty outside the world
bool loot_filter_mirror(int option, bool on);         // write the drawn box's state after Player::SetLootFilter (the game only refreshes on Show)
// The actor capture's "show every item label" modifier byte (what holding Alt sets, key action 0x23; read by the
// exe's ItemIgnore helper exe+0x20f70): written every frame while the mod's O latch is on.
bool set_show_all_items(bool on);
// ---- the crafting (blacksmith) window (docs/re_crafting_exe.md): a frame around a by-value crafting panel whose
// list box holds the recipe rows in the game's own order and grouping ----
struct CraftingRow { std::string text; unsigned formula = 0; bool selected = false; bool is_new = false; bool header() const { return formula == 0; } };
constexpr int kCraftingTabs = 5;
std::vector<CraftingRow> crafting_rows();       // the current category's rows, colour codes stripped, "[N] " prefix kept
int crafting_tab();                             // 0..4 in screen order (Relics, Melee, Ranged, Armor, Accessories+Consumables), -1 when closed
bool crafting_press_tab(int index);             // the tab button through the window's radio registry (rebuilds the rows)
const char* crafting_tab_tag(int index);        // tagCraftTab*A
std::string crafting_npc_name();                // the title text ("Angrim")
unsigned crafting_npc_id();                     // window+0x9c: the crafter (SetCrafter, RTTI-checked)
unsigned crafting_selected();                   // the selected formula id (0 = none)
bool crafting_select(unsigned formula_id);      // the list box's own SelectByData (signature-checked RVA); the panel follows on the next Update
bool crafting_combine_enabled();                // panel+0x2129 == 0
bool crafting_combine();                        // press the Combine button through the panel's registry (refused while disabled)
// The whole craft in one call, synchronous: select the row in the list box, write the panel's selected id and its
// Combine-enabled byte ourselves (what the panel's next Update would do from the same inputs -- the caller has
// already applied the game's gate, GetMaximumCraftable >= 1), then press Combine.
bool crafting_craft(unsigned formula_id);
std::string crafting_dump();

// ---- the Inventor's window (the exe's "enchanter" window, InGameUI+0x30dd8; docs/inventor.md, RE in
// docs/re_inventor_exe.md): a frame with up to four tab panels -- Salvage (recover a component / the item /
// strip an augment), Dismantle (break an item into scrap + a component for dynamite and bits), and the
// expansions' Convert / Reroll, absent from a base-game install. Each panel owns one "chamber" item box the
// player drops an item into; the item LEAVES the inventory while it sits there (the drop = box SetItem +
// PlayerInventoryCtrl::RemoveItem) and comes back when the panel hides or the box is clicked again.
constexpr int kInventorTabs = 4;   // Salvage, Dismantle, Convert, Reroll (screen order = the exe's tab index +0x8bc0)
struct InventorTab { bool present = false; bool enabled = false; std::string label; const char* info_tag = nullptr; };
struct InventorButton { void* p = nullptr; std::string caption; bool enabled = false; const char* warning_tag = nullptr; };
struct InventorSalvage {
  unsigned item = 0;              // the chamber's item id (0 = empty)
  std::string cost;               // the panel's own cost text ("1,234"), "" until an item is in
  bool too_expensive = false;     // panel+0x10a9
  bool dialog_pending = false;    // a confirm box is up for one of the buttons
  InventorButton keep_item, keep_addon, remove_augment;
};
struct InventorDismantle {
  unsigned item = 0;              // the chamber's item id
  unsigned result1 = 0, result2 = 0;   // the two output boxes (scrap; the bonus component)
  std::string cost, dynamite;     // the panel's own texts
  bool no_money = false, no_dynamite = false, dialog_pending = false;
  InventorButton dismantle;
};
bool inventor_open();                              // the window is visible
std::string inventor_npc_name();                   // "Darlet" (enchanterNameText +0x2e0)
unsigned inventor_npc_id();                        // window+0x9c
int inventor_tab();                                // the exe's current tab index (+0x8bc0), -1 when closed
std::vector<InventorTab> inventor_tabs();          // kInventorTabs entries; present = built by this game's records, enabled = the tab button not greyed
bool inventor_press_tab(int index);                // through the window's radio registry (+0x8bc8): the game's own listener shows/hides the panels
InventorSalvage inventor_salvage();
InventorDismantle inventor_dismantle();
// The chamber: put a bag item in (the exe's drop sequence without the cursor: box SetItem(id) + RemoveItem from the
// inventory ctrl + the box's "holds a detached item" byte), or give the chamber's item back (the exe's own return
// helper sequence: SetItem(0) + ControllerPlayer::GiveItemToPlayer). `which`: 0 = the current tab's chamber,
// 1 / 2 = the dismantle tab's result boxes (take only).
bool inventor_put(unsigned item_id);
bool inventor_take(int which);
// Press a panel button through the panel's own registry: the game's listener opens its confirm dialog (a
// DialogManager box the message_box screen answers) or, for a plain dismantle, acts at once.
enum class InventorAction { KeepItem, KeepAddon, RemoveAugment, Dismantle };
bool inventor_press(InventorAction a);
std::string inventor_dump();
// Illusionist: native equipment boxes, all appearance pages, and staged previews.
struct IllusionistState {
  bool open = false, can_apply = false;
  unsigned equipment = 0, appearance = 0;
  std::vector<unsigned> equipment_ids, appearance_ids;
  std::vector<std::pair<unsigned, unsigned>> pending;
  std::string cost, money;
};
IllusionistState illusionist_state();
bool illusionist_select(unsigned id, bool appearance);
bool illusionist_apply(const IllusionistState& expected);
std::string illusionist_dump();
// ---- the riftgate travel map (docs/exe-ui-layout.md "Riftgate travel"): the world map in riftgate mode ----
struct Riftgate {
  std::string name;      // the zone's localized name ("Devil's Crossing")
  int pos[3] = {};       // integer world coordinates (what the map's travel call takes)
  unsigned object_id = 0;  // the gate entity when loaded, else 0
  unsigned owner = 0;    // a personal riftgate's player id; 0 for the static gates
  int uid[4] = {};       // UniqueId (the discovered-set key)
  bool current = false;  // the gate the player is standing at
};
bool riftgate_map_open();                  // MiniMap visible + shown + mode byte 0 (the exe's own predicate, exe+0x21be20)
std::vector<Riftgate> riftgates();         // the discovered gates the map draws, in its section order
bool riftgate_travel(const Riftgate& g);   // what the click does: SetLastUsedTeleportId + the map's travel call (exe+0x291520)
void riftgate_map_close();                 // the close button: MiniMap Show(false)
std::string map_nuggets_dump(int maxn);    // dev: the aerial map's cached MinimapGameNugget vector
bool aerial_nugget_span(void*& begin, size_t& count);   // the live nugget vector (0xA0 stride); false when the map has not populated it
bool aerial_map_open();                    // the local aerial map (M / Ctrl+M): MiniMap shown, mode 1
// The aerial map's zoom: the map is an orthographic camera whose view height is zoom * 3 world units; the wheel
// clamps it to kAerialZoomMin..kAerialZoomMax (exe+0x174c42/+0x174c5d) and saves it as options.txt mapZoom.
// The icon list is gathered from that camera's frustum, so the zoom IS the reach of the map. Fields at
// MiniMap+0x166c (current) / +0x1670 (target), verified live 2026-09-11 (docs/map-icons.md).
inline constexpr float kAerialZoomMin = 40.0f, kAerialZoomMax = 135.0f;
float aerial_zoom();                       // the current value, 0 when the map object is unavailable
bool aerial_zoom_set(float zoom);          // writes current + target (no lerp), clamped to the wheel's range
void aerial_map_close();                   // MiniMap Show(false)
namespace ingame {
constexpr unsigned kPromptBox = 0x7378, kCharacter = 0x52258 /*the multiplayer Inspect twin*/, kInventory = 0xbbf0 /*the C/I window*/,  // (+0xb138/+0xb158 are record-path strings)
                   kQuest = 0x285a0, kSkills = 0x3fc20, kMiniMap = 0x42260, kExit = 0x4a300, kParty = 0x4b540,
                   kFactions = 0x6c9b8, kAchievements = 0x7d150, kDevotion = 0x813a0, kStack = 0x83ed8,
                   kPotions = 0x8a300, kQuestReward = 0x8efd8, kObjective = 0x90390, kLootFilter = 0xab410,
                   kTrade = 0x29cc8, kMarket = 0x2b538, kEnchanter = 0x30dd8, kTransmuter = 0x85378, kAltar = 0x87628,
                   kFactionVendor = 0x2e188, kCaravan = 0x4fd08, kShrine = 0x7da50 /*ruined: offerings*/, kShrineCorrupted = 0x7f6f8 /*desecrated: summon monsters; its own window class (vt exe+0x318128), same widget offsets*/, kCrafting = 0x3aa80, kAscension = 0x8baa8;
constexpr unsigned kHost = 0x7338;  // the widget host whose vtable +0x80 presses a child button
// The pause menu's Options: InGameUI+0x4def8 holds a POINTER to a 0x98-byte host window (ctor exe+0x29efc0,
// vtable exe+0x31dd90; visible byte +0x68) whose +0x90 is the framework-A Options screen itself (allocated
// 0x508 and built by the same ctor exe+0xc8e60 as the main menu's, in exe+0x29f2d0). The app state stays 10.
constexpr unsigned kOptionsHostPtr = 0x4def8, kOptionsHost_Visible = 0x68, kOptionsHost_Screen = 0x90;
}
struct WidgetB {
  void* p = nullptr;
  explicit operator bool() const { return p != nullptr; }
  uintptr_t vtable_rva() const;
  bool is_button() const;         // the plain bitmap button (no caption of its own)
  bool is_text_button() const;    // TextButton: localized caption at +0x358 (ctor exe+0x126fe0)
  bool is_text() const;           // text element: string at +0x40
  std::string text() const;       // caption / text, UTF-8
  bool visible() const;           // +0x28
  bool enabled() const;           // !+0x281 (the host's PressChild refuses disabled controls too)
  bool pressed() const;           // +0x282
  std::string state_bytes() const;  // dev
  // Press through a listener registry's PressChild (vtable +0x80): the registry of the window that owns the
  // control (window + its registry offset) performs a complete click (events 0,1,2); InGameUI's HUD host at
  // +0x7338 toggles instead. Returns false when the registry refused (control unregistered or disabled) or
  // the call faulted. Game thread.
  bool press(void* registry, bool sound = true) const;   // sound = the registry's playSound argument (the button click)
};

// The in-world Escape menu (hudExitWindow, ctor exe+0x26e060): Return to Game / Options Menu / Exit to Main
// Menu / Quit to Desktop as TextButtons at fixed offsets, pressed through the window's own registry (+0x108).
struct ExitWindow {
  void* p = nullptr;
  explicit operator bool() const { return p != nullptr; }
  bool visible() const;
  static constexpr unsigned kResume = 0x150, kExit = 0x500, kExitGame = 0x8b0, kOptions = 0xc60, kRegistry = 0x108, kTitle = 0x1010;
  WidgetB button(unsigned off) const { return {(char*)p + off}; }
  std::vector<WidgetB> buttons() const { return {button(kResume), button(kOptions), button(kExit), button(kExitGame)}; }  // player order
  bool press(WidgetB b) const { return b.press((char*)p + kRegistry); }
};
ExitWindow exit_window();
// The game's own key-binding actions by id (InGameUI::HandleKeyAction, signature-checked): 0x37 = Pickup (the
// nearest item on the ground), 0x36 = Interact, 1 = Character window ... (docs/ingame-ui-survey.md). Game thread.
bool ingame_key_action(int action);
// The skills window's panes (SkillsWindow::SetPane, signature-checked): put mastery `pane_index` (the mastery's
// enumeration, 0 = Soldier ...) or the class-selection pane (kSkillsClassSelectPane) on tab 0 / 1. The game's own
// choose-a-class path; permanent once the mastery skill has a point. skills_tab() = the window's current tab.
// The quickbar page the HUD shows (InGameUI+0x72f0, 0..3; the Y key cycles it), -1 outside the world.
int quickbar_page();
// The game's Toggle UI state (key action 0x3c, default ], flips InGameUI+0xac990; only the UI render pass reads it,
// so a hidden interface still takes keys and clicks): 1 = interface shown, 0 = hidden, -1 outside the world.
int ui_visible();
// A vendor window's market id (its marketGrid +0x2410 keeps it at +0x54; 0 outside a vendor).
unsigned vendor_market_id(const WindowB& vendor_window);
// The vendor window's tab map (window+0x26f8, mem::map<Market_TypeEnum, TabButton*>; read by its refresh at
// exe+0x273120 as node+0x20 -> GetMarketInventorySack(marketId, type), node+0x28 -> the tab button whose +0x281
// disabled byte it sets when that sack is empty). index = which of the five tab buttons (+0x550 +0x888 +0xbc0
// +0xef8 +0x1230, the master table's marketTab1..5Button). The faction vendor is the same class with its own
// master table: tabs 1-4 are the reputation tiers (tagFactionVendorTab01A..04A), tab 5 is Buyback.
struct VendorTab { int type = 0; int index = -1; void* button = nullptr; bool disabled = false; };
std::vector<VendorTab> vendor_tabs(const WindowB& vendor_window);
int vendor_selected_type(const WindowB& vendor_window);   // window+0x25e0: the Market_TypeEnum of the tab the game shows
// ---- the caravan (stash) window: two panels, private stash (+0x13c8) and transfer (+0x13d0); each keeps its record's
// tab cost array at +0x378 (InventoryCostArray / TransferPageCostArray: the price of tab N is costs[N-1], the first is
// free) and its tab-list object at +0x98. The exe's buy handlers (exe+0x131150 / +0x1316d0) are: money >= cost, fewer
// sacks than N, SubtractMoney, Player::AddSack / GameEngine::AddTransferSack, then rebuild the tab list from the sack
// vector (exe+0x25d890) and re-apply the sack dims (exe+0x12ec70). caravan_refresh does those two by RVA. ----
// The HUD's screen rectangles, from the values InGameUI::Init (exe+0x213840) copies out of its positioning windows
// after their records place them (hudwindow_toolbar / _portraitandstatus / _compass: 1024-wide, Center/Bottom):
// InGameUI+0xb748 the toolbar {x,y,w,h}, +0xb758 the portrait/status strip {x,y,w,h}, +0xb768 the HUD block's
// top-left point. Measured live 2026-09-04 at 1600x900: toolbar (398,854,805,46), status (441.7,815.2,716.7,38.5),
// top-left (398,781). The root mouse handler (exe+0xbef10) hands every event to the UI first and the world only sees
// what the UI did not consume, so a press whose point lies here clicks the HUD, never the world.
std::vector<Rect> hud_rects();                        // empty outside the world or when the values look wrong
bool point_over_hud(float x, float y);
struct CaravanPanel { void* panel = nullptr; std::vector<int> costs; };
CaravanPanel caravan_panel(bool shared);            // empty panel when the window or the layout check fails
bool caravan_refresh(bool shared, const void* sack_vector);   // after a purchase: the game's own tab-list rebuild
constexpr int kSkillsClassSelectPane = 0x50;
bool skills_set_pane(int tab, int pane_index);
int skills_tab();
// True while the skills window is in spirit-guide reclaim mode (the window's own byte +0x2639, set by
// DisplaySkillReallocationWindow; else either mastery pane's +0x1e4c): clicks reclaim instead of learn. Refunding
// a skill point is only allowed then.
bool skills_reclaim_mode();
// The mastery pane's "Undo Points" button on tab slot 0 / 1 (reverts the skill points spent since the window
// opened): whether the game shows it enabled, and pressing it through the pane's own registry. Takes the SCREEN's
// tab: the game's own tab can rest on the class-selection pane (a one-class character) while we show the mastery.
bool skills_undo_points_enabled(int tab);
bool skills_undo_points(int tab);
// Press a skill's icon on whichever mastery pane holds it (the game's own learn / reclaim click, which also records
// the pending delta Undo Points reverts). False when no mastery pane has the skill.
bool skills_press_skill(unsigned skill_id);
std::string skills_pane_dump();   // dev: the pane's icon entries (control, delta, skill id)

// ---- the devotion window's constellation graph (docs/re_devotion_exe.md; built by the exe at load, valid whether
// or not the window was ever shown). Read-only except set_star_host, which mirrors a celestial-power binding into
// the exe's Star so its own picker / validation agree with what the mod bound through the exports.
struct DevotionStarB {
  void* p = nullptr;
  unsigned index = 0;           // 1-based position in the constellation (what devotionLinks refer to)
  unsigned skill_id = 0;        // the star's Skill object id
  unsigned host_id = 0;         // the skill this star's celestial power is bound to (0 = none)
  std::vector<int> links;       // 1-based indices of the stars this one hangs off (empty = the root)
};
struct DevotionConstellationB {
  void* p = nullptr;
  std::string name_tag, info_tag;                        // constellationDisplayTag / constellationInfoTag
  std::vector<std::pair<int, unsigned>> required, given;  // {AffinityType 0..4, amount}
  std::vector<DevotionStarB> stars;
};
std::vector<DevotionConstellationB> devotion_constellations();   // empty when the window / layout is unavailable
bool devotion_set_star_host(void* star, unsigned host_id);        // Star+0x10c
// AffinityType (read off the exe's record parser): 0 Ascendant, 1 Chaos, 2 Eldritch, 3 Order, 4 Primordial.
constexpr int kAffinityCount = 5;

// The NPC conversation window (allocated on demand, pointer at InGameUI+0x8efd0; ctor exe+0x16e9a0): the
// speaker and speech text, and the response rows, each carrying its display text and the step it selects
// (null = end conversation). Choosing a row goes through the game's own click path (the step's quest
// actions run there): a click at the row's own rectangle.
struct ConvRow {
  void* p = nullptr;
  std::string text() const;     // +0x1c8
  void* step() const;           // +0x48
  Rect rect() const;            // +0x38, relative to the window
};
struct ConvWindow {
  void* p = nullptr;
  explicit operator bool() const { return p != nullptr; }
  bool open() const;            // +0x28 visible and fade state +0x1ab8 != closed
  std::string speaker() const;  // the +0x2a0 text element
  std::string speech() const;   // the full NPC speech +0x1ac0
  std::string page_text() const;  // the currently shown page (+0x378 text element)
  std::vector<ConvRow> rows() const;  // +0x1a60 vector
  Rect rect() const;            // the window's own rect (+0x260)
  bool choose(const ConvRow& r) const;  // click at the row's rectangle (game thread queues the events)
};
ConvWindow conv_window();
std::string conv_elements_dump();  // dev: the vtables/texts of the window's speaker and page elements
std::string peek(uintptr_t ptr, int n);
bool peek_u32(const void* p, unsigned& out);   // one guarded dword read (a widget field such as the shrine window's object id)  // dev: hex dump of n bytes (qwords that point into the exe are annotated)

// ---- tutorial tips / notifications: the tip manager at [main_obj+0xbe0] (ctor exe+0x1087f0) ----
// A tip is a heap struct holding its already-localized, line-split text (line 0 is the title); kind 1 =
// tutorial tip; state 0 fading in, 1 shown, 2 fading out, 3 dismissed (what a right click sets).
struct Tip {
  void* p = nullptr;
  explicit operator bool() const { return p != nullptr; }
  std::vector<std::string> lines() const;
  int state() const;
  int kind() const;
  int page() const;   // help page id, -1 when not clickable
  bool showing() const { return p && state() < 2; }
  void dismiss() const;  // state 3 + the fade-out timer, as the right-click path does
};
std::vector<Tip> tips();  // live tips, oldest first

// ---- message boxes (exported DialogManager) ----
bool dialog_open();
std::string dialog_text();
int dialog_type();               // 0 Okay, 1 Yes/No, -1 none
bool answer_dialog(bool yes);    // game thread; Okay boxes are just removed
// The last answer given through answer_dialog (frame 0 = none yet). A screen that pressed a game button whose
// listener opened a confirm box reads this to tell "the player said Yes, the command is in flight" from "No":
// the exe processes the response on its NEXT update, so right after the box closes the panel still looks untouched.
struct DialogAnswer { uint64_t frame = 0; int type = -1; int party = 0; bool yes = false; };
DialogAnswer last_dialog_answer();

// ---- dev dumps (game thread) ----
std::string ui_dump();           // app state, main menu slots, the whole framework A tree
std::string ingame_dump();       // every known InGameUI window with IsVisible(), the prompt state
std::string dialog_dump();
bool activate_ptr(uintptr_t p);  // /ui/activate?ptr= -- pointer must be a node in the current tree
}  // namespace gd::exe_ui
