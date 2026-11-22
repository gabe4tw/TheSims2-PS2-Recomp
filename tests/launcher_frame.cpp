// Local change: the borderless launcher window's picture (host/launcher_frame.cpp), no window needed.
// Build: ninja ps2launcherframetest, run build/.../ps2launcherframetest.
#include "../host/launcher_frame.hpp"

#include <cstdio>
#include <string>

namespace {
int failed = 0, checks = 0;
void check(bool ok, const std::string &what) {
    ++checks;
    if (!ok) {
        ++failed;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}
std::uint32_t at(const std::vector<std::uint32_t> &px, int w, int x, int y) { return px[static_cast<std::size_t>(y) * w + x]; }
int A(std::uint32_t p) { return static_cast<int>(p >> 24); }
int R(std::uint32_t p) { return static_cast<int>((p >> 16) & 0xFF); }  // (the output is BGRA in memory: 0xAARRGGBB)
int B(std::uint32_t p) { return static_cast<int>(p & 0xFF); }
}  // namespace

int main() {
    menudraw::Picture p;
    p.width = 4;
    p.height = 2;
    p.x = 3;
    p.y = 1;
    p.rgba.assign(8, 0);
    p.rgba[0] = menudraw::rgba(255, 0, 0, 255);   // opaque red
    p.rgba[1] = menudraw::rgba(143, 160, 224, 217);  // the panel's see-through fill
    p.rgba[2] = menudraw::rgba(0, 0, 255, 64);    // an anti-aliased edge pixel
    p.rgba[3] = 0;                                // a clear corner
    std::vector<std::uint32_t> out;
    compose_layered(p, 10, 5, out);
    check(out.size() == 50, "size: the whole window");
    check(A(at(out, 10, 0, 0)) == 0 && A(at(out, 10, 9, 4)) == 0, "outside_the_panel_is_clear");
    check(at(out, 10, 3, 1) == 0xFFFF0000u, "opaque_pixels_stay (BGRA 0xAARRGGBB)");
    check(A(at(out, 10, 4, 1)) == 255 && R(at(out, 10, 4, 1)) < 143, "panel_fill_made_solid over the dark backdrop");
    const std::uint32_t edge = at(out, 10, 5, 1);
    check(A(edge) == 64 && B(edge) <= A(edge) && R(edge) == 0, "edge_pixels_keep_their_alpha, premultiplied");
    check(A(at(out, 10, 6, 1)) == 0, "clear_corner_stays_clear");
    std::printf("launcher frame: %d of %d checks passed, %d failed\n", checks - failed, checks, failed);
    return failed ? 1 : 0;
}
