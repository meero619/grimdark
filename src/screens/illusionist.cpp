#include "screens/illusionist.h"
#include "screens/window_base.h"
#include "screens/list_picker.h"

namespace gd::screens {
using namespace gd::core;
class IllusionistScreen : public WindowScreen {
 public:
  IllusionistScreen() : WindowScreen("illusionist", std::string(strings::kIllusionist), exe_ui::ingame::kTransmuter, 14) {}
  void on_focus() override { snapshot_.invalidate(); WindowScreen::on_focus(); }
  void build(GraphBuilder& b) override {
    const auto& s = snapshot_.get([] { return exe_ui::illusionist_state(); }, 15);
    auto equipped = gameapi::equipment();
    b.begin_stop("equipment");
    for (unsigned id : s.equipment_ids) {
      MessageBuilder label;
      for (const auto& slot : equipped) if (slot.item_id == id) { label.fragment(slot.label); break; }
      label.list_item().fragment(gameapi::item_name(gameapi::object_by_id(id)));
      bool selected = s.equipment == id;
      b.add_item(ControlId::structural(std::format("illusionist.equipment{}", id)), row_item(label.build(),
        [selected] { return selected ? std::string(strings::kSelected) : std::string(); },
        [this, id] { select(id, false); }, item_tip(id, false), {}, item_tip(id, true)));
    }
    if (s.equipment_ids.empty()) b.add_item(ControlId::structural("illusionist.emptyEquipment"), line_item(std::string(strings::kIllusionEmptySlots)));
    b.begin_stop("appearances");
    for (unsigned id : s.appearance_ids) {
      bool selected = s.appearance == id;
      b.add_item(ControlId::structural(std::format("illusionist.appearance{}.{}", s.equipment, id)), row_item(gameapi::item_name(gameapi::object_by_id(id)),
        [selected] { return selected ? std::string(strings::kSelected) : std::string(); },
        [this, id] { select(id, true); }, item_tip(id, false), {}, item_tip(id, true)));
    }
    if (s.appearance_ids.empty()) b.add_item(ControlId::structural("illusionist.emptyAppearances"), line_item(std::string(strings::kIllusionEmptyLooks)));
    b.begin_stop("apply");
    MessageBuilder cost, money;
    cost.fragment(strings::kIllusionCost).fragment(s.cost);
    money.fragment(strings::kIllusionMoney).fragment(s.money);
    b.add_item(ControlId::structural("illusionist.cost"), line_item(cost.build()));
    b.add_item(ControlId::structural("illusionist.money"), line_item(money.build()));
    bool enabled = s.can_apply;
    b.add_item(ControlId::structural("illusionist.apply"), row_item(std::string(strings::kIllusionApply),
      [enabled] { return enabled ? std::string() : std::string(strings::kDisabled); }, [this] { confirm(); }));
    b.add_item(ControlId::structural("illusionist.help"), line_item(std::string(strings::kIllusionHint)));
  }
 private:
  Snapshot<exe_ui::IllusionistState> snapshot_;
  void select(unsigned id, bool appearance) {
    bool ok = exe_ui::illusionist_select(id, appearance);
    snapshot_.invalidate();
    if (!ok) { speech::speak(strings::kIllusionFailed, true); return; }
    MessageBuilder m;
    m.fragment(appearance ? strings::kIllusionPreview : strings::kSelected).fragment(gameapi::item_name(gameapi::object_by_id(id)));
    speech::speak(m.build(), true);
  }
  void confirm() {
    auto expected = exe_ui::illusionist_state();
    if (!expected.can_apply) { speech::speak(strings::kDisabled, true); return; }
    MessageBuilder title;
    title.fragment(strings::kIllusionConfirm).list_item().fragment(strings::kIllusionCost).fragment(expected.cost);
    for (const auto& [item, look] : expected.pending) {
      title.list_item().fragment(gameapi::item_name(gameapi::object_by_id(item)))
        .fragment(gameapi::item_name(gameapi::object_by_id(look)));
    }
    open_picker(title.build(), {{0, std::string(strings::kIllusionCancel), {}}, {1, std::string(strings::kIllusionApply), {}}},
      [this, expected](unsigned id) {
        if (id) speech::speak(exe_ui::illusionist_apply(expected) ? strings::kIllusionRequested : strings::kIllusionFailed, true);
        snapshot_.invalidate();
      });
  }
};
std::unique_ptr<Screen> make_illusionist() { return std::make_unique<IllusionistScreen>(); }
}
