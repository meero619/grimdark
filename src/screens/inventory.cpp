#include "screens/inventory.h"
#include <cstdlib>
#include <algorithm>
#include <format>
#include <set>
#include <optional>
#include "app.h"
#include "core/navigator.h"
#include "gameapi.h"
#include "screens/list_picker.h"
#include "screens/skills.h"
#include "screens/window_base.h"

namespace gd::screens {
using namespace gd::core;
namespace { int g_bag_source = 0; }
void set_bag_item_source(int s) { g_bag_source = s; }
int bag_item_source() { return g_bag_source; }

// The two weapon hands (EquipmentCtrlLocation): 9 = Right Hand (main), 10 = Left Hand (off-hand).
constexpr int kLocRightHand = 9, kLocLeftHand = 10;

class InventoryScreen : public WindowScreen, public AssignSource {
 public:
  InventoryScreen() : WindowScreen("inventory", std::string(strings::kInventory), exe_ui::ingame::kInventory, 11) {}

  // The equipment tab's weapon is an assign source: with a hand slot focused, the assign keys (Ctrl+J / Ctrl+I
  // -> left / right mouse, Ctrl+1..0 -> a slot) put the character's DEFAULT basic attack there. This is the
  // "get back to a basic attack" path -- the id comes from gameapi::default_skill_id (the game's own
  // GetDefaultSkillId), so it is always the right instance for the equipped weapon.
  unsigned focused_skill_id() override {
    if (tab() != 0) return 0;
    GraphNavigator* nav = app::navigator();
    std::optional<ControlId> id = nav ? nav->focused_id() : std::nullopt;
    if (!id || !id->structural_key().is_string()) return 0;
    const std::string& k = id->structural_key().text();
    if (k.rfind("inventory.eq", 0) != 0) return 0;
    int loc = std::atoi(k.c_str() + 12);
    if (loc != kLocRightHand && loc != kLocLeftHand) return 0;
    return gameapi::default_skill_id(0);
  }
  std::string focused_label() override { return focused_skill_id() ? std::string(strings::kBasicAttack) : std::string(); }

  void on_tab_changed(int) override { invalidate(); }   // browsing the tabs must NOT move the game's selected bag (below)
  // Ctrl+Enter on a bag tab: make it the game's SELECTED bag -- the one pickups fall into once bag 1 has no room
  // (PlayerInventoryCtrl::AddItem, read 2026-08-26: stacks merge in any bag, then bag 1, then the selected bag,
  // nothing else). A sighted player does this by leaving the window on that bag's tab; we make it explicit.
  void set_receiving_bag() {
    // The tab under FOCUS if the cursor is on the tab row (arrowing along it does not select), else the selected tab.
    int t = tab(), nbags = (int)bags_.value.size();
    GraphNavigator* nav = app::navigator();
    std::optional<ControlId> fid = nav ? nav->focused_id() : std::nullopt;
    if (fid && fid->structural_key().is_string()) {
      const std::string& k = fid->structural_key().text();
      if (k.rfind("inventory.tab", 0) == 0) t = std::atoi(k.c_str() + 13);
    }
    if (t < 1 || t > nbags) { speech::speak(strings::kNotABag, true); return; }
    bool ok = gameapi::select_bag(t - 1);
    MessageBuilder m; m.fragment(std::format("{} {}", strings::kBag, t)).list_item().fragment(ok ? strings::kSecondaryBag : strings::kCannot);
    speech::speak(m.build(), true);
    invalidate();
  }
  void on_focus() override { invalidate(); WindowScreen::on_focus(); }
  std::vector<ScreenAction> actions() override {
    auto result = WindowScreen::actions();
    result.push_back({"game.drop", [this] { confirm_drop(); }});
    return result;
  }
  void on_update() override {
    if (!drop_) return;
    if (!bag_item(drop_->id)) {
      MessageBuilder m; m.fragment(strings::kDropDone).fragment(drop_->label);
      speech::speak(m.build(), true);
      drop_.reset(); invalidate();
    } else if (hooks::frame() - drop_->frame >= 180) {
      speech::speak(strings::kDropFailed, true);
      drop_.reset(); invalidate();
    }
  }
  void on_pop() override { drop_.reset(); }

  void build(GraphBuilder& b) override {
    const std::vector<gameapi::Bag>& bags = bags_.get([] { return gameapi::bags(); }, 30);
    std::vector<std::string> labels{std::string(strings::kEquipment)};
    for (const gameapi::Bag& bag : bags) labels.push_back(bag.name.empty() ? std::format("{} {}", strings::kBag, bag.index + 1) : bag.name);   // labels stay stable: the tab is re-found by label
    labels.push_back(std::string(strings::kStats));
    // The tab strip says which bag is the secondary one (the game's selected bag: pickups overflow there once bag 1
    // is full; bag 1 itself is always tried first). Ctrl+Enter on a bag tab moves it (set_receiving_bag).
    std::vector<std::string> values(labels.size());
    int secondary = gameapi::selected_bag();
    if (bags.size() > 1 && secondary > 0 && secondary < (int)bags.size()) values[(size_t)secondary + 1] = std::string(strings::kSecondaryBag);
    add_tabs(b, labels, values);
    int t = tab();
    if (t == 0) build_equipment(b);
    else if (t >= 1 && t <= (int)bags.size()) build_bag(b, bags[(size_t)t - 1]);
    else build_sheet(b);
  }

 private:
  struct PendingDrop { unsigned id; std::string label; uint64_t frame; };
  std::optional<PendingDrop> drop_;
  static std::optional<gameapi::BagItem> bag_item(unsigned id) {
    for (const auto& bag : gameapi::bags())
      for (const auto& item : bag.items) if (item.id == id) return item;
    return {};
  }
  void confirm_drop() {
    if (drop_) { speech::speak(strings::kDropPending, true); return; }
    GraphNavigator* nav = app::navigator();
    auto focus = nav ? nav->focused_id() : std::optional<ControlId>{};
    unsigned id = 0;
    if (focus) for (const auto& bag : gameapi::bags()) for (const auto& item : bag.items)
      if (*focus == ControlId::structural(std::format("inventory.item{}", item.id))) id = item.id;
    auto item = bag_item(id);
    if (!item) { speech::speak(strings::kDropChoose, true); return; }
    const auto name = item->name;
    const auto count = item->stack;
    const auto record = gameapi::object_record(item->p);
    MessageBuilder label;
    strings::push_stack(label, name.empty() ? std::format("item {}", id) : name, count);
    if (item->component) label.fragment(strings::kWithComponent);
    const auto spoken = label.build();
    MessageBuilder title;
    title.fragment(strings::kDropConfirm).fragment(spoken);
    if (count > 1) title.list_item().fragment(strings::kDropWholeStack);
    open_picker(title.build(), {{0, std::string(strings::kDropCancel), {}}, {1, std::string(strings::kDropYes), {}}},
      [this, id, name, count, record, spoken](unsigned choice) {
        if (!choice) return;
        auto current = bag_item(id);
        if (!window().visible() || drop_ || !current || current->name != name || current->stack != count ||
            gameapi::object_record(current->p) != record) {
          speech::speak(strings::kDropChanged, true); return;
        }
        if (!gameapi::drop_item(id)) { speech::speak(strings::kDropFailed, true); return; }
        drop_ = PendingDrop{id, spoken, hooks::frame()};
        invalidate();
      });
  }
  void invalidate() { bags_.invalidate(); equipment_.invalidate(); sheet_.invalidate(); }
  // Activating a component in a bag opens the attach picker: every item (across bags + equipped) it fits,
  // from the game's own Player::GetCompatibleItems. Picking one attaches + consumes the component (no
  // blacksmith needed). Space reads the target item's tooltip.
  void open_component_picker(unsigned comp_id) {
    std::string comp_name = gameapi::item_name(gameapi::object_by_id(comp_id));
    std::vector<PickerItem> items;
    // The game's list spans bags, equipped items and the stash; say "equipped" on the ones you are wearing.
    std::set<unsigned> worn;
    for (const gameapi::EquipSlot& sl : gameapi::equipment()) if (sl.item_id) worn.insert(sl.item_id);
    for (unsigned tid : gameapi::compatible_items(comp_id)) {
      std::string name = gameapi::item_name(gameapi::object_by_id(tid));
      items.push_back({tid, name.empty() ? std::format("item {}", tid) : name, worn.count(tid) ? std::string(strings::kEquipped) : std::string()});
    }
    if (items.empty()) { speech::speak(strings::kNoCompatibleItems, true); return; }
    // Equipped targets first (what you most likely want to improve), the rest in the game's order.
    std::stable_partition(items.begin(), items.end(), [&](const PickerItem& it) { return worn.count(it.id) != 0; });
    MessageBuilder title; title.fragment(strings::kAttach).fragment(comp_name.empty() ? std::string(strings::kComponent) : comp_name);
    open_picker(title.build(), std::move(items),
                [this, comp_id](unsigned tid) {
                  if (!gameapi::attach_component(comp_id, tid, g_bag_source)) speech::speak(strings::kCannot, true);
                  invalidate();
                },
                [](unsigned tid, bool detail) { item_tip(tid, detail)(); });
  }
  void build_equipment(GraphBuilder& b) {
    const std::vector<gameapi::EquipSlot>& eq = equipment_.get([] { return gameapi::equipment(); }, 30);
    for (const gameapi::EquipSlot& s : eq) {
      std::string label = s.label.empty() ? std::format("slot {}", s.loc) : s.label;
      std::string name = s.name.empty() ? std::string(strings::kEmptySlot) : s.name;
      if (s.component) { MessageBuilder cm; cm.fragment(name).fragment(strings::kWithComponent); name = cm.build(); }   // "Splintered Club with component": part of the name
      if (s.inactive) { MessageBuilder im; im.fragment(name).list_item().fragment(strings::kInactive); name = im.build(); }   // "Devil's Grin, inactive": equipped but detached by the game (requirements no longer met)
      unsigned id = s.item_id; int loc = s.loc;
      // Enter opens the equip picker: everything across all bags that fits this slot (weapons/off-hands go to
      // the ACTIVE weapon set, since equip() uses the current EquipmentCtrl). Its first entry, "empty",
      // unequips. Backspace stays a direct unequip shortcut.
      auto open_equip = [this, loc, label] {
        std::vector<PickerItem> items;
        items.push_back({0, std::string(strings::kEmptySlot), {}});
        for (const gameapi::Bag& bag : gameapi::bags())
          for (const gameapi::BagItem& it : bag.items)
            if (gameapi::can_equip(it.id, loc))
              items.push_back({it.id, it.name.empty() ? std::format("item {}", it.id) : it.name, it.stack > 1 ? std::format("x{}", it.stack) : std::string()});
        open_picker(label, std::move(items), [this, loc](unsigned pid) {
          bool ok = pid ? gameapi::equip(pid, loc) : gameapi::unequip(loc);
          if (!ok) speech::speak(strings::kCannot, true);
          invalidate();
        }, [](unsigned pid, bool detail) { item_tip(pid, detail)(); });   // Space / Ctrl+Space = the item's tooltip
      };
      auto unequip = [this, id, loc] { if (!id) { speech::speak(strings::kEmptySlot, true); return; } if (!gameapi::unequip(loc)) speech::speak(strings::kCannot, true); invalidate(); };
      b.add_item(ControlId::structural(std::format("inventory.eq{}", s.loc)),
                 row_item(label, [name] { return name; }, open_equip, id ? item_tip(id, false) : std::function<void()>{}, unequip, id ? item_tip(id, true) : std::function<void()>{}));
    }
    MessageBuilder m; strings::push_stat(m, strings::kIronBits, std::format("{}", gameapi::money()));
    b.add_item(ControlId::structural("inventory.money"), line_item(m.build()));
  }
  void build_bag(GraphBuilder& b, const gameapi::Bag& bag) {
    if (bag.items.empty()) { b.add_item(ControlId::structural(std::format("inventory.bag{}.empty", bag.index)), line_item(std::string(strings::kEmpty))); return; }
    for (const gameapi::BagItem& it : bag.items) {
      std::string nm = it.name.empty() ? std::format("item {}", it.id) : it.name;
      if (it.component) { MessageBuilder cm; cm.fragment(nm).fragment(strings::kWithComponent); nm = cm.build(); }   // the grid tile's badge, as part of the name: "Gladius with component"
      MessageBuilder m; strings::push_stack(m, nm, it.stack);
      unsigned id = it.id;
      auto activate = [this, id] {
        if (gameapi::is_component(id)) { open_component_picker(id); return; }   // components attach; they aren't "used"
        void* p = gameapi::object_by_id(id);
        if (p && !gameapi::item_requirements_met(p)) {   // "requirements not met, Physique 391 of 392"
          MessageBuilder rm; rm.fragment(strings::kRequirementsNotMet);
          for (const gameapi::Shortfall& sf : gameapi::requirement_shortfalls(p)) strings::push_shortfall(rm, sf.label, sf.have, sf.need);
          speech::speak(rm.build(), true);
          return;
        }
        if (p && !gameapi::is_equipment(p) && !gameapi::is_usable(p)) { speech::speak(strings::kNotUsable, true); return; }   // crafting materials / quest items: UseItem would remove them
        gameapi::use_item(id, g_bag_source);
        invalidate();
      };
      auto v = row_item(m.build(), {}, activate, item_tip(id, false), {}, item_tip(id, true));
      v->on_compare = item_compare(id);   // Backslash: the equipped item in the slot this one fits
      b.add_item(ControlId::structural(std::format("inventory.item{}", it.id)), v);
    }
  }
  void build_sheet(GraphBuilder& b) {
    const std::vector<gameapi::Stat>& rows = sheet_.get([] { return gameapi::character_sheet(); }, 30);
    int i = 0;
    for (const gameapi::Stat& s : rows) {
      MessageBuilder m; strings::push_stat(m, s.label, s.value);
      std::string id = std::format("inventory.stat{}", i++);
      std::string desc = s.desc;   // the game's stat description, read on Space
      std::function<void()> tip;
      if (!desc.empty()) tip = [desc] { speech::speak(desc, true); };
      if (s.spend) {   // Enter spends an attribute point (the sheet's "+" button). Attributes are NEVER refundable
                       // in Grim Dawn (no attribute reclaim, even at a spirit guide), so no on_secondary is wired
                       // here -- intentional; Backspace does nothing on a stat row.
        int which = s.spend;
        auto spend = [this, which] {
          if (gameapi::attribute_points() == 0) { speech::speak(strings::kNoPoints, true); return; }
          speech::speak(gameapi::spend_attribute_point(which) ? std::string(strings::kPointSpent) : std::string(strings::kCannot), true);
          invalidate();
        };
        b.add_item(ControlId::structural(id), row_item(m.build(), {}, spend, tip));
      } else if (tip) {
        b.add_item(ControlId::structural(id), row_item(m.build(), {}, {}, tip));   // read-only stat with a Space tooltip
      } else {
        b.add_item(ControlId::structural(id), line_item(m.build()));
      }
    }
    if (rows.empty()) b.add_item(ControlId::structural("inventory.nostats"), line_item(std::string(strings::kEmpty)));
  }
  Snapshot<std::vector<gameapi::Bag>> bags_;
  Snapshot<std::vector<gameapi::EquipSlot>> equipment_;
  Snapshot<std::vector<gameapi::Stat>> sheet_;
};

void set_receiving_bag_focused() {
  InventoryScreen* s = dynamic_cast<InventoryScreen*>(app::screens().current());
  if (s) s->set_receiving_bag();   // a Windows-category chord (Ctrl+Enter): silent in the other windows
}
std::unique_ptr<Screen> make_inventory() { return std::make_unique<InventoryScreen>(); }
}  // namespace gd::screens
