#pragma once

#include <cstdint>

// Local change (The Sims 2 PAL SLES-53718): the PC-style mouse. In the game's cursor mode (Select in live mode) the cursor goes to the
// floor point under the mouse pointer, the camera stops chasing the cursor and scrolls when the pointer is at a screen edge instead.
// Game facts (found 2026-11-21, plans\mouse-split-ledger.md):
//   interactor manager [0x492F50]; per player [[mgr+0x1C] + 8p] = (mode, interactor); the cursor interactor's vtable (at +0x5C) is
//   0x4E5680, its +0x08/+0x0C = the cursor's floor x/y (writing them moves it);
//   camera per player [[0x484830] + 4p]: +0xF0 world matrix rows (right, up, back, eye), +0x170 P00, +0x184 P11, +0x444 target
//   (+0x44C = floor height + 1.25), +0x450 zoom distance;
//   0x51D8EC = how hard the camera follows a cursor that left its box (0.6); 0 = it does not follow.
// The window thread tells this module where the pointer is; frame() runs on the emulation thread once per refresh (ps2::set_frame_hook).

namespace mouse_pc {

// the camera as the game has it (world rows; the camera looks down -back)
struct Camera {
    float right[3], up[3], back[3], eye[3];
    float p00 = 1, p11 = 1;  // projection: x and y scale (p11 = p00 * aspect)
};

// The floor point under (u, v) (0..1 over the picture, v down) at height floor_z. False when the ray does not go down to it.
bool pick_floor(const Camera &cam, float u, float v, float floor_z, float &x, float &y);
// (x, y) on the floor -> (u, v) on the picture; false when behind the camera
bool project(const Camera &cam, float x, float y, float z, float &u, float &v);
// How far the camera target moves for an edge push (dx right, dy down, each -1..1) at this zoom distance, per refresh.
void edge_step(const Camera &cam, float dx, float dy, float distance, float &move_x, float &move_y);
// The edge push for a pointer at (px, py) in a picture of w x h pixels: 0 inside, up to +-1 within `band` pixels of an edge
// (and at full strength on or past it).
void edge_push(float px, float py, float w, float h, float band, float &dx, float &dy);

// --- shared with the window thread ---
void set_player(int player);       // which player's cursor and camera the mouse works (0 or 1: the keyboard & mouse player)
void set_enabled(bool on);         // the PC mouse setting (off: nothing is written, the follow strength goes back)
void set_pointer(bool active, float u, float v, float edge_dx, float edge_dy);  // active: the mouse is in charge now
bool in_cursor_mode();
bool in_world();
bool cas_open();                   // create-a-Sim is shown ([0x51FFB0] is its object while it is)                   // an interactor exists (live / build / buy mode); false in the main menu and other screens             // the game showed the cursor at the last refresh (player 1)
void frame();                      // emulation thread, once per refresh

// The game's interaction list (Cross on a Sim or an object opens it at the bottom right): a scrolling list whose highlight stays in
// the middle row. Owner [0x51FFB8], the list at owner + 0x740: +6 selected index (16-bit), +9 state (1 closed, 4 open and still,
// 5/6 scrolling), +0xA2 item count. A click on the middle row or the pill below it = Cross, on the row above / below = one step
// up / down; the wheel steps while it is open. These return true when the list took the input.
enum class ListSpot { none, above, middle, below };
ListSpot list_spot(float u, float v);  // where (u, v) is on the list's rows (none: not on them)
bool list_open();
bool list_click(float u, float v);
bool list_wheel(int dir);           // -1 up, +1 down
std::uint16_t injected_buttons();   // PS2 pad bits to add to the first controller now (the list's steps and picks)

}  // namespace mouse_pc
