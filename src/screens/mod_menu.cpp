#include "screens/mod_menu.h"
#include <string>
#include <vector>
#include "core/graph_builder.h"
#include "core/screen.h"
#include "core/strings.h"
#include "screens/announcements.h"
#include "screens/cue_settings.h"
#include "screens/mod_options.h"
#include "screens/sound_glossary.h"
#include "screens/window_base.h"
#include "world.h"
#include "travel.h"

namespace gd::screens {
using namespace gd::core;
namespace {
bool g_open = false;

class ModMenuScreen : public Screen {
 public:
  std::string_view key() const override { return "mod_menu"; }
  bool is_active() override { return g_open; }
  std::string screen_name() const override { return std::string(strings::kModMenu); }
  int layer() const override { return 31; }   // over anything (main menu included); the glossary replaces it
  bool exclusive() const override { return true; }
  std::vector<InputCategory> input_categories() const override { return {InputCategory::UI}; }
  std::vector<ScreenAction> actions() override { return {{std::string(action_ids::Back), [] { g_open = false; }}}; }
  void on_pop() override { g_open = false; }

  void build(GraphBuilder& b) override {
    b.begin_stop("page");
    b.add_item(ControlId::structural("mod.glossary"), row_item(std::string(strings::kSoundGlossary), {}, [] { g_open = false; open_sound_glossary(); }));
    if (world::in_world()) {   // the two in-world config overlays (T and Ctrl+T)
      b.add_item(ControlId::structural("mod.travel"), row_item("Travel", {}, [] { g_open = false; travel::open_menu(); }));
      b.add_item(ControlId::structural("mod.announcements"), row_item(std::string(strings::kAnnouncements), {}, [] { g_open = false; open_announcements(); }));
      b.add_item(ControlId::structural("mod.cues"), row_item(std::string(strings::kCueSettings), {}, [] { g_open = false; open_cue_settings(); }));
    }
    b.add_item(ControlId::structural("mod.options"), row_item(std::string(strings::kModOptions), {}, [] { g_open = false; open_mod_options(); }));
  }
};
}  // namespace

void open_mod_menu() { g_open = true; }
std::unique_ptr<Screen> make_mod_menu() { return std::make_unique<ModMenuScreen>(); }
}  // namespace gd::screens
