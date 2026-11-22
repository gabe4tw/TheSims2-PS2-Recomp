// The PC mouse's math (host/mouse_pc.hpp), no game needed: ninja ps2mousetest && ./ps2mousetest
// The camera is the game's own from a test scene (build.state in cursor mode, frame 13199; plans\mouse-split-ledger.md).
#include "../host/mouse_pc.hpp"

#include <cmath>
#include <cstdio>

namespace {

int g_failed = 0;

void check(bool ok, const char *what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}

bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

mouse_pc::Camera scene() {
    mouse_pc::Camera c;
    const float right[3] = {-0.070579f, 0.99751f, 0.0f}, up[3] = {-0.25653f, -0.018151f, 0.96637f},
                back[3] = {0.96396f, 0.068205f, 0.25717f}, eye[3] = {21.6419f, 18.6019f, 3.1471f};
    for (int i = 0; i < 3; ++i) c.right[i] = right[i], c.up[i] = up[i], c.back[i] = back[i], c.eye[i] = eye[i];
    c.p00 = 1.51865f;
    c.p11 = 2.69981f;
    return c;
}

}  // namespace

int main() {
    const mouse_pc::Camera c = scene();
    float u = 0, v = 0, x = 0, y = 0;
    // the camera target (the cursor) is in the middle of the picture
    check(mouse_pc::project(c, 14.531f, 18.099f, 1.25f, u, v) && near(u, 0.5f, 0.003f) && near(v, 0.5f, 0.003f), "target in the middle");
    check(mouse_pc::pick_floor(c, 0.5f, 0.5f, 1.25f, x, y) && near(x, 14.531f, 0.02f) && near(y, 18.099f, 0.02f), "middle -> target");
    // a cursor point measured in the game: (12.5, 19.5) drew its beam at x ~405 of 640 (the sink)
    check(mouse_pc::project(c, 12.5f, 19.5f, 1.25f, u, v) && near(u * 640, 405, 25), "sink point near x 405");
    // round trip over the picture
    bool round = true;
    for (float uu = 0.05f; uu < 1; uu += 0.15f)
        for (float vv = 0.3f; vv < 1; vv += 0.15f) {
            float px, py, bu, bv;
            if (!mouse_pc::pick_floor(c, uu, vv, 1.25f, px, py) || !mouse_pc::project(c, px, py, 1.25f, bu, bv) || !near(bu, uu, 1e-3f) ||
                !near(bv, vv, 1e-3f))
                round = false;
        }
    check(round, "picture -> floor -> picture");
    // above the horizon there is no floor
    check(!mouse_pc::pick_floor(c, 0.5f, 0.0f, 1.25f, x, y) || y > -1e6f, "top row: no floor or a far one");
    float dx, dy;
    mouse_pc::edge_push(320, 200, 640, 448, 12, dx, dy);
    check(dx == 0 && dy == 0, "no push in the middle");
    mouse_pc::edge_push(0, 200, 640, 448, 12, dx, dy);
    check(near(dx, -1, 1e-4f) && dy == 0, "full push at the left edge");
    mouse_pc::edge_push(639, 447, 640, 448, 12, dx, dy);
    check(near(dx, 1, 1e-4f) && near(dy, 1, 1e-4f), "full push at the bottom right");
    mouse_pc::edge_push(6, 200, 640, 448, 12, dx, dy);
    check(dx < 0 && dx > -1, "part push inside the band");
    // pushing right moves the target along the camera's right; pushing up moves it away from the camera
    float mx, my;
    mouse_pc::edge_step(c, 1, 0, 7.4f, mx, my);
    check(mx * c.right[0] + my * c.right[1] > 0, "right push goes right");
    mouse_pc::edge_step(c, 0, -1, 7.4f, mx, my);
    check(mx * -c.back[0] + my * -c.back[1] > 0, "up push goes forward");
    std::printf("%s\n", g_failed ? "FAILED" : "all ok");
    return g_failed ? 1 : 0;
}
