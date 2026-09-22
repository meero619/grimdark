#pragma once
#include <memory>
#include "core/screen.h"
namespace gd::screens {
std::unique_ptr<gd::core::Screen> make_illusionist();
}
