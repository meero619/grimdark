// The Inventor's window (the exe's "enchanter" window, InGameUI+0x30dd8; docs/inventor.md, RE in
// docs/re_inventor_exe.md), opened by the game when the player talks to an Inventor. Item-first, not chamber-first:
// the game's panel has ONE chamber the sighted player drags an item into and three buttons; here each tab lists
// the bag items the chamber would accept, with the price the game will charge, and Enter on an item does the whole
// sequence -- into the chamber (the exe's drop: box SetItem + PlayerInventoryCtrl::RemoveItem), the button through
// the panel's own registry (the game's confirm box follows, answered by the message_box screen), and afterwards
// either the game's result (spoken) or, on No, the item back into the bag (the exe's own return call).
//   Salvage: items carrying a component or an augment; Enter -> a picker of the actions that apply (Keep Item /
//     Keep Add-on for a component, Remove Augment for an augment; Space on a row = the game's warning text).
//   Dismantle: items above common quality; Enter dismantles (Dynamite + iron bits); the two results the game
//     drops into the panel's output boxes are taken into the bag and listed in the reward notice (the quest
//     reward shape, screens/reward_list.h): rolled on the press and landing under a loud effect, a spoken line
//     would be lost.
// Convert / Reroll are expansion tabs; a base-game install never builds them (the buttons stay null), so they are
// listed only when present. A tab the Inventor "has not yet learned" (Dismantle before its quest token) is
// greyed by the game and reads its info text instead of items.
#include "screens/inventor.h"
#include <format>
#include <optional>
#include "gameapi.h"
#include "screens/list_picker.h"
#include "screens/reward_list.h"
#include "screens/window_base.h"
#include "textcap.h"

namespace gd::screens {
using namespace gd::core;

class InventorScreen : public WindowScreen {
 public:
  InventorScreen() : WindowScreen("inventor", std::string(strings::kInventor), exe_ui::ingame::kEnchanter, 14) {}
  void on_tab_changed(int index) override {
    if (index >= 0 && index < (int)exe_tabs_.size()) exe_ui::inventor_press_tab(exe_tabs_[(size_t)index]);
    invalidate();
  }
  void on_focus() override { invalidate(); WindowScreen::on_focus(); }
  void on_pop() override { pending_.reset(); }
  // The action in flight: wait for the game's confirm box to be answered (its pending byte), then read what
  // happened -- chamber empty = done, item still there = the player said No (back into the bag).
  void on_update() override {
    if (!pending_ || press_if_due()) return;
    Pending& p = *pending_;
    uint64_t age = hooks::frame() - p.started;
    if (p.action == exe_ui::InventorAction::Dismantle) {
      exe_ui::InventorDismantle d = exe_ui::inventor_dismantle();
      if (d.dialog_pending || exe_ui::dialog_open()) { p.saw_dialog = true; return; }
      if (d.item == p.item) { if (still_in_chamber_wait(p, age)) return; cancel(); return; }
      // The item is gone: results arrive in the output boxes a few frames later (the command round trip).
      if (!d.result1 && !d.result2 && age < 120) return;
      // The scrap count and the bonus component were rolled on the press, and the game plays a loud effect over
      // the moment they land -- so they are a dialog the player closes, not a line (the quest reward shape).
      std::vector<std::string> got;
      for (int which : {1, 2}) {
        unsigned id = which == 1 ? d.result1 : d.result2;
        if (!id) continue;
        std::string name = gameapi::item_name(gameapi::object_by_id(id));
        if (exe_ui::inventor_take(which) && !name.empty()) got.push_back(name);
      }
      pending_.reset();
      gameapi::invalidate_objects();
      invalidate();
      open_reward_notice(std::string(strings::kDismantled), std::move(got));
      return;
    }
    exe_ui::InventorSalvage s = exe_ui::inventor_salvage();
    if (s.dialog_pending || exe_ui::dialog_open()) { p.saw_dialog = true; return; }
    if (s.item == p.item) { if (still_in_chamber_wait(p, age)) return; cancel(); return; }
    // Done: the game puts what you kept BACK INTO THE CHAMBER (GiveRecoveredItemToEnchanterWindow -> the box), the
    // component after Keep Add-on, the item itself otherwise -- a sighted player drags it out. Wait for it (the
    // command's round trip clears the box first), name it from the object, take it into the bag.
    if (!s.item && age < 120) return;
    MessageBuilder m;
    m.fragment(std::string(p.action == exe_ui::InventorAction::RemoveAugment ? strings::kAugmentRemoved : strings::kSalvaged));
    std::string name = s.item ? gameapi::item_name(gameapi::object_by_id(s.item)) : p.result_name;
    if (s.item) exe_ui::inventor_take(0);
    if (!name.empty()) m.list_item().fragment(name);
    finish(m.build());
  }
  void build(GraphBuilder& b) override {
    std::vector<exe_ui::InventorTab> tabs = exe_ui::inventor_tabs();
    std::vector<std::string> labels, values;
    exe_tabs_.clear();
    for (int i = 0; i < (int)tabs.size(); ++i) {
      if (!tabs[(size_t)i].present) continue;
      exe_tabs_.push_back(i);
      labels.push_back(game_text(tabs[(size_t)i].label.c_str(), tab_label(i)));
      values.push_back(tabs[(size_t)i].enabled ? std::string() : std::string(strings::kNotLearned));
    }
    // The game's tab is the truth (a real click on a tab changes it too).
    int game_tab = exe_ui::inventor_tab();
    for (size_t i = 0; i < exe_tabs_.size(); ++i)
      if (exe_tabs_[i] == game_tab && ((int)i != tab_ || tab_key_.empty())) { tab_ = (int)i; tab_key_ = labels[i]; }
    if (labels.empty()) { b.add_item(ControlId::structural("inventor.none"), line_item(std::string(strings::kEmpty))); return; }   // add_tabs cannot take an empty row
    add_tabs(b, labels, values);
    b.set_region("inventor.top");
    std::string npc = exe_ui::inventor_npc_name();
    if (!npc.empty()) b.add_item(ControlId::structural("inventor.npc"), line_item(npc));
    if (tab_ < 0 || tab_ >= (int)exe_tabs_.size()) return;
    int exe_tab = exe_tabs_[(size_t)tab_];
    const exe_ui::InventorTab& t = tabs[(size_t)exe_tab];
    if (!t.enabled || exe_tab > 1) {   // not learned, or an expansion tab we do not model: the game's own blurb
      b.add_item(ControlId::structural("inventor.info"), line_item(game_text(t.info_tag, std::string(strings::kNotLearned))));
      return;
    }
    b.set_region("inventor.items");
    unsigned money = gameapi::money();
    if (exe_tab == 1) {
      MessageBuilder m;
      m.list_item().fragment(std::to_string(gameapi::dynamite_count())).fragment(std::string(strings::kDynamite));
      m.list_item().fragment(std::to_string(money)).fragment(std::string(strings::kIronBits));
      b.add_item(ControlId::structural("inventor.have"), line_item(m.build()));
    }
    const std::vector<Row>& rows = rows_.get([exe_tab] { return load_rows(exe_tab); }, 30);
    for (const Row& r : rows) {
      unsigned id = r.id;
      std::string value = r.value;
      if (r.cost > money) { MessageBuilder v; v.fragment(value).list_item().fragment(std::string(strings::kTooExpensive)); value = v.build(); }
      if (exe_tab == 1 && gameapi::dynamite_count() == 0) { MessageBuilder v; v.fragment(value).list_item().fragment(std::string(strings::kNoDynamite)); value = v.build(); }
      std::function<void()> act = exe_tab == 0 ? std::function<void()>([this, id] { pick_action(id); }) : std::function<void()>([this, id] { start(id, exe_ui::InventorAction::Dismantle); });
      b.add_item(ControlId::structural(std::format("inventor.{}.{}", exe_tab, id)), row_item(r.label, [value] { return value; }, act, item_tip(id, false), {}, item_tip(id, true)));
    }
    if (rows.empty()) b.add_item(ControlId::structural("inventor.none"), line_item(std::string(strings::kNothingFits)));
  }

 private:
  struct Row { unsigned id; std::string label, value; unsigned cost; };
  struct Pending {
    unsigned item; exe_ui::InventorAction action; std::string result_name; uint64_t started;
    bool saw_dialog = false;       // the game's confirm box was seen up
    uint64_t dialog_closed = 0;    // the first frame it was seen gone again (0 = not yet)
  };
  // How long the chamber may keep the item after the confirm box closed before that means "the player said No".
  // The exe processes the box's response on its NEXT update (Do* then clears the box and sends the command), so
  // on the frame the box closes the panel still holds the item whatever the answer was; a Yes answered through
  // our message box is known exactly (exe_ui::last_dialog_answer) and waits for the command instead.
  static constexpr uint64_t kSettleFrames = 20, kCommandFrames = 180;
  // The item is still in the chamber after the confirm box closed: No (cancel now), or Yes with the command still
  // in flight (wait). Returns true when the caller should keep waiting.
  bool still_in_chamber_wait(Pending& p, uint64_t age) {
    if (age < 3) return true;   // the click itself needs a frame before the flag shows
    if (!p.dialog_closed) p.dialog_closed = hooks::frame();
    exe_ui::DialogAnswer a = exe_ui::last_dialog_answer();
    bool said_yes = a.frame >= p.started && a.type == 1 && a.yes;   // (the box may open and close between two of our updates)
    uint64_t since = hooks::frame() - p.dialog_closed;
    if (said_yes) return since < kCommandFrames;   // the game refused after all (bits) if it never took the item
    return since < kSettleFrames;                  // answered in the game's own UI, or no box at all: let it settle
  }
  static std::string game_text(const char* tag, std::string fallback) {
    std::string t = tag ? textcap::speakable(hooks::localize(tag)) : std::string();
    return t.empty() || t.starts_with("Tag not found:") ? fallback : t;
  }
  static std::string tab_label(int tab) {
    switch (tab) {
      case 0: return "Salvage";
      case 1: return "Dismantle";
      case 2: return "Convert";
      case 3: return "Reroll";
      default: return std::format("tab {}", tab + 1);
    }
  }
  void invalidate() { rows_.invalidate(); }
  // The chamber's accept test, per tab (the exe's box filters exe+0x1af8c0 / exe+0x1afe90 + the drop's quality gate):
  // salvage = equipment with a component or an augment; dismantle = equipment above common.
  static std::vector<Row> load_rows(int exe_tab) {
    std::vector<Row> out;
    for (const gameapi::Bag& bag : gameapi::bags()) {
      for (const gameapi::BagItem& it : bag.items) {
        if (!gameapi::is_equipment(it.p)) continue;
        bool comp = gameapi::has_component(it.p), aug = gameapi::has_augment(it.p);
        Row r{it.id, it.name, {}, 0};
        if (exe_tab == 0) {
          if (!comp && !aug) continue;
          MessageBuilder l;
          l.fragment(it.name);
          if (comp) l.list_item().fragment(std::string(strings::kWithComponent));
          if (aug) l.list_item().fragment(std::string(strings::kWithAugment));
          r.label = l.build();
          r.cost = gameapi::salvage_cost(it.p);
          MessageBuilder v;
          v.fragment(std::string(strings::kSalvageCost)).fragment(std::to_string(r.cost)).fragment(std::string(strings::kIronBits));
          r.value = v.build();
        } else {
          if (gameapi::item_classification(it.p) <= 0) continue;
          if (comp) { MessageBuilder l; l.fragment(it.name).list_item().fragment(std::string(strings::kWithComponent)); r.label = l.build(); }
          r.cost = gameapi::dismantle_cost(it.p);
          MessageBuilder v;
          v.fragment(std::string(strings::kDismantleCost)).fragment(std::to_string(r.cost)).fragment(std::string(strings::kIronBits));
          r.value = v.build();
        }
        out.push_back(r);
      }
    }
    return out;
  }
  // Salvage: which of the three buttons apply to this item. The picker's rows are the game's own button captions;
  // Space on one reads the game's warning for it.
  void pick_action(unsigned id) {
    void* item = gameapi::object_by_id(id);
    if (!item) { speech::speak(strings::kCannot, true); return; }
    exe_ui::InventorSalvage s = exe_ui::inventor_salvage();
    std::vector<PickerItem> items;
    std::vector<const char*> warnings(4, nullptr);
    auto add = [&](unsigned code, const exe_ui::InventorButton& btn, std::string_view fallback) {
      items.push_back({code, btn.caption.empty() ? std::string(fallback) : btn.caption, {}});
      warnings[code] = btn.warning_tag;
    };
    if (gameapi::has_component(item)) { add(1, s.keep_item, "Keep Item"); add(2, s.keep_addon, "Keep Add-on"); }
    if (gameapi::has_augment(item)) add(3, s.remove_augment, "Remove Augment");
    if (items.empty()) { speech::speak(strings::kCannot, true); return; }
    open_picker(gameapi::item_name(item), items,
                [this, id](unsigned code) {
                  if (code == 1) start(id, exe_ui::InventorAction::KeepItem);
                  else if (code == 2) start(id, exe_ui::InventorAction::KeepAddon);
                  else if (code == 3) start(id, exe_ui::InventorAction::RemoveAugment);
                },
                [warnings](unsigned code, bool) {
                  const char* tag = code < warnings.size() ? warnings[code] : nullptr;
                  std::string t = tag ? textcap::speakable(hooks::localize(tag)) : std::string();
                  speech::speak(t.empty() ? std::string(strings::kNoTooltip) : t, true);
                });
  }
  // Into the chamber, then the button. The confirm box (or, for a plain dismantle, the result) follows on the
  // game's next updates; on_update finishes the job.
  void start(unsigned id, exe_ui::InventorAction action) {
    if (pending_) { speech::speak(strings::kCannot, true); return; }
    void* item = gameapi::object_by_id(id);
    if (!item) { speech::speak(strings::kCannot, true); return; }
    int want = action == exe_ui::InventorAction::Dismantle ? 1 : 0;
    if (exe_ui::inventor_tab() != want && !exe_ui::inventor_press_tab(want)) { speech::speak(strings::kCannot, true); return; }
    Pending p{id, action, action == exe_ui::InventorAction::KeepAddon ? gameapi::component_name(item) : gameapi::item_name(item), hooks::frame()};
    if (!exe_ui::inventor_put(id)) { speech::speak(strings::kCannot, true); return; }
    // The button's enabled byte is the game's gate (affordable, dynamite, the item's kind), refreshed by its Update
    // -- one frame after the put. Press on the next update instead of now.
    pending_ = p;
    press_due_ = hooks::frame() + 2;
    invalidate();
  }
  void cancel() {
    exe_ui::inventor_take(0);
    pending_.reset();
    speech::speak(strings::kCancelled, true);
    gameapi::invalidate_objects();
    invalidate();
  }
  void finish(std::string text) {
    pending_.reset();
    speech::speak(text, true);
    gameapi::invalidate_objects();
    invalidate();
  }
  // Called from on_update before anything else: the deferred button press.
  bool press_if_due() {
    if (!pending_ || !press_due_ || hooks::frame() < press_due_) return false;
    press_due_ = 0;
    if (!exe_ui::inventor_press(pending_->action)) {   // greyed: the game says no (not enough bits / dynamite, wrong kind)
      exe_ui::inventor_take(0);
      pending_.reset();
      gameapi::invalidate_objects();
      invalidate();
      speech::speak(strings::kCannot, true);
      return true;
    }
    pending_->started = hooks::frame();
    return true;
  }
  Snapshot<std::vector<Row>> rows_;
  std::vector<int> exe_tabs_;   // screen tab index -> the exe's tab index
  std::optional<Pending> pending_;
  uint64_t press_due_ = 0;
};

std::unique_ptr<Screen> make_inventor() { return std::make_unique<InventorScreen>(); }
}  // namespace gd::screens
