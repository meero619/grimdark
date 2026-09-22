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

  void build(GraphBuilder& b) override {
    // The window's category rail begins around x=619 and its achievement text
    // begins around x=901.  Ignore HUD text, then preserve the game's visual
    // reading order.  This is deliberately read-only.
    std::vector<std::string> category_labels;
    std::vector<std::string> category_counts;
    std::vector<std::string> entries;
    std::unordered_set<std::string> seen;
    for (const textcap::Item& it : textcap::snapshot()) {
      std::string text = textcap::speakable(it.text);
      if (text.empty() || it.y < 240 || it.y > 800 || !seen.insert(text).second) continue;
      if (it.x >= 560 && it.x < 800) {
        // The rail alternates category label and its "completed / total" line.
        if (text.find(" / ") != std::string::npos) category_counts.push_back(std::move(text));
        else category_labels.push_back(std::move(text));
      } else if (it.x >= 820 && it.x < 1500) {
        // The quest tracker lives beyond the achievements pane at the far right.
        entries.push_back(std::move(text));
      }
    }
    std::vector<std::string> categories;
    for (size_t i = 0; i < category_labels.size(); ++i) {
      std::string label = category_labels[i];
      if (i < category_counts.size()) label += ", " + category_counts[i];
      categories.push_back(std::move(label));
    }
    b.begin_stop("categories");
    for (size_t i = 0; i < categories.size(); ++i)
      b.add_item(ControlId::structural(std::format("achievements.category{}", i)), line_item(categories[i]));
    if (categories.empty()) b.add_item(ControlId::structural("achievements.categories.none"), line_item("No achievement categories are visible."));
    b.begin_stop("entries");
    for (size_t i = 0; i < entries.size(); ++i)
      b.add_item(ControlId::structural(std::format("achievements.entry{}", i)), line_item(entries[i]));
    if (entries.empty()) b.add_item(ControlId::structural("achievements.entries.none"), line_item("No achievements are visible."));
  }
};

std::unique_ptr<Screen> make_achievements() { return std::make_unique<AchievementsScreen>(); }
}  // namespace gd::screens
