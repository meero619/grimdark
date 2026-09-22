#pragma once
// Rooms (docs/rooms.md): the place the player is in, announced in the player's own voice when it changes
// ("Devil's Crossing, the prison, cell block corridor" -- only the parts that changed), a description on X,
// and the current room's exits on V / Shift+V. Everything comes from assets/rooms.db, written by
// tools/rooms.py (segmentation) and the authoring workflow (titles, descriptions, sub-regions); the mod only
// looks up: the player's world position -> the region's label grid -> a room.
#include <string>
#include <vector>
#include "world.h"

namespace gd::rooms {
void init();                 // opens assets/rooms.db next to the DLL (missing = the feature stays silent)
void shutdown();             // closes the db; before db::shutdown()
void tick();                 // per frame from the in-game screen
void reset();                // forget the announced place (re-entering the world re-announces)
void speak_description();    // X
// T: append where the player stands (area name, chunk, position, region/room state) to untagged_rooms.txt at the
// repo root (next to the build tree), so the next areas to author can be read off the file. Speaks what it noted.
void note_place();
// The current room's exits as review items (ScanGroup::Exits; registered with world::set_exit_provider by
// init): V / Shift+V cycle them through the scanner like every other group (ping on landing, ; re-pings).
void announce_now();         // dev: repeat the current place
void reload();               // dev: drop the cached regions and reopen the db (after a tools rewrite)
void set_dwell_ms(int ms);        // wait before a change within the settle window counts (boundary flapping)
void set_settle_ms(int ms);       // time in a room after which any change is announced immediately
void set_say_untitled(bool on);   // untitled rooms are announced as "room N" (default on until authored)
std::string status();        // dev: /room
// Only the caller's active quest text is matched against authored place names. The route uses room
// adjacency for long distances; each resulting short leg must still pass the game's live pathfinder.
std::vector<std::string> places_in_text(const std::string& text);
std::vector<world::Vec3> travel_route(const std::string& area);
}  // namespace gd::rooms
