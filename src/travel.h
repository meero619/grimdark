#pragma once
#include <string>
#include "world.h"
namespace gd::travel {
void start(world::TravelTarget target);
void reviewed();
void followed();
void stop(const std::string& reason = "Travel stopped");
void tick();
void open_menu();
void open_map();
void return_to_town();
std::string status();
std::string preview(); // read-only route plan for the reviewed target; never starts movement
bool active();
bool background_test(bool on); // dev only; requires GRIMDARK_NOFOCUS=1 at launch
}
