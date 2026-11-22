// Local change: the PC-style mouse for The Sims 2 (see mouse_pc.hpp).
#include "mouse_pc.hpp"

#include "ps2/game_memory.hpp"
#include "ps2/os.hpp"
#include "ps2/system.hpp"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>
#include <string>

namespace mouse_pc {

bool pick_floor(const Camera &c, float u, float v, float floor_z, float &x, float &y) {
    const float nx = (u - 0.5f) / 0.5f, ny = (v - 0.5f) / -0.5f;  // (the game's 0x400690: 0..1 -> -1..1, y up)
    float d[3];
    for (int i = 0; i < 3; ++i) d[i] = nx / c.p00 * c.right[i] + ny / c.p11 * c.up[i] - c.back[i];
    if (d[2] > -1e-4f) return false;  // looking level or up: never meets the floor below
    const float t = (floor_z - c.eye[2]) / d[2];
    if (t <= 0) return false;
    x = c.eye[0] + t * d[0];
    y = c.eye[1] + t * d[1];
    return std::isfinite(x) && std::isfinite(y);
}

bool project(const Camera &c, float x, float y, float z, float &u, float &v) {
    const float p[3] = {x - c.eye[0], y - c.eye[1], z - c.eye[2]};
    const auto dot = [&](const float *a) { return p[0] * a[0] + p[1] * a[1] + p[2] * a[2]; };
    const float cx = dot(c.right), cy = dot(c.up), cz = dot(c.back);
    if (cz > -1e-4f) return false;
    u = 0.5f + 0.5f * c.p00 * cx / -cz;
    v = 0.5f - 0.5f * c.p11 * cy / -cz;
    return true;
}

void edge_step(const Camera &c, float dx, float dy, float distance, float &mx, float &my) {
    // right and forward along the floor (forward = -back, flattened)
    float rx = c.right[0], ry = c.right[1], fx = -c.back[0], fy = -c.back[1];
    const float rl = std::sqrt(rx * rx + ry * ry), fl = std::sqrt(fx * fx + fy * fy);
    if (rl > 1e-4f) rx /= rl, ry /= rl;
    if (fl > 1e-4f) fx /= fl, fy /= fl;
    const float speed = 0.012f * distance;  // ~ the visible floor's width in 2 s at full push
    mx = (dx * rx - dy * fx) * speed;
    my = (dx * ry - dy * fy) * speed;
}

void edge_push(float px, float py, float w, float h, float band, float &dx, float &dy) {
    const auto one = [&](float p, float size) {
        if (p < band) return -std::fmin(1.0f, (band - p) / band);
        if (p > size - 1 - band) return std::fmin(1.0f, (p - (size - 1 - band)) / band);
        return 0.0f;
    };
    dx = one(px, w);
    dy = one(py, h);
}

namespace {

constexpr std::uint32_t kInteractorManager = 0x492F50, kCameras = 0x484830, kFollow = 0x51D8EC;
constexpr std::uint32_t kCursorVtable = 0x4E5680;  // live mode's cursor (mode 1)
constexpr std::uint32_t kBuildToolVtable = 0x4E5CE8, kBuyPlaceVtable = 0x4E5058;  // build mode's tool (mode 3), buy mode's placing (mode 6)
constexpr float kFollowDefault = 0.6f;

std::mutex g_mutex;
bool g_enabled = false, g_active = false;
float g_u = 0.5f, g_v = 0.5f, g_edge_x = 0, g_edge_y = 0;
std::atomic<bool> g_cursor_mode{false};
std::atomic<bool> g_in_world{false};
std::atomic<bool> g_list_open{false};
std::atomic<int> g_list_index{0}, g_list_count{0};
std::atomic<std::uint16_t> g_inject{0};
// presses waiting for the game (each: held 4 refreshes, then 4 refreshes let go, so the game sees one press each)
std::mutex g_queue_mutex;
std::uint16_t g_queue[8];
int g_queue_n = 0, g_press_phase = 0;
constexpr std::uint32_t kListOwner = 0x51FFB8;
constexpr std::uint16_t kPadUp = 1u << 4, kPadRight = 1u << 5, kPadDown = 1u << 6, kPadLeft = 1u << 7, kPadCross = 1u << 14;
// the pause bar (Start in live mode: the icon row at the top, "Player 1"): [0x51FFE4] while it is open, +0x8C selected icon,
// +0xAC icon count
constexpr std::uint32_t kBar = 0x51FFE4;
std::atomic<bool> g_bar_open{false};
std::atomic<int> g_bar_index{0}, g_bar_count{0};
std::atomic<int> g_squeeze{75};
// the build / buy catalogue (Start > Bau-Katalog): [0x51FFE8] while it is open; +0xAC tab count, +0xB0 tab, +0xB4 first column
// shown (scroll), +0xB8 selected cell on the page (column * 6 + row; two columns of six), +0xC0 item count of the tab
constexpr std::uint32_t kCatalogue = 0x51FFE8;
constexpr std::uint16_t kPadL1 = 1u << 10, kPadR1 = 1u << 11;
std::atomic<bool> g_cat_open{false};
// the main menu (Story Mode / Sims Mode / Maxis): [0x51FFDC] while it is shown, +0xB8 selected item (0..2, left / right)
constexpr std::uint32_t kMainMenu = 0x51FFDC;
std::atomic<bool> g_main_open{false};
std::atomic<int> g_main_index{0};
std::atomic<int> g_main_level{0};  // +0xB0: 0 the main menu (3 items, +0xB8), 1 the Sims Mode wheel (left / right only, +0xA4 = 0 / 2)
// the front end's current screen object [0x4856E0]: +0xBC is its kind (0xD9 the language list at the first start, 0x59 the main
// menu, 0x121 the no-card box); the language list keeps its selected language (0..9, up / down) at +0x17C
constexpr std::uint32_t kFrontScreen = 0x4856E0;
std::atomic<bool> g_lang_open{false};
std::atomic<int> g_lang_index{0};
// a message box (Yes / No and the like) is shown while the word at 0x51FFA8 is 1
constexpr std::uint32_t kDialogFlag = 0x51FFA8;
std::atomic<bool> g_dialog_open{false};
// The message box's answers are Apt movie clips (no index is kept anywhere): each answer row has a marker clip (the X button
// picture) at local offset (-4, 2) under the row's clip; the marker of the highlighted row is visible (+0x5C bit 0x100000), an empty
// answer slot's row clip is hidden. Apt display objects: vtable 0x4EFD60 at +8, a 2x3 matrix at +0x10 (a b c d tx ty), the
// parent at +0x48, the flags at +0x5C. Positions are on the 640 x 448 stage. Scanned in the Apt heap while a box is open.
struct DialogRow { float x, y; bool selected; };
std::mutex g_dialog_mutex;
std::vector<DialogRow> g_dialog_rows;  // top to bottom
constexpr std::uint32_t kAptCih = 0x4EFD60, kAptHeapFrom = 0x1580000, kAptHeapTo = 0x1600000;
std::atomic<int> g_cat_tabs{0}, g_cat_tab{0}, g_cat_scroll{0}, g_cat_cell{0}, g_cat_count{0};  // PS2_UI_SQUEEZE (the bar is centred and narrows with it)

void queue_press(std::uint16_t bits) {
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    if (g_queue_n < 8) g_queue[g_queue_n++] = bits;
}

bool queue_empty() {
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    return g_queue_n == 0;
}

// one refresh of the press queue: what to hold now
std::uint16_t run_queue() {
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    if (!g_queue_n) return 0;
    const std::uint16_t bits = g_press_phase < 4 ? g_queue[0] : 0;
    if (++g_press_phase >= 8) {
        g_press_phase = 0;
        for (int i = 1; i < g_queue_n; ++i) g_queue[i - 1] = g_queue[i];
        --g_queue_n;
    }
    return bits;
}
bool g_follow_off = false;  // we set the follow strength to 0 (and put it back)

bool word(std::uint32_t a, std::uint32_t &out) { return ps2::read_game_memory(a, &out, 4); }
bool heap(std::uint32_t p) { return p >= 0x100000u && p < 0x2000000u && (p & 3) == 0; }

void set_follow(bool off) {
    if (off == g_follow_off) return;
    const float value = off ? 0.0f : kFollowDefault;
    ps2::write_game_memory(kFollow, &value, 4);
    g_follow_off = off;
}

bool read_camera(std::uint32_t cam, Camera &c, float &floor_z, float &distance) {
    float m[16];
    if (!ps2::read_game_memory(cam + 0xF0, m, sizeof m)) return false;
    for (int i = 0; i < 3; ++i) c.right[i] = m[i], c.up[i] = m[4 + i], c.back[i] = m[8 + i], c.eye[i] = m[12 + i];
    if (!ps2::read_game_memory(cam + 0x170, &c.p00, 4) || !ps2::read_game_memory(cam + 0x184, &c.p11, 4)) return false;
    if (!ps2::read_game_memory(cam + 0x44C, &floor_z, 4) || !ps2::read_game_memory(cam + 0x450, &distance, 4)) return false;
    const bool sane = std::isfinite(c.p00) && c.p00 > 0.1f && c.p00 < 20 && std::isfinite(c.p11) && c.p11 > 0.1f && c.p11 < 40 &&
                      std::isfinite(floor_z) && std::isfinite(distance) && distance > 0 && distance < 1000;
    return sane;
}

}  // namespace

std::atomic<int> g_player{0};
void set_player(int player) { g_player.store(player == 1 ? 1 : 0, std::memory_order_relaxed); }

void set_enabled(bool on) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_enabled = on;
}

void set_pointer(bool active, float u, float v, float edge_dx, float edge_dy) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_active = active;
    g_u = u, g_v = v, g_edge_x = edge_dx, g_edge_y = edge_dy;
}

bool in_cursor_mode() { return g_cursor_mode.load(std::memory_order_relaxed); }
bool in_world() { return g_in_world.load(std::memory_order_relaxed); }
std::atomic<bool> g_cas_open{false};
bool cas_open() { return g_cas_open.load(std::memory_order_relaxed); }

ListSpot list_spot(float u, float v) {
    // rows measured on the 640 x 511 picture (house rows 405-424, highlight 426-444, next 445-463, the pill 466-480); the list is at the
    // right edge (right-anchored by the interface squeeze too), so only its left end depends on the squeeze: u from 0.68 on
    if (u < 0.68f || u > 1.0f) return ListSpot::none;
    const float y = v * 511.0f;
    if (y >= 404 && y < 425) return ListSpot::above;
    if (y >= 425 && y < 444.5f) return ListSpot::middle;
    if (y >= 444.5f && y < 464) return ListSpot::below;
    if (y >= 464 && y < 482) return ListSpot::middle;  // the pill shows the highlighted item
    return ListSpot::none;
}

bool list_open() { return g_list_open.load(std::memory_order_relaxed); }

int bar_spot(float u, float v, int count, int squeeze) {
    // measured on the 640 x 511 picture at squeeze 75 with 8 icons: centres 160 .. 475, 45 apart, rows 85-148
    const float y = v * 511.0f;
    if (y < 85 || y > 148 || count < 1) return -1;
    const float x = 320.0f + (u * 640.0f - 320.0f) * 75.0f / static_cast<float>(std::clamp(squeeze, 50, 100));  // (back to squeeze 75)
    const float first = 317.5f - (count - 1) * 22.5f;
    const int i = static_cast<int>(std::floor((x - first) / 45.0f + 0.5f));
    if (i < 0 || i >= count || std::fabs(x - (first + i * 45.0f)) > 24.0f) return -1;
    return i;
}

bool bar_open() { return g_bar_open.load(std::memory_order_relaxed); }
bool cat_open() { return g_cat_open.load(std::memory_order_relaxed); }
bool main_open() { return g_main_open.load(std::memory_order_relaxed); }
bool lang_open() { return g_lang_open.load(std::memory_order_relaxed); }

// the language under (u, v): rows 49 + 40.8 i of 511 (10 languages), at the left half
int lang_spot(float u, float v) {
    if (u < 0.12f || u > 0.62f) return -1;
    const int i = static_cast<int>(std::lround((v * 511.0f - 49.0f) / 40.8f));
    return i >= 0 && i < 10 && std::fabs(v * 511.0f - (49.0f + i * 40.8f)) < 19.0f ? i : -1;
}
bool dialog_open() { return g_dialog_open.load(std::memory_order_relaxed); }

// a clip's place on the stage: its matrix times its parents'
bool stage_position(std::uint32_t o, float &x, float &y) {
    float m[6] = {1, 0, 0, 1, 0, 0};
    for (int depth = 0; depth < 16; ++depth) {
        float l[6];
        if (!ps2::read_game_memory(o + 0x10, l, sizeof l)) return false;
        const float n[6] = {l[0] * m[0] + l[2] * m[1], l[1] * m[0] + l[3] * m[1], l[0] * m[2] + l[2] * m[3],
                            l[1] * m[2] + l[3] * m[3], l[0] * m[4] + l[2] * m[5] + l[4], l[1] * m[4] + l[3] * m[5] + l[5]};
        for (int i = 0; i < 6; ++i) m[i] = n[i];
        std::uint32_t parent = 0, vt = 0;
        if (!word(o + 0x48, parent) || parent < 0x1000000u || parent >= 0x2000000u || !word(parent + 8, vt) || vt != kAptCih) break;
        o = parent;
    }
    x = m[4], y = m[5];
    return std::isfinite(x) && std::isfinite(y);
}

void scan_dialog_rows(std::vector<DialogRow> &rows) {
    rows.clear();
    static std::vector<std::uint32_t> heap_words;
    heap_words.resize((kAptHeapTo - kAptHeapFrom) / 4);
    if (!ps2::read_game_memory(kAptHeapFrom, heap_words.data(), kAptHeapTo - kAptHeapFrom)) return;
    for (std::size_t i = 4; i + 24 < heap_words.size(); ++i) {
        if (heap_words[i] != kAptCih) continue;
        const std::uint32_t o = kAptHeapFrom + static_cast<std::uint32_t>(i - 2) * 4;
        float l[6];
        std::memcpy(l, &heap_words[i + 2], sizeof l);
        if (std::fabs(l[0] - 1) > 0.01f || std::fabs(l[1]) > 1e-3f || std::fabs(l[2]) > 1e-3f || std::fabs(l[3] - 1) > 0.01f || l[4] != -4.0f || l[5] != 2.0f)
            continue;
        std::uint32_t flags = 0, parent = 0, parent_flags = 0;
        if (!word(o + 0x5C, flags) || !word(o + 0x48, parent) || !heap(parent) || !word(parent + 0x5C, parent_flags) || !(parent_flags & 0x100000u)) continue;
        DialogRow r;
        if (!stage_position(o, r.x, r.y)) continue;
        r.selected = (flags & 0x100000u) != 0;
        rows.push_back(r);
    }
    std::sort(rows.begin(), rows.end(), [](const DialogRow &a, const DialogRow &b) { return a.y < b.y; });
    // only a single column of answers counts: the selected marker's column. Screens whose choices stand side by side (the options
    // page: On / Off) have markers in a row; there the up / down steps would fight left / right, so they get no row clicks at all.
    const auto sel = std::find_if(rows.begin(), rows.end(), [](const DialogRow &r) { return r.selected; });
    if (sel == rows.end()) {
        rows.clear();
        return;
    }
    const float column = sel->x;
    std::vector<DialogRow> kept;
    for (const DialogRow &r : rows) {
        if (std::fabs(r.x - column) > 3.0f) continue;
        if (!kept.empty() && std::fabs(r.y - kept.back().y) < 8.0f) {  // two markers on one line: side by side, not a column
            rows.clear();
            return;
        }
        kept.push_back(r);
    }
    for (const DialogRow &r : rows)
        if (std::fabs(r.x - column) > 3.0f && std::fabs(r.y - sel->y) < 8.0f) {  // another choice beside the selected one
            rows.clear();
            return;
        }
    if (kept.size() < 2) kept.clear();
    rows.swap(kept);
}

// --- full-screen panels (relationships, family, create-a-Sim pages): the selection box is an Apt clip that jumps by one cell when
// the d-pad moves it. Which clip that is differs per page, so it is learned: on the first click the positions of all clips are
// noted, one Down (or Up) is pressed, and the clip that moved by about a cell is the box (and its move the row pitch). Then the
// clicks step the box to the clicked cell; a click on the box's own cell = Cross.
struct ClipPos { std::uint32_t addr; float x, y; };
struct PanelState {
    std::uint32_t box = 0;          // the learned selection box (0: not yet)
    float pitch_y = 39.5f, pitch_x = 38.0f;
    int phase = 0;                  // 0 idle, 1 waiting after the trial press, 2 after a second trial (other way)
    int wait = 0;
    float click_x = 0, click_y = 0;  // the click on the stage
    std::vector<ClipPos> before;
    bool no_box = false;            // learning found nothing on this screen: clicks are plain Cross until the screen changes
    std::uint64_t no_box_since = 0;
} g_panel;
std::mutex g_panel_mutex;
bool g_panel_click = false;
float g_panel_u = 0, g_panel_v = 0;

void visible_clips(std::vector<ClipPos> &out) {
    out.clear();
    constexpr std::uint32_t from = 0x1580000, to = 0x1700000;
    static std::vector<std::uint32_t> words;
    words.resize((to - from) / 4);
    if (!ps2::read_game_memory(from, words.data(), to - from)) return;
    for (std::size_t i = 4; i + 24 < words.size(); ++i) {
        if (words[i] != kAptCih || !(words[i + 21] & 0x100000u)) continue;  // (+0x5C flags: visible)
        float l[6];
        std::memcpy(l, &words[i + 2], sizeof l);
        if (std::fabs(l[0] - 1) > 0.01f || std::fabs(l[3] - 1) > 0.01f || std::fabs(l[1]) > 1e-3f || std::fabs(l[2]) > 1e-3f) continue;
        ClipPos c;
        c.addr = from + static_cast<std::uint32_t>(i - 2) * 4;
        if (stage_position(c.addr, c.x, c.y) && c.x >= -20 && c.x <= 660 && c.y >= -20 && c.y <= 470) out.push_back(c);
    }
}

bool clip_visible_at(std::uint32_t addr, float &x, float &y) {
    std::uint32_t vt = 0, flags = 0;
    return word(addr + 8, vt) && vt == kAptCih && word(addr + 0x5C, flags) && (flags & 0x100000u) && stage_position(addr, x, y);
}

// with the box known: the presses that take it to the clicked cell (Cross when it is there already)
void panel_steps(float cx, float cy) {
    float bx, by;
    if (!clip_visible_at(g_panel.box, bx, by)) {
        g_panel.box = 0;
        return;
    }
    const int dr = static_cast<int>(std::lround((cy - by) / g_panel.pitch_y));
    if (std::fabs(cx - bx) > 120.0f || std::abs(dr) > 5) {  // not on the box's column of cells: a plain click
        queue_press(kPadCross);
        return;
    }
    // (rows only: on an unknown screen left / right can be a slider - create-a-Sim's proportions - so no column steps here)
    const int dc = 0;
    (void)cx, (void)bx;
    if (dr == 0 && dc == 0) {
        queue_press(kPadCross);
        return;
    }
    for (int k = 0; k < std::abs(dc) && k < 8; ++k) queue_press(dc > 0 ? kPadRight : kPadLeft);
    for (int k = 0; k < std::abs(dr) && k < 12; ++k) queue_press(dr > 0 ? kPadDown : kPadUp);
}

// emulation thread, every refresh while a panel is up
void panel_frame(int squeeze) {
    bool click;
    float u, v;
    {
        std::lock_guard<std::mutex> lock(g_panel_mutex);
        click = g_panel_click, u = g_panel_u, v = g_panel_v;
        g_panel_click = false;
    }
    if (click && g_panel.phase == 0) {
        g_panel.click_x = 320.0f + (u * 640.0f - 320.0f) * 100.0f / static_cast<float>(std::clamp(squeeze, 50, 100));
        g_panel.click_y = v * 448.0f;
        float bx, by;
        if (g_panel.no_box && ps2::frame_count() - g_panel.no_box_since < 50 * 20) {
            queue_press(kPadCross);
        } else if (g_panel.box && clip_visible_at(g_panel.box, bx, by)) {
            panel_steps(g_panel.click_x, g_panel.click_y);
        } else {
            g_panel.no_box = false;
            visible_clips(g_panel.before);
            queue_press(kPadDown);
            g_panel.phase = 1, g_panel.wait = 0;
        }
        return;
    }
    if (g_panel.phase == 0 || !queue_empty() || ++g_panel.wait < 8) return;
    std::vector<ClipPos> after;
    visible_clips(after);
    std::uint32_t best = 0;
    float best_dx = 1e9f, step = 0;
    for (const ClipPos &a : after)
        for (const ClipPos &b : g_panel.before)
            if (a.addr == b.addr) {
                const float dy = a.y - b.y, dx = a.x - b.x;
                if (std::fabs(dx) < 2 && std::fabs(dy) > 20 && std::fabs(dy) < 70 && std::fabs(a.x - g_panel.click_x) < best_dx)
                    best = a.addr, best_dx = std::fabs(a.x - g_panel.click_x), step = std::fabs(dy);
            }
    if (best) {
        g_panel.box = best, g_panel.pitch_y = step, g_panel.phase = 0;
        panel_steps(g_panel.click_x, g_panel.click_y);
        return;
    }
    if (g_panel.phase == 1) {  // nothing moved: perhaps the box was at the bottom; try up
        g_panel.before.swap(after);
        queue_press(kPadUp);
        g_panel.phase = 2, g_panel.wait = 0;
        return;
    }
    g_panel.phase = 0;  // no box found (the trial presses went down and up again): nothing more - a Cross could pick the wrong item;
    g_panel.no_box = true, g_panel.no_box_since = ps2::frame_count();  // the next clicks on this panel are plain Cross
}

bool panel_click(float u, float v) {
    std::lock_guard<std::mutex> lock(g_panel_mutex);
    g_panel_click = true, g_panel_u = u, g_panel_v = v;
    return true;
}

// the answer row under (u, v), -1 none
int dialog_spot(float u, float v, int squeeze) {
    const float x = 320.0f + (u * 640.0f - 320.0f) * 100.0f / static_cast<float>(std::clamp(squeeze, 50, 100));  // (stage: before the squeeze)
    const float y = v * 448.0f;
    std::lock_guard<std::mutex> lock(g_dialog_mutex);
    for (std::size_t i = 0; i < g_dialog_rows.size(); ++i) {
        const DialogRow &r = g_dialog_rows[i];
        if (y >= r.y - 6 && y < r.y + 24 && x >= r.x - 24 && x < r.x + 330) return static_cast<int>(i);
    }
    return -1;
}

int dialog_selected() {
    std::lock_guard<std::mutex> lock(g_dialog_mutex);
    for (std::size_t i = 0; i < g_dialog_rows.size(); ++i)
        if (g_dialog_rows[i].selected) return static_cast<int>(i);
    return -1;
}

// main menu item under (u, v) (centres measured at squeeze 75: 240,372 / 322,345 / 400,372 on 640 x 511), -1 none
int main_spot(float u, float v, int squeeze) {
    const float x = 320.0f + (u * 640.0f - 320.0f) * 75.0f / static_cast<float>(std::clamp(squeeze, 50, 100)), y = v * 511.0f;
    static const float kItems[3][2] = {{240, 372}, {322, 345}, {400, 372}};
    int best = -1;
    float best_d = 40.0f * 40.0f;
    const int level = g_main_level.load();
    for (int i = 0; i < 3; ++i) {
        if (level == 1 && i == 1) continue;  // (the Sims Mode wheel has no middle item)
        const float dx = x - kItems[i][0], dy = y - kItems[i][1], d = dx * dx + dy * dy;
        if (d < best_d) best = i, best_d = d;
    }
    return best;
}

// the HUD wheel at the bottom left (live mode): which d-pad direction its segment under (u, v) stands for, 0 none. Centre measured
// at (50, 440) of 640 x 511 at squeeze 75; the HUD sits at the left edge, so its x scales with the squeeze.
std::uint16_t hud_spot(float u, float v, int squeeze) {
    const float x = u * 640.0f * 75.0f / static_cast<float>(std::clamp(squeeze, 50, 100)), y = v * 511.0f;
    const float dx = x - 50.0f, dy = y - 440.0f, r = std::sqrt(dx * dx + dy * dy);
    if (r < 9.0f || r > 38.0f) return 0;
    if (std::fabs(dx) > std::fabs(dy)) return dx < 0 ? kPadLeft : kPadRight;
    return dy < 0 ? kPadUp : kPadDown;
}

// (u, v) on the catalogue, measured on the 640 x 511 picture at squeeze 75: the grid's cell (0..11, -1 none), a tab (-1 none),
// or the L1 / R1 arrows (-1 / +1)
void cat_spot(float u, float v, int tabs, int squeeze, int &cell, int &tab, int &arrow) {
    cell = tab = -1, arrow = 0;
    const float x = 320.0f + (u * 640.0f - 320.0f) * 75.0f / static_cast<float>(std::clamp(squeeze, 50, 100));
    const float y = v * 511.0f;
    static const float kRows[7] = {202, 244, 289, 335, 381, 427, 466};
    if (x >= 145 && x < 201 && y >= kRows[0] && y < kRows[6]) {
        int r = 0;
        while (r < 5 && y >= kRows[r + 1]) ++r;
        cell = (x < 172 ? 0 : 6) + r;
        return;
    }
    if (y >= 90 && y < 146) {
        const float step = tabs <= 7 ? 32.2f : tabs >= 9 ? 34.5f : 33.3f;  // (build 7 tabs, buy 9: measured)
        const int i = static_cast<int>(std::floor((x - 182.0f) / step + 0.5f));
        if (i >= 0 && i < tabs && std::fabs(x - (182.0f + i * step)) <= step * 0.5f) {
            tab = i;
            return;
        }
    }
    if (y >= 90 && y < 146) {
        if (x >= 104 && x < 140) arrow = -1;
        if (x >= 505 && x < 540) arrow = 1;
    }
}


bool list_click(float u, float v) {
    if (lang_open()) {
        const int target = lang_spot(u, v), at = g_lang_index.load();
        if (target < 0) return false;
        for (int k = 0; k < std::abs(target - at) && k < 10; ++k) queue_press(target > at ? kPadDown : kPadUp);
        queue_press(kPadCross);
        return true;
    }
    if (dialog_open()) {
        const int target = dialog_spot(u, v, g_squeeze.load()), at = dialog_selected();
        if (target < 0 || at < 0) return false;  // (elsewhere: Cross, the highlighted answer)
        for (int k = 0; k < std::abs(target - at) && k < 6; ++k) queue_press(target > at ? kPadDown : kPadUp);
        queue_press(kPadCross);
        return true;
    }
    // full-screen panels: the learned selection box (create-a-Sim and other screens are not: there the learned clip was not a
    // selection box and the steps went wrong - a click is a plain Cross there)
    // (not the pause menu's options pages, [0x51FFD8]: their tiles light up instead of a box moving, the learning found nothing
    // and its trial presses plus the Cross after them toggled the wrong tile - there a click is a plain Cross)
    std::uint32_t options = 0;
    const bool options_page = word(0x51FFD8, options) && heap(options);
    if (ps2::ui_panel_up() && in_world() && !options_page && !cat_open() && !main_open() && !bar_open() && !list_open() && !dialog_open())
        return panel_click(u, v);
    if (main_open() && !dialog_open()) {
        const int target = main_spot(u, v, g_squeeze.load());
        if (target < 0) return false;
        int at = g_main_index.load();
        if (g_main_level.load() == 1) {
            if (target != at) queue_press(target > at ? kPadRight : kPadLeft);  // (0 <-> 2 is one press there)
        } else {
            for (int k = 0; k < 3 && at != target; ++k, at += target > at ? 1 : -1) queue_press(target > at ? kPadRight : kPadLeft);
        }
        queue_press(kPadCross);
        return true;
    }
    if (in_world() && !dialog_open() && !cat_open() && !bar_open() && !list_open()) {
        if (const std::uint16_t dir = hud_spot(u, v, g_squeeze.load())) {  // the HUD wheel's segments open their panels
            queue_press(dir);
            return true;
        }
    }
    if (cat_open()) {
        int cell, tab, arrow;
        cat_spot(u, v, g_cat_tabs.load(), g_squeeze.load(), cell, tab, arrow);
        if (arrow) {
            queue_press(arrow < 0 ? kPadL1 : kPadR1);
            return true;
        }
        if (tab >= 0) {
            const int at = g_cat_tab.load();
            for (int k = 0; k < std::abs(tab - at) && k < 8; ++k) queue_press(tab > at ? kPadR1 : kPadL1);
            return true;
        }
        if (cell < 0) return false;
        if (g_cat_scroll.load() * 6 + cell >= g_cat_count.load()) return true;  // an empty cell
        const int at = g_cat_cell.load();
        if (cell == at) {
            queue_press(kPadCross);  // a click on the chosen item takes it
            return true;
        }
        const int dc = cell / 6 - at / 6, dr = cell % 6 - at % 6;
        for (int k = 0; k < std::abs(dc); ++k) queue_press(dc > 0 ? kPadRight : kPadLeft);
        for (int k = 0; k < std::abs(dr) && k < 6; ++k) queue_press(dr > 0 ? kPadDown : kPadUp);
        return true;
    }
    if (bar_open()) {
        const int target = bar_spot(u, v, g_bar_count.load(), g_squeeze.load());
        if (target < 0) return false;
        int at = g_bar_index.load();
        for (int k = 0; k < 6 && at != target; ++k, at += target > at ? 1 : -1) queue_press(target > at ? kPadRight : kPadLeft);
        if (at == target) queue_press(kPadCross);
        return true;
    }
    if (!list_open()) return false;
    const int index = g_list_index.load(), count = g_list_count.load();
    switch (list_spot(u, v)) {
    case ListSpot::middle: queue_press(kPadCross); return true;
    case ListSpot::above: if (index > 0) queue_press(kPadUp); return true;
    case ListSpot::below: if (index + 1 < count) queue_press(kPadDown); return true;
    default: return false;
    }
}

bool list_wheel(int dir) {
    // left / right where the game's items run across (main menu, pause bar, catalogue columns); up / down for the interaction list,
    // message boxes, full-screen panels and every screen outside the game world; elsewhere (live / build / buy) the wheel zooms
    if ((main_open() && !dialog_open()) || cat_open() || bar_open()) {
        queue_press(dir < 0 ? kPadLeft : kPadRight);
        return true;
    }
    if (list_open() || dialog_open() || ps2::ui_panel_up() || !in_world()) {
        queue_press(dir < 0 ? kPadUp : kPadDown);
        return true;
    }
    return false;
}

std::uint16_t injected_buttons() { return g_inject.load(std::memory_order_relaxed); }

void frame() {
    // test switch: PS2_MOUSE_TEST=frame:u:v[:edge_dx:edge_dy] puts the pointer there from that refresh on (headless checks)
    // the interaction list
    {
        std::uint32_t owner = 0;
        bool open = false;
        if (word(kListOwner, owner) && heap(owner)) {
            std::uint8_t state = 0, count = 0;
            std::int16_t index = 0;
            ps2::read_game_memory(owner + 0x740 + 9, &state, 1);
            ps2::read_game_memory(owner + 0x740 + 0xA2, &count, 1);
            ps2::read_game_memory(owner + 0x740 + 6, &index, 2);
            open = state >= 2 && state <= 8 && count >= 1 && index >= 0 && index < count;
            g_list_index.store(index);
            g_list_count.store(count);
        }
        g_list_open.store(open, std::memory_order_relaxed);
        // the pause bar
        std::uint32_t bar = 0;
        bool bar_is_open = false;
        if (word(kBar, bar) && heap(bar)) {
            std::uint32_t index = 0, count = 0;
            if (word(bar + 0x8C, index) && word(bar + 0xAC, count) && count >= 1 && count <= 16 && index < count) {
                bar_is_open = true;
                g_bar_index.store(static_cast<int>(index));
                g_bar_count.store(static_cast<int>(count));
            }
        }
        g_bar_open.store(bar_is_open, std::memory_order_relaxed);
        // the catalogue
        std::uint32_t cat = 0;
        bool cat_is_open = false;
        if (word(kCatalogue, cat) && heap(cat)) {
            std::int32_t f[6] = {}, shown = 0;  // +0xAC tabs, +0xB0 tab, +0xB4 scroll, +0xB8 cell, +0xBC (pointer), +0xC0 count
            // (+0xEC: 1 while the catalogue is shown, 0 while an item from it is being placed)
            if (ps2::read_game_memory(cat + 0xAC, f, sizeof f) && ps2::read_game_memory(cat + 0xEC, &shown, 4) && shown == 1 && f[0] >= 1 && f[0] <= 12 && f[1] >= 0 && f[1] < f[0] && f[2] >= 0 && f[3] >= 0 &&
                f[3] < 12 && f[5] >= 0 && f[5] < 100000) {
                cat_is_open = true;
                g_cat_tabs.store(f[0]), g_cat_tab.store(f[1]), g_cat_scroll.store(f[2]), g_cat_cell.store(f[3]), g_cat_count.store(f[5]);
            }
        }
        g_cat_open.store(cat_is_open, std::memory_order_relaxed);
        // the main menu and message boxes
        std::uint32_t main = 0, dialog = 0;
        bool main_is_open = false;
        std::uint32_t front_screen = 0, front_kind = 0;
        if (word(kMainMenu, main) && heap(main) && word(kFrontScreen, front_screen) && heap(front_screen) && word(front_screen + 0xBC, front_kind) &&
            front_kind == 0x59) {
            std::uint32_t level = 0, index = 0;
            if (word(main + 0xB0, level) && level <= 1 && word(main + (level == 0 ? 0xB8 : 0xA4), index) && index < 3 &&
                (level == 0 || index != 1)) {  // (other wheels - Story Mode's - are not mapped: no mouse there)
                main_is_open = true;
                g_main_level.store(static_cast<int>(level));
                g_main_index.store(static_cast<int>(index));
            }
        }
        g_main_open.store(main_is_open, std::memory_order_relaxed);
        std::uint32_t cas = 0;
        g_cas_open.store(word(0x51FFB0, cas) && heap(cas), std::memory_order_relaxed);
        g_dialog_open.store(word(kDialogFlag, dialog) && dialog == 1, std::memory_order_relaxed);
        {
            std::uint32_t front = 0, kind = 0;
            std::int32_t index = -1;
            const bool lang = word(kFrontScreen, front) && heap(front) && word(front + 0xBC, kind) && kind == 0xD9 &&
                              ps2::read_game_memory(front + 0x17C, &index, 4) && index >= 0 && index < 10;
            g_lang_open.store(lang, std::memory_order_relaxed);
            if (lang) g_lang_index.store(index);
        }
        {
            std::vector<DialogRow> rows;
            if (g_dialog_open.load()) scan_dialog_rows(rows);
            std::lock_guard<std::mutex> lock(g_dialog_mutex);
            g_dialog_rows.swap(rows);
        }
        static bool was_panel = false;
        const bool panel_now = ps2::ui_panel_up() && in_world();
        if (panel_now != was_panel) g_panel.box = 0, g_panel.no_box = false;  // (a new screen learns its box again)
        was_panel = panel_now;
        if (panel_now || g_panel.phase) panel_frame(g_squeeze.load());
        {  // hovering an answer / a language moves the highlight to it - one step per pointer move, so it can never swing to and fro
            static float last_u = -1, last_v = -1;
            bool active;
            float hu, hv;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                active = g_active && g_enabled, hu = g_u, hv = g_v;
            }
            const bool moved = std::fabs(hu - last_u) > 0.002f || std::fabs(hv - last_v) > 0.002f;
            int target = -1, at = -1;
            if (active && g_lang_open.load()) target = lang_spot(hu, hv), at = g_lang_index.load();
            else if (active && g_dialog_open.load()) target = dialog_spot(hu, hv, g_squeeze.load()), at = dialog_selected();
            if (moved && target >= 0 && at >= 0 && target != at && queue_empty()) {
                for (int k = 0; k < std::abs(target - at) && k < 10; ++k) queue_press(target > at ? kPadDown : kPadUp);
                last_u = hu, last_v = hv;
            }
        }
        if (main_is_open && !g_dialog_open.load()) {  // hovering an item of the main menu moves the highlight to it
            bool active;
            float hu, hv;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                active = g_active && g_enabled, hu = g_u, hv = g_v;
            }
            static float last_u = -1, last_v = -1;
            const bool moved = std::fabs(hu - last_u) > 0.002f || std::fabs(hv - last_v) > 0.002f;
            const int target = active ? main_spot(hu, hv, g_squeeze.load()) : -1;
            const int at = g_main_index.load();
            if (moved && target >= 0 && target != at && queue_empty()) {
                const int steps = g_main_level.load() == 1 ? 1 : std::abs(target - at);
                for (int k = 0; k < steps; ++k) queue_press(target > at ? kPadRight : kPadLeft);
                last_u = hu, last_v = hv;
            }
        }
        if (bar_is_open) {  // hovering an icon moves the highlight to it
            bool active;
            float hu, hv;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                active = g_active && g_enabled, hu = g_u, hv = g_v;
            }
            const int target = active ? bar_spot(hu, hv, g_bar_count.load(), g_squeeze.load()) : -1;
            if (target >= 0 && target != g_bar_index.load() && queue_empty()) queue_press(target > g_bar_index.load() ? kPadRight : kPadLeft);
        }
        g_inject.store(run_queue(), std::memory_order_relaxed);
    }
    static const bool squeeze_read = [] {
        const int s = std::atoi(ps2::os::env("PS2_UI_SQUEEZE").c_str());
        g_squeeze.store(s == 1 ? 75 : s >= 50 && s <= 100 ? s : 100);  // (unset: no squeeze = 100; 1 = the old on = 75)
        return true;
    }();
    (void)squeeze_read;
    static const std::string test = ps2::os::env("PS2_MOUSE_TEST");
    // test switch: PS2_MOUSE_TEST_CLICK=frame:u:v[;frame:u:v...] clicks there (headless; the presses go to the pad directly)
    static const std::string test_click = ps2::os::env("PS2_MOUSE_TEST_CLICK");
    if (!test_click.empty()) {
        for (std::size_t at = 0; at < test_click.size();) {
            std::size_t end = test_click.find(';', at);
            if (end == std::string::npos) end = test_click.size();
            unsigned long long f = 0;
            float cu = 0, cv = 0;
            if (std::sscanf(test_click.substr(at, end - at).c_str(), "%llu:%f:%f", &f, &cu, &cv) == 3 && ps2::frame_count() == f) {
                {
                    std::lock_guard<std::mutex> lock(g_dialog_mutex);
                    for (const DialogRow &r : g_dialog_rows) std::fprintf(stderr, "[mouse] dialog row at %.0f %.0f%s\n", r.x, r.y, r.selected ? " (highlighted)" : "");
                }
                const bool took = list_click(cu, cv);
                std::fprintf(stderr, "[mouse] test click at %.3f %.3f: %s (list open %d index %d of %d; bar open %d index %d of %d)\n", cu, cv, took ? "the list's" : "not the list's",
                             list_open(), g_list_index.load(), g_list_count.load(), bar_open(), g_bar_index.load(), g_bar_count.load());
            }
            at = end + 1;
        }
        // PS2_MOUSE_TEST_WHEEL=frame:dir[;...] turns the wheel (dir -1 up, 1 down) in the same test runs
        static const std::string test_wheel = ps2::os::env("PS2_MOUSE_TEST_WHEEL");
        for (std::size_t at = 0; at < test_wheel.size();) {
            std::size_t end = test_wheel.find(';', at);
            if (end == std::string::npos) end = test_wheel.size();
            unsigned long long f = 0;
            int dir = 0;
            if (std::sscanf(test_wheel.substr(at, end - at).c_str(), "%llu:%d", &f, &dir) == 2 && ps2::frame_count() == f)
                std::fprintf(stderr, "[mouse] test wheel %d: %s (panel %d dialog %d in world %d)\n", dir, list_wheel(dir) ? "a menu's" : "zoom", ps2::ui_panel_up(), dialog_open(), in_world());
            at = end + 1;
        }
        static std::uint16_t was = 0;
        const std::uint16_t now = injected_buttons();
        if (now != was) ps2::set_pad_buttons(now, 0);  // (only on a change, so --press scripts still work around it)
        if (now != was)
            std::fprintf(stderr, "[mouse] frame %llu inject %04X (cat open %d tab %d scroll %d cell %d count %d)\n", static_cast<unsigned long long>(ps2::frame_count()), now,
                         cat_open(), g_cat_tab.load(), g_cat_scroll.load(), g_cat_cell.load(), g_cat_count.load());
        was = now;
    }
    if (!test.empty()) {
        unsigned long long from = 0;
        float tu = 0, tv = 0, tx = 0, ty = 0;
        if (std::sscanf(test.c_str(), "%llu:%f:%f:%f:%f", &from, &tu, &tv, &tx, &ty) >= 3 && ps2::frame_count() >= from) set_pointer(true, tu, tv, tx, ty);
    }
    bool enabled, active;
    float u, v, ex, ey;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        enabled = g_enabled, active = g_active, u = g_u, v = g_v, ex = g_edge_x, ey = g_edge_y;
    }
    std::uint32_t mgr = 0, entries = 0, entries_end = 0, interactor = 0, vtable = 0, cams = 0, cam = 0;
    bool cursor = word(kInteractorManager, mgr) && heap(mgr) && word(mgr + 0x1C, entries) && heap(entries) &&
                  word(mgr + 0x20, entries_end) && entries + 8u * static_cast<std::uint32_t>(g_player.load()) + 8 <= entries_end &&
                  word(entries + 8u * static_cast<std::uint32_t>(g_player.load()) + 4, interactor) && heap(interactor) && word(interactor + 0x5C, vtable) &&
                  (vtable == kCursorVtable || vtable == kBuildToolVtable || vtable == kBuyPlaceVtable);
    g_cursor_mode.store(cursor, std::memory_order_relaxed);
    // in the game world (live, build, buy: an interactor is there) the wheel zooms; elsewhere (main menu, dialogs, panels) it steps
    g_in_world.store(heap(interactor) && heap(vtable), std::memory_order_relaxed);
    static const bool log = ps2::os::env("PS2_MOUSE_LOG") == "1";  // test switch: the state once a second
    if (log && ps2::frame_count() % 50 == 0)
        std::fprintf(stderr, "[mouse] frame %llu on %d active %d cursor mode %d (vtable %08X) u %.3f v %.3f edge %.2f %.2f follow off %d\n",
                     static_cast<unsigned long long>(ps2::frame_count()), enabled, active, cursor, vtable, u, v, ex, ey, g_follow_off);
    const bool ours = enabled && active && cursor && !list_open() && !bar_open() && !cat_open() && word(kCameras, cams) && heap(cams) && word(cams + 4u * static_cast<std::uint32_t>(g_player.load()), cam) && heap(cam);
    if (!ours) {
        if (g_follow_off) set_follow(false);  // the stick moves the cursor again: let the camera follow it
        return;
    }
    Camera c;
    float floor_z = 0, distance = 0;
    if (!read_camera(cam, c, floor_z, distance)) return;
    set_follow(true);
    if (ex != 0 || ey != 0) {
        float target[2], mx, my;
        edge_step(c, ex, ey, distance, mx, my);
        if (ps2::read_game_memory(cam + 0x444, target, 8)) {
            target[0] += mx, target[1] += my;
            ps2::write_game_memory(cam + 0x444, target, 8);
        }
    }
    float x, y;
    // the camera's target floats 1.25 above the floor it looks at (measured: a buy-mode chair lands at target z - 1.25 under the pointer)
    // live mode's cursor picks the Sim or object it stands under, so it aims at waist height (a click on a Sim's body or a counter
    // finds it); build and buy mode place on the floor itself
    const float aim = vtable == kCursorVtable ? floor_z - 1.25f + 0.6f : floor_z - 1.25f;
    if (pick_floor(c, u, v, aim, x, y)) {
        const float xy[2] = {x, y};
        ps2::write_game_memory(interactor + 8, xy, 8);
    }
}

}  // namespace mouse_pc
