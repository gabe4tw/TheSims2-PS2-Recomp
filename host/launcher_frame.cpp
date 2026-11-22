#include "launcher_frame.hpp"

void compose_layered(const menudraw::Picture &panel, int ww, int wh, std::vector<std::uint32_t> &out) {
    out.assign(static_cast<std::size_t>(ww > 0 ? ww : 0) * static_cast<std::size_t>(wh > 0 ? wh : 0), 0u);
    constexpr int kSolidFrom = 200;                       // the panel's fill (217) and everything on it count as solid
    constexpr int kBackR = 0x26, kBackG = 0x2C, kBackB = 0x5C;  // the dark navy behind the see-through fill
    for (int y = 0; y < panel.height; ++y) {
        const int wy = panel.y + y;
        if (wy < 0 || wy >= wh) continue;
        for (int x = 0; x < panel.width; ++x) {
            const int wx = panel.x + x;
            if (wx < 0 || wx >= ww) continue;
            const std::uint32_t s = panel.rgba[static_cast<std::size_t>(y) * panel.width + x];
            const int a = static_cast<int>(s >> 24);
            if (a == 0) continue;
            int r = static_cast<int>(s & 0xFF), g = static_cast<int>((s >> 8) & 0xFF), b = static_cast<int>((s >> 16) & 0xFF), oa = a;
            if (a >= kSolidFrom) {  // over the backdrop, solid
                r = (r * a + kBackR * (255 - a) + 127) / 255;
                g = (g * a + kBackG * (255 - a) + 127) / 255;
                b = (b * a + kBackB * (255 - a) + 127) / 255;
                oa = 255;
            } else {  // a soft edge: premultiplied
                r = (r * a + 127) / 255;
                g = (g * a + 127) / 255;
                b = (b * a + 127) / 255;
            }
            out[static_cast<std::size_t>(wy) * ww + wx] =
                static_cast<std::uint32_t>(oa) << 24 | static_cast<std::uint32_t>(r) << 16 | static_cast<std::uint32_t>(g) << 8 | static_cast<std::uint32_t>(b);
        }
    }
}
