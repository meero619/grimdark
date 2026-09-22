# The exe's UI objects (Grim Dawn v1.3.0.8 x64, exe timestamp 0x6a85fbec, image 0x482000)

How `src/exe_ui.cpp` reaches the game's menus and in-world windows without screen space. Static RE on the
unpacked image (`tools/dump_exe.py` -> `build/GrimDawn.unpacked.bin`, `tools/exe_dis.py`), 2026-08-22, then
checked live through the `/ui`, `/ingame`, `/dialog` dev routes. All addresses are RVAs (`exe+...`); nothing
here is absolute (ASLR). "Proof" = the instruction that establishes the fact. A game patch moves all of it:
`exe_ui::available()` compares code bytes at eight of these sites on first use and the menus fall back to the
unsupported screen when they differ.

The exe holds **two private widget frameworks** (neither Engine.dll nor Game.dll exports them; MSVC RTTI is
stripped from all three binaries, the `.?AV...` strings are unreferenced) plus one exported message-box API.

## Framework A -- the menu tree (main menu, Create/Delete Character, Difficulty, Options, Multiplayer)

Roots and screen state:
- `main_obj = [exe+0x3ceef8]` (stored at exe+0x86cff). `app = [main_obj+0x88]`: the exe's `Display`
  subclass = the UI root `DisplayWidget` (vtable exe+0x30c958; mouse handler exe+0xbef10). The tree root
  node is `app+8` (node vtable exe+0x30c888; thunks at exe+0xc324c prove the +8). NOT `[main_obj+0x250]`
  (measured: garbage).
- `app+0x260` current app state (exe+0xbe3d0): 3/4/8 main menu, 5 options, 6 multiplayer, 7 and 9 unnamed,
  10 entering the world, 1 in the world. `app+0x258` requested state; `App::RequestState` = exe+0xbb2c0.
- The main-menu manager (ctor exe+0xd4fc0, vtable exe+0x30cca0) is the child of the root with that vtable
  (its primary vtable IS the node vtable, the node pointer is the object). `app+0x298` points to it and
  `manager+0x118` back to `app`. Rebuilt whenever state 3/4/8 is re-entered (pointers do not survive a game
  start or character creation).
- Manager slots (click handler exe+0xd8f80; captions measured live): `+0x2a0` Create, `+0x2b0` Delete,
  `+0x2f8` Options (icon, no caption), `+0x300` Credits, `+0x308` Exit (icon), `+0x310` DLC, `+0x318` Game
  Guide, `+0x320` null so far, `+0x328` Community, `+0x330` Multiplayer, `+0x338` Start, `+0x340` the
  difficulty button ("Normal Difficulty"), `+0x348` the game-mode button ("Main Campaign").
  Sub-windows: `+0x2b8` CreateCharacterWindow (vtable exe+0x30b048, hidden byte +0x288), `+0x2c0`
  DeleteCharacter (exe+0x30b138), `+0x2d0` Difficulty (exe+0x30b220, hidden +0x350), `+0x2d8` 4th dialog
  (exe+0x30b328, hidden +0x29e), `+0xf8` the open sub-window. A sub-window object is its own tree node (it
  appears as is in the manager's child vector).

Tree node (base class; GetRect = exe+0xa2b70 `lea rax,[rcx+0x18]`; child walk exe+0xa30a0):
- `+0x08` parent, `+0x18..+0x24` float x,y,w,h parent-relative, `+0x38/+0x40` `std::vector<Node*>` children,
  `+0x50` active (gates render and input), `+0x51` enabled (a greyed button has 0 -- measured on Next).
- Mouse dispatch: children last-to-first, first `true` wins, events pass unmodified; hit test = absolute rect
  (parent chain + scroll offsets, exe+0xa2b80).
- Vtable: `+0x20` HandleMouseEvent, `+0x28` HandleKeyEvent, `+0x48` Render, `+0x88` GetRect. No keyboard focus
  exists anywhere in the tree (buttons' key slot is `xor al,al; ret`); gamepad focus is a virtual cursor.

Classes (by vtable):
- **Button A1** exe+0x30c018 (HandleMouseEvent exe+0xa01d0): caption u16 string `+0x268` (localized from the
  record's textTag, exe+0x9f870), listeners `std::vector<Listener*>` `+0x230/+0x238`, hovered `+0x248`, pressed
  `+0x249`, is-toggle `+0x24a`. Left-down: `pressed = toggle ? !pressed : 1`, `L->vt[0](L, button, 0)` for each
  listener. Left-up (non-toggle, pressed): `L->vt[0](L, button, hovered ? 2 : 1)`, clear pressed. **Activation
  = those listener calls** (`WidgetA::activate`). The main menu's Create/Delete are A1 toggles.
- **Button A2** exe+0x30bf40 (exe+0xa22d0): same layout; ALWAYS a toggle (left-down flips pressed, fires the
  listeners once, no release path). Used for radio/check boxes (Male/Female/Hardcore): `pressed` = selected.
- **Static text** exe+0x30c5e8 (render exe+0xb1ba0): u16 string `+0xc0` (size +0xd0), style name `+0x58`,
  wrap width `+0xe0` (-1 none), alignment `+0xe4`, colour `+0x54`. `^n` etc. are the game's inline codes.
- **Edit box** exe+0x30d588 (render exe+0xee730, keys exe+0xedc80): u16 text `+0x238`, caret `+0x21c`. Focus
  comes from a click inside its rect (its mouse handler exe+0xed5a0); no focus byte found (+0x218 stays 0).
- Image exe+0x30b4e0 (no text). Popup layer exe+0x30bd80 at the root holding a popup window exe+0x30d650 with
  text widgets and buttons ("A character with that name already exists." + Ok). The multiplayer screen
  (state 6) is vtable exe+0x30d308 with list/scroll classes exe+0x30c6a0, 0x30ce40, 0x30cf48, 0x30c278.

Measured windows (1600x900 positions are the widgets' own, not needed by the mod):
- Create Character: image, TEXT 'Create Character', TEXT 'Name', EDIT, A2 Male/Female/Hardcore, A1 Next
  (enabled only with a name), A1 Cancel.
- Difficulty Select: TEXT title, TEXT selected name, A1 Normal/Veteran/Elite/Ultimate tiles (pressed = selected,
  enabled = unlocked), TEXT description + stat lines (inactive ones belong to other tiles), A1 Create/Back
  (or Accept/Cancel from the main menu's difficulty button).

## Framework B -- InGameUI's embedded windows (the world)

- `InGameUI = [[main_obj+0x90]+0x2f0]` (exe+0x200dc). `InGameUI::Init` = exe+0x213840 (`mov r14, rcx`):
  every window is a by-value member. Offsets (from the hud_mastertable field fetches): prompt box `+0x7378`,
  character `+0x52258`, quest `+0x285a0`, skills `+0x3fc20`, minimap `+0x42260`, **exit `+0x4a300`**, party
  `+0x4b540`, factions `+0x6c9b8`, achievements `+0x7d150`, devotion `+0x813a0`, stack `+0x83ed8`, potions
  `+0x8a300`, quest reward `+0x8efd8`, objective `+0x90390`, loot filter `+0xab410`, trade `+0x29cc8`, market
  `+0x2b538`, enchanter `+0x30dd8`, transmuter `+0x85378`, altar `+0x87628`. (`+0xb138/+0xb158` are record-path
  strings, not the NPC dialog window -- the conversation UI is unresolved, around exe+0x16d000..0x171000.)
- Every window: vtable `+0xb0` Show(bool), `+0xb8` IsVisible() (InGameUI::CloseAllWindows exe+0x219910,
  IsAnyWindowOpen exe+0x219db0). The prompt box reports visible=1 permanently; use DialogManager for it.
- Input priority (exe+0x212590): a pending Dialog -> prompt box first; then `[+0x4def0]`, exit window, stack
  window, devotion window; then every window in order.
- Controls are by-value members at per-window offsets; no child list. Base control (ctor exe+0x123390):
  `+0x28` visible, `+0x260..+0x26c` float rect, `+0x20` parent. **Button** exe+0x313e78 (0x388 bytes, bitmap
  states only). **TextButton** exe+0x313ce8 (0x3b0 bytes, ctor exe+0x126fe0): caption u16 `+0x358` (localized
  textTag), `+0x281` disabled, `+0x282` pressed, `+0x283` rollover. **Text element** exe+0x31c7c0: u16 text
  `+0x40` (render exe+0x25b700 has the element in rdi at its RenderText2d call).
- Press notifications go through a listener registry (`std::map<Control*, vector<Listener*>>` at
  registry+0x30; `exe+0x12a800(registry, control, listener)` registers). Registry vtable `+0x80` =
  `PressChild(control, playSound)`: refuses unregistered or disabled (+0x281) controls, then dispatches.
  InGameUI's HUD host `+0x7338` (exe+0x12b680) toggles (one event); **a window's own registry** (exe+0x12ac80)
  fires the full 0,1,2 sequence = one complete click. The game's key bindings press HUD buttons this way
  (exe+0x211bb0..).
- **Exit window** (ctor exe+0x26e060, vtable exe+0x31d148, 0x1108 bytes): TextButtons `+0x150` Return to
  Game, `+0xc60` Options Menu, `+0x500` Exit to Main Menu, `+0x8b0` Quit to Desktop; title text element
  `+0x1010`; registry `+0x108`; visible byte `+0x68`. Its listener (exe+0x26e750) acts on event 2: Resume =
  Show(false); the two exits post DialogManager Yes/No prompts with InterestedParty 8 (main menu) / 9 (desktop).

### Options (app state 5; screen vtable exe+0x30cad0, ctor exe+0xc8e60)
- Tree: screen -> panel (exe+0x30d650: TEXT 'Options', Apply, Close, the active page exe+0x30cbb8 with
  'Default' and the controls) + the seven tab toggles (A1, empty captions, rollover tags at `+0x78`:
  tagGameplayTitle / tagHUDTitle / tagHealthBarsTitle / tagKeybindingTitle01 / tagAudioTitle / tagVideoTitle /
  tagNetworkTitle; `hooks::localize` resolves them through `LocalizationManager::LocalizeWithoutParams` on the
  instance captured by the GetText hook). Page switch = the tab's listeners (exe+0xcd300(screen, index);
  index at `screen+0x280`). Page children come in declaration order: a check box carries its caption and its
  `…Desc##` tooltip tag; a TEXT label immediately precedes the slider / drop-down it names.
- **Slider** exe+0x30d4c0 (ctor exe+0xeac40): float max `+0x31c`, min `+0x320`, value `+0x324` (options
  sliders are 0..1), listeners `+0x300/+0x308` called `L->vt[0](L, slider)`; a drag writes the value then fires
  them -- `WidgetA::set_slider` does the same. No text, no keys.
- **Drop-down** exe+0x30c278: items vector `+0xd0/+0xd8` (0x30 bytes each, u16 text first), selected `+0xec`,
  highlighted `+0x108`, open flag `+0x130`, listeners `+0xb8/+0xc0`; commit (exe+0xab951) = write `+0xec`,
  fire listeners -- `WidgetA::set_combo`.
- **Key-binding table** exe+0x30c530 (ctor exe+0xaf8f0, `screen+0x260`): rows `vector<vector<u16string>*>`
  at `+0x260/+0x268`, cell 0 = action name, cells 1/2 = the bound key names (filled by exe+0xafe20 from
  `InputDevice->vt[0x50]`); selected row/col `+0x19c/+0x1a0`. Rebinding (listener exe+0xcc510 sets
  `screen+0x250` action, `+0x254` slot, `+0x258` waiting) is read-only in the mod so far.
- Apply = exe+0xcd780(screen, false): diffs the screen's private `GAME::Options` (`screen+0x378`) against
  `Engine::GetOptions()`, saves, reloads. Defaults is per page (`Options::SetToDefaults(opts, group)`).
- **From the pause menu** (2026-08-25): the exit window's Options Menu button calls exe+0x21bbc0(InGameUI), which
  shows the host window at `[InGameUI+0x4def8]` (0x98 bytes, ctor exe+0x29efc0, vtable exe+0x31dd90, visible
  byte `+0x68`); the host's create (exe+0x29f2d0) allocates 0x508 bytes and runs the SAME Options ctor
  exe+0xc8e60, keeping the screen at host `+0x90`. App state stays 10 and the screen is NOT in the menu tree
  root, so `exe_ui::options_screen` reads it from there when `ingame_ui()` is present. The screen's "closed"
  flag is `+0x228` (vtable `+0xd0` returns it); the host polls it, deletes the screen and hides itself. The
  host's Escape (exe+0x29f1c0) sets `+0x268` when the private Options differ from the live ones (the discard
  prompt) else `+0x228`.
- **Confirmation popups** (verified 2026-09-22): the Yes/No layer used by Download Cloud Saves, Delete Cloud
  Saves, and the Options discard question has vtable exe+0x30bc88 and is a child of the Options screen. Its
  child is the same framework-A popup window shape as the older root-level exe+0x30bd80 modal layer;
  `popup()` checks both locations and accepts both layer types. The Network page briefly creates an empty
  layer while probing UPnP, so a modal must also contain nonempty text and a captioned button.
- Delete Character window (vtable exe+0x30b138): TEXT 'DELETE', TEXT prompt, EDIT, A1 Accept (enabled once the
  box reads DELETE), A1 Cancel; no hidden byte known -- open = `manager+0xf8 == window`.

## Tutorial tips -- the tip manager at `[main_obj+0xbe0]` (ctor exe+0x1087f0, a DisplayWidget)
No `UITutorial` object exists; tutorial tips and notifications are entries of one manager. `+0x08` is a
`std::deque<Tip*>` (MSVC: map +0x10, mapsize +0x18, offset +0x20, size +0x28; 2 pointers per block). A Tip
(ctor exe+0x109070, 0xda8 bytes, no vtable): `+0x00` `vector<u16string>` of the localized, line-split text
(line 0 = title), `+0x18` state (0 fade in, 1 shown 10 s, 2 fade out, 3 dismissed), `+0x1c` timer ms,
`+0x20..+0x2c` rect, `+0xd98` kind (1 = tutorial tip), `+0xda0` help page id (-1 = not clickable). The right
click (exe+0x1099f0, type 2) sets timer 0x1f4 and state 3 -- `Tip::dismiss` does the same; `exe+0x109ba0(mgr)`
dismisses all. Left click opens the Journal's Tutorials tab (`GameEngine::ShowTutorialPage(uint)` is exported).
Tips are gated on `Options::GetBool(0x1a)`.

- 2026-08-22 additions (details in `docs/ingame-ui-survey.md`): the C/I inventory-character window is
  `+0xbbf0` (`+0x52258` is the multiplayer Inspect twin); more windows: faction vendor `+0x2e188`, caravan
  `+0x4fd08`, shrine `+0x7da50` (ruined) / `+0x7f6f8` (desecrated, its own class), crafting `+0x3aa80`
  (mapped: `docs/re_crafting_exe.md`), item ascension `+0x8baa8`. Vtable `+0xf0` is not
  universally OnControlEvent (the stack-split window has setters there). The game's key-binding actions are
  `InGameUI::HandleKeyAction(ui, action, bool, bool, bool)` = exe+0x211980 (signature-checked; `ingame_key_action`).

## Conversation window (allocated on demand; pointer at `InGameUI+0x8efd0`; ctor exe+0x16e9a0, vtable exe+0x3157a8)
Not an InGameUI window: visible byte `+0x28` (not Show/IsVisible), fade state `+0x1ab8` (3 = closed), rect
`+0x40` (absolute; measured 543,626 548x146 at 1600x900). `+0xa0` Conversation*, `+0xa8` NPC id, `+0xac`
player id, `+0x2a0` speaker text element and `+0x378` current-page element (class exe+0x31b830 -- its u16
string sits at `+0x38`), `+0x1ac0` the full NPC speech, `+0x1a48` current step, `+0x1a50` requested step,
`+0x1a88` available response steps. Rows: `vector<Row*>` at `+0x1a60/+0x1a68`; Row (ctor exe+0x16d9b0,
vtable exe+0x315710): `+0x48` the `ConversationStep*` it selects (null = end), `+0x1c8` its u16 text,
`+0x38..+0x44` rect relative to the window. The row set is one "Continue" row (paginated speech), one row per
available response, or one "End conversation" row. The click handler runs the step's quest actions
(`ConversationStep::GetActions` -> OnActivate/Execute/OnDeactivate) and then writes `+0x1a50`; `Update`
(exe+0x172450) advances on the difference. `ConvWindow::choose` therefore clicks at the row's own rectangle
(window rect + row rect) so the game's path, actions included, runs. Escape goes through the game's key.

## Screen identity summary (what `is_active` reads)
main menu: app state 3/4/8 + no sub-window + no popup. Create/Delete Character, Difficulty: the manager's
sub-window slot (+ hidden byte). Options: app state 5 + the screen in the tree. Message box: DialogManager
(world) or the popup layer (menus). Loading: app state 10. In game: InGameUI present. Pause: exit window
IsVisible. Conversation: the window's visible byte + fade state. Tip: a kind-1 tip with state < 2.

## Message boxes -- GAME::DialogManager (exported, Game.dll)

`GameEngine::GetDialogManager(engine)`; `GetNumDialog() > 0` = a box is up; `PeekTopDialog()` -> `Dialog`:
`+0x00` u16 message, `+0x20/+0x40` char strings, `+0x60` InterestedParty, `+0x64` DialogType (0 Okay, 1
Yes/No), size 0x6b. The prompt box (PromptBox::Update exe+0x1903b0, OnWidgetClicked exe+0x190d30) answers with
`YesNoResponse{int party; bool yes}` -> `AddResponse` then `RemoveTopDialog`; Okay boxes are just removed (the
Escape path exe+0x1913e0 does the same). `exe_ui::answer_dialog` is exactly that. Verified live on "Are you
sure you want to exit to the main menu?" (type 1, party 8). Main-menu boxes are framework A popups instead.

## Dev routes
`/ui` (app state, manager slots, popup, full tree dump with class-decoded text/edit/button lines), `/ui/activate?ptr=`
(a framework A button through its listeners), `/ingame` (InGameUI windows with IsVisible, the prompt box, the
exit window's buttons), `/dialog[?answer=yes|no|okay]`, `/tips[?dismiss=1]`, `/convwin`, `/loc?tag=`,
`/peek?ptr=&n=` (hex dump with exe pointers annotated -- how the conversation window's rect and text offsets
were settled live).

## Riftgate travel (static RE 2026-08-22, verified live: list, close, travel)
The riftgate travel UI is the world map: `WorldMapWindow` = MiniMap (`InGameUI+0x42260`) `+0x7940`, record
`records/ui/riftgatemap/riftgate_mastertable.dbr`, vtable exe+0x31db78 (HandleMouseEvent exe+0x294500). MiniMap
`+0x68` = shown, `+0x418` = mode byte (1 = the local map from M, 0 = the riftgate map); the exe's own predicate
"riftgate map open" is exe+0x21be20 (IsVisible && +0x68 && +0x418 == 0; signature-checked). Openers: using a
`StaticTeleporter` (`RequestToUse` -> `EventManager::Send` 0x1c -> listener exe+0x21ce84: `InGameUI+0x49da0` =
the gate's object id (= worldmap+0x200), mode 0, Show(true)); the L key (action 0x21 -> exe+0x211ea3 -> listener
exe+0x1cde76, a toggle that leaves +0x200 alone; did nothing for a character without the personal riftgate).
The AERIAL map (mode 1) is a sub-object at MiniMap+0xba0 (its update = exe+0x174e80): nugget vector at +0x210
(= MiniMap+0xdb0), the icon frustum's camera at +0x638, zoom current/target floats at +0xacc/+0xad0 (= MiniMap+0x166c/
+0x1670, verified live 2026-09-11; the wheel handler exe+0x174c20 clamps 40..135 and saves options.txt mapZoom; view
height = zoom * 3 units). docs/map-icons.md has the nugget layout and the type table.
Close: the close button / Escape = MiniMap Show(false) (vtable +0xb0). Sections: `worldmap+0x118` std::list
(node+0x30 = section; section +0x08/+0x10 = vector<Icon*>). Icon (ctor exe+0x28ed20): +0x00 state (1 = current),
+0x128 int[3] world position, +0x134 owner player id (personal gates), +0x138 the gate's object id, +0x140
u16string name, +0x160 UniqueId. **The map pre-builds an icon for every gate of the world (27); undiscovered ones
sit at (0,0,0) with a zero UniqueId.** Travel = what the click does: `GameEngine::SetLastUsedTeleportId(uid)` then
exe+0x291520(unused, const int pos[3]) (the distance guard + `InitiatePlayerTeleport(x, y, z, effect 1, true)`
with integer world coordinates); the game closes the map itself. `exe_ui::riftgate_map_open/riftgates/
riftgate_travel/riftgate_map_close`; dev `/riftgates`. Discovery is the player's per-difficulty teleport UID list
(`Player::GetTeleportUIDs`, `AddTeleportUID` on binding).

## Character picker (static RE 2026-08-22, verified live)
The main menu's character list is a `CharacterPicker` at `main_menu+0x370` (class size 0x610, vtable exe+0x30af80,
HandleMouseEvent exe+0x8a040, signature-checked), created from the "characterPicker" widget record but never pushed
into the manager's child vector: the menu renders it and dispatches mouse to it by hand, so the tree walk never
sees it. `+0xc0` selected index (-1 none), `+0xc4` first visible, `+0xc8/+0xd0` vector<Entry> stride 0x90,
`+0x118` rows per page, `+0xf8/+0x100` the up/down arrow buttons (children of the picker; they move the
selection). Entry: `+0x00` save path, `+0x20` u16 name, `+0x40` display name, `+0x60` class tag (std::string,
empty = no mastery), `+0x80` level, `+0x84` hardcore, `+0x85` female, `+0x88` the preview Player's object id.
The list comes from `CharacterList` (`app+0xa18`; Build exe+0x5e90 walks `Engine::GetSavePath()` for `_Name`
folders and reads each `player.gdc` header). A left-up on a row writes `+0xc0` and nothing else; the next
`MainMenu::Update` (exe+0xd95a0) commits it: swaps the preview entity, `GameInfo::SetPlayerInfo`, and stores
`main_menu+0x378` = the preview id (what Start at exe+0xda700 and Delete read). So selecting = write `+0xc0`
(and keep `+0xc4` in range): `MainMenu::characters/selected_character/select_character`.
