// The quick menu's shapes (host/menu_draw.hpp), no game and no window needed: ninja ps2menudrawtest && ./ps2menudrawtest
#include "../host/menu_draw.hpp"

#include <cstdio>

namespace {

int g_failed = 0;

void check(bool ok, const char *what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}

int R(std::uint32_t c) { return c & 255; }
int G(std::uint32_t c) { return c >> 8 & 255; }
int B(std::uint32_t c) { return c >> 16 & 255; }
int A(std::uint32_t c) { return c >> 24 & 255; }

menudraw::Picture blank(int w, int h, std::uint32_t c = 0) {
    menudraw::Picture p;
    p.width = w;
    p.height = h;
    p.rgba.assign(static_cast<std::size_t>(w) * h, c);
    return p;
}

}  // namespace

int main() {
    using namespace menudraw;
    {
        Picture p = blank(1, 1);
        Canvas c(p);
        c.blend(0, 0, rgba(200, 100, 50, 255), 0.5f);
        const std::uint32_t v = p.rgba[0];
        check((A(v) == 127 || A(v) == 128) && R(v) == 200 && G(v) == 100 && B(v) == 50, "blend_over_transparent");
    }
    {
        Picture p = blank(1, 1, rgba(0, 0, 0, 255));
        Canvas c(p);
        c.blend(0, 0, rgba(255, 255, 255, 128), 1.0f);
        const std::uint32_t v = p.rgba[0];
        check(R(v) >= 127 && R(v) <= 129 && G(v) >= 127 && G(v) <= 129 && B(v) >= 127 && B(v) <= 129 && A(v) == 255, "blend_over_opaque");
    }
    {
        Picture p = blank(100, 100);
        Canvas c(p);
        c.fill_round_rect(10, 10, 80, 60, 20, hex(0x1E2A78));
        check(A(p.rgba[40 * 100 + 50]) == 255 && A(p.rgba[10 * 100 + 10]) == 0 && A(p.rgba[40 * 100 + 11]) > 0, "round_rect_inside_and_corner");
        bool partial = false;
        for (int y = 10; y < 30; ++y)
            for (int x = 10; x < 30; ++x) {
                const int a = A(p.rgba[y * 100 + x]);
                if (a > 0 && a < 255) partial = true;
            }
        check(partial, "edges_are_smooth");
    }
    {
        Picture p = blank(20, 20);
        Canvas c(p);
        c.fill_circle(-50, -50, 500, hex(0xFFFFFF));
        bool all = true;
        for (std::uint32_t v : p.rgba) all = all && A(v) > 0;
        Picture q = blank(20, 20);
        Canvas d(q);
        d.fill_rect(1000, 1000, 10, 10, hex(0xFFFFFF));
        bool none = true;
        for (std::uint32_t v : q.rgba) none = none && v == 0;
        check(all && none, "nothing_outside_picture");
    }
    {
        Picture p = blank(40, 40);
        Canvas c(p);
        c.stroke_round_rect(0, 0, 40, 40, 8, 3, hex(0x1E2A78));
        check(A(p.rgba[20 * 40 + 20]) == 0 && A(p.rgba[20 * 40 + 1]) > 0, "stroke_leaves_middle");
    }
    std::printf(g_failed ? "%d FAILED\n" : "all ok\n", g_failed);
    return g_failed ? 1 : 0;
}
