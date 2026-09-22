#pragma once
#include <memory>
#include "core/screen.h"

namespace gd::screens {
// The native Achievements window is drawn entirely through 2D text.  Its game
// objects have no useful public data accessors, so this reader presents the
// live labels the game actually draws without attempting to change progress.
std::unique_ptr<gd::core::Screen> make_achievements();
}  // namespace gd::screens
