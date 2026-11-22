#include "menu_draw.hpp"

#include <algorithm>
#include <cmath>

namespace menudraw {
namespace {

// the share of a pixel inside a shape whose edge is `d` pixels away from the pixel's centre (negative: the centre is inside)
float cover(float d) { return std::clamp(0.5f - d, 0.0f, 1.0f); }

// signed distance from (px, py) to a rounded rectangle
float round_rect_distance(float px, float py, float x, float y, float w, float h, float r) {
    r = std::clamp(r, 0.0f, std::min(w, h) * 0.5f);
    const float cx = x + w * 0.5f, cy = y + h * 0.5f;
    const float qx = std::fabs(px - cx) - (w * 0.5f - r), qy = std::fabs(py - cy) - (h * 0.5f - r);
    const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

}  // namespace

void Canvas::blend(int x, int y, std::uint32_t c, float coverage) {
    if (x < 0 || y < 0 || x >= p_.width || y >= p_.height || coverage <= 0.0f) return;
    std::uint32_t &d = p_.rgba[static_cast<std::size_t>(y) * p_.width + x];
    const float as = static_cast<float>(c >> 24) / 255.0f * std::min(coverage, 1.0f);
    if (as <= 0.0f) return;
    const float ad = static_cast<float>(d >> 24) / 255.0f;
    const float ao = as + ad * (1.0f - as);
    const float keep = ad * (1.0f - as);
    std::uint32_t out = 0;
    for (int shift = 0; shift < 24; shift += 8) {
        const float s = static_cast<float>(c >> shift & 255), t = static_cast<float>(d >> shift & 255);
        const float v = (s * as + t * keep) / ao;
        out |= static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 255.0f))) << shift;
    }
    out |= static_cast<std::uint32_t>(std::lround(ao * 255.0f)) << 24;
    d = out;
}

void Canvas::clear(std::uint32_t c) { std::fill(p_.rgba.begin(), p_.rgba.end(), c); }

void Canvas::fill_rect(float x, float y, float w, float h, std::uint32_t c) {
    const int x0 = std::max(0, static_cast<int>(std::floor(x))), y0 = std::max(0, static_cast<int>(std::floor(y)));
    const int x1 = std::min(p_.width, static_cast<int>(std::ceil(x + w))), y1 = std::min(p_.height, static_cast<int>(std::ceil(y + h)));
    for (int j = y0; j < y1; ++j) {
        const float cy = std::min(y + h, j + 1.0f) - std::max(y, static_cast<float>(j));
        for (int i = x0; i < x1; ++i) {
            const float cx = std::min(x + w, i + 1.0f) - std::max(x, static_cast<float>(i));
            blend(i, j, c, cx * cy);
        }
    }
}

// Every shape below walks the pixels of its box (clipped to the picture) and covers each by its distance to the edge.
#define MENU_SHAPE_LOOP(left, top, right, bottom, DISTANCE_COVER)                                                        \
    const int x0 = std::max(0, static_cast<int>(std::floor(left)) - 1), y0 = std::max(0, static_cast<int>(std::floor(top)) - 1); \
    const int x1 = std::min(p_.width, static_cast<int>(std::ceil(right)) + 1);                                          \
    const int y1 = std::min(p_.height, static_cast<int>(std::ceil(bottom)) + 1);                                        \
    for (int j = y0; j < y1; ++j)                                                                                       \
        for (int i = x0; i < x1; ++i) {                                                                                 \
            const float px = i + 0.5f, py = j + 0.5f;                                                                   \
            blend(i, j, c, DISTANCE_COVER);                                                                             \
        }

void Canvas::fill_round_rect(float x, float y, float w, float h, float r, std::uint32_t c) {
    if (w <= 0 || h <= 0) return;
    MENU_SHAPE_LOOP(x, y, x + w, y + h, cover(round_rect_distance(px, py, x, y, w, h, r)))
}

void Canvas::stroke_round_rect(float x, float y, float w, float h, float r, float width, std::uint32_t c) {
    if (w <= 0 || h <= 0) return;
    const float ir = std::max(0.0f, r - width), iw = w - 2 * width, ih = h - 2 * width;
    MENU_SHAPE_LOOP(x, y, x + w, y + h,
                    cover(round_rect_distance(px, py, x, y, w, h, r)) *
                        (iw > 0 && ih > 0 ? 1.0f - cover(round_rect_distance(px, py, x + width, y + width, iw, ih, ir)) : 1.0f))
}

void Canvas::fill_circle(float cx, float cy, float r, std::uint32_t c) {
    MENU_SHAPE_LOOP(cx - r, cy - r, cx + r, cy + r, cover(std::hypot(px - cx, py - cy) - r))
}

void Canvas::stroke_circle(float cx, float cy, float r, float width, std::uint32_t c) {
    MENU_SHAPE_LOOP(cx - r, cy - r, cx + r, cy + r,
                    cover(std::hypot(px - cx, py - cy) - r) * (1.0f - cover(std::hypot(px - cx, py - cy) - (r - width))))
}

void Canvas::fill_triangle(float x0_, float y0_, float x1_, float y1_, float x2_, float y2_, std::uint32_t c) {
    // the largest signed distance to the three edge lines (inside: all negative), for either winding
    const float area = (x1_ - x0_) * (y2_ - y0_) - (y1_ - y0_) * (x2_ - x0_);
    if (area == 0.0f) return;
    const float s = area > 0 ? 1.0f : -1.0f;
    const auto edge = [s](float ax, float ay, float bx, float by, float px, float py) {
        const float ex = bx - ax, ey = by - ay, len = std::hypot(ex, ey);
        return s * ((px - ax) * ey - (py - ay) * ex) / len;
    };
    MENU_SHAPE_LOOP(std::min({x0_, x1_, x2_}), std::min({y0_, y1_, y2_}), std::max({x0_, x1_, x2_}), std::max({y0_, y1_, y2_}),
                    cover(std::max({edge(x0_, y0_, x1_, y1_, px, py), edge(x1_, y1_, x2_, y2_, px, py), edge(x2_, y2_, x0_, y0_, px, py)})))
}

void Canvas::line(float ax, float ay, float bx, float by, float width, std::uint32_t c) {
    const float half = width * 0.5f, ex = bx - ax, ey = by - ay, len2 = ex * ex + ey * ey;
    const auto distance = [&](float px, float py) {
        const float t = len2 > 0 ? std::clamp(((px - ax) * ex + (py - ay) * ey) / len2, 0.0f, 1.0f) : 0.0f;
        return std::hypot(px - (ax + t * ex), py - (ay + t * ey)) - half;
    };
    MENU_SHAPE_LOOP(std::min(ax, bx) - half, std::min(ay, by) - half, std::max(ax, bx) + half, std::max(ay, by) + half, cover(distance(px, py)))
}

#undef MENU_SHAPE_LOOP

}  // namespace menudraw
