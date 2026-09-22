#include "screens/achievements.h"
#include <algorithm>
#include <unordered_set>
#include "screens/window_base.h"
#include "textcap.h"

namespace gd::screens {
using namespace gd::core;

class AchievementsScreen : public WindowScreen {
 public:
  AchievementsScreen() : WindowScreen("achievements", "Achievements", exe_ui::ingame::kAchievements, 13) {}

  static bool compact_number(std::string_view text) {
    // HUD health and energy use compact forms such as "3236/3316".  The
    // achievement rail uses spaced forms ("0 / 17"), so this cannot hide it.
    bool slash = false;
    if (text.empty()) return false;
    for (char c : text) {
      if (c == '/') { if (slash) return false; slash = true; }
      else if (c < '0' || c > '9') return false;
    }
    return slash;
  }

  void build(GraphBuilder& b) override {
    // The window's category rail begins around x=619 and its achievement text
    // begins around x=901.  Ignore HUD text, then preserve the game's visual
    // reading order.  This is deliberately read-only.
    struct Category { std::string label; int x, y; };
    std::vector<Category> category_labels;
    std::vector<std::string> category_counts;
    std::vector<std::string> entries;
    std::unordered_set<std::string> seen;
    for (const textcap::Item& it : textcap::snapshot()) {
      std::string text = textcap::speakable(it.text);
      if (text.empty() || compact_number(text) || it.y < 240) continue;
      if (it.x >= 560 && it.x < 800 && it.y <= 800) {
        // The rail alternates category label and its "completed / total" line.
        if (text.find(" / ") != std::string::npos) category_counts.push_back(std::move(text));
        else category_labels.push_back({std::move(text), it.x, it.y});
      } else if (it.x >= 820 && it.x < 1500) {
        // The quest tracker lives beyond the achievements pane at the far right.
        if (seen.insert(text).second) entries.push_back(std::move(text));
      }
    }
    std::vector<std::string> categories;
    for (size_t i = 0; i < category_labels.size(); ++i) {
      std::string label = category_labels[i].label;
      if (i < category_counts.size()) label += ", " + category_counts[i];
      categories.push_back(std::move(label));
    }
    b.begin_stop("categories");
    for (size_t i = 0; i < categories.size(); ++i) {
      // Native selection is mouse-only.  Enter therefore clicks the exact
      // live text position; the game changes the achievement list itself.
      int x = category_labels[i].x, y = category_labels[i].y;
      b.add_item(ControlId::structural(std::format("achievements.category{}", i)), row_item(categories[i], {}, [x, y] { hooks::click((float)x, (float)y); }));
    }
    if (categories.empty()) b.add_item(ControlId::structural("achievements.categories.none"), line_item("No achievement categories are visible."));
    b.begin_stop("entries");
    for (size_t i = 0; i < entries.size(); ++i)
      b.add_item(ControlId::structural(std::format("achievements.entry{}", i)), line_item(entries[i]));
    if (entries.empty()) b.add_item(ControlId::structural("achievements.entries.none"), line_item("No achievements are visible."));
  }
};

std::unique_ptr<Screen> make_achievements() { return std::make_unique<AchievementsScreen>(); }
}  // namespace gd::screens
