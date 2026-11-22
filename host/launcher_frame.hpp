#pragma once

// Local change (the remaster): the launcher's borderless window shows only the menu panel; everything around it is see-through.
// This turns the panel's picture (straight RGBA, see-through fill) into the picture of the whole window for a Windows layered window:
// premultiplied BGRA (0xAARRGGBB), clear (alpha 0) outside the panel, the panel's see-through fill made solid over a dark backdrop so
// it reads well over any desktop, and the anti-aliased outer edge kept soft.

#include "menu_draw.hpp"

#include <cstdint>
#include <vector>

void compose_layered(const menudraw::Picture &panel, int window_width, int window_height, std::vector<std::uint32_t> &out);
