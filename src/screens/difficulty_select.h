#pragma once
#include <memory>
#include "core/screen.h"

namespace gd::screens {
// The "Difficulty Select" dialog that follows Create Character (Normal / Veteran / Elite / Ultimate tiles,
// Create / Back). Layer 20, modal. The chosen difficulty is our state (the game opens on Normal); locked
// tiles are read from the game at the checkpoint (it greys their labels out).
std::unique_ptr<gd::core::Screen> make_difficulty_select();
std::unique_ptr<gd::core::Screen> make_game_mode_select();
}  // namespace gd::screens
