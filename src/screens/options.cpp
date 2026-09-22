#include "screens/options.h"
#include <cmath>
#include <format>
#include "core/graph_builder.h"
#include "core/message_builder.h"
#include "core/strings.h"
#include "exe_ui.h"
#include "hooks.h"
#include "screens/controls.h"
#include "speech.h"
#include "textcap.h"

namespace gd::screens {
using namespace gd::core;
using exe_ui::OptionsScreen;
using exe_ui::WidgetA;

static const ControlType kRowType{"text", {"value"}, [] { return std::vector<NodeAnnouncement>{}; }};
constexpr float kSliderStep = 0.05f, kSliderLargeStep = 0.2f;

// Options (measured 2026-08-22; docs/exe-ui-layout.md "Options"): seven icon tabs named by their rollover
// tags, a page whose children come in declaration order -- check boxes carry their caption, a slider or
// drop-down is preceded by the TEXT that labels it, the Keybinding page holds a table -- and Apply / Default /
// Close. Values are the widgets' own; changes go through the same field writes + listener calls a mouse does.
class OptionsMenuScreen : public Screen {
 public:
  std::string_view key() const override { return "options"; }
  bool is_active() override { return exe_ui::available() && (bool)exe_ui::options_screen(); }
  std::string screen_name() const override { return std::string(strings::kOptionsScreen); }
  // Above the in-game screen (0): from the pause menu the options live over the world, not in a menu state.
  int layer() const override { return 20; }
  std::vector<ScreenAction> actions() override {
    return {{std::string(action_ids::Back), [] { OptionsScreen o = exe_ui::options_screen(); WidgetA c = close_button(o); if (c) c.activate(); }}};
  }

  void build(GraphBuilder& b) override {
    OptionsScreen o = exe_ui::options_screen();
    if (!o) return;
    b.begin_stop("tabs");
    b.start_row("tabs");
    for (size_t i = 0; i < o.tabs.size(); ++i) {
      WidgetA t = o.tabs[i];
      std::string label = hooks::localize(t.tooltip_tag().c_str());
      bool selected = t.pressed();
      auto v = std::make_shared<NodeVtable>();
      v->control_type = &kTabType;
      v->announcements = {NodeAnnouncement([label] { return label; }, false, announcement_kinds::kLabel),
                          NodeAnnouncement([selected] { return selected ? std::string(strings::kSelected) : std::string(); }, true, announcement_kinds::kSelected)};
      v->on_activate = [t, selected] { if (!selected) t.activate(); };
      b.add_item(ControlId::structural(std::format("options.tab{}", i)), v);
    }
    b.end_row();
    b.begin_stop("page");
    std::vector<WidgetA> kids = o.page.children();
    std::string pending_label;  // the TEXT that labels the next value control
    int n = 0;
    auto next_id = [&] { return std::format("options.page{}.{}", o.tab_index(), n++); };
    auto flush_pending_text = [&] {
      if (pending_label.empty()) return;
      add_text(b, next_id(), pending_label);
      pending_label.clear();
    };
    for (WidgetA c : kids) {
      if (!c.active()) continue;
      if (c.is_text()) {
        std::string text = textcap::speakable(c.text());
        if (!text.empty()) {
          if (!pending_label.empty()) pending_label += " ";
          pending_label += text;
        }
        continue;
      }
      if (c.is_button() && c.is_toggle()) { flush_pending_text(); add_checkbox(b, next_id(), c); }
      else if (c.is_slider()) { add_slider(b, next_id(), pending_label, c); pending_label.clear(); }
      else if (c.is_combo()) { add_combo(b, next_id(), pending_label, c); pending_label.clear(); }
      else if (c.is_edit()) { add_edit(b, next_id(), pending_label, c); pending_label.clear(); }
      else if (c.is_button()) {
        flush_pending_text();
        // Some icon-only controls (the disabled Steam Controller configuration button, for example) have
        // no caption but do have a localized rollover description. Give NVDA that text as the label.
        std::string label = c.caption();
        if (label.empty()) label = textcap::speakable(hooks::localize(c.tooltip_tag().c_str()));
        b.add_item(ControlId::structural(next_id()), widget_button(c, label));
      }
      else if (c.is_list()) { flush_pending_text(); add_list(b, next_id(), c); }
      else flush_pending_text();
    }
    flush_pending_text();
    b.begin_stop("buttons");
    b.start_row("buttons");
    int k = 0;
    for (WidgetA btn : ordered_buttons(o)) b.add_item(ControlId::structural(std::format("options.button{}", k++)), widget_button(btn));
    b.end_row();
  }

 private:
  // Tree order is Apply, Close (panel) then Default (page); present Apply, Default, Close.
  static std::vector<WidgetA> ordered_buttons(const OptionsScreen& o) {
    if (o.buttons.size() == 3) return {o.buttons[0], o.buttons[2], o.buttons[1]};
    return o.buttons;
  }
  static WidgetA close_button(const OptionsScreen& o) { return o.buttons.size() >= 2 ? o.buttons[1] : WidgetA{}; }

  static void add_checkbox(GraphBuilder& b, const std::string& id, WidgetA c) {
    std::string label = c.caption(), tag = c.tooltip_tag();
    bool on = c.pressed();
    auto v = std::make_shared<NodeVtable>();
    v->control_type = &kToggleType;
    v->announcements = {NodeAnnouncement([label] { return label; }, false, announcement_kinds::kLabel),
                        NodeAnnouncement([on] { return std::string(on ? strings::kOn : strings::kOff); }, true, announcement_kinds::kValue)};
    v->on_activate = [c] { c.activate(); };
    v->state_text = [c] { return std::string(c.pressed() ? strings::kOn : strings::kOff); };
    v->on_tooltip = [tag] { std::string t = textcap::speakable(hooks::localize(tag.c_str())); speech::speak(t.empty() ? std::string(strings::kNoTooltip) : t, true); };
    b.add_item(ControlId::structural(id), v);
  }
  static std::string percent(float v01) { return std::format("{} {}", (int)std::lround(v01 * 100), strings::kPercent); }
  static void add_slider(GraphBuilder& b, const std::string& id, std::string label, WidgetA c) {
    float value = c.slider_value();
    auto v = std::make_shared<NodeVtable>();
    v->control_type = &kSliderType;
    v->announcements = {NodeAnnouncement([label] { return label; }, false, announcement_kinds::kLabel),
                        NodeAnnouncement([value] { return percent(value); }, true, announcement_kinds::kValue)};
    v->on_adjust = [c](int sign, bool large) { c.set_slider(c.slider_value() + sign * (large ? kSliderLargeStep : kSliderStep)); };
    v->state_text = [c] { return percent(c.slider_value()); };
    b.add_item(ControlId::structural(id), v);
  }
  static void add_combo(GraphBuilder& b, const std::string& id, std::string label, WidgetA c) {
    std::vector<std::string> items = c.combo_items();
    int idx = c.combo_index();
    std::string value = idx >= 0 && idx < (int)items.size() ? textcap::speakable(items[(size_t)idx]) : std::string();
    auto v = std::make_shared<NodeVtable>();
    v->control_type = &kComboType;
    v->announcements = {NodeAnnouncement([label] { return label; }, false, announcement_kinds::kLabel),
                        NodeAnnouncement([value] { return value; }, true, announcement_kinds::kValue)};
    v->on_adjust = [c](int sign, bool) {
      int n = (int)c.combo_items().size(), i = c.combo_index() + sign;
      if (n == 0) return;
      if (i < 0) i = 0; if (i >= n) i = n - 1;
      if (i != c.combo_index()) c.set_combo(i);
    };
    v->state_text = [c] {
      std::vector<std::string> items = c.combo_items();
      int i = c.combo_index();
      MessageBuilder m;
      m.fragment(i >= 0 && i < (int)items.size() ? textcap::speakable(items[(size_t)i]) : std::string(strings::kEmpty));
      gd::strings::push_position(m, i + 1, (int)items.size());
      return m.build();
    };
    b.add_item(ControlId::structural(id), v);
  }
  static void add_text(GraphBuilder& b, const std::string& id, std::string text) {
    auto v = std::make_shared<NodeVtable>();
    v->control_type = &kRowType;
    v->announcements = {NodeAnnouncement([text] { return text; }, false, announcement_kinds::kValue)};
    b.add_item(ControlId::structural(id), v);
  }
  static void add_edit(GraphBuilder& b, const std::string& id, std::string label, WidgetA c) {
    std::string value = textcap::speakable(c.text());
    auto v = std::make_shared<NodeVtable>();
    v->control_type = &kEditType;
    v->announcements = {NodeAnnouncement([label] { return label; }, false, announcement_kinds::kLabel),
                        NodeAnnouncement([value] { return value.empty() ? std::string(strings::kEmpty) : value; }, true, announcement_kinds::kValue)};
    v->state_text = [c] {
      std::string value = textcap::speakable(c.text());
      return value.empty() ? std::string(strings::kEmpty) : value;
    };
    b.add_item(ControlId::structural(id), v);
  }
  // The key-binding table, read-only for now: one row per action, "action, key, key".
  static void add_list(GraphBuilder& b, const std::string& id, WidgetA c) {
    std::vector<std::vector<std::string>> rows = c.list_rows();
    b.push_context(strings::kKeyBindings, strings::kList);
    for (size_t i = 0; i < rows.size(); ++i) {
      MessageBuilder m;
      for (const std::string& cell : rows[i]) { std::string s = textcap::speakable(cell); if (!s.empty()) m.list_item().fragment(s); }
      std::string text = m.empty() ? std::string(strings::kEmpty) : m.build();
      auto v = std::make_shared<NodeVtable>();
      v->control_type = &kRowType;
      v->announcements = {NodeAnnouncement([text] { return text; }, false, announcement_kinds::kValue)};
      b.add_item(ControlId::structural(std::format("{}.row{}", id, i)), v);
    }
    b.pop_context();
  }
};

std::unique_ptr<Screen> make_options() { return std::make_unique<OptionsMenuScreen>(); }
}  // namespace gd::screens
