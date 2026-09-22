#include "screens/map_markers.h"
#include <cmath>
#include <format>
#include "screens/shrine_list.h"
#include "screens/window_base.h"
#include "world.h"
#include "travel.h"

namespace gd::screens {
using namespace gd::core;

// Ctrl+M injects the game's M, which opens the local aerial map; this screen becomes current while that map
// is open (exe_ui::aerial_map_open) and reads the map's own icon set (world::map_markers, filled live while
// the map is shown). One flat list, nearest-first, each icon named as the game names it: a point of
// interest by its own text ("Burrwitch Road", "Burial Hill Entrance" -- the quest-bound ones exist only while
// their task is active), a person or merchant by the entity under the icon, a barricade "obstacle", the rest by
// the game's rollover word (Riftgate, Healer, Smith, Spirit Guide ...). The map is an orthographic camera and its
// icons come from that camera's frustum, so while this screen is open the zoom is pushed to the wheel's maximum
// (the sighted reach: 405 x ~650 units) and restored on leaving. Activating a row picks it as the follow target
// and closes the map; the ' key then pings it with distance and heading. A second Tab stop lists every devotion
// shrine the character has discovered anywhere. docs/map-icons.md has the survey.
class MapMarkersScreen : public WindowScreen {
 public:
  MapMarkersScreen() : WindowScreen("mapmarkers", std::string(strings::kMapMarkers), exe_ui::ingame::kMiniMap, 13) {}
  bool is_active() override { return exe_ui::aerial_map_open(); }
  void close() override { exe_ui::aerial_map_close(); }
  void on_focus() override {
    WindowScreen::on_focus();
    if (saved_zoom_ <= 0.0f) {
      saved_zoom_ = exe_ui::aerial_zoom();
      if (saved_zoom_ > 0.0f && exe_ui::aerial_zoom_set(exe_ui::kAerialZoomMax)) markers_.invalidate();
    }
  }
  void on_unfocus() override {
    if (saved_zoom_ > 0.0f) { exe_ui::aerial_zoom_set(saved_zoom_); saved_zoom_ = 0.0f; }
    WindowScreen::on_unfocus();
  }

  void build(GraphBuilder& b) override {
    const std::vector<world::MapMarker>& all = markers_.get([] { return world::map_markers(); }, 15);
    world::Vec3 me{};
    bool have_me = world::player_position(me);
    b.begin_stop("list");
    b.push_context(strings::kMapPoints, strings::kList);   // a stop title, announced on entry
    size_t shown = 0;
    for (size_t i = 0; i < all.size(); ++i) {
      const world::MapMarker& mk = all[i];
      ++shown;
      std::string label = mk.label.empty() ? std::string(strings::kMapPoints) : mk.label;
      std::function<std::string()> value;
      if (have_me) {
        world::Vec3 p = mk.pos;
        float d = std::sqrt((p.x - me.x) * (p.x - me.x) + (p.z - me.z) * (p.z - me.z));
        int hour = world::clock_hour(p);
        value = [d, hour] { MessageBuilder m; strings::push_distance_bearing(m, d, hour); return m.build(); };
      }
      world::MapMarker copy = mk;
      b.add_item(ControlId::structural(std::format("marker.{}", i)),
                 row_item(label, value, [this, copy] {
                   world::set_follow_target(copy.id, copy.pos, copy.label);
                   MessageBuilder m;
                   m.fragment(strings::kFollowing).fragment(copy.label);
                   speech::speak(m.build(), true);
                   close();   // return to the world; ' now follows the picked marker
                 }, [] { speech::speak("Enter sets a direction beacon. Backspace starts automatic walking. Movement keys stop travel.", true); }, [this, copy] {
                   world::set_follow_target(copy.id, copy.pos, copy.label);
                   close();
                   world::TravelTarget t;
                   t.id = world::is_point_id(copy.id) ? 0 : copy.id;
                   t.pos = copy.pos; t.label = copy.label;
                   travel::start(std::move(t));
                 }));
    }
    if (shown == 0)
      b.add_item(ControlId::structural("marker.none"), line_item(std::string(strings::kNoMarkersHere)));
    b.pop_context();

    // The whole world's discovered devotion shrines (the aerial map itself only shows what is nearby). Enter
    // follows one like a marker: the ' key pings it, to chunk precision.
    b.begin_stop("shrines");
    b.push_context(strings::kShrinesStop, strings::kList);
    add_shrine_rows(b, shrines_, me, have_me, [this](const shrine_table::Shrine& s, const std::string& label) {
      world::set_follow_target(0, world::Vec3{(float)s.x, (float)s.y, (float)s.z}, label);
      MessageBuilder m;
      m.fragment(strings::kFollowing).fragment(label);
      speech::speak(m.build(), true);
      close();
    });
    b.pop_context();
  }

 private:
  Snapshot<std::vector<world::MapMarker>> markers_;
  Snapshot<gameapi::ShrineUids> shrines_;
  float saved_zoom_ = 0.0f;   // the player's own mapZoom while we hold the map at the maximum
};

std::unique_ptr<Screen> make_map_markers() { return std::make_unique<MapMarkersScreen>(); }
}  // namespace gd::screens
