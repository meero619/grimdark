#include "screens/difficulty_select.h"
#include "core/graph_builder.h"
#include "core/message_builder.h"
#include "core/strings.h"
#include "exe_ui.h"
#include "screens/controls.h"
#include "speech.h"
#include "textcap.h"

namespace gd::screens {
using namespace gd::core;
using exe_ui::MainMenu;
using exe_ui::WidgetA;

// Difficulty Select, read from its window in the exe's menu tree (measured 2026-08-22): the four tiles are
// buttons (pressed = selected, disabled = locked on the account), the description and its stat lines are
// text widgets (the inactive ones belong to other tiles), and the last two buttons are Create/Back (after
// Create Character) or Accept/Cancel (from the main menu's difficulty button).
struct Dialog {
  WidgetA window;
  std::vector<WidgetA> tiles, buttons;
  explicit operator bool() const { return window && tiles.size() == 4 && buttons.size() == 2; }
};
static Dialog dialog() {
  Dialog d;
  MainMenu mm = exe_ui::main_menu();
  void* win = mm ? mm.sub_window(MainMenu::kWinDifficulty) : nullptr;
  if (!win || exe_ui::window_hidden_flag(win, MainMenu::kDifficultyHidden)) return d;
  d.window = exe_ui::window_node(win);
  std::vector<WidgetA> all = d.window.buttons();
  if (all.size() < 6) return d;
  d.tiles.assign(all.begin(), all.begin() + 4);
  d.buttons.assign(all.end() - 2, all.end());
  return d;
}

class DifficultySelectScreen : public Screen {
 public:
  std::string_view key() const override { return "difficulty_select"; }
  bool is_active() override { return exe_ui::available() && (bool)dialog() && !exe_ui::popup(); }
  std::string screen_name() const override { return "Difficulty Select"; }
  int layer() const override { return 20; }
  bool exclusive() const override { return true; }
  std::vector<ScreenAction> actions() override {
    return {{std::string(action_ids::Back), [] { Dialog d = dialog(); if (d) d.buttons[1].activate(); }}};
  }

  void build(GraphBuilder& b) override {
    Dialog d = dialog();
    if (!d) return;
    b.begin_stop("difficulty");
    b.start_row("difficulty");
    for (WidgetA t : d.tiles) {
      std::string name = t.caption();
      bool selected = t.pressed(), locked = !t.enabled();
      auto v = std::make_shared<NodeVtable>();
      v->control_type = &kRadioType;
      v->announcements = {NodeAnnouncement([name] { return name; }, false, announcement_kinds::kLabel),
                          NodeAnnouncement([selected] { return selected ? std::string(strings::kSelected) : std::string(); }, true, announcement_kinds::kSelected),
                          NodeAnnouncement([locked] { return locked ? std::string(strings::kDisabled) : std::string(); }, false, announcement_kinds::kEnabled)};
      v->on_activate = [t, selected, locked] { if (!locked && !selected) t.activate(); };
      // The dialog describes the SELECTED difficulty; that text is the tile's tooltip.
      v->on_tooltip = [selected] {
        if (!selected) { speech::speak(strings::kNoTooltip, false); return; }
        speech::speak(description(), true);
      };
      v->state_text = [t, locked] {  // live: activation changed the tile within this frame
        MessageBuilder m;
        m.fragment(locked ? strings::kDisabled : t.pressed() ? strings::kSelected : strings::kNotSelected);
        return m.build();
      };
      b.add_item(ControlId::structural("difficulty." + name), v);
    }
    b.end_row();
    b.begin_stop("buttons");
    b.start_row("buttons");
    b.add_item(ControlId::structural("difficulty.accept"), widget_button(d.buttons[0]));
    b.add_item(ControlId::structural("difficulty.back"), widget_button(d.buttons[1]));
    b.end_row();
  }

 private:
  // The active text widgets below the title: the selected tile's name, its description and stat lines.
  // Game text verbatim (control codes stripped the same way as captured text).
  static std::string description() {
    Dialog d = dialog();
    MessageBuilder m;
    if (d) {
      std::vector<WidgetA> texts = d.window.texts();
      for (size_t i = 1; i < texts.size(); ++i) {
        if (!texts[i].active()) continue;
        std::string t = textcap::speakable(texts[i].text());
        if (!t.empty()) m.fragment(t);
      }
    }
    if (m.empty()) m.fragment(strings::kNoDetails);
    return m.build();
  }
};

std::unique_ptr<Screen> make_difficulty_select() { return std::make_unique<DifficultySelectScreen>(); }

struct GameModeDialog {
  WidgetA window;
  std::vector<WidgetA> choices, buttons;
  explicit operator bool() const { return window && !choices.empty() && buttons.size() == 2; }
};
static GameModeDialog game_mode_dialog() {
  GameModeDialog d;
  MainMenu mm = exe_ui::main_menu();
  void* win = mm ? mm.sub_window(MainMenu::kWin4th) : nullptr;
  if (!win || exe_ui::window_hidden_flag(win, MainMenu::k4thHidden)) return d;
  d.window = exe_ui::window_node(win);
  std::vector<WidgetA> all = d.window.buttons();
  if (all.size() < 3) return d;
  d.choices.assign(all.begin(), all.end() - 2);
  d.buttons.assign(all.end() - 2, all.end());
  return d;
}

class GameModeSelectScreen : public Screen {
 public:
  std::string_view key() const override { return "game_mode_select"; }
  bool is_active() override { return exe_ui::available() && (bool)game_mode_dialog() && !exe_ui::popup(); }
  std::string screen_name() const override { return "Game Mode Select"; }
  int layer() const override { return 20; }
  bool exclusive() const override { return true; }
  std::vector<ScreenAction> actions() override {
    return {{std::string(action_ids::Back), [] { GameModeDialog d = game_mode_dialog(); if (d) d.buttons[1].activate(); }}};
  }

  void build(GraphBuilder& b) override {
    GameModeDialog d = game_mode_dialog();
    if (!d) return;
    b.begin_stop("game_mode");
    b.start_row("game_mode");
    for (WidgetA t : d.choices) {
      std::string name = t.caption();
      bool selected = t.pressed(), locked = !t.enabled();
      auto v = std::make_shared<NodeVtable>();
      v->control_type = &kRadioType;
      v->announcements = {NodeAnnouncement([name] { return name; }, false, announcement_kinds::kLabel),
                          NodeAnnouncement([selected] { return selected ? std::string(strings::kSelected) : std::string(); }, true, announcement_kinds::kSelected),
                          NodeAnnouncement([locked] { return locked ? std::string(strings::kDisabled) : std::string(); }, false, announcement_kinds::kEnabled)};
      v->on_activate = [t, selected, locked] { if (!locked && !selected) t.activate(); };
      v->on_tooltip = [] { speech::speak(game_mode_description(), true); };
      v->state_text = [t, locked] {
        MessageBuilder m;
        m.fragment(locked ? strings::kDisabled : t.pressed() ? strings::kSelected : strings::kNotSelected);
        return m.build();
      };
      b.add_item(ControlId::structural("game_mode." + name), v);
    }
    b.end_row();
    b.begin_stop("buttons");
    b.start_row("buttons");
    b.add_item(ControlId::structural("game_mode.accept"), widget_button(d.buttons[0]));
    b.add_item(ControlId::structural("game_mode.back"), widget_button(d.buttons[1]));
    b.end_row();
  }

 private:
  static std::string game_mode_description() {
    GameModeDialog d = game_mode_dialog();
    MessageBuilder m;
    if (d) {
      std::vector<WidgetA> texts = d.window.texts();
      for (size_t i = 1; i < texts.size(); ++i) {
        if (!texts[i].active()) continue;
        std::string t = textcap::speakable(texts[i].text());
        if (!t.empty()) m.fragment(t);
      }
    }
    if (m.empty()) m.fragment(strings::kNoDetails);
    return m.build();
  }
};

std::unique_ptr<Screen> make_game_mode_select() { return std::make_unique<GameModeSelectScreen>(); }
}  // namespace gd::screens
