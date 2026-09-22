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
    std::vector<std::string> categories;
    std::vector<std::string> entries;
    std::unordered_set<std::string> seen;
    for (const textcap::Item& it : textcap::snapshot()) {
      std::string text = textcap::speakable(it.text);
      if (text.empty() || it.x < 560 || it.y < 240 || !seen.insert(text).second) continue;
      if (it.x < 820) categories.push_back(std::move(text));
      else entries.push_back(std::move(text));
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
