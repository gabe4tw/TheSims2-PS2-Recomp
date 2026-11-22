#pragma once

// Local change: smooth shapes for the quick menu (host/quickmenu.cpp). The menu is a picture made on the CPU and laid over the game's
// picture (runtime/src/gs/gpu/overlay.hpp); these draw into it with soft (anti-aliased) edges: every pixel gets the share of it that the
// shape covers, worked out from its distance to the shape's edge.

#include <cstdint>
#include <vector>

namespace menudraw {

// Straight (not premultiplied) RGBA, one std::uint32_t per pixel with red in the low byte, placed at x, y in window pixels.
struct Picture {
    std::vector<std::uint32_t> rgba;
    int width = 0, height = 0, x = 0, y = 0;
};

constexpr std::uint32_t rgba(int r, int g, int b, int a) {
    return static_cast<std::uint32_t>(r) | static_cast<std::uint32_t>(g) << 8 | static_cast<std::uint32_t>(b) << 16 | static_cast<std::uint32_t>(a) << 24;
}
// 0xRRGGBB as written in colour pickers
constexpr std::uint32_t hex(std::uint32_t rrggbb, int a = 255) {
    return rgba(static_cast<int>(rrggbb >> 16 & 255), static_cast<int>(rrggbb >> 8 & 255), static_cast<int>(rrggbb & 255), a);
}

class Canvas {
public:
    explicit Canvas(Picture &p) : p_(p) {}
    int width() const { return p_.width; }
    int height() const { return p_.height; }
    void clear(std::uint32_t c);
    void fill_rect(float x, float y, float w, float h, std::uint32_t c);
    void fill_round_rect(float x, float y, float w, float h, float r, std::uint32_t c);
    void stroke_round_rect(float x, float y, float w, float h, float r, float width, std::uint32_t c);
    void fill_circle(float cx, float cy, float r, std::uint32_t c);
    void stroke_circle(float cx, float cy, float r, float width, std::uint32_t c);
    void fill_triangle(float x0, float y0, float x1, float y1, float x2, float y2, std::uint32_t c);
    void line(float x0, float y0, float x1, float y1, float width, std::uint32_t c);
    // lays colour c over one pixel, `coverage` (0 to 1) of it; pixels outside the picture are ignored
    void blend(int x, int y, std::uint32_t c, float coverage);

private:
    Picture &p_;
};

}  // namespace menudraw
