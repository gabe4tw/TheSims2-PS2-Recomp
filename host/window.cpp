#include "window.hpp"
#include "big_stack_thread.hpp"
#include "webcam.hpp"

#include "ps2/disc.hpp"
#include "ps2/system.hpp"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
#endif
#include <SDL.h>
#include <SDL_syswm.h>
#ifdef __APPLE__
#include <SDL_metal.h>
#include <mach/mach.h>
#endif
#ifndef _WIN32
#include <sys/resource.h>
#include <unistd.h>
#endif
#ifdef __linux__
#include <csignal>
#include <cerrno>
#include <cstring>
#include <sys/prctl.h>
#include <sys/syscall.h>
#ifdef __GLIBC__
#include <execinfo.h>
#endif
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

#include "keymap.hpp"
#include "mouse_pc.hpp"
#include "card_library.hpp"
#include "remaster_ini.hpp"
#include "mod_library.hpp"
#include "settings_fill.hpp"
#include "card_page.hpp"
#include "quickmenu.hpp"
#include "menu_sound.hpp"
#include "sims2_debug_table.hpp"
#include "ps2/game_memory.hpp"
#include "ps2/crash.hpp"
#include "ps2/hitch.hpp"
#include "ps2/os.hpp"
#include "ps2/video.hpp"
#include "stretch.hpp"
#ifdef PS2_GPU
#include "../runtime/src/gs/gpu/overlay.hpp"  // the quick menu's panel in the WebGPU present pass
#endif

namespace {

enum Button : std::uint16_t {
    kSelect = 1u << 0, kL3 = 1u << 1, kR3 = 1u << 2, kStart = 1u << 3,
    kUp = 1u << 4, kRight = 1u << 5, kDown = 1u << 6, kLeft = 1u << 7,
    kL2 = 1u << 8, kR2 = 1u << 9, kL1 = 1u << 10, kR1 = 1u << 11,
    kTriangle = 1u << 12, kCircle = 1u << 13, kCross = 1u << 14, kSquare = 1u << 15,
};

struct PadBinding {
    SDL_GameControllerButton button;
    std::uint16_t mask;
};

constexpr PadBinding kPadDefaults[] = {
    {SDL_CONTROLLER_BUTTON_A, kCross},          {SDL_CONTROLLER_BUTTON_B, kCircle},
    {SDL_CONTROLLER_BUTTON_X, kSquare},         {SDL_CONTROLLER_BUTTON_Y, kTriangle},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, kL1},  {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, kR1},
    {SDL_CONTROLLER_BUTTON_LEFTSTICK, kL3},     {SDL_CONTROLLER_BUTTON_RIGHTSTICK, kR3},
    {SDL_CONTROLLER_BUTTON_BACK, kSelect},      {SDL_CONTROLLER_BUTTON_START, kStart},
    {SDL_CONTROLLER_BUTTON_DPAD_UP, kUp},       {SDL_CONTROLLER_BUTTON_DPAD_DOWN, kDown},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT, kLeft},   {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, kRight},
};

// local change: which button of the controller works which button of the game's controller can be changed in the quick menu
// ("Change controller buttons"). kPadButtons is the table in use; the first 8 and the last 4 entries can be changed, Select and
// Start (entries 8 and 9) stay. It is kept in remaster.ini as PS2_PADMAP: 12 SDL button names in the order of padmap::kRows.
PadBinding kPadButtons[std::size(kPadDefaults)] = {
    kPadDefaults[0], kPadDefaults[1], kPadDefaults[2], kPadDefaults[3], kPadDefaults[4], kPadDefaults[5], kPadDefaults[6],
    kPadDefaults[7], kPadDefaults[8], kPadDefaults[9], kPadDefaults[10], kPadDefaults[11], kPadDefaults[12], kPadDefaults[13],
};
namespace padmap {
constexpr int kCount = 12;
constexpr int kRows[kCount] = {0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 12, 13};  // entries of kPadButtons
constexpr const char *kLabels[kCount] = {"Cross", "Circle", "Square", "Triangle", "L1", "R1", "L3 (stick click)", "R3 (stick click)",
                                         "D-pad up", "D-pad down", "D-pad left", "D-pad right"};

const char *button_name(SDL_GameControllerButton b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return "A (bottom button)";
    case SDL_CONTROLLER_BUTTON_B: return "B (right button)";
    case SDL_CONTROLLER_BUTTON_X: return "X (left button)";
    case SDL_CONTROLLER_BUTTON_Y: return "Y (top button)";
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "Left shoulder";
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "Right shoulder";
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "Left stick click";
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "Right stick click";
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return "D-pad up";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "D-pad down";
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "D-pad left";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "D-pad right";
    default: {
        const char *name = SDL_GameControllerGetStringForButton(b);
        return name ? name : "?";
    }
    }
}

bool usable(SDL_GameControllerButton b) {
    return b != SDL_CONTROLLER_BUTTON_BACK && b != SDL_CONTROLLER_BUTTON_START && b != SDL_CONTROLLER_BUTTON_GUIDE && b != SDL_CONTROLLER_BUTTON_INVALID;
}

std::string text() {
    std::string out;
    for (int i = 0; i < kCount; ++i) {
        const char *name = SDL_GameControllerGetStringForButton(kPadButtons[kRows[i]].button);
        out += (i ? "," : "") + std::string(name ? name : "");
    }
    return out;
}

void defaults() {
    for (int i = 0; i < kCount; ++i) kPadButtons[kRows[i]].button = kPadDefaults[kRows[i]].button;
}

bool is_default() {
    for (int i = 0; i < kCount; ++i)
        if (kPadButtons[kRows[i]].button != kPadDefaults[kRows[i]].button) return false;
    return true;
}

// "a,b,x,y,leftshoulder,..." as text() writes it; anything that cannot be read leaves that button as it is
void read(const std::string &spec) {
    std::size_t at = 0;
    for (int i = 0; i < kCount && at <= spec.size(); ++i) {
        const std::size_t end = std::min(spec.find(',', at), spec.size());
        const SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(spec.substr(at, end - at).c_str());
        if (usable(b)) kPadButtons[kRows[i]].button = b;
        at = end + 1;
    }
}

// Gives the row this button. A row that had it gets the row's old button, so no button is lost.
void assign(int row, SDL_GameControllerButton b) {
    const SDL_GameControllerButton old = kPadButtons[kRows[row]].button;
    for (int i = 0; i < kCount; ++i)
        if (i != row && kPadButtons[kRows[i]].button == b) kPadButtons[kRows[i]].button = old;
    kPadButtons[kRows[row]].button = b;
}
}

SDL_Window *g_window = nullptr;
#ifdef __APPLE__
SDL_MetalView g_metal_view = nullptr;
#endif
SDL_Renderer *g_renderer = nullptr;
SDL_Texture *g_texture = nullptr;
SDL_AudioDeviceID g_audio = 0;
ps2sound::SoundPipe *g_pipe = nullptr;  // the game's sound: pushed by the emulation thread, played (and stretched) by the device thread, host/stretch.hpp
// the keyboard drives the first controller port along with the first game controller, a second one is player 2
SDL_GameController *g_pads[2] = {};
// local change (PS2_PAD_MERGE=1): every controller drives port 1 together, none goes to port 2. For PCs where virtual
// controllers (Steam, ViGEm, streaming tools) take the two ports before the real one.
bool g_pad_merge = false;
std::vector<SDL_GameController *> g_more;
// local change: every connected controller is opened (g_all, in the order they came), so each one can work the quick menu; which
// one plays which port is chosen in the menu (Settings > Controls > Players), kept in remaster.ini as PS2_PLAYER1 / PS2_PLAYER2:
// "auto" (the first free controller), "all" (player 1 only: every controller but player 2's), "none" (player 2 only), or a
// controller's id (its GUID and which one of that GUID it is, "030000005e0400008e02000000007200#1").
std::vector<SDL_GameController *> g_all;
std::string g_player_choice[2] = {"auto", "auto"};
std::string g_screen_res;  // local change: the quick menu's screen resolution, PS2_SCREEN_RES=WxH ("": the desktop's)
SDL_JoystickID g_last_pad = -1;  // the controller that pressed a button last (the menu's list says "pressed just now")
// local change: the webcam that is the game's EyeToy camera (Settings > Controls > EyeToy camera), kept in remaster.ini as PS2_WEBCAM:
// "none", a webcam's name, or "test" (a moving test picture). host/main.cpp plugs it in at the start (host/webcam.cpp), so a change
// is used at the next start.
std::string g_webcam_now = "none", g_webcam_next = "none";
std::vector<std::string> g_webcam_names;  // the webcams Windows had when the window opened

std::vector<std::string> webcam_ids() {
    std::vector<std::string> ids = {"none"};
    for (const std::string &name : g_webcam_names) ids.push_back(name);
    for (const std::string *keep : {&g_webcam_now, &g_webcam_next})
        if (std::find(ids.begin(), ids.end(), *keep) == ids.end()) ids.push_back(*keep);
    return ids;
}

std::string webcam_word(const std::string &id) {
    if (id == "none") return "None (unplugged)";
    if (id == "test") return "Test picture";
    if (id == "auto") return g_webcam_names.empty() ? "First webcam (none found)" : "First webcam";
    if (std::find(g_webcam_names.begin(), g_webcam_names.end(), id) == g_webcam_names.end()) return id + " (not found)";
    return id;
}

// local change: cheats and debug settings set in the quick menu (its Cheats and Debug pages). Each is a value kept in the game's memory
// on every refresh (ps2::set_forced_pokes), by address; `was` is what the game had before, put back when the player turns it off.
// None of them lasts across starts (the user's choice: a cheat or debug setting that breaks something must be gone after a restart).
// An older remaster.ini's PS2_GAME_POKES line is ignored and goes away the next time the file is written. The one way to keep
// something: the Debug page's "Keep these settings for the next start (once)" (g_keep_next) writes the Debug page's values as
// PS2_GAME_POKES_NEXT; the next start uses them from its first frame (debug settings the game reads only while it starts, such as
// skip_to_lot) and takes the line out of the file again, so the start after that is clean.
struct GameForce {
    ps2::GamePoke poke;
    std::uint32_t was = 0;
    bool keep = false;  // a Debug page value: written as PS2_GAME_POKES_NEXT while g_keep_next is on (cheats never)
};
std::map<std::uint64_t, GameForce> g_game_forced;  // key: address << 32 | mask
bool g_keep_next = false;     // the Debug page's "Keep these settings for the next start (once)"
bool g_kept_loaded = false;   // this start used PS2_GAME_POKES_NEXT (the file is written again without it)
std::uint64_t force_key(std::uint32_t addr, std::uint32_t mask) { return static_cast<std::uint64_t>(addr) << 32 | mask; }

std::string game_forced_text() {
    std::string out;
    char item[80];
    for (const auto &[key, f] : g_game_forced) {
        if (!f.keep) continue;
        std::snprintf(item, sizeof item, "%s%X:%u:%X:%X:%X", out.empty() ? "" : ";", f.poke.addr, static_cast<unsigned>(f.poke.size), f.poke.mask,
                      f.poke.value, f.was);
        out += item;
    }
    return out;
}

void push_game_forced() {
    std::vector<ps2::GamePoke> pokes;
    for (const auto &[key, f] : g_game_forced) pokes.push_back(f.poke);
    ps2::set_forced_pokes(pokes);
}

void read_game_forced(const std::string &text) {
    g_game_forced.clear();
    if (!text.empty()) std::fprintf(stderr, "[cheats] the cheat and debug values of an older remaster.ini are not used: every start begins with the game's own\n");
    // the Debug page's values kept for this one start
    const std::string next = ps2::os::env("PS2_GAME_POKES_NEXT");
    for (std::size_t at = 0; at < next.size();) {
        std::size_t end = next.find(';', at);
        if (end == std::string::npos) end = next.size();
        unsigned addr = 0, size = 0, mask = 0, value = 0, was = 0;
        if (std::sscanf(next.substr(at, end - at).c_str(), "%X:%u:%X:%X:%X", &addr, &size, &mask, &value, &was) == 5 && (size == 1 || size == 2 || size == 4)) {
            GameForce f;
            f.poke = {addr, static_cast<std::uint8_t>(size), value, mask};
            f.was = was;
            f.keep = true;
            g_game_forced[force_key(addr, mask)] = f;
        }
        at = end + 1;
    }
    if (!next.empty()) {
        g_kept_loaded = true;
        std::fprintf(stderr, "[cheats] %zu debug value%s kept for this start (once; remaster.ini is written again without them)\n", g_game_forced.size(),
                     g_game_forced.size() == 1 ? "" : "s");
    }
    push_game_forced();
}
// PS2_PAD_LOG=1 prints every change of what the two controller ports send to the game, and every rumble request
bool g_pad_log = false;
std::uint32_t g_tex_w = 0, g_tex_h = 0;
std::uint64_t g_last = 0;
std::vector<std::uint32_t> g_pixels;
bool g_gpu_present = false;

// Where the window thread is in its own loop ("asking the system for events", "drawing") and since when. Always on: two stores per
// step. The Linux watchdog prints it when no frame comes, so a window thread that is stuck in one call names the call (docs/linux.md,
// "The window freeze").
std::atomic<const char *> g_window_stage{nullptr};
std::atomic<std::int64_t> g_window_stage_since_ns{0};
void window_stage(const char *name) {
    g_window_stage_since_ns.store(ps2::hitch::now_ns(), std::memory_order_relaxed);
    g_window_stage.store(name, std::memory_order_release);
}

// --- keyboard and mouse (host/keymap.cpp, docs/keys.md) ---
//
// Which key works which button comes from keys.ini (written with the defaults on the first start, read again when the window
// gets the focus back after the file changed). The arrow keys are the D-pad only: they used to push the left stick as well, and
// a menu that reads both moved two entries for one press.
keymap::Config g_keys = keymap::defaults();
std::string g_keys_path;

// local change: the remaster options of the quick menu. They live in remaster.ini in the game folder as "PS2_NAME=value" lines,
// which host/main.cpp turns into environment variables at the start; so a change here is used when the game starts the next time.
}  // namespace (anonymous): the window's part of remaster is in ::remaster, with host/remaster_ini.cpp
namespace remaster {  // (the table and the file: host/remaster_ini.cpp)
int g_window_mode = 0;  // PS2_WINDOW_MODE: 0 windowed, 1 borderless, 2 full screen
// the bits of ps2::set_video_options that say how much to smooth colour bands
std::uint32_t deband_bits() { return static_cast<std::uint32_t>(std::clamp(g_options[5].next, 0, 2)) << 8; }

void read_now() {
    read_env();
    g_mod_owned = mods::owners_from(ps2::os::env("PS2_MOD_SETTINGS"));  // local change (launcher stage 3): host/main.cpp, --mods
}

// this run's values of the lines besides the options, then the file
void write_file() {
    Extra e;
    e.window_mode = g_window_mode;
    e.player[0] = g_player_choice[0];
    e.player[1] = g_player_choice[1];
    e.webcam = g_webcam_next;
    if (g_keep_next) e.pokes_next = game_forced_text();
    e.screen_res = g_screen_res;
    e.card_library = !ps2::os::env("PS2_CARD_LIBRARY").empty();
    e.card[0] = ps2::os::env("PS2_CARD1");
    e.card[1] = ps2::os::env("PS2_CARD2");
    if (!padmap::is_default()) e.padmap = padmap::text();
    write(ps2::os::env("PS2_REMASTER_INI"), e);
}

// left or right on an option: the next value of its list, written to the file; returns the line for the notice
std::string step(int index, int by) {
    Option &o = g_options[index];
    if (const std::string mod = mod_owner(index); !mod.empty()) return word(index, o.next) + " (set by mod " + mod + "; turn the mod off in the launcher to choose here)";  // local change
    const int n = static_cast<int>(o.values.size());
    int at = 0;
    for (int i = 0; i < n; ++i)
        if (o.values[i] == o.next) at = i;
    o.next = o.values[((at + by) % n + n) % n];
    write_file();
    return word(index, o.next) + (o.next == o.now ? "" : ": used when the game starts the next time");
}
}  // namespace remaster
namespace {
std::filesystem::file_time_type g_keys_time{};
int g_mouse_sum_x = 0, g_mouse_sum_y = 0;  // mouse counts since the last sample (only while the mouse pushes a stick)
Uint32 g_mouse_sample = 0;
double g_mouse_vx = 0.0, g_mouse_vy = 0.0;  // counts per millisecond, smoothed
Uint32 g_wheel_up_until = 0, g_wheel_down_until = 0;
int g_last_dx = 0, g_last_dy = -1;  // the last direction the walk keys gave, for a roll without one

// The picture look (F2): how the picture is enlarged and smoothed for the window (display only, the game and the saved pictures do not
// change). The choice sits in keys.ini ('look = ...'); PS2_LOOK=0..5 or a look word overrides it for a run and is not written.
int g_look = keymap::kDefaultLook;
bool g_look_forced = false;
// local change: the present's option bits: the filter of the look (bits 1-2), anti-aliasing Fast (bit 0, when this run has it: PS2_AA
// now 1), the old one-pass edge filter for side-by-side checks (bit 4, PS2_AA_OLD=1), colour bands (bits 8-9)
std::uint32_t video_bits() {
    static const bool old = !ps2::os::env("PS2_AA_OLD").empty();
    const std::uint32_t fast = remaster::g_options[remaster::kAaOption].now == 1 ? 1u : 0u;
    return (static_cast<std::uint32_t>(g_look) & ~1u) | fast | (old ? 16u : 0u) | remaster::deband_bits();
}

// The title shows the speed; for a few seconds after F5, F8 or F9 a message is put in front of it.
std::string g_speed_title = "ps2-native-recomp";
std::string g_game_title;  // optional PS2_WINDOW_TITLE, supplied by the launcher
std::string g_notice_text;
const char *const kStartNotice = "F1 = menu";  // the title for the first 30 s
Uint32 g_notice_until = 0;
// A short tone mixed into the game's sound says that a key worked, which the title cannot do in full screen. 0 none, else the kind of
// the message (1 saved, 2 loaded, 3 report written, 4 trouble). PS2_NO_BEEP=1 turns it off.
std::atomic<int> g_beep_request{0};
bool g_beep_on = true;

// The quick menu (F1, the extra menu key of keys.ini (none by default), hold Select then press Start or the Guide button on a controller;
// host/quickmenu.cpp). While it is open the
// emulation thread waits at the end of its frame (on_vblank), so the game stands still and makes no sound, and the window thread keeps
// drawing the last picture with the panel over it. The game's controller ports get nothing while it is open, and after it a port gets
// nothing until its buttons are all let go (the Enter or Cross that chose "Resume" must not reach the game).
quickmenu::Menu g_menu;
namespace pcmouse {
bool g_on = false;  // local change: the PC mouse (PS2_MOUSE_MODE; the rest of it is beside read_keyboard)
}
std::atomic<bool> g_menu_hold{false};
std::string g_key_note;  // the keys page's message line: what the last change did
bool g_menu_redraw = false;  // show the picture again: the panel changed while the game sends none
quickmenu::Picture g_menu_picture;
SDL_Texture *g_menu_texture = nullptr;  // the panel for the SDL renderer (PS2_SDL_RENDERER=1)
int g_menu_texture_w = 0, g_menu_texture_h = 0;
Uint32 g_menu_opened_ticks = 0;
bool g_block_input[2] = {};
bool g_restart_wanted = false;  // local change: "Restart the game" - run_window starts the program again once the game has ended
#ifdef _WIN32
// The environment as the program got it, taken before main() runs: main.cpp turns remaster.ini's lines into variables, and a new
// run that inherited those would take them for the launcher's own settings (which win over the file), so changes would not count.
const std::wstring g_start_environment = [] {
    std::wstring copy;
    if (wchar_t *block = GetEnvironmentStringsW()) {
        const wchar_t *end = block;
        while (*end) end += std::wcslen(end) + 1;  // the block ends with an empty string
        copy.assign(block, static_cast<std::size_t>(end - block) + 1);
        FreeEnvironmentStringsW(block);
    }
    return copy;
}();
#endif

// starts this program again with the same command line, folder and starting environment (after "Restart the game")
void start_again() {
    if (!g_restart_wanted) return;
#ifdef _WIN32
    std::wstring command = GetCommandLineW();
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> environment(g_start_environment.begin(), g_start_environment.end());
    environment.push_back(L'\0');
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
                       g_start_environment.empty() ? nullptr : environment.data(), nullptr, &si, &pi)) {
        std::fprintf(stderr, "[menu] the game starts again (process %lu)\n", static_cast<unsigned long>(pi.dwProcessId));
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        std::fprintf(stderr, "[menu] cannot start the game again (error %lu)\n", static_cast<unsigned long>(GetLastError()));
    }
#else
    std::fprintf(stderr, "[menu] restarting is only done on Windows: start the game again yourself\n");
#endif
}
// The inner resolution (PS2_SCALE): the one of this run, and the one keys.ini holds for the next start (the menu changes that one).
int g_scale_now = 1, g_scale_next = 1;
bool g_scale_locked = false;  // PS2_SCALE was set before the start: keys.ini does not count
// The filter of upscaled 2D art (PS2_SHARP_2D: 0 soft, 1 sharp, 2 sharpest), the same way: this run's and keys.ini's for the next start.
int g_sharp_now = 1, g_sharp_next = 1;
bool g_sharp_locked = false;
// A short note drawn over the picture for a moment after the menu is closed ("Saved", "Loaded"): the menu asked for a quick save or load
bool g_note_wanted = false;  // a save or load was asked by the menu: the next message about it is shown over the picture
std::string g_note_text;  // not empty while the note is up
Uint32 g_note_until = 0;
bool g_overlay_on = false;  // g_menu_picture is laid over the game's picture (the menu, or the note)
std::uint64_t g_script_extra = 0;  // the pad script's clock goes on while the menu is open (the game's frames stand still)

void refresh_title() {
    std::string title = g_speed_title;
    if (!g_notice_text.empty()) title = g_notice_text + "   |   " + title;
    if (!g_game_title.empty()) title = g_game_title + "   |   " + title;
    SDL_SetWindowTitle(g_window, title.c_str());
}

bool create_renderer() {
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    if (!g_renderer) g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    if (!g_renderer) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return false;
    }
    return true;
}

#ifdef __APPLE__
void destroy_metal_view() {
    if (!g_metal_view) return;
    SDL_Metal_DestroyView(g_metal_view);
    g_metal_view = nullptr;
}
#endif

bool attach_gpu() {
    if (std::getenv("PS2_SDL_RENDERER")) return false;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(g_window, &info)) return false;
    ps2::WindowTarget target;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_window, &w, &h);
    target.width = static_cast<std::uint32_t>(w);
    target.height = static_cast<std::uint32_t>(h);
    switch (info.subsystem) {
#if defined(__APPLE__) && defined(SDL_VIDEO_DRIVER_COCOA)
    case SDL_SYSWM_COCOA:
        g_metal_view = SDL_Metal_CreateView(g_window);
        if (!g_metal_view) return false;
        target.kind = ps2::WindowTarget::Metal;
        target.surface = SDL_Metal_GetLayer(g_metal_view);
        break;
#endif
#if defined(SDL_VIDEO_DRIVER_X11)
    case SDL_SYSWM_X11:
        target.kind = ps2::WindowTarget::X11;
        target.display = info.info.x11.display;
        target.window = static_cast<std::uint64_t>(info.info.x11.window);
        break;
#endif
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
    case SDL_SYSWM_WAYLAND:
        target.kind = ps2::WindowTarget::Wayland;
        target.display = info.info.wl.display;
        target.surface = info.info.wl.surface;
        break;
#endif
#if defined(SDL_VIDEO_DRIVER_WINDOWS)
    case SDL_SYSWM_WINDOWS:
        target.kind = ps2::WindowTarget::Win32;
        target.display = info.info.win.hinstance;
        target.surface = info.info.win.window;
        break;
#endif
    default: return false;
    }
    const bool attached = ps2::attach_window(target);
#ifdef __APPLE__
    if (!attached) destroy_metal_view();
#endif
    return attached;
}

std::uint8_t axis(SDL_GameController *pad, SDL_GameControllerAxis a) {
    const int v = SDL_GameControllerGetAxis(pad, a);
    return static_cast<std::uint8_t>(std::clamp((v + 32768) >> 8, 0, 255));
}

void rumble(int port) {
    static double last_strong[2] = {};
    static bool last_weak[2] = {};
    static Uint32 sent[2] = {};
    SDL_GameController *pad = g_pads[port];
    if (!pad) return;
    const std::uint16_t motors = ps2::pad_rumble(port);
    const double strong = (motors >> 8) < 16 ? 0.0 : std::sqrt((motors >> 8) / 255.0);
    const bool weak = motors & 1u;
    const Uint32 now = SDL_GetTicks();
    if (std::fabs(strong - last_strong[port]) <= 0.08 && weak == last_weak[port] && (!(strong > 0.0 || weak) || now - sent[port] < 100)) return;
    if (g_pad_log && (strong != last_strong[port] || weak != last_weak[port]))
        std::fprintf(stderr, "[pad] frame %llu port %d rumble: large motor %u, small motor %s\n", static_cast<unsigned long long>(ps2::frames_run()), port + 1,
                     motors >> 8, weak ? "on" : "off");
    last_strong[port] = strong;
    last_weak[port] = weak;
    sent[port] = now;
    SDL_GameControllerRumble(pad, static_cast<Uint16>(strong * 65535.0), weak ? 0xFFFF : 0, strong > 0.0 || weak ? 200 : 0);
}

const char *pad_kind(SDL_GameController *pad) {
    switch (SDL_GameControllerGetType(pad)) {
    case SDL_CONTROLLER_TYPE_XBOX360: return "Xbox 360";
    case SDL_CONTROLLER_TYPE_XBOXONE: return "Xbox One/Series";
    case SDL_CONTROLLER_TYPE_PS3: return "DualShock 3";
    case SDL_CONTROLLER_TYPE_PS4: return "DualShock 4";
    case SDL_CONTROLLER_TYPE_PS5: return "DualSense";
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: return "Switch Pro";
    case SDL_CONTROLLER_TYPE_VIRTUAL: return "virtual";
    default: return "generic";
    }
}

const char *or_unknown(const char *name) { return name ? name : "a controller"; }

SDL_JoystickID pad_instance(SDL_GameController *pad) { return SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)); }

std::string pad_guid(SDL_GameController *pad) {
    char text[64] = {};
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(SDL_GameControllerGetJoystick(pad)), text, sizeof text);
    return text;
}

// a controller's id for remaster.ini: its GUID, and which one of the controllers with that GUID it is (three virtual Xbox 360
// controllers share one GUID; their order is the order they came in)
std::string pad_id(SDL_GameController *pad) {
    const std::string guid = pad_guid(pad);
    int nth = 0;
    for (SDL_GameController *other : g_all) {
        if (other == pad) break;
        if (pad_guid(other) == guid) ++nth;
    }
    return guid + "#" + std::to_string(nth);
}

SDL_GameController *pad_by_id(const std::string &id) {
    for (SDL_GameController *pad : g_all)
        if (pad_id(pad) == id) return pad;
    return nullptr;
}

// local change: which port the keyboard and the mouse play (a player's list can choose "Keyboard & mouse"; else port 1, as always)
int keyboard_port() { return g_player_choice[1] == "keyboard" ? 1 : 0; }

// the ports from the players' choices and the controllers there are now
void assign_pads() {
    SDL_GameController *const was[2] = {g_pads[0], g_pads[1]};
    const std::string &one = g_player_choice[0], &two = g_player_choice[1];
    SDL_GameController *two_fixed = two != "auto" && two != "none" && two != "keyboard" ? pad_by_id(two) : nullptr;
    SDL_GameController *one_fixed = one != "auto" && one != "all" && one != "keyboard" ? pad_by_id(one) : nullptr;
    if (one_fixed && one_fixed == two_fixed) two_fixed = nullptr;
    const auto first_free = [&](SDL_GameController *skip) -> SDL_GameController * {
        for (SDL_GameController *pad : g_all)
            if (pad != skip && pad != two_fixed) return pad;
        return nullptr;
    };
    g_pads[0] = g_pads[1] = nullptr;
    g_more.clear();
    if (one == "all") {
        for (SDL_GameController *pad : g_all) {
            if (pad == two_fixed) continue;
            if (!g_pads[0]) g_pads[0] = pad;
            else g_more.push_back(pad);
        }
    } else if (one != "keyboard") {  // (the keyboard and mouse player gets no controller)
        g_pads[0] = one_fixed ? one_fixed : first_free(nullptr);  // (a chosen controller that is not there: the first free one)
    }
    if (two_fixed) g_pads[1] = two_fixed;
    else if (two == "auto" && one != "all") g_pads[1] = first_free(g_pads[0]);
    mouse_pc::set_player(keyboard_port());  // (the PC mouse moves that player's cursor and camera)
    std::fprintf(stderr, "[pad] the keyboard and mouse play port %d\n", keyboard_port() + 1);
    for (int port = 0; port < 2; ++port)
        if (g_pads[port] != was[port]) {
            if (g_pads[port]) std::fprintf(stderr, "[pad] %s (%s) in port %d\n", or_unknown(SDL_GameControllerName(g_pads[port])), pad_kind(g_pads[port]), port + 1);
            else std::fprintf(stderr, "[pad] no controller in port %d\n", port + 1);
        }
    if (!g_more.empty()) std::fprintf(stderr, "[pad] %zu more controller%s also drive port 1\n", g_more.size(), g_more.size() == 1 ? "" : "s");
}

// every controller is opened (each can work the quick menu); assign_pads says which ones the game gets
void open_pad(int device) {
    const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device);
    for (SDL_GameController *pad : g_all)
        if (pad_instance(pad) == id) return;
    SDL_GameController *pad = SDL_GameControllerOpen(device);
    if (!pad) {
        std::fprintf(stderr, "[pad] cannot open controller %d: %s\n", device, SDL_GetError());
        return;
    }
    g_all.push_back(pad);
    std::fprintf(stderr, "[pad] %s (%s) connected, id %s\n", or_unknown(SDL_GameControllerName(pad)), pad_kind(pad), pad_id(pad).c_str());
    assign_pads();
}

void close_pad(SDL_JoystickID id) {
    for (auto it = g_all.begin(); it != g_all.end(); ++it)
        if (pad_instance(*it) == id) {
            std::fprintf(stderr, "[pad] %s unplugged\n", or_unknown(SDL_GameControllerName(*it)));
            SDL_GameController *pad = *it;
            g_all.erase(it);
            assign_pads();  // (before the close: the ports must not hold it)
            SDL_GameControllerClose(pad);
            return;
        }
}

// --- PS2_PAD_SCRIPT: virtual controllers, to test without hardware ---
//
// PS2_PAD_SCRIPT=<file> plugs in virtual game controllers (SDL's virtual joysticks, which go through the same game controller
// code as a real pad) and plays a sequence on them, one step per line, at a video frame of the game:
//   <frame> plug <n>                          plugs in virtual controller n (0 to 3)
//   <frame> unplug <n>                        pulls it out again
//   <frame> button <n> <name> <frames>        holds a button: a b x y back guide start leftstick rightstick leftshoulder
//                                             rightshoulder dpup dpdown dpleft dpright
//   <frame> axis <n> <name> <value> <frames>  holds a stick or a trigger: leftx lefty rightx righty from -32768 to 32767 (0 is
//                                             the middle, negative is left or up), lefttrigger righttrigger from 0 to 32767
// Lines starting with # are comments. Controller input then counts while the window is in the background too, so a test
// does not depend on which window has the focus.
struct Step {
    enum Kind { kPlug, kUnplug, kButton, kAxis };
    std::uint64_t frame = 0, frames = 0;
    Kind kind = kPlug;
    int pad = 0, control = 0, value = 0;
};

struct Hold {
    std::uint64_t until = 0;
    int pad = 0, control = 0;
    bool axis = false;
};

struct VirtualPad {
    SDL_Joystick *joystick = nullptr;
    Uint16 low = 0, high = 0;
};

std::vector<Step> g_script;
std::size_t g_next_step = 0;
std::vector<Hold> g_holds;
VirtualPad g_virtual[4];

int SDLCALL virtual_rumble(void *user, Uint16 low, Uint16 high) {
    const int n = static_cast<int>(reinterpret_cast<std::intptr_t>(user));
    VirtualPad &v = g_virtual[n];
    if (low != v.low || high != v.high)
        std::fprintf(stderr, "[pad] frame %llu virtual controller %d rumbles: low %u high %u\n", static_cast<unsigned long long>(ps2::frames_run()), n, low, high);
    v.low = low;
    v.high = high;
    return 0;
}

Sint16 raw_axis(int control, int value) {
    // the game controller layer maps a trigger's whole joystick range onto 0..32767, so a trigger at rest is the joystick minimum
    if (control == SDL_CONTROLLER_AXIS_TRIGGERLEFT || control == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
        return static_cast<Sint16>(std::clamp(value * 2 - 32768, -32768, 32767));
    return static_cast<Sint16>(std::clamp(value, -32768, 32767));
}

void load_pad_script(const std::string &path) {
    std::FILE *f = ps2::os::open(path, "r");
    if (!f) {
        std::fprintf(stderr, "[pad] cannot read %s\n", path.c_str());
        return;
    }
    char line[256];
    int number = 0;
    while (std::fgets(line, sizeof line, f)) {
        ++number;
        char what[32] = {}, name[32] = {};
        unsigned long long frame = 0, frames = 0;
        int pad = 0, value = 0;
        if (line[0] == '#') continue;
        const int fields = std::sscanf(line, "%llu %31s %d %31s", &frame, what, &pad, name);
        if (fields <= 0) continue;
        Step step;
        step.frame = frame;
        step.pad = pad;
        const std::string w = what;
        bool ok = fields >= 3 && pad >= 0 && pad < 4;
        if (ok && (w == "plug" || w == "unplug")) step.kind = w == "plug" ? Step::kPlug : Step::kUnplug;
        else if (ok && w == "button" && std::sscanf(line, "%*u %*s %*d %*s %llu", &frames) == 1) {
            step.kind = Step::kButton;
            step.control = SDL_GameControllerGetButtonFromString(name);
            ok = step.control != SDL_CONTROLLER_BUTTON_INVALID;
        } else if (ok && w == "axis" && std::sscanf(line, "%*u %*s %*d %*s %d %llu", &value, &frames) == 2) {
            step.kind = Step::kAxis;
            step.control = SDL_GameControllerGetAxisFromString(name);
            step.value = value;
            ok = step.control != SDL_CONTROLLER_AXIS_INVALID;
        } else ok = false;
        if (!ok) {
            std::fprintf(stderr, "[pad] %s line %d is not a step: %s", path.c_str(), number, line);
            continue;
        }
        step.frames = frames;
        g_script.push_back(step);
    }
    std::fclose(f);
    std::stable_sort(g_script.begin(), g_script.end(), [](const Step &a, const Step &b) { return a.frame < b.frame; });
    std::fprintf(stderr, "[pad] playing %zu steps from %s on virtual controllers\n", g_script.size(), path.c_str());
}

void plug_virtual(int n, bool on) {
    VirtualPad &v = g_virtual[n];
    if (on == (v.joystick != nullptr)) return;
    if (!on) {
        const SDL_JoystickID id = SDL_JoystickInstanceID(v.joystick);
        SDL_JoystickClose(v.joystick);
        v.joystick = nullptr;
        for (int i = 0; i < SDL_NumJoysticks(); ++i)
            if (SDL_JoystickGetDeviceInstanceID(i) == id) {
                SDL_JoystickDetachVirtual(i);
                break;
            }
        return;
    }
    static const char *const kNames[4] = {"ps2run virtual controller 0", "ps2run virtual controller 1", "ps2run virtual controller 2",
                                          "ps2run virtual controller 3"};
    SDL_VirtualJoystickDesc desc;
    SDL_zero(desc);
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_DPAD_RIGHT + 1;
    desc.name = kNames[n];
    desc.userdata = reinterpret_cast<void *>(static_cast<std::intptr_t>(n));
    desc.Rumble = virtual_rumble;
    const int device = SDL_JoystickAttachVirtualEx(&desc);
    v.joystick = device >= 0 ? SDL_JoystickOpen(device) : nullptr;
    if (!v.joystick) {
        std::fprintf(stderr, "[pad] cannot attach virtual controller %d: %s\n", n, SDL_GetError());
        return;
    }
    SDL_JoystickSetVirtualAxis(v.joystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT, SDL_JOYSTICK_AXIS_MIN);
    SDL_JoystickSetVirtualAxis(v.joystick, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, SDL_JOYSTICK_AXIS_MIN);
}

// runs before SDL reads its events, which apply the values set here (and bring the plug and unplug events)
void play_pad_script() {
    static Uint32 last_ticks = SDL_GetTicks(), menu_ms = 0;
    const Uint32 ticks = SDL_GetTicks();
    if (g_menu.is_open()) {
        menu_ms += ticks - last_ticks;
        g_script_extra += menu_ms / 17;  // about one frame of the script per 1/60 s while the game waits
        menu_ms %= 17;
    }
    last_ticks = ticks;
    const std::uint64_t now = ps2::frames_run() + g_script_extra;
    for (auto h = g_holds.begin(); h != g_holds.end();) {
        if (now < h->until) {
            ++h;
            continue;
        }
        if (SDL_Joystick *j = g_virtual[h->pad].joystick) {
            if (h->axis) SDL_JoystickSetVirtualAxis(j, h->control, raw_axis(h->control, 0));
            else SDL_JoystickSetVirtualButton(j, h->control, SDL_RELEASED);
        }
        h = g_holds.erase(h);
    }
    for (; g_next_step < g_script.size() && g_script[g_next_step].frame <= now; ++g_next_step) {
        const Step &s = g_script[g_next_step];
        if (s.kind == Step::kPlug || s.kind == Step::kUnplug) {
            plug_virtual(s.pad, s.kind == Step::kPlug);
            continue;
        }
        SDL_Joystick *j = g_virtual[s.pad].joystick;
        if (!j) {
            std::fprintf(stderr, "[pad] frame %llu: virtual controller %d is not plugged in\n", static_cast<unsigned long long>(now), s.pad);
            continue;
        }
        if (s.kind == Step::kAxis) SDL_JoystickSetVirtualAxis(j, s.control, raw_axis(s.control, s.value));
        else SDL_JoystickSetVirtualButton(j, s.control, SDL_PRESSED);
        g_holds.push_back(Hold{now + s.frames, s.pad, s.control, s.kind == Step::kAxis});
    }
}

// --- the key file and the key card ---

void apply_mouse_mode() {
    // a mouse that pushes a stick needs the pointer locked in the window, else it runs into the window's edge
    const bool want = g_window && g_keys.mouse_stick != 0 && !pcmouse::g_on && !g_menu.is_open() && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS);
    if ((SDL_GetRelativeMouseMode() == SDL_TRUE) != want) SDL_SetRelativeMouseMode(want ? SDL_TRUE : SDL_FALSE);
    g_mouse_sum_x = g_mouse_sum_y = 0;
    g_mouse_vx = g_mouse_vy = 0.0;
}

void remember_keys_time() {
    std::error_code error;
    const auto t = std::filesystem::last_write_time(ps2::os::path(g_keys_path), error);
    if (!error) g_keys_time = t;
}

// Reads keys.ini; writes it with the defaults when it is missing. A line that cannot be read is logged and ignored.
void load_keys() {
    std::string text;
    bool have = false;
    if (std::FILE *f = ps2::os::open(g_keys_path, "rb")) {
        char buffer[4096];
        std::size_t n;
        while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0) text.append(buffer, n);
        std::fclose(f);
        have = true;
    } else if (std::FILE *out = ps2::os::open(g_keys_path, "w")) {
        const std::string defaults = keymap::default_file_text();
        std::fwrite(defaults.data(), 1, defaults.size(), out);
        std::fclose(out);
        std::fprintf(stderr, "[keys] wrote %s with the default keys\n", g_keys_path.c_str());
    } else {
        std::fprintf(stderr, "[keys] cannot write %s: the default keys are used and cannot be changed\n", g_keys_path.c_str());
    }
    keymap::Config config = keymap::defaults();
    std::vector<std::string> problems;
    if (have) keymap::parse(text, config, problems);
    for (const std::string &p : problems) std::fprintf(stderr, "[keys] %s, %s\n", g_keys_path.c_str(), p.c_str());
    g_keys = config;
    if (!g_look_forced && config.look >= 0) g_look = config.look;
    remaster::aa_from_look(g_look);  // local change: no PS2_AA line yet -> the old smooth-edges bit decides
    g_keys.look = g_look;
    ps2::set_video_options(video_bits());
    remember_keys_time();
    std::fprintf(stderr, "[keys] %s: %s%zu problem%s; mouse stick %s\n", g_keys_path.c_str(), have ? "read, " : "defaults, ", problems.size(),
                 problems.size() == 1 ? "" : "s", g_keys.mouse_stick == 0 ? "off" : g_keys.mouse_stick == 1 ? "left" : "right");
    apply_mouse_mode();
}

// when the window gets the focus back, and when the key card opens: pick up changes the player made to keys.ini
void reload_keys_if_changed() {
    if (g_keys_path.empty()) return;
    std::error_code error;
    const auto t = std::filesystem::last_write_time(ps2::os::path(g_keys_path), error);
    if (error || t != g_keys_time) load_keys();  // missing: written again with the defaults
}

#ifdef _WIN32
void edit_keys_file() {
    const std::wstring path = ps2::os::path(g_keys_path).wstring();
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}
#endif

// F1 (or the help key, H by default): the key card. A message box of the system. The buttons are the quick save and the quick load,
// for laptops where F5 and F8 need the Fn key.
//
// Windows and macOS: the box blocks the window thread while it is open (Windows keeps answering the window's messages inside the box;
// the game holds: the picture stands, the sound goes quiet, and it goes on where it was when the box closes).
//
// Everywhere else (Linux, the BSDs): the box runs on a thread of its own and the game keeps running behind it. SDL's box there is a
// separate program (zenity) or a window of the compositor, and the window thread must never wait for it: a box that is hidden by the
// compositor, opened on another workspace, or simply not noticed stopped the whole game for as long as it stood (the window thread
// answers the compositor, draws the pictures and takes the emulation's frames; GTA III froze 14 s that way on 2026-10-29,
// docs/linux.md "The window freeze"). The answer is taken in poll(), on the window thread.
#if !defined(_WIN32) && !defined(__APPLE__)
#define PS2_KEY_CARD_ON_A_THREAD 1
#endif

enum KeyCardChoice { kCardClose, kCardSave, kCardLoad, kCardEdit };

struct KeyCard {
    std::string text;
    std::atomic<int> state{0};  // 0 no box, 1 the box is open (its thread is in SDL_ShowMessageBox), 2 closed, the answer waits in poll()
    std::atomic<int> chosen{kCardClose};
    std::atomic<bool> failed{false};
    ps2::hitch::Clock::time_point opened{};
};
// never destroyed: a box that is still open when the program ends must not meet a destroyed object
KeyCard &g_card = *new KeyCard;
Uint32 g_focus_gained_ticks = 0;  // when the window last got the keyboard focus, for the log line when the card opens

void close_menu_if_open(const char *why);

// what happens after the box: the cursor back, the mouse mode again, the button that was chosen
void key_card_answer(int chosen) {
    SDL_ShowCursor(g_menu.is_open() ? SDL_ENABLE : SDL_DISABLE);
    apply_mouse_mode();
    if (chosen == kCardSave || chosen == kCardLoad) {
        close_menu_if_open("a button of the key card");  // the card was opened from the menu, which still holds the game
        g_note_wanted = true;
        ps2::quick_request(chosen == kCardSave ? ps2::QuickKey::save : ps2::QuickKey::load);
    }
#ifdef _WIN32
    else if (chosen == kCardEdit) edit_keys_file();
#endif
}

// the window thread, once per pass of poll(): the box on its thread has closed
void take_key_card_answer() {
#ifdef PS2_KEY_CARD_ON_A_THREAD
    if (g_card.state.load(std::memory_order_acquire) != 2) return;
    const double seconds = ps2::hitch::ms_since(g_card.opened) / 1000.0;
    if (!g_card.failed.load()) std::fprintf(stderr, "[keys] the key card was closed after %.1f s (the game ran on behind it)\n", seconds);
    const int chosen = g_card.chosen.load();
    g_card.state.store(0, std::memory_order_release);
    key_card_answer(chosen);
#endif
}

// why: the name of the key that opened it, for the log (how a card opens that nobody meant to open is the question this answers)
void show_key_card(const char *why) {
#ifdef PS2_KEY_CARD_ON_A_THREAD
    if (g_card.state.load(std::memory_order_acquire) != 0) return;  // one box at a time
#endif
    reload_keys_if_changed();
    g_card.text = keymap::card_text(g_keys);
    if (!g_keys_path.empty())
        g_card.text += "\nTo change keys: \"Change keys\" in the quick menu, or edit this file (the game reads it again when you come back):\n" + g_keys_path + "\n";
    if (g_notice_text == kStartNotice) {
        g_notice_text.clear();
        refresh_title();
    }
    const Uint32 since_focus = SDL_GetTicks() - g_focus_gained_ticks;
    std::fprintf(stderr, "[keys] the key card opens (%s; the window got the keyboard focus %s)\n", why,
                 g_focus_gained_ticks && since_focus < 3000 ? (std::to_string(since_focus) + " ms before").c_str() : "more than 3 s before or never");
    SDL_SetRelativeMouseMode(SDL_FALSE);  // the box needs the pointer
    SDL_ShowCursor(SDL_ENABLE);
    g_card.opened = ps2::hitch::Clock::now();
#ifdef PS2_KEY_CARD_ON_A_THREAD
    g_card.failed.store(false);
    g_card.chosen.store(kCardClose);
    g_card.state.store(1, std::memory_order_release);
    std::thread([] {
        SDL_MessageBoxButtonData buttons[] = {
            {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, kCardClose, "Close"},
            {0, kCardSave, "Quick save (F5)"},
            {0, kCardLoad, "Quick load (F8)"},
        };
        SDL_MessageBoxData box{};
        box.flags = SDL_MESSAGEBOX_INFORMATION;
        box.window = nullptr;  // the box is not tied to the game window, whose thread is not the one that waits for it
        box.title = "Keys (F1)";
        box.message = g_card.text.c_str();
        box.numbuttons = static_cast<int>(std::size(buttons));
        box.buttons = buttons;
        int chosen = kCardClose;
        if (SDL_ShowMessageBox(&box, &chosen) != 0) {
            std::fprintf(stderr, "[keys] cannot show the key card: %s\n", SDL_GetError());
            g_card.failed.store(true);
            chosen = kCardClose;
        }
        g_card.chosen.store(chosen);
        g_card.state.store(2, std::memory_order_release);
    }).detach();
#else
    SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, kCardClose, "Close"},
        {0, kCardSave, "Quick save (F5)"},
        {0, kCardLoad, "Quick load (F8)"},
#ifdef _WIN32
        {0, kCardEdit, "Edit keys.ini"},
#endif
    };
    SDL_MessageBoxData box{};
    box.flags = SDL_MESSAGEBOX_INFORMATION;
    box.window = g_window;
    box.title = "Keys (F1)";
    box.message = g_card.text.c_str();
    box.numbuttons = static_cast<int>(std::size(buttons));
    box.buttons = buttons;
    int chosen = kCardClose;
    if (SDL_ShowMessageBox(&box, &chosen) != 0) {
        std::fprintf(stderr, "[keys] cannot show the key card: %s\n", SDL_GetError());
        chosen = kCardClose;
    }
    key_card_answer(chosen);
#endif
}

// A stick pushed by keys: all the way in a direction; two keys make a diagonal that is as far from the middle as a straight push
// (not 1.4 times as far, which the game would take as a push past the stick's circle).
void push_stick(int dx, int dy, std::uint8_t &x, std::uint8_t &y) {
    const bool diagonal = dx != 0 && dy != 0;
    if (dx) x = diagonal ? static_cast<std::uint8_t>(0x80 + dx * 90) : (dx < 0 ? 0x00 : 0xFF);
    if (dy) y = diagonal ? static_cast<std::uint8_t>(0x80 + dy * 90) : (dy < 0 ? 0x00 : 0xFF);
}

// the mouse as a stick: how fast it moves, sampled every 16 ms (the window thread runs more often than the game's frames)
void mouse_stick_value(std::uint8_t &x, std::uint8_t &y) {
    const Uint32 now = SDL_GetTicks();
    const Uint32 dt = now - g_mouse_sample;
    if (dt >= 16) {
        g_mouse_vx = g_mouse_vx * 0.5 + 0.5 * g_mouse_sum_x / dt;
        g_mouse_vy = g_mouse_vy * 0.5 + 0.5 * g_mouse_sum_y / dt;
        g_mouse_sum_x = g_mouse_sum_y = 0;
        g_mouse_sample = now;
    }
    const auto level = [&](double v) {
        double d = v * 60.0 * g_keys.mouse_speed;
        // past the game's own dead zone at once, so slow mouse moves still turn the camera
        if (std::abs(d) > 2.0) d += std::copysign(g_keys.mouse_deadzone * 127.0, d);
        return static_cast<std::uint8_t>(std::clamp(128.0 + d, 0.0, 255.0));
    };
    x = level(g_keys.mouse_invert & 1 ? -g_mouse_vx : g_mouse_vx);
    y = level(g_keys.mouse_invert & 2 ? -g_mouse_vy : g_mouse_vy);
}

// --- local change: the PC mouse (host/mouse_pc.cpp does the game's side once per refresh) ---
// PS2_MOUSE_MODE=pc (default): the pointer stays free and shown; in the game's cursor mode the cursor follows it and the camera
// scrolls at the window's edges; left button = Cross, a short right click = Triangle, the right button held and moved = the camera
// turns (right stick x), the wheel zooms (right stick y). keys.ini's other keys are unchanged; its left/right mouse and wheel lines
// and mouse_stick are not used. PS2_MOUSE_MODE=pad: keys.ini's mouse lines as before.
namespace pcmouse {
bool g_owner = false;  // the mouse was used last (a stick or walk keys hand the cursor back to the stick)
bool g_right = false, g_left = false, g_spin = false;  // g_spin: create-a-Sim, the left button held on the Sim
Uint32 g_right_at = 0, g_left_until = 0;
int g_right_travel = 0, g_turn_sum = 0, g_tilt_sum = 0;
double g_turn_v = 0, g_tilt_v = 0;
Uint32 g_turn_sample = 0, g_triangle_until = 0, g_zoom_in_until = 0, g_zoom_out_until = 0;
constexpr float kEdgeBand = 10.0f;  // window points

// the pointer (window points) as fractions of the game's picture
void picture_fraction(int x, int y, float &u, float &v) {
    int ww = 1, wh = 1;
    SDL_GetWindowSize(g_window, &ww, &wh);
    const float aspect = ps2::display_aspect();
    const float vw = std::min(static_cast<float>(ww), wh * aspect), vh = vw / aspect;
    u = (x - (ww - vw) * 0.5f) / vw;
    v = (y - (wh - vh) * 0.5f) / vh;
}

// a left click on the open interaction list is the list's (mouse_pc::list_click)
bool list_takes(int x, int y) {
    float u, v;
    picture_fraction(x, y, u, v);
    return mouse_pc::list_click(u, v);
}

void event(const SDL_Event &e) {
    const Uint32 now = SDL_GetTicks();
    static const bool log = ps2::os::env("PS2_MOUSE_LOG") == "1";
    if (log && (e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONDOWN))
        std::fprintf(stderr, "[mouse] event %x at %d %d\n", e.type, e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x, e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y);
    if (e.type == SDL_MOUSEMOTION) {
        g_owner = true;
        if (g_right || g_spin) {
            g_right_travel += std::abs(e.motion.xrel) + std::abs(e.motion.yrel);
            g_turn_sum += e.motion.xrel;
            if (!g_spin) g_tilt_sum += e.motion.yrel;
        }
    } else if (e.type == SDL_MOUSEBUTTONDOWN) {
        g_owner = true;
        float su = 0, sv = 0;
        if (e.button.button == SDL_BUTTON_LEFT) picture_fraction(e.button.x, e.button.y, su, sv);
        if (log && e.button.button == SDL_BUTTON_LEFT)
            std::fprintf(stderr, "[mouse] left down at %.3f %.3f, create-a-Sim %d\n", su, sv, mouse_pc::cas_open());
        static const bool spin_anywhere = ps2::os::env("PS2_MOUSE_SPIN_ANYWHERE") == "1";  // (test switch: posted clicks land at the real pointer)
        if (e.button.button == SDL_BUTTON_LEFT && mouse_pc::cas_open() && (spin_anywhere || (su >= 0.3f && su <= 0.92f && sv >= 0.05f && sv <= 0.95f))) {
            // create-a-Sim: on the Sim the left button held and moved spins it (the right stick); a click without moving is Cross
            g_spin = true;
            g_right_travel = 0;
            g_right_at = now;
        } else if (e.button.button == SDL_BUTTON_LEFT && !list_takes(e.button.x, e.button.y)) {
            g_left = true;
            g_left_until = now + 60;  // a quick click still reaches the game (it reads the pad once a frame)
        }
        if (e.button.button == SDL_BUTTON_RIGHT) {
            g_right = true;
            g_right_at = now;
            g_right_travel = 0;
        }
    } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
        if (g_spin && g_right_travel < 6 && now - g_right_at < 400) g_left_until = now + 60;  // (a click on the Sim: Cross)
        g_spin = false;
        g_left = false;
    } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_RIGHT) {
        if (g_right && g_right_travel < 6 && now - g_right_at < 400) g_triangle_until = now + 120;  // a click, not a turn
        g_right = false;
    } else if (e.type == SDL_MOUSEWHEEL && e.wheel.y != 0) {
        if (mouse_pc::list_wheel(e.wheel.y > 0 ? -1 : 1)) return;  // the open interaction list steps instead of the camera zooming
        Uint32 &until = e.wheel.y > 0 ? g_zoom_in_until : g_zoom_out_until;
        until = std::max(until, now) + 120;
        if (until - now > 600) until = now + 600;
    }
}

// the mouse's share of the first controller
void pad(std::uint16_t &pressed, std::uint8_t &rx, std::uint8_t &ry) {
    const Uint32 now = SDL_GetTicks();
    if (g_left || static_cast<Sint32>(g_left_until - now) > 0) pressed |= kCross;
    pressed |= mouse_pc::injected_buttons();  // the interaction list's steps and picks
    if (static_cast<Sint32>(g_triangle_until - now) > 0) pressed |= kTriangle;
    const Uint32 dt = now - g_turn_sample;
    if (dt >= 16) {
        g_turn_v = g_turn_v * 0.5 + 0.5 * (g_right || g_spin ? g_turn_sum : 0) / static_cast<double>(dt);
        g_tilt_v = g_tilt_v * 0.5 + 0.5 * (g_right ? g_tilt_sum : 0) / static_cast<double>(dt);
        g_turn_sum = g_tilt_sum = 0;
        g_turn_sample = now;
    }
    // the right button held: moving left / right turns the camera (right stick x), up / down zooms it (right stick y) - in the
    // cursor view and in the third-person view alike; keys.ini's mouse_invert turns a direction round
    const auto stick = [](double v) {
        double d = v * 60.0 * g_keys.mouse_speed;
        if (std::abs(d) > 2.0) d += std::copysign(0.25 * 127.0, d);  // past the game's dead zone at once
        return static_cast<std::uint8_t>(std::clamp(128.0 + d, 0.0, 255.0));
    };
    if ((g_right || g_spin) && rx == 0x80) rx = stick(g_keys.mouse_invert & 1 ? -g_turn_v : g_turn_v);
    if (g_right && ry == 0x80 && std::abs(g_tilt_v) > std::abs(g_turn_v) * 0.5) ry = stick(g_keys.mouse_invert & 2 ? -g_tilt_v : g_tilt_v);
    if (ry == 0x80) {
        if (static_cast<Sint32>(g_zoom_in_until - now) > 0) ry = 0x00;
        else if (static_cast<Sint32>(g_zoom_out_until - now) > 0) ry = 0xFF;
    }
}

// a stick pushed by something else than the mouse (a controller, the walk keys): the cursor is the stick's again
void stick_seen(std::uint8_t lx, std::uint8_t ly) {
    if (std::abs(lx - 128) > 48 || std::abs(ly - 128) > 48) g_owner = false;
}

// where the pointer is over the game's picture, for mouse_pc::frame
void update() {
    if (!g_on || !g_window) return;
    // (test switch PS2_MOUSE_ANY_FOCUS=1: posted mouse messages from a script, whose window never gets the input focus)
    static const bool any_focus = ps2::os::env("PS2_MOUSE_ANY_FOCUS") == "1";
    const bool focus = any_focus || ((SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS) != 0 && SDL_GetMouseFocus() == g_window);
    int x = 0, y = 0, ww = 1, wh = 1;
    SDL_GetMouseState(&x, &y);
    SDL_GetWindowSize(g_window, &ww, &wh);
    static const bool log = ps2::os::env("PS2_MOUSE_LOG") == "1";
    static Uint32 logged = 0;
    if (log && SDL_GetTicks() - logged > 1000) {
        logged = SDL_GetTicks();
        std::fprintf(stderr, "[mouse] window %d x %d, pointer %d %d\n", ww, wh, x, y);
    }
    const float aspect = ps2::display_aspect();
    const float vw = std::min(static_cast<float>(ww), wh * aspect), vh = vw / aspect;
    const float x0 = (ww - vw) * 0.5f, y0 = (wh - vh) * 0.5f;
    const float u = std::clamp((x - x0) / vw, 0.0f, 1.0f), v = std::clamp((y - y0) / vh, 0.0f, 1.0f);
    float ex = 0, ey = 0;
    if (focus && !g_right) mouse_pc::edge_push(static_cast<float>(x), static_cast<float>(y), static_cast<float>(ww), static_cast<float>(wh), kEdgeBand, ex, ey);
    mouse_pc::set_pointer(g_owner && focus && !g_menu.is_open(), u, v, ex, ey);
}
}  // namespace pcmouse

// the keyboard and the mouse as the first controller
void read_keyboard(std::uint16_t &pressed, std::uint8_t &lx, std::uint8_t &ly, std::uint8_t &rx, std::uint8_t &ry) {
    static const std::uint16_t kDpad[] = {kUp, kDown, kLeft, kRight};
    const Uint8 *keys = SDL_GetKeyboardState(nullptr);
    // mouse buttons only count while the window is the one in front (a click on another window is not for the game)
    const bool focus = !g_window || (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS);
    const Uint32 mouse = focus ? SDL_GetMouseState(nullptr, nullptr) : 0;
    const Uint32 now = SDL_GetTicks();
    const auto down = [&](int input) {
        if (pcmouse::g_on && (input == keymap::kMouseLeft || input == keymap::kMouseRight || input == keymap::kWheelUp || input == keymap::kWheelDown))
            return false;  // the PC mouse has these (pcmouse::pad)
        switch (input) {
        case keymap::kMouseLeft: return (mouse & SDL_BUTTON_LMASK) != 0;
        case keymap::kMouseMiddle: return (mouse & SDL_BUTTON_MMASK) != 0;
        case keymap::kMouseRight: return (mouse & SDL_BUTTON_RMASK) != 0;
        case keymap::kMouseX1: return (mouse & SDL_BUTTON_X1MASK) != 0;
        case keymap::kMouseX2: return (mouse & SDL_BUTTON_X2MASK) != 0;
        case keymap::kWheelUp: return static_cast<Sint32>(g_wheel_up_until - now) > 0;
        case keymap::kWheelDown: return static_cast<Sint32>(g_wheel_down_until - now) > 0;
        default: return input > 0 && input < SDL_NUM_SCANCODES && keys[input] != 0;
        }
    };
    const auto any = [&](int action) {
        for (int input : g_keys.inputs[action])
            if (down(input)) return true;
        return false;
    };
    for (int i = 0; i < 4; ++i)
        if (any(keymap::DpadUp + i)) pressed |= kDpad[i];
    static const struct {
        int action;
        std::uint16_t mask;
    } kButtons[] = {
        {keymap::Cross, kCross}, {keymap::Circle, kCircle}, {keymap::Square, kSquare}, {keymap::Triangle, kTriangle},
        {keymap::L1, kL1},       {keymap::R1, kR1},         {keymap::L2, kL2},         {keymap::R2, kR2},
        {keymap::L3, kL3},       {keymap::R3, kR3},         {keymap::Start, kStart},   {keymap::Select, kSelect},
    };
    for (const auto &b : kButtons)
        if (any(b.action)) pressed |= b.mask;
    if (any(keymap::Rage)) pressed |= kL3 | kR3;
    // two opposite keys together cancel each other
    const int ldx = (any(keymap::LeftRight) ? 1 : 0) - (any(keymap::LeftLeft) ? 1 : 0);
    const int ldy = (any(keymap::LeftDown) ? 1 : 0) - (any(keymap::LeftUp) ? 1 : 0);
    const int rdx = (any(keymap::RightRight) ? 1 : 0) - (any(keymap::RightLeft) ? 1 : 0);
    const int rdy = (any(keymap::RightDown) ? 1 : 0) - (any(keymap::RightUp) ? 1 : 0);
    push_stick(ldx, ldy, lx, ly);
    push_stick(rdx, rdy, rx, ry);
    if (ldx || ldy) {
        g_last_dx = ldx;
        g_last_dy = ldy;
    }
    // The roll: God of War's evade is a push of the right stick. While the roll key is held the walk keys push it too (the left
    // stick keeps its push, so Kratos faces the way he rolls); held with no walk key, it rolls the way he was last walking.
    if (any(keymap::Roll)) {
        const bool walking = ldx || ldy;
        push_stick(walking ? ldx : g_last_dx, walking ? ldy : g_last_dy, rx, ry);
    }
    if (pcmouse::g_on) pcmouse::pad(pressed, rx, ry);
    if (g_keys.mouse_stick && !pcmouse::g_on) {
        std::uint8_t mx = 0x80, my = 0x80;
        mouse_stick_value(mx, my);
        std::uint8_t &sx = g_keys.mouse_stick == 1 ? lx : rx, &sy = g_keys.mouse_stick == 1 ? ly : ry;
        if (sx == 0x80) sx = mx;
        if (sy == 0x80) sy = my;
    }
}

void poll_port(int port) {
    static bool plugged[2] = {};
    SDL_GameController *pad = g_pads[port];
    const bool keys_here = port == keyboard_port();
    if (port > 0 && plugged[port] != (pad != nullptr || keys_here)) {
        plugged[port] = pad != nullptr || keys_here;
        ps2::set_pad_connected(port, plugged[port]);
    }
    std::uint16_t pressed = 0;
    std::uint8_t l2 = 0xFF, r2 = 0xFF;
    std::uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    if (keys_here) read_keyboard(pressed, lx, ly, rx, ry);
    if (pad) {
        for (const PadBinding &b : kPadButtons)
            if (b.button != SDL_CONTROLLER_BUTTON_BACK && SDL_GameControllerGetButton(pad, b.button)) pressed |= b.mask;
        // Select, then Start opens the quick menu, and the game must get neither. So the game gets this Select late: once it has been
        // held for 300 ms without Start, or as a short press when it is let go without Start. (Start first and then Select would
        // already have reached the game: the way is Select first.) A Select of the keyboard (Tab) is not held back.
        static Uint32 select_since[2] = {}, select_tap_until[2] = {};
        static bool select_combo[2] = {};
        const bool sel = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK), st = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START);
        const Uint32 now = SDL_GetTicks();
        bool select_to_game = false;
        if (sel) {
            if (!select_since[port]) select_since[port] = now ? now : 1;
            if (st) select_combo[port] = true;
            select_to_game = !select_combo[port] && now - select_since[port] >= 300;
        } else {
            if (select_since[port] && !select_combo[port]) select_tap_until[port] = now + 80;
            select_since[port] = 0;
            select_combo[port] = false;
        }
        // local change: Select goes to the game at once again (it was held back to tell Select + Start, the old way to open the
        // quick menu, from a plain Select). Both sticks pressed in open the menu now, and the game gets neither of the two.
        (void)select_to_game;
        (void)select_tap_until;
        if (sel) pressed |= kSelect;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSTICK) && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) pressed &= ~(kL3 | kR3);
        const auto trigger = [&](SDL_GameControllerAxis a, std::uint16_t bit, std::uint8_t &pressure) {
            const int v = SDL_GameControllerGetAxis(pad, a);
            if (v <= 2000 || (pressed & bit)) return;
            pressed |= bit;
            pressure = static_cast<std::uint8_t>(std::max(1, v * 255 / 32767));
        };
        trigger(SDL_CONTROLLER_AXIS_TRIGGERLEFT, kL2, l2);
        trigger(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, kR2, r2);
        if (lx == 0x80) lx = axis(pad, SDL_CONTROLLER_AXIS_LEFTX);
        if (ly == 0x80) ly = axis(pad, SDL_CONTROLLER_AXIS_LEFTY);
        if (rx == 0x80) rx = axis(pad, SDL_CONTROLLER_AXIS_RIGHTX);
        if (ry == 0x80) ry = axis(pad, SDL_CONTROLLER_AXIS_RIGHTY);
    }
    if (port == 0)
        for (SDL_GameController *more : g_more) {
            for (const PadBinding &b : kPadButtons)
                if (SDL_GameControllerGetButton(more, b.button)) pressed |= b.mask;
            if (SDL_GameControllerGetAxis(more, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 2000) pressed |= kL2;
            if (SDL_GameControllerGetAxis(more, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 2000) pressed |= kR2;
            if (lx == 0x80) lx = axis(more, SDL_CONTROLLER_AXIS_LEFTX);
            if (ly == 0x80) ly = axis(more, SDL_CONTROLLER_AXIS_LEFTY);
            if (rx == 0x80) rx = axis(more, SDL_CONTROLLER_AXIS_RIGHTX);
            if (ry == 0x80) ry = axis(more, SDL_CONTROLLER_AXIS_RIGHTY);
        }
    if (keys_here && pcmouse::g_on) pcmouse::stick_seen(lx, ly);
    if (g_menu.is_open()) g_block_input[port] = true;
    if (g_block_input[port]) {
        if (!pressed) g_block_input[port] = false;  // all let go: from the next pass on the game gets this port again
        pressed = 0;
        lx = ly = rx = ry = 0x80;
        l2 = r2 = 0xFF;
    } else {
        rumble(port);  // while the game waits its rumble is not sent again, so it stops after 200 ms
    }
    static std::uint64_t last[2] = {~0ull, ~0ull};
    const std::uint64_t state = pressed | (static_cast<std::uint64_t>(rx) << 16) | (static_cast<std::uint64_t>(ry) << 24) | (static_cast<std::uint64_t>(lx) << 32) |
                                (static_cast<std::uint64_t>(ly) << 40) | (static_cast<std::uint64_t>(l2) << 48) | (static_cast<std::uint64_t>(r2) << 56);
    if (state == last[port]) return;
    last[port] = state;
    if (g_pad_log) {
        static const char *const kNames[16] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                               "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};
        std::string names;
        for (int i = 0; i < 16; ++i)
            if (pressed & (1u << i)) names += std::string(" ") + kNames[i];
        std::fprintf(stderr, "[pad] frame %llu port %d buttons %04X%s, left stick %u %u, right stick %u %u, l2 %u r2 %u\n",
                     static_cast<unsigned long long>(ps2::frames_run()), port + 1, pressed, names.empty() ? " none" : names.c_str(), lx, ly, rx, ry,
                     pressed & kL2 ? l2 : 0, pressed & kR2 ? r2 : 0);
    }
    ps2::set_pad_pressure(8, l2, port);
    ps2::set_pad_pressure(9, r2, port);
    ps2::set_pad_buttons(pressed, port);
    ps2::set_pad_axis(0, rx, port);
    ps2::set_pad_axis(1, ry, port);
    ps2::set_pad_axis(2, lx, port);
    ps2::set_pad_axis(3, ly, port);
}

// keys.ini with some lines changed (`change` maps the file's text to the new text, e.g. with keymap::with_look), the other lines as they are
void rewrite_keys_file(const std::function<std::string(const std::string &)> &change, const char *what) {
    if (g_keys_path.empty()) return;
    std::string text;
    if (std::FILE *f = ps2::os::open(g_keys_path, "rb")) {
        char buffer[4096];
        std::size_t n;
        while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0) text.append(buffer, n);
        std::fclose(f);
    } else {
        text = keymap::default_file_text();
    }
    text = change(text);
    if (std::FILE *out = ps2::os::open(g_keys_path, "wb")) {
        std::fwrite(text.data(), 1, text.size(), out);
        std::fclose(out);
        remember_keys_time();
    } else {
        std::fprintf(stderr, "[keys] cannot write %s: the %s is kept until the game closes\n", g_keys_path.c_str(), what);
    }
}

// F2: the next look (step -1: the one before, from the quick menu). The title says which one, and the choice goes into keys.ini (the
// other lines of the file stay as they are).
void set_look(int look) {
    g_look = (look % keymap::kLookCount + keymap::kLookCount) % keymap::kLookCount;
    g_keys.look = g_look;
    ps2::set_video_options(video_bits());
    g_notice_text = std::string("Picture look (F2): ") + keymap::look_name(g_look);
    g_notice_until = SDL_GetTicks() + 4000;
    refresh_title();
    std::fprintf(stderr, "[video] picture look %d: %s\n", g_look, keymap::look_name(g_look));
    if (g_look_forced) return;
    rewrite_keys_file([](const std::string &t) { return keymap::with_look(t, g_look); }, "look");
}
void cycle_look(int step = 1) { set_look(g_look + 2 * step); }  // local change: the filters only (bit 0 is anti-aliasing Fast's old place)
// local change: anti-aliasing (remaster.ini PS2_AA). Off and Fast switch at once when this run started with one of them; Quality and
// Extra need the next start. keys.ini's look keeps the old smooth-edges bit in step with Fast, for older runners.
void set_aa_notice() {
    const remaster::Option &o = remaster::g_options[remaster::kAaOption];
    if (o.now <= 1 && o.next <= 1) remaster::g_options[remaster::kAaOption].now = o.next;
    ps2::set_video_options(video_bits());
    const int look = g_look / 2 * 2 + (o.next == 1 ? 1 : 0);
    if (look != g_look && !g_look_forced) {
        g_look = g_keys.look = look;
        rewrite_keys_file([](const std::string &t) { return keymap::with_look(t, g_look); }, "look");
    }
    std::fprintf(stderr, "[video] anti-aliasing %s (this run: %s)\n", remaster::word(remaster::kAaOption, o.next).c_str(), remaster::word(remaster::kAaOption, o.now).c_str());
}

// --- the quick menu ---

// local change: three window modes (the quick menu's Window entry, kept in remaster.ini as PS2_WINDOW_MODE):
// 0 windowed, 1 borderless (a window without a frame that covers the desktop), 2 full screen (the screen is given to the game,
// at the desktop's resolution and refresh rate). F11 switches between windowed and the last mode that was not windowed.
int g_window_mode_wanted = 1;  // the mode F11 goes to, and the one written to the file while the window is windowed by F11
int window_mode() {
    const Uint32 flags = SDL_GetWindowFlags(g_window);
    if ((flags & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP) return 1;
    return flags & SDL_WINDOW_FULLSCREEN ? 2 : 0;
}
const char *window_mode_word(int mode) { return mode == 1 ? "borderless" : mode == 2 ? "full screen" : "windowed"; }
// local change: the screen resolution of the quick menu (PS2_SCREEN_RES=WxH in remaster.ini, empty: the desktop's). Windowed it is the
// window's size; in true full screen the screen mode; a borderless window always covers the desktop.
// (g_screen_res: declared beside g_player_choice)

// the screen's modes, largest first, each size once; [0] is the desktop
std::vector<std::pair<int, int>> screen_sizes() {
    std::vector<std::pair<int, int>> out;
    const int display = std::max(0, SDL_GetWindowDisplayIndex(g_window));
    SDL_DisplayMode desktop{};
    if (SDL_GetDesktopDisplayMode(display, &desktop) == 0) out.push_back({desktop.w, desktop.h});
    for (int i = 0, n = SDL_GetNumDisplayModes(display); i < n; ++i) {
        SDL_DisplayMode m{};
        if (SDL_GetDisplayMode(display, i, &m) != 0 || m.w < 640 || m.h < 480) continue;
        if (std::find(out.begin(), out.end(), std::make_pair(m.w, m.h)) == out.end()) out.push_back({m.w, m.h});
    }
    if (out.size() > 1) std::sort(out.begin() + 1, out.end(), [](const auto &a, const auto &b) { return a.first * a.second > b.first * b.second; });
    return out;
}

bool chosen_size(int &w, int &h) {
    return !g_screen_res.empty() && std::sscanf(g_screen_res.c_str(), "%dx%d", &w, &h) == 2 && w >= 320 && h >= 240;
}

int window_mode();
void apply_screen_res() {
    int w = 0, h = 0;
    if (!chosen_size(w, h)) return;
    const int mode = window_mode();
    if (mode == 0) {
        SDL_SetWindowSize(g_window, w, h);
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    } else if (mode == 2) {
        SDL_DisplayMode want{}, got{};
        const int display = std::max(0, SDL_GetWindowDisplayIndex(g_window));
        SDL_GetDesktopDisplayMode(display, &want);
        want.w = w;
        want.h = h;
        if (SDL_GetClosestDisplayMode(display, &want, &got)) {
            SDL_SetWindowDisplayMode(g_window, &got);
            SDL_SetWindowFullscreen(g_window, 0);
            SDL_SetWindowFullscreen(g_window, SDL_WINDOW_FULLSCREEN);
        }
    }
    std::fprintf(stderr, "[window] screen resolution %dx%d (%s)\n", w, h, mode == 0 ? "the window's size" : mode == 2 ? "full screen mode" : "borderless: the desktop's");
}

void set_window_mode(int mode) {
    if (mode == 2) {
        SDL_DisplayMode desktop;
        const int display = std::max(0, SDL_GetWindowDisplayIndex(g_window));
        if (SDL_GetDesktopDisplayMode(display, &desktop) == 0) {
            int w = 0, h = 0;
            SDL_DisplayMode got{};
            if (chosen_size(w, h)) {  // local change: the quick menu's screen resolution
                desktop.w = w;
                desktop.h = h;
                if (SDL_GetClosestDisplayMode(display, &desktop, &got)) desktop = got;
            }
            SDL_SetWindowDisplayMode(g_window, &desktop);
        }
        if (window_mode() == 1) SDL_SetWindowFullscreen(g_window, 0);  // from borderless: through windowed, or SDL keeps the borderless one
        if (SDL_SetWindowFullscreen(g_window, SDL_WINDOW_FULLSCREEN) != 0) {
            std::fprintf(stderr, "[window] full screen is not possible here (%s): borderless instead\n", SDL_GetError());
            SDL_SetWindowFullscreen(g_window, SDL_WINDOW_FULLSCREEN_DESKTOP);
        }
    } else {
        SDL_SetWindowFullscreen(g_window, mode == 1 ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    }
    if (mode != 0) g_window_mode_wanted = mode;
}
void toggle_fullscreen() { set_window_mode(window_mode() == 0 ? g_window_mode_wanted : 0); }

void window_pixels(int &w, int &h) {
    w = h = 0;
    if (g_renderer) SDL_GetRendererOutputSize(g_renderer, &w, &h);
    else SDL_GetWindowSizeInPixels(g_window, &w, &h);
}

// local change: the choices of the players' lists, as the ids kept in remaster.ini: "auto", then "all" (player 1) or "none"
// (player 2), then every connected controller, then the chosen one when it is not connected
std::vector<std::string> player_ids(int player) {
    std::vector<std::string> ids = {"auto", player ? "none" : "all", "keyboard"};
    for (SDL_GameController *pad : g_all) ids.push_back(pad_id(pad));
    if (std::find(ids.begin(), ids.end(), g_player_choice[player]) == ids.end()) ids.push_back(g_player_choice[player]);
    return ids;
}

std::string player_word(int player, const std::string &id) {
    if (id == "auto") return player ? "Automatic (the next free controller)" : "Automatic (the first controller)";
    if (id == "all") return "All controllers";
    if (id == "none") return "Nobody";
    if (id == "keyboard") return "Keyboard & mouse";
    for (std::size_t n = 0; n < g_all.size(); ++n)
        if (pad_id(g_all[n]) == id) {
            SDL_GameController *pad = g_all[n];
            std::string word = std::to_string(n + 1) + ": " + or_unknown(SDL_GameControllerName(pad));
            if (SDL_GameControllerGetType(pad) == SDL_CONTROLLER_TYPE_VIRTUAL) word += " (virtual)";
            if (pad_instance(pad) == g_last_pad) word += "   < pressed just now";
            return word;
        }
    return "A controller that is not connected";
}

// --- local change: the Cheats and Debug pages of the quick menu (The Sims 2 PAL SLES-53718) ---
//
// The game's button cheats (entered in the pause menu's relationship panel; matcher at 0x103220): a table of 11 codes in the main
// object 0x5201D0 (+0x2, 12 bytes each). A matched code calls 0x142428, which flips bit n of the halfword at +0x9C (0x52026C): the cheat
// is then on the gnome on the lot (bit 9 is the credits' team photo). Code 10 only plays a horn sound: the menu asks for the same call,
// 0x142428(object, 10), at the next moment every game thread waits (ps2::request_game_call). The
// "hidden" cheats are the game's own debug settings that act like cheats (host/sims2_debug_table.hpp). enable_all_player_cheats
// (+0x3FC) is read by 0x142428: with it every gnome cheat counts as on.
namespace gamemenu {

constexpr std::uint32_t kCheatBits = 0x52026C;

struct GnomeCheat {
    const char *label, *code, *help;
};
constexpr GnomeCheat kGnomeCheats[10] = {
    {"Cheat gnome", "L1, R1, Up, Cross, R2", "Puts the cheat gnome on your lot. The other gnome cheats are used through it: select the gnome."},
    {"Give 9,999 Simoleons", "R1, L1, R2, Right, Left", "Adds 9,999 Simoleons to the household each time it is chosen on the gnome."},
    {"Max all motives", "Up, Circle, Up, Right, L2", "Fills every motive of the household's Sims (not the room)."},
    {"Unlock all objects", "L2, Circle, Down, Left, Up", "Every object in the buy catalogue."},
    {"Unlock all lots", "Circle, L2, Left, Circle, Up, Circle", "Every location of the story mode."},
    {"Unlock all clothing", "Square, R2, Down, Right, Square", "Every piece of clothing in Create-a-Sim."},
    {"Advance 6 hours", "Circle, Square, L1, Up, Down", "Moves the clock six hours on."},
    {"Change skill levels", "Triangle, Circle, Square, R2, Left", "Sets the selected Sim's skills to any level from 0 to 10."},
    {"Unlock all recipes", "R2, Square, Up, Down, Right, Cross", "Every recipe in the recipe book."},
    {"Team photo in the credits", "Right, Down, Right, Down, Right", "Shows the development team's photo sliding by behind the credits (main menu > credits)."},
};
constexpr int kGnomeCount = static_cast<int>(sizeof kGnomeCheats / sizeof kGnomeCheats[0]);

struct HiddenCheat {
    const char *setting, *label;
};
constexpr HiddenCheat kHiddenCheats[] = {
    {"enable_all_player_cheats", "All gnome cheats at once"},
    {"god_mode", "God mode"},
    {"enable_free_items", "Free items"},
    {"enable_object_catalog", "Every object in build / buy"},
    {"enable_clothing_catalog", "Every piece of clothing in Create-a-Sim"},
    {"enable_unlock_all_lots", "All lots unlocked"},
    {"enable_unlock_all_lot_shopping_items", "All shopping items of the lot"},
    {"enable_freeplay_available", "Freeplay unlocked"},
    {"enable_unlock_all_xam_items", "All ringtones and phone skins"},
    {"enable_all_objects_moveable", "Move any object"},
    {"enable_build_past_fire_code", "Build past the fire code"},
    {"disable_player_inventory_requirements", "No inventory needed for interactions"},
    {"enable_edith_debug_interactions", "Debug interactions"},
    {"display_motive_values", "Show motives as numbers"},
};

const sims2::DebugSetting *setting(const char *name) {
    for (const sims2::DebugSetting &s : sims2::kDebugSettings)
        if (std::strcmp(s.name, name) == 0) return &s;
    return nullptr;
}

// what a row of the page stands for
struct Ref {
    int gnome = -1;                          // a gnome cheat bit
    const sims2::DebugSetting *s = nullptr;  // a debug setting
    bool horn = false;                       // the horn (a game call)
    int effect = -1;                         // a cheat done directly: 0 money, 1 the clock
    bool keep_next = false;                  // the Debug page's "Keep these settings for the next start (once)"
};
std::vector<Ref> g_refs;  // the rows of the page last filled in

int size_of(int type) { return type == 3 ? 1 : 4; }

std::uint32_t read_value(std::uint32_t addr, int size) {
    std::uint32_t v = 0;
    ps2::read_game_memory(addr, &v, static_cast<std::uint32_t>(size));
    return v;
}

std::string pretty(const char *name) {
    std::string out = name;
    for (char &c : out)
        if (c == '_') c = ' ';
    if (!out.empty()) out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

std::string number_text(const sims2::DebugSetting &s, std::uint32_t v) {
    char t[32];
    if (s.type == 7) {
        float f = 0;
        std::memcpy(&f, &v, 4);
        std::snprintf(t, sizeof t, "%.1f", f);
    } else if (s.type == 3) {
        if (v == 255) return "off (255)";
        std::snprintf(t, sizeof t, "%u", v);
    } else {
        std::snprintf(t, sizeof t, "%d", static_cast<int>(v));
    }
    return t;
}

bool is_forced(std::uint32_t addr, std::uint32_t mask) { return g_game_forced.count(force_key(addr, mask)) != 0; }

quickmenu::ListRow setting_row(const sims2::DebugSetting &s, const char *label) {
    quickmenu::ListRow r;
    const std::uint32_t v = read_value(s.addr, size_of(s.type));
    r.label = label ? label : pretty(s.name);
    r.help = std::string(s.help) + (*s.help ? "  " : "") + "(" + s.name + ")";
    if (s.type == 2) {
        r.checkbox = true;
        r.on = v != 0;
    } else {
        r.number = true;
        r.value = number_text(s, v);
    }
    r.changed = is_forced(s.addr, 0xFFFFFFFFu);
    return r;
}

void fill(bool cheats, std::vector<quickmenu::ListRow> &rows) {
    rows.clear();
    g_refs.clear();
    const auto header = [&](const char *text) {
        quickmenu::ListRow r;
        r.header = true;
        r.label = text;
        rows.push_back(r);
        g_refs.push_back({});
    };
    if (cheats) {
        header("Gnome cheats  (the game's button codes)");
        const std::uint32_t bits = read_value(kCheatBits, 2);
        for (int i = 0; i < kGnomeCount; ++i) {
            quickmenu::ListRow r;
            if (i == 1 || i == 6) {  // money and the clock: done at once, as often as pressed (not through the gnome)
                r.action = true;
                r.label = kGnomeCheats[i].label;
                r.value = i == 1 ? "Give" : "+6 h";
                r.help = std::string(i == 1 ? "Adds 9,999 Simoleons to the household, each press." : "Moves the clock six hours on, each press.") +
                         "  (Only on a lot.)  Code: " + kGnomeCheats[i].code + ".";
                rows.push_back(r);
                Ref ref;
                ref.effect = i == 1 ? 0 : 1;
                g_refs.push_back(ref);
                continue;
            }
            r.checkbox = true;
            r.on = (bits >> i) & 1;
            r.label = kGnomeCheats[i].label;
            r.value = kGnomeCheats[i].code;
            r.help = std::string(kGnomeCheats[i].help) + "  Code: " + kGnomeCheats[i].code + ".";
            r.changed = is_forced(kCheatBits, 1u << i);
            rows.push_back(r);
            g_refs.push_back({i, nullptr});
        }
        {
            quickmenu::ListRow r;
            r.action = true;
            r.label = "Horn";
            r.value = "Play";
            r.help = "Plays the game's horn sound once, when you go back to the game.  Code: R1, L1, R1, L1, Triangle.";
            rows.push_back(r);
            Ref ref;
            ref.horn = true;
            g_refs.push_back(ref);
        }
        header("Hidden cheats  (left in the game by its developers)");
        for (const HiddenCheat &h : kHiddenCheats)
            if (const sims2::DebugSetting *s = setting(h.setting)) {
                rows.push_back(setting_row(*s, h.label));
                g_refs.push_back({-1, s});
            }
        return;
    }
    {
        quickmenu::ListRow r;
        r.checkbox = true;
        r.on = g_keep_next;
        r.label = "Keep these settings for the next start (once)";
        r.help = "For settings the game reads only while it starts (skip to lot, sequence lots...): the values changed here are used on the "
                 "next start too, then forgotten, so the start after that is clean again.";
        rows.push_back(r);
        Ref ref;
        ref.keep_next = true;
        g_refs.push_back(ref);
    }
    header("On / off");
    for (const sims2::DebugSetting &s : sims2::kDebugSettings)
        if (s.type == 2) {
            rows.push_back(setting_row(s, nullptr));
            g_refs.push_back({-1, &s});
        }
    header("Numbers");
    for (const sims2::DebugSetting &s : sims2::kDebugSettings)
        if (s.type != 2) {
            rows.push_back(setting_row(s, nullptr));
            g_refs.push_back({-1, &s});
        }
}

// keeps addr (the bits of mask) at value from now on; back to what the game had when the value is that again
void force(std::uint32_t addr, int size, std::uint32_t mask, std::uint32_t value, bool keep) {
    const std::uint64_t key = force_key(addr, mask);
    const auto it = g_game_forced.find(key);
    const std::uint32_t was = it != g_game_forced.end() ? it->second.was : (read_value(addr, size) & mask);
    if ((value & mask) == was) {
        if (it != g_game_forced.end()) g_game_forced.erase(it);
        std::uint32_t now = read_value(addr, size);
        now = (now & ~mask) | was;
        ps2::write_game_memory(addr, &now, static_cast<std::uint32_t>(size));
    } else {
        GameForce f;
        f.poke = {addr, static_cast<std::uint8_t>(size), value & mask, mask};
        f.was = was;
        f.keep = keep;
        g_game_forced[key] = f;
    }
    push_game_forced();
    remaster::write_file();
}

void toggle(int row) {
    if (row < 0 || row >= static_cast<int>(g_refs.size())) return;
    const Ref &ref = g_refs[row];
    if (ref.keep_next) {
        g_keep_next = !g_keep_next;
        remaster::write_file();
        g_menu.set_status(g_keep_next ? "The settings changed on this page are used on the next start too (once)." : "Nothing is kept for the next start.");
        std::fprintf(stderr, "[cheats] keep debug values for the next start: %s\n", g_keep_next ? "on" : "off");
        return;
    }
    if (ref.effect >= 0) {
        // the household's globals (found by comparing saves): [0x518690] is the lot's global block, +0x74 money (32-bit), +0x14
        // the hour (16-bit, 0 to 23); 0 there when no lot is loaded
        const std::uint32_t globals = read_value(0x518690, 4);
        if (globals < 0x100000u || globals + 0x80u > 0x2000000u) {
            g_menu.set_status("Only on a lot: load a household first.");
            return;
        }
        if (ref.effect == 0) {
            const std::int32_t money = static_cast<std::int32_t>(read_value(globals + 0x74, 4));
            const std::int32_t more = static_cast<std::int32_t>(std::min<std::int64_t>(static_cast<std::int64_t>(money) + 9999, 999999999));
            ps2::write_game_memory(globals + 0x74, &more, 4);
            g_menu.set_status("Simoleons: " + std::to_string(money) + " -> " + std::to_string(more));
            std::fprintf(stderr, "[cheats] money %d -> %d\n", money, more);
        } else {
            const std::uint16_t hour = static_cast<std::uint16_t>(read_value(globals + 0x14, 2));
            const std::uint16_t later = static_cast<std::uint16_t>((hour % 24 + 6) % 24);
            ps2::write_game_memory(globals + 0x14, &later, 2);
            g_menu.set_status("The clock is now " + std::to_string(later) + " o'clock (was " + std::to_string(hour) + ").");
            std::fprintf(stderr, "[cheats] hour %u -> %u\n", static_cast<unsigned>(hour), static_cast<unsigned>(later));
        }
        return;
    }
    if (ref.horn) {
        ps2::request_game_call(0x142428, 0x5201D0, 10);  // as the matcher does for code 10 (runs when the game goes on)
        g_menu.set_status("The horn plays when you go back to the game.");
        std::fprintf(stderr, "[cheats] horn asked for\n");
        return;
    }
    if (ref.gnome >= 0) {
        const std::uint32_t bit = 1u << ref.gnome;
        const std::uint32_t now = read_value(kCheatBits, 2);
        force(kCheatBits, 2, bit, (now & bit) ? 0 : bit, false);
        std::fprintf(stderr, "[cheats] %s %s\n", kGnomeCheats[ref.gnome].label, (now & bit) ? "off" : "on");
        return;
    }
    if (!ref.s || ref.s->type != 2) return;
    const std::uint32_t now = read_value(ref.s->addr, 4);
    force(ref.s->addr, 4, 0xFFFFFFFFu, now ? 0 : 1, g_menu.page() == quickmenu::Page::debug);  // (cheats: this run only)
    std::fprintf(stderr, "[cheats] %s %s\n", ref.s->name, now ? "off" : "on");
}

void step(int row, int by) {
    if (row < 0 || row >= static_cast<int>(g_refs.size()) || !g_refs[row].s) return;
    const sims2::DebugSetting &s = *g_refs[row].s;
    const int size = size_of(s.type);
    std::uint32_t v = read_value(s.addr, size);
    if (s.type == 7) {
        float f = 0;
        std::memcpy(&f, &v, 4);
        const float a = std::fabs(f), d = a < 10 ? 1.0f : a < 100 ? 5.0f : 50.0f;
        f = std::max(0.0f, f + by * d);
        std::memcpy(&v, &f, 4);
    } else if (s.type == 3) {
        v = (v + (by < 0 ? 255u : 1u)) % 256u;  // 255 (off), 0, 1, ... 254
    } else if (s.type == 2) {
        v = v ? 0 : 1;
    } else {
        v = static_cast<std::uint32_t>(std::max(0, static_cast<int>(v) + by));
    }
    force(s.addr, size, size == 1 ? 0xFFu : 0xFFFFFFFFu, v, true);
}

// everything this page set goes back to what the game had
void reset_all(bool cheats) {
    std::vector<std::uint64_t> keys;
    for (const Ref &ref : g_refs) {
        if (ref.gnome >= 0) keys.push_back(force_key(kCheatBits, 1u << ref.gnome));
        else if (ref.s) keys.push_back(force_key(ref.s->addr, size_of(ref.s->type) == 1 ? 0xFFu : 0xFFFFFFFFu));
    }
    int n = 0;
    for (std::uint64_t key : keys) {
        const auto it = g_game_forced.find(key);
        if (it == g_game_forced.end()) continue;
        const GameForce f = it->second;
        g_game_forced.erase(it);
        std::uint32_t now = read_value(f.poke.addr, f.poke.size);
        now = (now & ~f.poke.mask) | f.was;
        ps2::write_game_memory(f.poke.addr, &now, f.poke.size);
        ++n;
    }
    // the gnome bits the player entered with the button codes go off too (they are cheats on this page)
    if (cheats) {
        const std::uint16_t bits = static_cast<std::uint16_t>(read_value(kCheatBits, 2) & ~0x3FFu);
        ps2::write_game_memory(kCheatBits, &bits, 2);
    }
    push_game_forced();
    remaster::write_file();
    std::fprintf(stderr, "[cheats] %d value%s back to the game's own (%s page)\n", n, n == 1 ? "" : "s", cheats ? "cheats" : "debug");
}

}  // namespace gamemenu

// local change: the Memory cards page (host/card_library.hpp). The runtime swaps the cards (ps2::swap_memory_card); this keeps
// remaster.ini's PS2_CARD1 / PS2_CARD2 in step and does the file work of the library.
namespace gamecards {  // (the page's actions: host/card_page.cpp; here a slot is the running game's)
std::string dir() { return ps2::os::env("PS2_CARD_LIBRARY"); }

// the card in a slot (0 / 1) as a library name, "" for an empty slot (a swap asked for counts already)
std::string in_slot(int slot) {
    const std::string file = ps2::memory_card_file(slot);
    return file.empty() ? std::string() : ps2::os::utf8(ps2::os::path(file).stem());
}

cardpage::Slots slots() {
    cardpage::Slots s;
    s.in_slot = in_slot;
    s.put = [](int slot, const std::string &file, std::string *why) { return ps2::swap_memory_card(slot, file, why); };
    s.rename_in_slot = [](int slot, const std::string &to, std::string *why) { return ps2::rename_memory_card(slot, to, why); };
    s.flush = [] { ps2::flush_memory_card(); };
    s.remember = [] {  // the slots' cards into the environment and remaster.ini, so the next start puts the same cards in
        for (int slot = 0; slot < 2; ++slot) {
            const std::string name = in_slot(slot);
            ps2::os::set_env(slot == 0 ? "PS2_CARD1" : "PS2_CARD2", name.empty() ? "none" : name);
        }
        remaster::write_file();
    };
    return s;
}
}  // namespace gamecards

// local change: a save state slot's name, kept beside it as slot_<n>.state.name (UTF-8, one line)
std::string slot_name(int slot) {
    std::string name;
    if (std::FILE *f = ps2::os::open(ps2::slot_file(slot) + ".name", "rb")) {
        char line[256] = {};
        if (std::fgets(line, sizeof line, f)) name = line;
        std::fclose(f);
    }
    while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
    return name;
}

void set_slot_name(int slot, const std::string &name) {
    const std::string path = ps2::slot_file(slot) + ".name";
    if (name.empty()) {
        std::error_code error;
        std::filesystem::remove(ps2::os::path(path), error);
        return;
    }
    if (std::FILE *f = ps2::os::open(path, "wb")) {
        std::fwrite(name.data(), 1, name.size(), f);
        std::fclose(f);
    }
}

// local change: what each setting of the quick menu shows - its choices, the chosen one, and whether it waits for the next start
void fill_menu_values(quickmenu::Values &v) {
    if (g_menu.list_page()) gamemenu::fill(g_menu.page() == quickmenu::Page::cheats, v.list_rows);  // (read live from the game's memory)
    if (g_menu.page() == quickmenu::Page::cards) cardpage::fill(v, gamecards::dir(), gamecards::slots());  // local change: the memory card library
    if (g_menu.page() == quickmenu::Page::states) {  // local change: the save state slots, with the time each was saved
        for (int s = 1; s <= 10; ++s) {
            std::error_code error;
            const std::filesystem::path file = ps2::os::path(ps2::slot_file(s));
            if (!std::filesystem::exists(file, error)) continue;
            const auto written = std::filesystem::last_write_time(file, error);
            if (error) continue;
            // (file time to wall time through "now" on both clocks: libc++ here has no clock_cast)
            const auto wall = std::chrono::system_clock::now() +
                              std::chrono::duration_cast<std::chrono::system_clock::duration>(written - std::filesystem::file_time_type::clock::now());
            const std::time_t t = std::chrono::system_clock::to_time_t(wall);
            char text[64] = "saved";
            if (const std::tm *local = std::localtime(&t)) std::strftime(text, sizeof text, "%d %b %Y, %H:%M", local);
            v.slots[s - 1] = {true, text, slot_name(s)};
        }
    }
    // local change: the Settings values come from host/settings_fill.cpp (shared with the launcher); here the live state
    SettingsState st;
    st.look = g_look;
    st.scale_now = g_scale_now;
    st.scale_next = g_scale_next;
    st.scale_locked = g_scale_locked;
    st.sharp_now = g_sharp_now;
    st.sharp_next = g_sharp_next;
    st.sharp_locked = g_sharp_locked;
    st.window_mode = window_mode();
    // the screen's refresh rate (the Updated preset's frame rate is held to it)
    SDL_DisplayMode mode;
    if (ps2::os::env("PS2_REFRESH_ANY").empty() && SDL_GetCurrentDisplayMode(std::max(0, SDL_GetWindowDisplayIndex(g_window)), &mode) == 0 &&
        mode.refresh_rate >= 50)
        st.screen_hz = mode.refresh_rate;
    st.texture_pack = !ps2::os::env("PS2_TEX_PACK").empty() || !ps2::os::env("PS2_TEX_PACKS").empty();  // local change: or a mod's pack
    st.screen_sizes = screen_sizes();
    {
        int w = 0, h = 0;
        if (chosen_size(w, h))
            for (std::size_t i = 0; i < st.screen_sizes.size(); ++i)
                if (st.screen_sizes[i].first == w && st.screen_sizes[i].second == h) st.screen_choice = static_cast<int>(i);
    }
    for (int player = 0; player < 2; ++player) {
        st.player_ids[player] = player_ids(player);
        for (const std::string &id : st.player_ids[player]) st.player_words[player].push_back(player_word(player, id));
        st.player_choice[player] = g_player_choice[player];
    }
    st.webcam_ids = webcam_ids();
    for (const std::string &id : st.webcam_ids) st.webcam_words.push_back(webcam_word(id));
    st.webcam_now = g_webcam_now;
    st.webcam_next = g_webcam_next;
    fill_settings(v, st);
}

// Makes the panel (the menu, or the note after it) for the window's size and hands it to the one who draws (or takes it away).
void menu_paint(bool show_again = true) {
    if (show_again) g_menu_redraw = true;
    g_overlay_on = g_menu.is_open() || !g_note_text.empty();
    if (!g_overlay_on) {
#ifdef PS2_GPU
        if (g_gpu_present) ps2::gs::gpu::set_overlay(nullptr, 0, 0, 0, 0);
#endif
        return;
    }
    int w = 0, h = 0;
    window_pixels(w, h);
    if (g_menu.is_open()) {
        quickmenu::Values v;
        fill_menu_values(v);
        if (g_menu.on_keys_page()) {
            for (int a = 0; a < keymap::kChangeableCount; ++a) v.key_rows.push_back({keymap::action_label(a), keymap::keys_text(g_keys, a)});
            v.key_note = g_key_note;
        }
        if (g_menu.on_pad_page()) {
            for (int i = 0; i < padmap::kCount; ++i) v.key_rows.push_back({padmap::kLabels[i], padmap::button_name(kPadButtons[padmap::kRows[i]].button)});
            v.key_note = g_key_note;
        }
        g_menu.draw(w, h, v, g_menu_picture);
    } else {
        quickmenu::Menu::draw_note(w, h, g_note_text, g_menu_picture);
    }
    const quickmenu::Picture &p = g_menu_picture;
#ifdef PS2_GPU
    if (g_gpu_present) {
        ps2::gs::gpu::set_overlay(p.rgba.data(), static_cast<std::uint32_t>(p.width), static_cast<std::uint32_t>(p.height), static_cast<std::uint32_t>(p.x),
                                  static_cast<std::uint32_t>(p.y));
        return;
    }
#endif
    if (!g_renderer) return;
    if (!g_menu_texture || g_menu_texture_w != p.width || g_menu_texture_h != p.height) {
        if (g_menu_texture) SDL_DestroyTexture(g_menu_texture);
        // ABGR8888 is a packed format: red in the low byte of the 32-bit value, as the panel is made
        g_menu_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, p.width, p.height);
        if (g_menu_texture) SDL_SetTextureBlendMode(g_menu_texture, SDL_BLENDMODE_BLEND);
        g_menu_texture_w = p.width;
        g_menu_texture_h = p.height;
    }
    if (g_menu_texture) SDL_UpdateTexture(g_menu_texture, nullptr, p.rgba.data(), p.width * 4);
}

// the SDL renderer: the panel on top of the game's picture, in window pixels (the game's picture is drawn with a logical size)
void draw_menu_sdl() {
    if (!g_overlay_on || !g_menu_texture) return;
    int lw = 0, lh = 0;
    SDL_RenderGetLogicalSize(g_renderer, &lw, &lh);
    SDL_RenderSetLogicalSize(g_renderer, 0, 0);
    const SDL_Rect place{g_menu_picture.x, g_menu_picture.y, g_menu_picture.width, g_menu_picture.height};
    SDL_RenderCopy(g_renderer, g_menu_texture, nullptr, &place);
    if (lw && lh) SDL_RenderSetLogicalSize(g_renderer, lw, lh);
}

// the last picture once more, with the panel as it is now: the game sends no new ones while it waits
void menu_show_again() {
    g_menu_redraw = false;
#ifdef PS2_GPU
    if (g_gpu_present) {
        ps2::gs::gpu::present_again();
        return;
    }
#endif
    if (!g_renderer || !g_texture) return;
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
    draw_menu_sdl();
    SDL_RenderPresent(g_renderer);
}

void open_menu(const char *why) {
    if (g_menu.is_open()) return;
    reload_keys_if_changed();
    g_note_text.clear();
    g_key_note.clear();
    g_menu.open();
    g_menu.set_key_rows(keymap::kChangeableCount);
    g_menu_hold.store(true, std::memory_order_release);
    g_menu_opened_ticks = SDL_GetTicks();
    SDL_SetRelativeMouseMode(SDL_FALSE);  // the pointer can click the entries
    SDL_ShowCursor(SDL_ENABLE);
    if (g_notice_text == kStartNotice) {
        g_notice_text.clear();
        refresh_title();
    }
    menusound::play(menusound::Clip::select);
    std::fprintf(stderr, "[menu] the quick menu opens (%s) at frame %llu; the game waits\n", why, static_cast<unsigned long long>(ps2::frames_run()));
    menu_paint();
}

void close_menu(const char *why) {
    if (!g_menu.is_open()) return;
    g_menu.close();
    menusound::play(menusound::Clip::back);
    g_block_input[0] = g_block_input[1] = true;
    menu_paint();  // takes the panel away (shown at once, before the game's next picture)
    SDL_ShowCursor(SDL_DISABLE);
    apply_mouse_mode();
    std::fprintf(stderr, "[menu] the quick menu closes (%s) after %.1f s; the game goes on\n", why, (SDL_GetTicks() - g_menu_opened_ticks) / 1000.0);
    g_menu_hold.store(false, std::memory_order_release);
}

void close_menu_if_open(const char *why) { close_menu(why); }

void set_notice(const std::string &text) {
    g_notice_text = text;
    g_notice_until = SDL_GetTicks() + 5000;
    refresh_title();
}

// the lines of keys.ini for these buttons, written with the keys now in force (comments and every other line stay as they are)
void write_key_lines(const std::vector<int> &actions) {
    if (actions.empty()) return;
    rewrite_keys_file(
        [&](const std::string &text) {
            std::string out = text;
            for (int a : actions) out = keymap::with_inputs(out, a, g_keys.inputs[a]);
            return out;
        },
        "new keys");
}

// the key card's and the menu's idea of a mouse button
int mouse_input(Uint8 button) {
    switch (button) {
    case SDL_BUTTON_LEFT: return keymap::kMouseLeft;
    case SDL_BUTTON_MIDDLE: return keymap::kMouseMiddle;
    case SDL_BUTTON_RIGHT: return keymap::kMouseRight;
    case SDL_BUTTON_X1: return keymap::kMouseX1;
    case SDL_BUTTON_X2: return keymap::kMouseX2;
    default: return 0;
    }
}

// The keys page waits for a key: the next key or mouse button (wheel too) is the highlighted button's new key. Esc or a controller button
// cancels. Always true for the events that belong to input (nothing reaches the game).
bool capture_event(const SDL_Event &e) {
    if (g_menu.on_pad_page()) {  // local change: the controller buttons page waits for a button of the controller
        if (e.type == SDL_CONTROLLERBUTTONDOWN) {
            const auto b = static_cast<SDL_GameControllerButton>(e.cbutton.button);
            if (!padmap::usable(b)) {
                g_key_note = "Select, Start and Guide stay as they are. Press another button.";
            } else {
                const int row = g_menu.key_row();
                padmap::assign(row, b);
                remaster::write_file();
                g_menu.stop_capture();
                g_key_note = std::string(padmap::kLabels[row]) + " is now " + padmap::button_name(b) + ".";
                std::fprintf(stderr, "[menu] controller: %s is now %s\n", padmap::kLabels[row], padmap::button_name(b));
                for (bool &blocked : g_block_input) blocked = true;  // the press must not reach the game
            }
            menu_paint();
            return true;
        }
        if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            g_menu.stop_capture();
            g_key_note = "Nothing changed.";
            menu_paint();
            return true;
        }
        return e.type == SDL_KEYUP || e.type == SDL_MOUSEBUTTONUP || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEMOTION ||
               e.type == SDL_CONTROLLERBUTTONUP || e.type == SDL_CONTROLLERAXISMOTION || e.type == SDL_MOUSEWHEEL;
    }
    int input = 0;
    switch (e.type) {
    case SDL_KEYDOWN:
        if (e.key.repeat) return true;
        if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
            g_menu.stop_capture();
            g_key_note = "Nothing changed.";
            menu_paint();
            return true;
        }
        input = e.key.keysym.scancode;
        break;
    case SDL_MOUSEBUTTONDOWN: input = mouse_input(e.button.button); break;
    case SDL_MOUSEWHEEL: input = e.wheel.y > 0 ? keymap::kWheelUp : e.wheel.y < 0 ? keymap::kWheelDown : 0; break;
    case SDL_CONTROLLERBUTTONDOWN:
        g_menu.stop_capture();
        g_key_note = "Nothing changed.";
        menu_paint();
        return true;
    case SDL_KEYUP: case SDL_MOUSEBUTTONUP: case SDL_MOUSEMOTION: case SDL_CONTROLLERBUTTONUP: case SDL_CONTROLLERAXISMOTION: return true;
    default: return false;
    }
    if (input == 0) return true;
    std::vector<int> changed;
    std::string message;
    const int action = g_menu.key_row();
    if (!keymap::assign_input(g_keys, action, input, changed, message)) {
        g_key_note = message;  // (it stays waiting)
    } else {
        g_menu.stop_capture();
        g_key_note = message;
        write_key_lines(changed);
        std::fprintf(stderr, "[menu] keys: %s\n", message.c_str());
    }
    menu_paint();
    return true;
}

// what an entry does; step is -1 or 1 for left and right (Enter and a click go forward)
void menu_command(quickmenu::Command c, int step) {
    using quickmenu::Command;
    switch (c) {
    case Command::none: menu_paint(); return;  // (Quit asked once: the entry shows the question)
    case Command::resume: close_menu("Resume"); return;
    case Command::save:
        close_menu("Quick save");
        g_note_wanted = true;  // the answer ("Saved") is shown over the picture
        ps2::quick_request(ps2::QuickKey::save);
        return;
    case Command::load:
        close_menu("Quick load");
        g_note_wanted = true;
        ps2::quick_request(ps2::QuickKey::load);
        return;
    case Command::look: cycle_look(step); break;
    case Command::scale:
        if (g_scale_locked) {
            set_notice("The resolution is set by PS2_SCALE; remove it to choose here");
            break;
        }
        g_scale_next = (g_scale_next - 1 + step + 4) % 4 + 1;
        g_keys.scale = g_scale_next;
        rewrite_keys_file([](const std::string &t) { return keymap::with_scale(t, g_scale_next); }, "resolution");
        set_notice("Resolution " + std::to_string(g_scale_next) + "x" + (g_scale_next == g_scale_now ? "" : ": used when the game starts the next time"));
        std::fprintf(stderr, "[menu] resolution %dx for the next start (this run: %dx)\n", g_scale_next, g_scale_now);
        break;
    case Command::sharp_2d:
        if (g_sharp_locked) {
            set_notice("Text and menus are set by PS2_SHARP_2D; remove it to choose here");
            break;
        }
        g_sharp_next = (g_sharp_next + step + 3) % 3;
        g_keys.sharp_2d = g_sharp_next;
        rewrite_keys_file([](const std::string &t) { return keymap::with_sharp_2d(t, g_sharp_next); }, "2D art filter");
        set_notice(std::string("Text and menus: ") + keymap::sharp_2d_word(g_sharp_next) + (g_sharp_next == g_sharp_now ? "" : ", used when the game starts the next time"));
        std::fprintf(stderr, "[menu] 2D art %s for the next start (this run: %s)\n", keymap::sharp_2d_word(g_sharp_next), keymap::sharp_2d_word(g_sharp_now));
        break;
    case Command::fullscreen: {
        const int mode = ((window_mode() + step) % 3 + 3) % 3;
        set_window_mode(mode);
        remaster::g_window_mode = mode;
        remaster::write_file();
        set_notice(std::string("Window: ") + window_mode_word(mode));
        std::fprintf(stderr, "[menu] window mode: %s\n", window_mode_word(mode));
        break;
    }
    case Command::opt_frame_rate: set_notice("Frame rate " + remaster::step(0, step)); break;
    case Command::opt_ui_width: set_notice("Menus and HUD: " + remaster::step(1, step)); break;
    case Command::opt_load_turbo: set_notice("Fast-forward loading " + remaster::step(2, step)); break;
    case Command::opt_disc_speed: set_notice("Disc drive speed " + remaster::step(3, step)); break;
    case Command::opt_pad_merge: set_notice("Controllers: " + remaster::step(4, step)); break;
    case Command::opt_vsync: set_notice("V-sync " + remaster::step(7, step)); break;
    case Command::opt_skip_intro: set_notice("Skip intro " + remaster::step(8, step)); break;
    case Command::opt_aspect: set_notice("Aspect ratio " + remaster::step(9, step)); break;
    case Command::opt_mouse_mode:  // local change: used at once
        remaster::step(remaster::kMouseOption, step);
        remaster::g_options[remaster::kMouseOption].now = remaster::g_options[remaster::kMouseOption].next;
        pcmouse::g_on = remaster::g_options[remaster::kMouseOption].next == 1;
        mouse_pc::set_enabled(pcmouse::g_on);
        apply_mouse_mode();
        set_notice("Mouse: " + remaster::word(remaster::kMouseOption, remaster::g_options[remaster::kMouseOption].next));
        break;
    case Command::opt_webcam: break;  // (menu_set)
    case Command::opt_screen_res: break;  // (menu_set)
    case Command::opt_deband:
        if (const std::string mod = remaster::mod_owner(5); !mod.empty()) {  // local change
            set_notice("Smooth colour bands is set by mod " + mod);
            break;
        }
        remaster::step(5, step);
        remaster::g_options[5].now = remaster::g_options[5].next;  // used at once
        ps2::set_video_options(video_bits());
        set_notice("Smooth colour bands: " + remaster::word(5, remaster::g_options[5].next));
        break;
    case Command::opt_textures:
        if (ps2::os::env("PS2_TEX_PACK").empty() && ps2::os::env("PS2_TEX_PACKS").empty()) {
            set_notice("No updated textures: no texture pack mod is on (launcher: Mods) and the game folder has no textures/enhanced");
            break;
        }
        if (const std::string mod = remaster::mod_owner(6); !mod.empty()) {
            set_notice("Textures are set by mod " + mod);
            break;
        }
        remaster::step(6, step);
        remaster::g_options[6].now = remaster::g_options[6].next;  // used at once
        ps2::set_textures_enhanced(remaster::g_options[6].next != 0);
        set_notice("Textures: " + remaster::word(6, remaster::g_options[6].next));
        break;
    // local change: the picture look in two parts (filter, smoothed edges); the pages switch by themselves
    case Command::look_filter: set_look(g_look + 2 * step); break;
    case Command::opt_aa:
        if (const std::string mod = remaster::mod_owner(remaster::kAaOption); !mod.empty()) {
            set_notice("Anti-aliasing is set by mod " + mod);
            break;
        }
        {
            const std::string text = remaster::step(remaster::kAaOption, step);
            set_aa_notice();
            const remaster::Option &o = remaster::g_options[remaster::kAaOption];
            set_notice("Anti-aliasing: " + remaster::word(remaster::kAaOption, o.next) + (o.next == o.now ? "" : ": used when the game starts the next time"));
            (void)text;
        }
        break;
    case Command::open_settings: case Command::open_cheats: case Command::open_debug: case Command::count_: break;
    // local change: the settings page's buttons (the menu filled in the choices itself; Apply is menu_do's)
    case Command::preset_vanilla: g_menu.set_status("Vanilla filled in. Press Apply changes to use it."); break;
    case Command::preset_updated: g_menu.set_status("Updated filled in. Press Apply changes to use it."); break;
    case Command::defaults: g_menu.set_status("The standard settings of this tab are filled in. Press Apply changes to use them."); break;
    case Command::apply: case Command::opt_player1: case Command::opt_player2: break;  // (menu_do / menu_set)
    case Command::list_toggle: case Command::list_step: case Command::list_reset_all: break;  // (menu_do)
    case Command::open_states: case Command::discard_changes: break;  // (the menu switches page / does it itself)
    case Command::open_cards: case Command::card_new: case Command::card_insert: case Command::card_eject: case Command::card_rename:
    case Command::card_copy: case Command::card_delete: break;  // (menu_do: the memory cards page)
    case Command::play: case Command::open_mods: case Command::install_start: case Command::install_cancel: case Command::install_retry:
    case Command::pick_iso: case Command::picker_open: case Command::update_now: break;  // (the launcher's own commands; the game's menu has no such entries)
    // local change: the save state slots (1 to 10); the game must run on to the next frame boundary, so the menu closes
    case Command::state_save:
        close_menu("Save state");
        g_note_wanted = true;
        ps2::slot_request(ps2::QuickKey::save, step);
        return;
    case Command::state_load:
        close_menu("Load state");
        g_note_wanted = true;
        ps2::slot_request(ps2::QuickKey::load, step);
        return;
    case Command::restart:
        std::fprintf(stderr, "[menu] Restart the game: the memory card is written, the game starts again\n");
        g_restart_wanted = true;
        ps2::flush_memory_card();
        ps2::halt("restart from the quick menu");
        close_menu("Restart the game");
        return;
    case Command::change_keys: g_key_note.clear(); break;  // (the menu has switched to its keys page)
    case Command::change_pad: g_key_note.clear(); break;   // (the menu has switched to its controller buttons page)
    case Command::key_edit: g_key_note.clear(); break;     // (the highlighted line waits for a key now)
    case Command::key_defaults: {
        if (g_menu.on_pad_page()) {
            const bool were = padmap::is_default();
            padmap::defaults();
            remaster::write_file();
            g_key_note = were ? "These are the default buttons already." : "The default buttons are back.";
            break;
        }
        std::vector<int> changed;
        keymap::restore_default_inputs(g_keys, changed);
        write_key_lines(changed);
        g_key_note = changed.empty() ? "These are the default keys already." : "The default keys are back.";
        std::fprintf(stderr, "[menu] the default keys are back (%zu button%s changed)\n", changed.size(), changed.size() == 1 ? "" : "s");
        break;
    }
    case Command::keys:
        // the menu stays open behind the card (the game keeps holding); the card's own buttons close it when they save or load
        show_key_card("Keys in the quick menu");
        break;
    case Command::quit:
        std::fprintf(stderr, "[menu] Quit the game: the memory card is written, the game ends\n");
        ps2::flush_memory_card();  // right away, as when the window is closed
        ps2::halt("quit from the quick menu");
        close_menu("Quit the game");  // a game that waits for the menu must go on to see the halt
        return;
    }
    menu_paint();
}

// local change: a setting set to one of its choices (a round button, a list row, a slider step): the step from the chosen one now,
// through menu_command (whose stepping goes round, so any step lands on the choice)
void menu_set(quickmenu::Command c, int index) {
    using quickmenu::Command;
    if (c == Command::look_filter) {
        set_look((index + 3) % 4 * 2 + g_look % 2);  // (the menu's order Off, Crisp, Soft, Sharp = look / 2: 3, 0, 1, 2)
        menu_paint();
        return;
    }
    if (c == Command::opt_screen_res) {  // used at once
        const auto sizes = screen_sizes();
        if (index < 0 || index >= static_cast<int>(sizes.size())) return;
        g_screen_res = index == 0 ? std::string() : std::to_string(sizes[index].first) + "x" + std::to_string(sizes[index].second);
        if (index == 0) {  // the desktop's size: windowed keeps its size, full screen goes back to the desktop's mode
            if (window_mode() == 2) set_window_mode(2);
        } else {
            apply_screen_res();
        }
        remaster::write_file();
        menu_paint();
        return;
    }
    if (c == Command::opt_player1 || c == Command::opt_player2) {  // used at once
        const int player = c == Command::opt_player2 ? 1 : 0;
        const std::vector<std::string> ids = player_ids(player);
        if (index < 0 || index >= static_cast<int>(ids.size())) return;
        g_player_choice[player] = ids[index];
        if (ids[index] == "keyboard" && g_player_choice[1 - player] == "keyboard") g_player_choice[1 - player] = "auto";  // (one keyboard)
        remaster::g_options[4].next = g_player_choice[0] == "all" ? 1 : 0;  // (PS2_PAD_MERGE, for older runners)
        assign_pads();
        remaster::write_file();
        std::fprintf(stderr, "[menu] player %d: %s\n", player + 1, ids[index].c_str());
        menu_paint();
        return;
    }
    if (c == Command::opt_webcam) {  // written for the next start
        const std::vector<std::string> ids = webcam_ids();
        if (index < 0 || index >= static_cast<int>(ids.size())) return;
        g_webcam_next = ids[index];
        remaster::write_file();
        std::fprintf(stderr, "[menu] EyeToy camera at the next start: %s\n", g_webcam_next.c_str());
        menu_paint();
        return;
    }
    quickmenu::Values v;
    fill_menu_values(v);
    menu_command(c, index - v.settings[static_cast<int>(c)].index);
}

// local change: what the menu answered to a press or a click
void menu_do(const quickmenu::Action &a) {
    using quickmenu::Command;
    switch (a.cmd) {
    case Command::look_filter: case Command::opt_aa: case Command::scale: case Command::sharp_2d: case Command::fullscreen:
    case Command::opt_frame_rate: case Command::opt_ui_width: case Command::opt_load_turbo: case Command::opt_disc_speed:
    case Command::opt_pad_merge: case Command::opt_deband: case Command::opt_textures: case Command::opt_player1: case Command::opt_player2:
    case Command::opt_vsync: case Command::opt_skip_intro: case Command::opt_webcam: case Command::opt_aspect: case Command::opt_screen_res:
    case Command::opt_mouse_mode:
        menu_set(a.cmd, a.value);
        return;
    case Command::state_save: case Command::state_load: menu_command(a.cmd, a.value); return;  // (value: the slot)
    // local change: the memory cards page
    case Command::card_new: case Command::card_insert: case Command::card_eject: case Command::card_rename: case Command::card_copy:
    case Command::card_delete: {
        const std::string line = cardpage::act(a, g_menu.edit_text(), gamecards::dir(), gamecards::slots());
        if (!line.empty()) g_menu.set_status(line);
        menu_paint();
        return;
    }
    case Command::slot_rename:  // local change: a save state slot's name
        set_slot_name(a.value, g_menu.edit_text());
        g_menu.set_status(g_menu.edit_text().empty() ? "Slot " + std::to_string(a.value) + " has no name now."
                                                     : "Slot " + std::to_string(a.value) + " is now called \"" + g_menu.edit_text() + "\".");
        std::fprintf(stderr, "[menu] slot %d named \"%s\"\n", a.value, g_menu.edit_text().c_str());
        menu_paint();
        return;
    // local change: the Cheats and Debug pages (used at once)
    case Command::list_toggle: gamemenu::toggle(a.value); menu_paint(); return;
    case Command::list_step: gamemenu::step(a.value, a.step); menu_paint(); return;
    case Command::list_reset_all:
        gamemenu::reset_all(g_menu.page() == quickmenu::Page::cheats);
        g_menu.set_status("Everything on this page is back to the game's own values.");
        menu_paint();
        return;
    case Command::apply: {
        // local change: the choices picked in the settings, used now (or written for the next start)
        const std::vector<quickmenu::Action> changes = g_menu.changes();
        g_menu.clear_changes();
        for (const quickmenu::Action &change : changes) menu_set(change.cmd, change.value);
        quickmenu::Values v;
        fill_menu_values(v);
        bool restart = false;
        for (const quickmenu::Action &change : changes) restart = restart || v.settings[static_cast<int>(change.cmd)].next_start;
        const std::string count = std::to_string(changes.size()) + (changes.size() == 1 ? " change" : " changes");
        g_menu.set_status(restart ? count + " applied. Those marked \"needs restart\" are used after Restart the game (main menu)." : count + " applied.");
        std::fprintf(stderr, "[menu] %s applied%s\n", count.c_str(), restart ? " (some need a restart)" : "");
        menu_paint();
        return;
    }
    default: menu_command(a.cmd, 1); return;
    }
}

void menu_press(quickmenu::Input in, const char *why) {
    // local change: the game's own sounds for moving, choosing and going back
    menusound::play(in == quickmenu::Input::confirm ? menusound::Clip::select : in == quickmenu::Input::back ? menusound::Clip::back : menusound::Clip::move);
    quickmenu::Values v;
    fill_menu_values(v);
    const quickmenu::Action a = g_menu.press(in, v);
    if (a.cmd == quickmenu::Command::resume && in == quickmenu::Input::back) {
        close_menu(why);  // back on the main page
        return;
    }
    menu_do(a);
}

bool is_help_key(SDL_Scancode k) {
    const std::vector<int> &keys = g_keys.inputs[keymap::Help];
    return std::find(keys.begin(), keys.end(), static_cast<int>(k)) != keys.end();
}

bool is_menu_key(SDL_Scancode k) {
    const std::vector<int> &keys = g_keys.inputs[keymap::Menu];
    // local change: Ctrl + Alt + 0 as well (top row or keypad), for a remote desktop on a phone that has no F keys
    const SDL_Keymod mod = SDL_GetModState();
    if ((k == SDL_SCANCODE_0 || k == SDL_SCANCODE_KP_0) && (mod & KMOD_CTRL) && (mod & KMOD_ALT)) return true;
    return k == SDL_SCANCODE_F1 || std::find(keys.begin(), keys.end(), static_cast<int>(k)) != keys.end();
}

// the pointer in window pixels (events can be in the SDL renderer's logical units; the mouse state is not)
void menu_pointer(int &x, int &y) {
    int ww = 1, wh = 1, pw = 0, ph = 0;
    SDL_GetMouseState(&x, &y);
    SDL_GetWindowSize(g_window, &ww, &wh);
    window_pixels(pw, ph);
    x = x * pw / std::max(ww, 1);
    y = y * ph / std::max(wh, 1);
}

// Keys, buttons and the mouse for the menu. True when the event was the menu's (while it is open every key and button is).
// Local change: the new menu takes Inputs (up, down, left, right, confirm, back, the tabs) and answers with an Action.
bool menu_event(const SDL_Event &e) {
    using quickmenu::Command;
    using quickmenu::Input;
    if (!g_menu.is_open()) {
        if (e.type == SDL_KEYDOWN && !e.key.repeat && is_menu_key(e.key.keysym.scancode)) {
            open_menu((std::string("key ") + SDL_GetScancodeName(e.key.keysym.scancode)).c_str());
            return true;
        }
        if (e.type == SDL_CONTROLLERBUTTONDOWN) {
            SDL_GameController *pad = SDL_GameControllerFromInstanceID(e.cbutton.which);
            const int b = e.cbutton.button;
            // local change: both sticks pressed in (L3 + R3) open the menu. It was Select + Start, which Steam takes for its
            // on-screen keyboard.
            const bool both = pad && ((b == SDL_CONTROLLER_BUTTON_LEFTSTICK && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) ||
                                      (b == SDL_CONTROLLER_BUTTON_RIGHTSTICK && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSTICK)));
            if (b == SDL_CONTROLLER_BUTTON_GUIDE || both) {
                open_menu(b == SDL_CONTROLLER_BUTTON_GUIDE ? "the controller's Guide button" : "L3 + R3 on the controller");
                return true;
            }
        }
        return false;
    }
    if (g_menu.capturing()) return capture_event(e);
    // local change: naming a save state slot - the computer's keyboard types (text input), Backspace deletes, Enter is Done, Esc cancels;
    // the arrow keys and the controller work the on-screen keyboard (menu_press)
    if (g_menu.editing()) {
        if (e.type == SDL_TEXTINPUT) {
            g_menu.type_text(e.text.text);
            menu_paint();
            return true;
        }
        if (e.type == SDL_KEYDOWN) {
            SDL_StartTextInput();
            const SDL_Scancode k = e.key.keysym.scancode;
            if (!e.key.repeat && is_menu_key(k)) {
                close_menu("its key");
                return true;
            }
            switch (k) {
            case SDL_SCANCODE_BACKSPACE: g_menu.erase_text(); menu_paint(); break;
            case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER:
                if (!e.key.repeat) {
                    menusound::play(menusound::Clip::select);
                    menu_do(g_menu.finish_edit(true));
                    menu_paint();
                }
                break;
            case SDL_SCANCODE_ESCAPE:
                if (!e.key.repeat) {
                    menusound::play(menusound::Clip::back);
                    g_menu.finish_edit(false);
                    menu_paint();
                }
                break;
            case SDL_SCANCODE_UP: menu_press(quickmenu::Input::up, ""); break;
            case SDL_SCANCODE_DOWN: menu_press(quickmenu::Input::down, ""); break;
            case SDL_SCANCODE_LEFT: menu_press(quickmenu::Input::left, ""); break;
            case SDL_SCANCODE_RIGHT: menu_press(quickmenu::Input::right, ""); break;
            default: break;  // (letters come as text input)
            }
            return true;
        }
        if (e.type == SDL_KEYUP) return true;
    }
    switch (e.type) {
    case SDL_KEYDOWN: {
        const SDL_Scancode k = e.key.keysym.scancode;
        if (!e.key.repeat && is_menu_key(k)) {
            close_menu("its key");
            return true;
        }
        switch (k) {
        case SDL_SCANCODE_UP: case SDL_SCANCODE_W: menu_press(Input::up, ""); break;
        case SDL_SCANCODE_DOWN: case SDL_SCANCODE_S: menu_press(Input::down, ""); break;
        case SDL_SCANCODE_LEFT: case SDL_SCANCODE_A: menu_press(Input::left, ""); break;
        case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_D: menu_press(Input::right, ""); break;
        case SDL_SCANCODE_Q: case SDL_SCANCODE_PAGEUP: menu_press(Input::tab_prev, ""); break;
        case SDL_SCANCODE_E: case SDL_SCANCODE_PAGEDOWN: menu_press(Input::tab_next, ""); break;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE:
            if (!e.key.repeat) menu_press(Input::confirm, "");
            break;
        case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_BACKSPACE:
            if (!e.key.repeat) menu_press(Input::back, k == SDL_SCANCODE_ESCAPE ? "Esc" : "Backspace");
            break;
        // the program's keys keep working; those that need the game running close the menu first
        case SDL_SCANCODE_F2: if (!e.key.repeat) menu_command(Command::look, 1); break;
        case SDL_SCANCODE_F5: if (!e.key.repeat) menu_command(Command::save, 1); break;
        case SDL_SCANCODE_F8: if (!e.key.repeat) menu_command(Command::load, 1); break;
        case SDL_SCANCODE_F9:
            if (!e.key.repeat) {
                close_menu("F9");
                ps2::quick_request(ps2::QuickKey::report);
            }
            break;
        case SDL_SCANCODE_F11: if (!e.key.repeat) menu_command(Command::fullscreen, 1); break;
        default:
            if (!e.key.repeat && is_help_key(k)) menu_command(Command::keys, 1);
            break;
        }
        return true;
    }
    case SDL_KEYUP: return true;
    case SDL_CONTROLLERBUTTONDOWN:
        g_last_pad = e.cbutton.which;  // the players' lists mark this controller ("pressed just now")
        switch (e.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: menu_press(Input::up, ""); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: menu_press(Input::down, ""); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: menu_press(Input::left, ""); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: menu_press(Input::right, ""); break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: menu_press(Input::tab_prev, ""); break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: menu_press(Input::tab_next, ""); break;
        case SDL_CONTROLLER_BUTTON_A: menu_press(Input::confirm, ""); break;   // Cross (the bottom button)
        // local change: Triangle (the top button) steps back, as in the game's own menus; Circle does nothing here
        case SDL_CONTROLLER_BUTTON_Y: menu_press(Input::back, "Triangle"); break;
        // local change: streaming tools (Sunshine, Steam) send a Guide press for Select + Start, which opened the menu a moment
        // ago; a Guide or Start in the first half second after the menu opened does not close it again
        case SDL_CONTROLLER_BUTTON_GUIDE:
            if (SDL_GetTicks() - g_menu_opened_ticks > 500) close_menu("the Guide button");
            break;
        case SDL_CONTROLLER_BUTTON_START:
            if (SDL_GetTicks() - g_menu_opened_ticks > 500) close_menu("Start");
            break;
        default: menu_paint(); break;  // (the players' lists show which controller this was)
        }
        return true;
    case SDL_CONTROLLERBUTTONUP: return true;
    case SDL_CONTROLLERAXISMOTION:
        if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY || e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
            // the left stick moves once per push (up / down, and left / right for the settings)
            // (every controller can work the menu: one state per controller)
            static std::map<SDL_JoystickID, std::array<int, 2>> was;  // [0 vertical, 1 sideways]
            const int axis = e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX ? 1 : 0;
            int &before = was[e.caxis.which][axis];
            const int now = e.caxis.value < -20000 ? -1 : e.caxis.value > 20000 ? 1 : (std::abs(e.caxis.value) < 8000 ? 0 : before);
            if (now && now != before)
                menu_press(axis ? (now < 0 ? Input::left : Input::right) : (now < 0 ? Input::up : Input::down), "");
            before = now;
        }
        return true;
    case SDL_MOUSEMOTION:
        if (e.motion.xrel || e.motion.yrel) {  // only a pointer that moved (not one that the window moved under)
            int x = 0, y = 0;
            menu_pointer(x, y);
            quickmenu::Values v;
            fill_menu_values(v);
            if (g_menu.hover(x, y, v)) menu_paint();
        }
        return true;
    case SDL_MOUSEBUTTONDOWN:
        if (e.button.button == SDL_BUTTON_LEFT) {
            int x = 0, y = 0;
            menu_pointer(x, y);
            quickmenu::Values v;
            fill_menu_values(v);
            menusound::play(menusound::Clip::select);
            menu_do(g_menu.click(x, y, v));
        } else if (e.button.button == SDL_BUTTON_RIGHT) {
            menu_press(Input::back, "the right mouse button");
        }
        return true;
    case SDL_MOUSEBUTTONUP: return true;
    case SDL_MOUSEWHEEL:
        if (e.wheel.y) menu_press(e.wheel.y > 0 ? Input::up : Input::down, "");
        return true;
    default: return false;
    }
}

void show_notice() {
    ps2::QuickNotice n;
    if (ps2::quick_notice(n)) {
        g_notice_text = n.text;
        g_notice_until = SDL_GetTicks() + (n.kind == 0 ? 5000u : 6000u);
        if (g_note_wanted && n.kind > 0) {  // the answer to a save or load that the menu asked for: shown over the picture for a moment
            g_note_wanted = false;
            if (!g_menu.is_open()) {
                g_note_text = n.text;
                g_note_until = SDL_GetTicks() + 2500;
                menu_paint(false);
            }
        }
        if (n.kind == 2 && g_pipe) g_pipe->flush();  // the sound from before the load (the tone comes on top of the game's sound, so it stays)
        if (g_beep_on && n.kind > 0) g_beep_request = n.kind;
        refresh_title();
    } else if (!g_notice_text.empty() && static_cast<Sint32>(SDL_GetTicks() - g_notice_until) >= 0) {
        g_notice_text.clear();
        refresh_title();
    }
    if (!g_note_text.empty() && static_cast<Sint32>(SDL_GetTicks() - g_note_until) >= 0) {
        g_note_text.clear();
        if (!g_menu.is_open()) menu_paint(false);
    }
}

// PS2_HITCH_LOG=<ms>: SDL_PollEvent also pumps the system's messages and looks for controllers; say when it, or the handling of
// one event, took long (docs/hitches.md).
// local change: the pointer shows while the mouse moves over the game and goes away again after 2.5 s without moving (it was
// always hidden over the game). Not while the mouse works a stick (relative mode), and the quick menu keeps it shown.
void pointer_seen(const SDL_Event *e) {
    static Uint32 moved = 0;
    static bool shown = false;
    const Uint32 now = SDL_GetTicks();
    if (e && e->type == SDL_MOUSEMOTION && SDL_GetRelativeMouseMode() != SDL_TRUE) {
        moved = now;
        if (!shown) {
            SDL_ShowCursor(SDL_ENABLE);
            shown = true;
        }
    } else if (shown && now - moved > 2500 && !g_menu.is_open() && !pcmouse::g_on) {  // (the PC mouse keeps it shown)
        SDL_ShowCursor(SDL_DISABLE);
        shown = false;
    }
}

bool timed_poll_event(SDL_Event &e) {
    window_stage("asking the system for window events (SDL_PollEvent)");
    if (!ps2::hitch::on()) {
        const bool got = SDL_PollEvent(&e) != 0;
        pointer_seen(got ? &e : nullptr);
        return got;
    }
    const auto t0 = ps2::hitch::Clock::now();
    const bool got = SDL_PollEvent(&e) != 0;
    const double ms = ps2::hitch::ms_since(t0);
    if (ms > ps2::hitch::threshold_ms() / 3)
        std::fprintf(stderr, "[hitch] SDL_PollEvent took %.1f ms (it returned %s, event type 0x%x)%s", ms, got ? "an event" : "nothing", got ? e.type : 0u, "\n");
    return got;
}

struct EventTimer {
    Uint32 type;
    ps2::hitch::Clock::time_point t0 = ps2::hitch::on() ? ps2::hitch::Clock::now() : ps2::hitch::Clock::time_point{};
    ~EventTimer() {
        if (!ps2::hitch::on()) return;
        const double ms = ps2::hitch::ms_since(t0);
        if (ms > ps2::hitch::threshold_ms() / 3) std::fprintf(stderr, "[hitch] handling the event of type 0x%x took %.1f ms%s", type, ms, "\n");
    }
};

void poll() {
    window_stage("window thread: starting a pass of poll()");
    if (!g_script.empty()) play_pad_script();
    ps2::quick_watchdog();
    take_key_card_answer();
    SDL_Event e;
    while (timed_poll_event(e)) {
        window_stage("handling a window event");
        const EventTimer event_timer{e.type};
        if (e.type == SDL_QUIT) {
            ps2::flush_memory_card();  // right away: a save made a moment ago must not wait for the emulation to wind down
            ps2::halt("window closed");
            close_menu("the window was closed");  // a game that waits for the menu must go on to see the halt
        }
        if (menu_event(e)) continue;
        if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F11 && !e.key.repeat) toggle_fullscreen();
        // F1 the quick menu (menu_event), F5 quick save, F8 quick load, F9 "something is wrong here" (the game's own buttons: keys.ini,
        // host/keymap.cpp); a help key of the player's own shows the key card at once
        if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            if (is_help_key(e.key.keysym.scancode))
                show_key_card((std::string("key ") + SDL_GetScancodeName(e.key.keysym.scancode) + " went down").c_str());
            if (e.key.keysym.scancode == SDL_SCANCODE_F2) cycle_look();
            if (e.key.keysym.scancode == SDL_SCANCODE_F5) ps2::quick_request(ps2::QuickKey::save);
            else if (e.key.keysym.scancode == SDL_SCANCODE_F8) ps2::quick_request(ps2::QuickKey::load);
            else if (e.key.keysym.scancode == SDL_SCANCODE_F9) ps2::quick_request(ps2::QuickKey::report);
        }
        if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED && g_gpu_present) {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(g_window, &w, &h);
            ps2::resize_window(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
        }
        if (e.type == SDL_WINDOWEVENT && (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_EXPOSED) && g_overlay_on)
            menu_paint();  // the panel for the new size
        if (e.type == SDL_WINDOWEVENT && (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED || e.window.event == SDL_WINDOWEVENT_FOCUS_LOST)) {
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                g_focus_gained_ticks = SDL_GetTicks();
                reload_keys_if_changed();
            }
            apply_mouse_mode();
        }
        if (pcmouse::g_on && !g_menu.is_open()) pcmouse::event(e);
        if (e.type == SDL_MOUSEMOTION && g_keys.mouse_stick) {
            g_mouse_sum_x += e.motion.xrel;
            g_mouse_sum_y += e.motion.yrel;
        }
        if (e.type == SDL_MOUSEWHEEL) {
            // a turn of the wheel is a press of about a tenth of a second, which the game sees for a few frames
            (e.wheel.y > 0 ? g_wheel_up_until : g_wheel_down_until) = SDL_GetTicks() + 100;
        }
        if (e.type == SDL_CONTROLLERDEVICEADDED) open_pad(e.cdevice.which);
        if (e.type == SDL_CONTROLLERDEVICEREMOVED) close_pad(e.cdevice.which);
        if (e.type == SDL_JOYDEVICEADDED && !SDL_IsGameController(e.jdevice.which))
            std::fprintf(stderr, "[pad] %s is not used: SDL knows no button layout for it (docs/sound-pad.md: gamecontrollerdb.txt)\n",
                         or_unknown(SDL_JoystickNameForIndex(e.jdevice.which)));
    }
    window_stage("reading the controllers");
    const auto after_events = ps2::hitch::on() ? ps2::hitch::Clock::now() : ps2::hitch::Clock::time_point{};
    pcmouse::update();
    poll_port(0);
    poll_port(1);
    show_notice();
    if (g_menu_redraw) menu_show_again();
    if (ps2::hitch::on() && ps2::hitch::ms_since(after_events) > ps2::hitch::threshold_ms() / 3)
        std::fprintf(stderr, "[hitch] reading the controllers and the notice took %.1f ms%s", ps2::hitch::ms_since(after_events), "\n");
}

void show_pixels(const std::uint32_t *rgba, std::uint32_t w, std::uint32_t h) {
    if (w != g_tex_w || h != g_tex_h) {
        if (g_texture) SDL_DestroyTexture(g_texture);
        g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, static_cast<int>(w), static_cast<int>(h));
        SDL_RenderSetLogicalSize(g_renderer, static_cast<int>(w), static_cast<int>(static_cast<float>(w) / ps2::display_aspect()));
        g_tex_w = w;
        g_tex_h = h;
    }
    SDL_UpdateTexture(g_texture, nullptr, rgba, static_cast<int>(w * 4));
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
    draw_menu_sdl();
    SDL_RenderPresent(g_renderer);
}

void present() {
    if (g_gpu_present) {
        ps2::present_frame();
        return;
    }
    std::uint32_t w = 0, h = 0;
    if (ps2::display_frame(g_pixels, w, h)) show_pixels(g_pixels.data(), w, h);
}

double audio_seconds() { return static_cast<double>(SDL_GetPerformanceCounter()) / static_cast<double>(SDL_GetPerformanceFrequency()); }

double queued_ms() { return g_pipe ? g_pipe->queued_ms() : 0.0; }  // the game's sound waiting for the device, in milliseconds of game time

void write_audio_dumps();
void note_audio();

// PS2_NO_LIMIT=1 (docs/speed.md): a window run goes as fast as it can, with no wait for the sound device and no wait for the clock, to
// measure what the processor threads and the graphics chip can do. The sound plays out of step (it is dropped when it runs too far
// behind). Off unless set; "0" counts as off.
bool no_limit() {
    static const bool on = [] {
        const std::string value = ps2::os::env("PS2_NO_LIMIT");
        const bool set = !value.empty() && value != "0";
        if (set) std::fprintf(stderr, "[pace] PS2_NO_LIMIT is set: the game runs with no speed limit; the sound plays out of step\n");
        return set;
    }();
    return on;
}

// PS2_PACE=audio: the sound device paces the game (the game waits while more than ~80 ms of sound is waiting), as before. A device
// that takes its sound in large blocks (wireless headsets over WASAPI) then makes the frames uneven: seven of 20 ms, one of 30 ms, one
// of 3 ms, about six times a second (measured with Urban Reign and a Corsair HS80), which shows as judder in every camera pan even
// at a steady 50 fps. By default the clock paces the game instead, one frame every 1/50 or 1/60 s, and the sound only steers that
// clock: frames up to 1% longer while more sound than kSteerMs is waiting, up to 1% shorter while less. The sound stretcher still
// covers a game that runs slow.
bool audio_paced() {
    static const bool on = ps2::os::env("PS2_PACE") == "audio";
    return on;
}

constexpr double kSteerMs = 90.0;  // the sound the clock-paced game keeps waiting for the device: enough that a device taking large blocks does not dip it under the stretcher's gap guard (~45 ms with a 20 ms frame), which would stretch the sound for no reason

// Waits until the performance counter reaches `target`: sleeps while more than 2 ms are left, then spins, for a frame edge within
// a few microseconds (SDL_Delay alone wakes up to a millisecond late, which is the unevenness this is meant to remove).
void wait_until(std::uint64_t target) {
    const std::uint64_t frequency = SDL_GetPerformanceFrequency();
    for (;;) {
        const std::uint64_t now = SDL_GetPerformanceCounter();
        if (now >= target) return;
        const double left_ms = static_cast<double>(target - now) * 1000.0 / static_cast<double>(frequency);
        if (left_ms > 2.0) SDL_Delay(static_cast<Uint32>(left_ms - 1.5));
        else std::this_thread::yield();
    }
}

// local change: PS2_LOAD_TURBO=1 lets the game run with no speed limit while it reads a lot from the disc (loading), and for one
// more second after. Counted in game frames: 1000 sectors (2 MB) within 25 frames turns it on; films and streamed sound read far less.
bool load_turbo() {
    static const bool enabled = ps2::os::env("PS2_LOAD_TURBO") == "1";
    if (!enabled) return false;
    static std::uint64_t window_start = 0, turbo_frames = 0;
    static int frames = 0, hold = 0;
    static bool on = false;
    if (++frames >= 25) {
        const std::uint64_t now = ps2::disc::sectors_read();
        if (now - window_start >= 1000) hold = 50;
        window_start = now;
        frames = 0;
    }
    const bool want = hold > 0;
    if (hold > 0) --hold;
    if (want) ++turbo_frames;
    if (want != on) {
        on = want;
        if (on) std::fprintf(stderr, "[turbo] frame %llu: loading from the disc, no speed limit\n", static_cast<unsigned long long>(ps2::frames_run()));
        else {
            // The game made sound faster than it plays while it ran unpaced (~480 ms waiting after a build-catalogue load). pace() would
            // hold the game until that played out, a ~220 ms frozen frame: drop it instead, as after loading a state.
            const double waiting = queued_ms();
            const bool drop = g_pipe && waiting > 3.0 * kSteerMs;
            if (drop) g_pipe->flush();
            std::fprintf(stderr, "[turbo] frame %llu: back to normal speed after %llu fast frames (%.0f ms of sound waiting%s)\n", static_cast<unsigned long long>(ps2::frames_run()), static_cast<unsigned long long>(turbo_frames), waiting, drop ? ", dropped" : "");
            turbo_frames = 0;
        }
    }
    return want;
}

void pace() {
    if (no_limit()) return;
    if (load_turbo()) return;
    static bool stuck = false;  // the device took no sound for a second; the clock paces the game until it takes some again
    if (g_audio && !audio_paced() && !stuck) {
        g_pipe->frame(audio_seconds());
        const double started = audio_seconds();
        const double frequency = static_cast<double>(SDL_GetPerformanceFrequency());
        const double steer = std::clamp((g_pipe->queued_ms() - kSteerMs) / kSteerMs * 0.02, -0.01, 0.01);
        static const double refresh_hz = [] { const std::string e = ps2::os::env("PS2_REFRESH_HZ"); const int v = e.empty() ? 0 : std::atoi(e.c_str()); return v <= 0 ? 0.0 : static_cast<double>(std::clamp(v, 25, 480)); }();  // local change, see refresh_hz() in runtime/src/system.cpp
        const auto period = static_cast<std::uint64_t>(frequency / (refresh_hz > 0 ? refresh_hz : ps2::g_options.pal ? 50.0 : 60.0) * (1.0 + steer));
        const std::uint64_t now = SDL_GetPerformanceCounter();
        // more than two frames behind (a load, a hold): start counting again from now rather than rushing to catch up
        if (g_last && now < g_last + 2 * period) {
            g_last += period;
            wait_until(g_last);
        } else {
            g_last = now;
        }
        // a device that takes far less than the game makes (or none at all) must still hold the game: the old wait, as a ceiling
        const double hold = audio_seconds();
        while (g_pipe->queued_ms() > 3.0 * kSteerMs && audio_seconds() - hold < 1.0) SDL_Delay(1);
        const double waited = audio_seconds() - started;
        g_pipe->note_wait(waited);
        note_audio();
        if (audio_seconds() - hold >= 1.0) {
            std::fprintf(stderr, "[audio] the sound device took no sound for a second (unplugged?); the clock paces the game until it does\n");
            stuck = true;
        }
        return;
    }
    if (g_audio && !(stuck && g_pipe->full())) {
        // The audio device clock paces emulation: wait while more than ~80 ms of sound is waiting. The pipe times the game between
        // the frames (how much it slows down decides how much the sound is stretched) and does not count this wait as slowness.
        g_pipe->frame(audio_seconds());
        const double started = audio_seconds();
        while (g_pipe->full() && audio_seconds() - started < 1.0) SDL_Delay(1);  // at most a second: a stopped device must not hold the game
        const double waited = audio_seconds() - started;
        g_pipe->note_wait(waited);
        note_audio();
        if (!stuck && waited >= 1.0) std::fprintf(stderr, "[audio] the sound device took no sound for a second (unplugged?); the clock paces the game until it does\n");
        stuck = waited >= 1.0;
        return;
    }
    const std::uint64_t frequency = SDL_GetPerformanceFrequency();
    const std::uint64_t period = frequency / (ps2::g_options.pal ? 50 : 60);
    const std::uint64_t now = SDL_GetPerformanceCounter();
    if (g_last && now - g_last < period) {
        SDL_Delay(static_cast<Uint32>((period - (now - g_last)) * 1000 / frequency));
        g_last += period;
    } else {
        g_last = now;
    }
}

double process_cpu_seconds() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    const auto ticks = [](const FILETIME &t) { return static_cast<double>((static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime); };
    return (ticks(kernel) + ticks(user)) / 1e7;
#else
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) + static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
#endif
}

double resident_mb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters);
    return static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) return 0.0;
    return static_cast<double>(info.resident_size) / (1024.0 * 1024.0);
#else
    long pages = 0, resident = 0;
    if (std::FILE *f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
        std::fclose(f);
    }
    return static_cast<double>(resident) * static_cast<double>(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
#endif
}

void show_speed() {
    static std::uint64_t start = 0, frames = 0;
    static double cpu_start = 0.0, gpu_start = 0.0;
    const std::uint64_t now = SDL_GetPerformanceCounter();
    if (!start) {
        start = now;
        cpu_start = process_cpu_seconds();
        gpu_start = ps2::gpu_busy_ms();
    }
    ++frames;
    const double seconds = static_cast<double>(now - start) / static_cast<double>(SDL_GetPerformanceFrequency());
    if (seconds < 1.0) return;
    const double cpu = process_cpu_seconds(), gpu = ps2::gpu_busy_ms();
    char title[192];
    const double queued = queued_ms();
    char stretched[40] = "";  // "audio 62 ms" is the game's sound waiting; when it is played slower, by how much
    if (g_pipe && g_pipe->tempo() < 0.995) std::snprintf(stretched, sizeof stretched, ", stretched x%.1f", 1.0 / g_pipe->tempo());
    std::snprintf(title, sizeof title, "ps2-native-recomp  %.1f fps  CPU %.0f%%  GPU %.0f%%  RAM %.0f MB  audio %.0f ms%s", static_cast<double>(frames) / seconds,
                  (cpu - cpu_start) / seconds * 100.0, (gpu - gpu_start) / (seconds * 10.0), resident_mb(), queued, stretched);
    g_speed_title = title;
    static const bool log = std::getenv("PS2_SPEED_LOG") != nullptr;  // local change: the title's numbers once a second in the log (window timing)
    if (log) std::fprintf(stderr, "[speed] %s\n", title);
    refresh_title();
    start = now;
    frames = 0;
    cpu_start = cpu;
    gpu_start = gpu;
}

// PS2_PRESENT_LOG=1: once a second, how long the emulation thread waited per frame for the window thread (post_present: the
// window thread is two pictures behind) and for the sound (pace), and how the window thread's time was spent (docs/speed.md).
struct WaitLog {
    using Clock = std::chrono::steady_clock;
    bool on = std::getenv("PS2_PRESENT_LOG") != nullptr;
    Clock::time_point since = Clock::now();
    double post = 0, sound = 0, seen_working = 0, seen_idle = 0;
    std::atomic<double> working{0}, idle{0};  // written by the window thread, read by the emulation thread
    std::uint32_t frames = 0;
    static double ms(Clock::time_point from) { return std::chrono::duration<double, std::milli>(Clock::now() - from).count(); }
    void emulation_frame() {
        if (++frames < 60) return;
        const double seconds = std::chrono::duration<double>(Clock::now() - since).count();
        std::fprintf(stderr, "[waits] %.1f frames/s; emulation thread per frame: waiting for the window thread %.2f ms, for the sound %.2f ms; window thread per second: rendering %.0f ms, no work %.0f ms\n",
                     frames / seconds, post / frames, sound / frames, (working.load() - seen_working) / seconds, (idle.load() - seen_idle) / seconds);
        since = Clock::now();
        post = sound = 0;
        seen_working = working.load();
        seen_idle = idle.load();
        frames = 0;
    }
};
WaitLog g_waits;

// PS2_FRAME_LOG=<file>: one line per emulated frame, to find what happens when a scene suddenly gets slow (docs/speed.md, the arena).
// Columns: frame number, milliseconds since the previous frame, CPU milliseconds all the threads used meanwhile, milliseconds the emulation
// thread waited for the window thread and for the sound, shapes drawn, vector programs run by the slow interpreter, sound queued (ms).
struct FrameLog {
    std::FILE *file = nullptr;
    std::uint64_t last_counter = 0, last_prims = 0, last_interpreted = 0;
    double last_cpu = 0;
    FrameLog() {
        const std::string path = ps2::os::env("PS2_FRAME_LOG");
        if (!path.empty()) {
            file = ps2::os::open(path, "w");
            if (file) std::fprintf(file, "frame wall_ms cpu_ms wait_window_ms wait_sound_ms shapes interpreted audio_ms\n");
        }
    }
    void frame(double wait_window_ms, double wait_sound_ms, double queued_ms) {
        const std::uint64_t now = SDL_GetPerformanceCounter();
        const double cpu = process_cpu_seconds();
        const std::uint64_t prims = ps2::primitives_drawn(), interpreted = ps2::vu1_interpreted_calls();
        if (last_counter)
            std::fprintf(file, "%llu %.2f %.2f %.2f %.2f %llu %llu %.0f\n", static_cast<unsigned long long>(ps2::frames_run()),
                         static_cast<double>(now - last_counter) * 1000.0 / static_cast<double>(SDL_GetPerformanceFrequency()), (cpu - last_cpu) * 1000.0,
                         wait_window_ms, wait_sound_ms, static_cast<unsigned long long>(prims - last_prims), static_cast<unsigned long long>(interpreted - last_interpreted), queued_ms);
        last_counter = now;
        last_cpu = cpu;
        last_prims = prims;
        last_interpreted = interpreted;
        if (ps2::frames_run() % 60 == 0) std::fflush(file);
    }
};
FrameLog g_frame_log;

void on_vblank() {
    show_speed();
    poll();
    present();
    pace();
    if (g_frame_log.file) g_frame_log.frame(0.0, 0.0, queued_ms());
    if (g_menu.is_open()) {  // the quick menu: this thread is the window's too, so it answers the window while the game waits
        const double since = audio_seconds();
        while (g_menu.is_open()) {
            poll();
            SDL_Delay(10);
        }
        if (g_pipe) g_pipe->note_wait(audio_seconds() - since);  // the wait is no slowness of the game: no stretched sound after it
        g_last = 0;
    }
}

// The quick menu with threads: the emulation thread waits here, at the end of its frame, until the menu closes.
void hold_for_menu() {
    if (!g_menu_hold.load(std::memory_order_acquire)) return;
    const double since = audio_seconds();
    while (g_menu_hold.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (g_pipe) g_pipe->note_wait(audio_seconds() - since);  // the wait is no slowness of the game: no stretched sound after it
    g_last = 0;
}

// PS2_AUDIO_DUMP=<file.wav>: the sound exactly as the stretcher hands it to the sound device (no silence where it ran short),
// PS2_AUDIO_HEARD=<file.wav>: what the speaker got, silence included. 48000 Hz 16-bit stereo. The header is brought up to date
// about once a second, so a run that is killed keeps a file that plays up to there.
class WavOut {
public:
    ~WavOut() { close(); }
    bool open(const std::string &path) {
        file_ = ps2::os::open(path, "wb");
        if (!file_) std::fprintf(stderr, "[audio] cannot write %s\n", path.c_str());
        path_ = path;
        if (file_) header();
        return file_ != nullptr;
    }
    void write(const std::int16_t *samples, std::size_t frames) {
        if (!file_ || frames_ + frames > (0xFFFFFFFFull - 36) / 4) return;
        std::fwrite(samples, 4, frames, file_);
        frames_ += frames;
        if (frames_ - synced_ >= 48000) header();
    }
    void close() {
        if (!file_) return;
        header();
        std::fclose(file_);
        file_ = nullptr;
        std::fprintf(stderr, "[audio] wrote %llu samples (%.2f s) to %s\n", static_cast<unsigned long long>(frames_), static_cast<double>(frames_) / 48000.0, path_.c_str());
    }

private:
    void header() {
        const std::uint32_t bytes = static_cast<std::uint32_t>(frames_ * 4);
        const std::uint32_t words[] = {0x46464952u, 36 + bytes, 0x45564157u, 0x20746D66u, 16, 0x00020001u, 48000, 48000 * 4, 0x00100004u, 0x61746164u, bytes};
        std::fseek(file_, 0, SEEK_SET);
        std::fwrite(words, 4, sizeof words / 4, file_);
        std::fseek(file_, 0, SEEK_END);
        std::fflush(file_);
        synced_ = frames_;
    }
    std::FILE *file_ = nullptr;
    std::string path_;
    std::uint64_t frames_ = 0, synced_ = 0;
};

WavOut g_dump, g_heard;

// PS2_AUDIO_PUSHLOG=<file>: when the sound chip pushed its sound and when each video frame ended (time in seconds, kind and size, waiting
// ms, frame), written at the end. tests/stretch_test.cpp replay <file> runs the sound pipe on the real timing (docs/sound.md).
struct PushLog {
    struct Row {
        double time;
        int kind, frames;
        float waiting;
        unsigned frame;
    };
    std::vector<Row> rows;
    std::string path = ps2::os::env("PS2_AUDIO_PUSHLOG");
    void add(int kind, int frames, double now) {
        if (!path.empty()) rows.push_back({now, kind, frames, static_cast<float>(queued_ms()), static_cast<unsigned>(ps2::frames_run())});
    }
    void write() {
        if (path.empty()) return;
        if (std::FILE *f = ps2::os::open(path, "w")) {
            for (const Row &r : rows) std::fprintf(f, "%.6f %d %.1f %u\n", r.time, r.kind * 100000 + r.frames, static_cast<double>(r.waiting), r.frame);
            std::fclose(f);
        }
        path.clear();
    }
};
PushLog g_pushlog;

struct Tone {
    int hz, ms;
};
constexpr Tone kBeepSaved[] = {{1047, 90}};
constexpr Tone kBeepLoaded[] = {{784, 80}, {0, 40}, {1047, 80}};
constexpr Tone kBeepReport[] = {{1047, 70}, {0, 40}, {784, 70}, {0, 40}, {523, 110}};
constexpr Tone kBeepTrouble[] = {{196, 300}};
bool g_beep_running = false;

// Adds the tone that was asked for to the game's sound (48000 Hz stereo), in place. Runs on the sound device's thread.
void mix_beep(std::int16_t *samples, std::size_t frames) {
    static const Tone *seq = nullptr;
    static std::size_t count = 0, index = 0;
    static int left = 0, total = 0;
    static double phase = 0.0;
    if (const int kind = g_beep_request.exchange(0)) {
        seq = kind == 1 ? kBeepSaved : kind == 2 ? kBeepLoaded : kind == 3 ? kBeepReport : kBeepTrouble;
        count = kind == 1 ? std::size(kBeepSaved) : kind == 2 ? std::size(kBeepLoaded) : kind == 3 ? std::size(kBeepReport) : std::size(kBeepTrouble);
        index = 0;
        total = left = seq[0].ms * 48;
        phase = 0.0;
    }
    g_beep_running = seq != nullptr;
    for (std::size_t i = 0; seq && i < frames; ++i) {
        while (left == 0) {
            if (++index >= count) {
                seq = nullptr;
                g_beep_running = false;
                return;
            }
            total = left = seq[index].ms * 48;
            phase = 0.0;
        }
        const int position = total - left--;
        if (!seq[index].hz) continue;
        const double ramp = std::min({1.0, position / 240.0, left / 240.0});  // 5 ms in and out, so the tone does not click
        const int v = static_cast<int>(std::sin(phase) * ramp * 3500.0);
        phase += 2.0 * 3.14159265358979 * seq[index].hz / 48000.0;
        for (int ch = 0; ch < 2; ++ch) samples[i * 2 + ch] = static_cast<std::int16_t>(std::clamp(samples[i * 2 + ch] + v, -32768, 32767));
    }
}

// The sound chip's batches of 32 frames arrive here on the emulation thread.
void queue_audio(const std::int16_t *samples, std::size_t frames) {
    const double now = audio_seconds();
    g_pushlog.add(0, static_cast<int>(frames), now);
    g_pipe->push(samples, frames, now);
}

// The sound device asks for its next block (its own thread): the pipe hands over the game's sound, stretched when the game is slow,
// and the tone for F5, F8 and F9 is mixed on top.
void audio_callback(void *, Uint8 *stream, int length) {
    static thread_local bool named = false;  // the sound device's own thread
    if (!named) {
        named = true;
        ps2::crash::enter_thread(ps2::crash::Thread::Sound);
    }
    ps2::crash::test_poll(ps2::crash::Thread::Sound);
    g_pipe->render(reinterpret_cast<std::int16_t *>(stream), static_cast<std::size_t>(length) / 4, audio_seconds(), mix_beep);
    menusound::mix(reinterpret_cast<std::int16_t *>(stream), static_cast<std::size_t>(length) / 4);  // local change: the quick menu's sounds
}

void write_audio_dumps() {
    static std::vector<std::int16_t> chunk;
    if (g_pipe->device_tap.on) {
        chunk.clear();
        g_pipe->device_tap.take(chunk);
        g_dump.write(chunk.data(), chunk.size() / 2);
    }
    if (g_pipe->heard_tap.on) {
        chunk.clear();
        g_pipe->heard_tap.take(chunk);
        g_heard.write(chunk.data(), chunk.size() / 2);
    }
}

// once per video frame (from pace): the dump files, and a line when the sound had to stop for lack of sound
void note_audio() {
    static std::uint64_t seen = 0, last_report = 0;
    write_audio_dumps();
    g_pushlog.add(1, 0, audio_seconds());
    const std::uint64_t dry = g_pipe->stats().dry;
    const std::uint64_t now = SDL_GetPerformanceCounter();
    if (dry > seen && (!last_report || now - last_report > 5 * SDL_GetPerformanceFrequency())) {
        std::fprintf(stderr, "[audio] the sound ran dry at frame %llu (%llu times so far): the game runs slower than real time%s\n", static_cast<unsigned long long>(ps2::frames_run()),
                     static_cast<unsigned long long>(dry), g_pipe->config().stretch ? " than the sound can be stretched to" : "");
        last_report = now;
    }
    seen = dry;
}

// at the end of a window run: how much sound the game made, how it was played, and how much silence there was
void finish_audio() {
    if (g_audio && g_pipe) {
        SDL_CloseAudioDevice(g_audio);  // the device thread stops here
        g_audio = 0;
        write_audio_dumps();
        const ps2sound::PipeStats s = g_pipe->stats();
        const double wall = s.first >= 0.0 ? s.last - s.first : 0.0;
        std::fprintf(stderr, "[audio] the device got %.2f s of sound in %.2f s; the queue ran dry %llu times (about %.2f s of silence in between), %llu samples dropped\n",
                     static_cast<double>(s.pushed) / 48000.0, wall, static_cast<unsigned long long>(s.dry), static_cast<double>(s.silence) / 48000.0,
                     static_cast<unsigned long long>(s.dropped));
        if (g_pipe->config().stretch)
            std::fprintf(stderr, "[audio] stretching: %.2f s of game sound played as %.2f s, %.2f s of that stretched (slowest tempo x%.2f; %.2f s at the limit of x%.1f, where it runs dry like without stretching)\n",
                         s.consumed / 48000.0, static_cast<double>(s.played) / 48000.0, static_cast<double>(s.stretched) / 48000.0, 1.0 / s.slowest,
                         static_cast<double>(s.at_cap) / 48000.0, g_pipe->config().max_ratio);
        else
            std::fprintf(stderr, "[audio] stretching is off (PS2_AUDIO_STRETCH=0)\n");
        g_pushlog.write();
        g_dump.close();
        g_heard.close();
    }
}

}

void poll_input() { poll(); }

bool open_window(bool fullscreen) {
    const std::string script = ps2::os::env("PS2_PAD_SCRIPT"), pad_log = ps2::os::env("PS2_PAD_LOG"), no_beep = ps2::os::env("PS2_NO_BEEP");
    g_pad_merge = ps2::os::env("PS2_PAD_MERGE") == "1";
    // local change: the quick menu's sounds, beside its font (<game folder>/menu/sounds)
    if (const std::string font = ps2::os::env("PS2_MENU_FONT"); !font.empty()) {
        const std::size_t slash = font.find_last_of("/\\");
        menusound::load((slash == std::string::npos ? std::string(".") : font.substr(0, slash)) + "/sounds");
    }
    // local change: the players' controllers; an old remaster.ini only has PS2_PAD_MERGE, which is player 1 = all controllers
    pcmouse::g_on = ps2::os::env("PS2_MOUSE_MODE") != "0" && ps2::os::env("PS2_MOUSE_MODE") != "pad";  // local change: the PC mouse (default on)
    g_player_choice[0] = ps2::os::env("PS2_PLAYER1");
    g_player_choice[1] = ps2::os::env("PS2_PLAYER2");
    if (g_player_choice[0].empty()) g_player_choice[0] = g_pad_merge ? "all" : "auto";
    if (g_player_choice[1].empty()) g_player_choice[1] = "auto";
    g_webcam_now = ps2::os::env("PS2_WEBCAM");
    if (g_webcam_now.empty() || g_webcam_now == "0" || g_webcam_now == "off") g_webcam_now = "none";
    g_webcam_next = g_webcam_now;
    g_webcam_names = webcam::list();
    read_game_forced(ps2::os::env("PS2_GAME_POKES"));
    g_screen_res = ps2::os::env("PS2_SCREEN_RES");
    remaster::read_now();
    if (g_kept_loaded) remaster::write_file();  // local change: PS2_GAME_POKES_NEXT was for this start only
    if (const std::string map = ps2::os::env("PS2_PADMAP"); !map.empty()) padmap::read(map);
    g_beep_on = no_beep.empty() || no_beep == "0";
    g_pad_log = !pad_log.empty() && pad_log != "0";
    if (!script.empty()) SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return false;
    }
    // local change: a frame rate above what the screen shows cannot be reached (the swap chain hands out one picture per refresh
    // of the screen, measured: a 60 Hz screen holds the game at 60 pictures a second whatever is asked), and the game then runs
    // slower than real time with stretched sound. So the frame rate of this run is at most the screen's; the file keeps its line.
    {
        SDL_DisplayMode mode{};
        const int asked = remaster::g_options[0].now;
        if (SDL_GetCurrentDisplayMode(0, &mode) == 0 && mode.refresh_rate >= 50 && asked > mode.refresh_rate + 1 && ps2::os::env("PS2_REFRESH_ANY").empty()) {
            int fits = 50;
            for (int v : remaster::g_options[0].values)
                if (v <= mode.refresh_rate + 1) fits = std::max(fits, v);
            std::fprintf(stderr, "[options] the screen shows %d pictures a second: frame rate %d instead of %d for this run (PS2_REFRESH_ANY=1 to keep it)\n", mode.refresh_rate, fits, asked);
            remaster::g_options[0].now = fits;
            ps2::os::set_env("PS2_REFRESH_HZ", std::to_string(fits));
        }
    }
    // button layouts for pads SDL does not know by itself: a gamecontrollerdb.txt next to ps2run (SDL_GAMECONTROLLERCONFIG_FILE
    // and SDL_GAMECONTROLLERCONFIG work too)
    if (char *base = SDL_GetBasePath()) {
        const std::string db = std::string(base) + "gamecontrollerdb.txt";
        SDL_free(base);
        if (SDL_RWops *file = SDL_RWFromFile(db.c_str(), "rb")) std::fprintf(stderr, "[pad] %d button layouts from %s\n", SDL_GameControllerAddMappingsFromRW(file, 1), db.c_str());
    }
    if (!script.empty()) load_pad_script(script);
    g_keys_path = ps2::os::env("PS2_KEYS");
    if (g_keys_path.empty()) g_keys_path = ps2::quick_folder("keys.ini");  // next to the memory card, like the quick saves
    if (const std::string word = ps2::os::env("PS2_LOOK"); !word.empty()) {
        for (int i = 0; i < keymap::kLookCount; ++i)
            if (word == keymap::look_word(i) || word == std::to_string(i)) g_look = i, g_look_forced = true;
        if (!g_look_forced) std::fprintf(stderr, "[video] PS2_LOOK=%s is not crisp, crisp_edges, soft, soft_edges, sharp, sharp_edges or 0 to 5, ignored\n", word.c_str());
    }
    load_keys();
    // The inner resolution: PS2_SCALE when it is set, else the 'scale' line of keys.ini (the quick menu writes it), else 1. It is read
    // when the graphics start (run_window, after this), so setting it here is in time.
    g_scale_locked = !ps2::os::env("PS2_SCALE").empty();
    if (!g_scale_locked && g_keys.scale >= 1 && g_keys.scale <= 4) {
        ps2::os::set_env("PS2_SCALE", std::to_string(g_keys.scale));
        std::fprintf(stderr, "[video] resolution %dx from %s (scale = %d)\n", g_keys.scale, g_keys_path.c_str(), g_keys.scale);
    }
    // The picture pace (ps2/pace.hpp): the 'pace' line of keys.ini, unless PS2_PACE is set. The runtime reads it at boot, after this.
    if (!g_keys.pace.empty() && ps2::os::env("PS2_PACE").empty()) {
        ps2::os::set_env("PS2_PACE", g_keys.pace);
        ps2::os::set_env("PS2_PACE_FROM", g_keys_path);
    }
    g_scale_now = std::clamp(std::atoi(ps2::os::env("PS2_SCALE").c_str()), 1, 4);
    g_scale_next = g_scale_locked ? g_scale_now : (g_keys.scale >= 1 ? g_keys.scale : g_scale_now);
    // the 2D art filter the same way: PS2_SHARP_2D, else the 'sharp_2d' line of keys.ini, else sharp (gpu.cpp reads it at its start)
    g_sharp_locked = !ps2::os::env("PS2_SHARP_2D").empty();
    if (!g_sharp_locked && g_keys.sharp_2d >= 0) ps2::os::set_env("PS2_SHARP_2D", std::to_string(g_keys.sharp_2d));
    g_sharp_now = ps2::os::env("PS2_SHARP_2D").empty() ? 1 : std::clamp(std::atoi(ps2::os::env("PS2_SHARP_2D").c_str()), 0, 2);
    g_sharp_next = g_sharp_locked ? g_sharp_now : (g_keys.sharp_2d >= 0 ? g_keys.sharp_2d : g_sharp_now);
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0u);
#ifdef __APPLE__
    if (!std::getenv("PS2_SDL_RENDERER")) flags |= SDL_WINDOW_METAL;
#endif
    g_game_title = ps2::os::env("PS2_WINDOW_TITLE");
    const std::string initial_title = g_game_title.empty() ? g_speed_title : g_game_title + "   |   " + g_speed_title;
    g_window = SDL_CreateWindow(initial_title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, static_cast<int>(720 * ps2::display_aspect()), 720, flags);
    if (!g_window) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return false;
    }
    // local change: the window mode chosen in the quick menu (PS2_WINDOW_MODE from remaster.ini); --fullscreen still wins
    if (const std::string mode = ps2::os::env("PS2_WINDOW_MODE"); !mode.empty()) {
        remaster::g_window_mode = std::clamp(std::atoi(mode.c_str()), 0, 2);
        if (!fullscreen && remaster::g_window_mode != 0) set_window_mode(remaster::g_window_mode);
    }
    if (!fullscreen && remaster::g_window_mode == 0) apply_screen_res();  // local change: the window's size from the quick menu
    SDL_ShowWindow(g_window);
    SDL_RaiseWindow(g_window);
#ifdef _WIN32
    SDL_SysWMinfo wm_info;
    SDL_VERSION(&wm_info.version);
    if (SDL_GetWindowWMInfo(g_window, &wm_info) && wm_info.subsystem == SDL_SYSWM_WINDOWS) {
        ShowWindow(static_cast<HWND>(wm_info.info.win.window), SW_SHOWNORMAL);
        SetForegroundWindow(static_cast<HWND>(wm_info.info.win.window));
    }
#endif
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_AudioSpec want{}, have{};
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    // PS2_AUDIO_STRETCH=0 plays the sound as it comes (silence where it runs short); PS2_AUDIO_STRETCH_MAX=<ratio> changes the limit
    static ps2sound::PipeConfig config;
    const std::string stretch = ps2::os::env("PS2_AUDIO_STRETCH"), stretch_max = ps2::os::env("PS2_AUDIO_STRETCH_MAX");
    config.stretch = stretch != "0";
    if (const double limit = std::atof(stretch_max.c_str()); limit >= 1.5 && limit <= 20.0) config.max_ratio = limit;
    static ps2sound::SoundPipe pipe(config);
    g_pipe = &pipe;
    want.callback = audio_callback;
    g_audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (g_audio) {
        // SDL converts the 48000 Hz stereo queue to whatever the device itself runs at
        SDL_AudioSpec device{};
        char *name = nullptr;
        if (SDL_GetDefaultAudioInfo(&name, &device, 0) == 0)
            std::fprintf(stderr, "[audio] sound device %s (%s): %d Hz, %d channels; the game's sound is 48000 Hz stereo\n", name ? name : "?",
                         SDL_GetCurrentAudioDriver(), device.freq, device.channels);
        SDL_free(name);
        const std::string dump = ps2::os::env("PS2_AUDIO_DUMP");
        if (!dump.empty() && g_dump.open(dump)) {
            g_pipe->device_tap.on = true;
            std::fprintf(stderr, "[audio] writing the sound that goes to the device to %s\n", dump.c_str());
        }
        const std::string heard = ps2::os::env("PS2_AUDIO_HEARD");
        if (!heard.empty() && g_heard.open(heard)) {
            g_pipe->heard_tap.on = true;
            std::fprintf(stderr, "[audio] writing what the speaker plays (silence included) to %s\n", heard.c_str());
        }
        std::fprintf(stderr, "[audio] %s\n", config.stretch ? "when the game runs slower than real time its sound is played slower, with the same pitch (PS2_AUDIO_STRETCH=0 switches that off)"
                                                          : "stretching is off: the sound is played as it comes, with silence where the game was too slow");
        SDL_PauseAudioDevice(g_audio, 0);
        ps2::set_audio_sink(queue_audio);
    } else {
        std::fprintf(stderr, "[audio] no sound device: %s\n", SDL_GetError());
    }
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_IsGameController(i)) open_pad(i);
    SDL_ShowCursor(SDL_DISABLE);  // no pointer over the game (the title bar and the window's edge still show it)
    g_notice_text = kStartNotice;
    std::fprintf(stderr, "[menu] the quick menu: F1%s on the keyboard, press both sticks in, L3 + R3 (or Guide), on a controller\n",
                 g_keys.inputs[keymap::Menu].empty() ? "" : (" or " + keymap::input_name(g_keys.inputs[keymap::Menu][0])).c_str());
    g_notice_until = SDL_GetTicks() + 30000;
    refresh_title();
    apply_mouse_mode();
    return true;
}

#ifdef __linux__
// A window run whose game stops (no new frame) used to stay silent: the owner closed the window, nothing answered, the compositor's
// "not responding" window killed the process, and the log ended without a word about where the threads were. This watches the frame
// counter; after kWatchdogSeconds without a new frame it writes one line and the stack of every thread (each thread is sent SIGUSR2 and
// writes its own, which is what the unwinder needs), once per stall. Nothing here touches the game.
namespace {
constexpr int kWatchdogSeconds = 10;
std::atomic<int> g_stack_done{0};

void write_number(long long value) {
    char digits[24];
    int n = 0;
    unsigned long long v = value < 0 ? 0ull : static_cast<unsigned long long>(value);
    do digits[n++] = static_cast<char>('0' + v % 10);
    while ((v /= 10) && n < 23);
    char text[24];
    for (int i = 0; i < n; ++i) text[i] = digits[n - 1 - i];
    ssize_t ignored = write(2, text, static_cast<std::size_t>(n));
    (void)ignored;
}

void on_stack_request(int) {
    const int saved = errno;
    char name[32] = {};
    prctl(PR_GET_NAME, name, 0, 0, 0);
    ssize_t ignored = write(2, "[watchdog] thread ", 18);
    write_number(syscall(SYS_gettid));
    ignored = write(2, " (", 2);
    ignored = write(2, name, std::strlen(name));
    ignored = write(2, "):\n", 3);
    (void)ignored;
#ifdef __GLIBC__
    void *frames[48];
    const int count = backtrace(frames, 48);
    backtrace_symbols_fd(frames, count, 2);
#endif
    g_stack_done.fetch_add(1);
    errno = saved;
}

void report_threads(std::uint64_t frame, int stalled_seconds) {
    std::fprintf(stderr, "[watchdog] no new frame for %d s (stuck at frame %llu); the stack of every thread, as addresses in ps2run (addr2line -f -C -e ps2run <address>):\n", stalled_seconds,
                 static_cast<unsigned long long>(frame));
    {
        const ps2::hitch::PresentStage &stage = ps2::hitch::present_stage();
        const char *where = stage.name.load(std::memory_order_acquire);
        if (where)
            std::fprintf(stderr, "[watchdog] the window thread has been in \"%s\" for %.1f s\n", where,
                         static_cast<double>(ps2::hitch::now_ns() - stage.since_ns.load(std::memory_order_relaxed)) / 1e9);
        else
            std::fprintf(stderr, "[watchdog] the window thread is not in the hand-over of a picture to the screen\n");
        const char *loop = g_window_stage.load(std::memory_order_acquire);
        if (loop)
            std::fprintf(stderr, "[watchdog] the window thread's loop has been in \"%s\" for %.1f s\n", loop,
                         static_cast<double>(ps2::hitch::now_ns() - g_window_stage_since_ns.load(std::memory_order_relaxed)) / 1e9);
        if (g_card.state.load() == 1) std::fprintf(stderr, "[watchdog] the key card is open (its box runs on a thread of its own)\n");
    }
    std::fflush(stderr);
    const pid_t self = getpid();
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/task", error)) {
        const long tid = std::strtol(entry.path().filename().string().c_str(), nullptr, 10);
        if (tid <= 0 || tid == syscall(SYS_gettid)) continue;
        const int before = g_stack_done.load();
        if (syscall(SYS_tgkill, self, tid, SIGUSR2) != 0) continue;
        for (int waited = 0; g_stack_done.load() == before && waited < 500; ++waited) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (g_stack_done.load() == before) std::fprintf(stderr, "[watchdog] thread %ld did not answer in 1 s (blocked inside the kernel)\n", tid);
    }
}

void watchdog() {
    ps2::crash::enter_thread(ps2::crash::Thread::Watchdog);
    std::signal(SIGUSR2, on_stack_request);
#ifdef __GLIBC__
    void *warm[2];
    backtrace(warm, 2);  // loads the unwinder now, not inside the handler
#endif
    std::uint64_t seen = ps2::frames_run();
    auto since = std::chrono::steady_clock::now();
    bool reported = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const std::uint64_t frame = ps2::frames_run();
        const auto now = std::chrono::steady_clock::now();
        const int stalled = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(now - since).count());
        if (g_menu_hold.load()) since = now;  // the game waits for the quick menu: no stall
        if (frame != seen) {
            if (reported) std::fprintf(stderr, "[watchdog] the game runs again at frame %llu after %d s\n", static_cast<unsigned long long>(frame), stalled);
            seen = frame;
            since = now;
            reported = false;
        } else if (!reported && stalled >= kWatchdogSeconds) {
            reported = true;
            report_threads(frame, stalled);
        }
    }
}
}  // namespace
#endif

int run_window(const std::function<int()> &emulate) {
    ps2::init_graphics();
    g_gpu_present = attach_gpu();
    std::fprintf(stderr, "[window] %s\n", g_gpu_present ? "presenting through WebGPU" : "presenting through SDL");
    if (!g_gpu_present && !create_renderer()) return 1;
    const bool threads = !std::getenv("PS2_ONE_THREAD") && !std::getenv("PS2_TIME") && ps2::g_options.frames_dir.empty();
    if (!threads) {
        ps2::g_options.on_vblank = on_vblank;
        // The game runs on this thread, so while it waits for a live translation (15 to 35 s) it must still take the window's messages, or
        // Windows marks the window "not responding" and ends the program when it is clicked (ee_live.cpp, while_pumping).
        ps2::g_options.keep_alive = [] { poll(); };
        const int result = emulate();
        finish_audio();
#ifdef __APPLE__
        destroy_metal_view();
#endif
        start_again();
        return result;
    }
    ps2::g_options.on_vblank = [] {
        const auto t0 = WaitLog::Clock::now();
        ps2::post_present();
        const auto t1 = WaitLog::Clock::now();
        pace();
        hold_for_menu();
        if (ps2::hitch::on()) {  // PS2_HITCH_LOG=<ms>: a frame of the emulation thread that took long, and what it waited for (docs/hitches.md)
            static auto last = WaitLog::Clock::now();
            static double last_cpu = ps2::hitch::thread_cpu_ms();
            const auto now = WaitLog::Clock::now();
            const double frame_ms = std::chrono::duration<double, std::milli>(now - last).count(), cpu = ps2::hitch::thread_cpu_ms();
            if (frame_ms > ps2::hitch::threshold_ms())
                std::fprintf(stderr, "[hitch] frame %llu took %.1f ms: waited for the window thread %.1f ms, for the sound %.1f ms; the emulation thread itself used %.1f ms of CPU; sound queued %.0f ms\n",
                             static_cast<unsigned long long>(ps2::frames_run()), frame_ms, std::chrono::duration<double, std::milli>(t1 - t0).count(), WaitLog::ms(t1), cpu - last_cpu, queued_ms());
            last = now;
            last_cpu = cpu;
        }
        if (g_frame_log.file)
            g_frame_log.frame(std::chrono::duration<double, std::milli>(t1 - t0).count(), WaitLog::ms(t1), queued_ms());
        if (g_waits.on) {
            g_waits.post += std::chrono::duration<double, std::milli>(t1 - t0).count();
            g_waits.sound += WaitLog::ms(t1);
            g_waits.emulation_frame();
        }
    };
    ps2::start_render_thread();
    if (!std::getenv("PS2_NO_VU_THREAD")) {
        ps2::start_vu_thread();
        std::thread(ps2::run_vu_thread).detach();
    }
#ifdef __linux__
    if (!std::getenv("PS2_NO_WATCHDOG")) std::thread(watchdog).detach();
#endif
    std::atomic<bool> done{false};
    int result = 0;
    ps2::host::BigStackThread emulation([&] {
        result = emulate();
        done.store(true);
        ps2::wake_render_thread();
    });
    const std::function<void(const std::uint32_t *, std::uint32_t, std::uint32_t)> copy = show_pixels, none;
    while (!done.load()) {
        if (ps2::hitch::on()) {  // PS2_HITCH_LOG=<ms>: the window's own event handling
            const auto p0 = WaitLog::Clock::now();
            poll();
            const double p_ms = WaitLog::ms(p0);
            if (p_ms > ps2::hitch::threshold_ms() / 3) std::fprintf(stderr, "[hitch] window event polling took %.1f ms (around frame %llu)\n", p_ms, static_cast<unsigned long long>(ps2::frames_run()));
        } else {
            poll();
        }
        window_stage("drawing the pictures of the emulation (render_pending)");
        if (!g_waits.on) {
            if (ps2::render_pending(g_gpu_present, g_gpu_present ? none : copy, show_speed, 8.0) == ps2::Rendered::idle) {
                window_stage("waiting for the emulation to post work (wait_render_work)");
                ps2::wait_render_work(4);
            }
            continue;
        }
        const auto t0 = WaitLog::Clock::now();
        const bool idle = ps2::render_pending(g_gpu_present, g_gpu_present ? none : copy, show_speed, 8.0) == ps2::Rendered::idle;
        const auto t1 = WaitLog::Clock::now();
        if (idle) {
            window_stage("waiting for the emulation to post work (wait_render_work)");
            ps2::wait_render_work(4);
        }
        g_waits.working.store(g_waits.working.load() + std::chrono::duration<double, std::milli>(t1 - t0).count());
        if (idle) g_waits.idle.store(g_waits.idle.load() + WaitLog::ms(t1));
    }
    emulation.join();
    ps2::flush_memory_card();
    finish_audio();
#ifdef __APPLE__
    destroy_metal_view();
#endif
    if (g_card.state.load() == 0) SDL_Quit();  // a key card still open is on a thread inside SDL: the program ends without taking SDL down under it
    if (ps2::g_options.on_exit) ps2::g_options.on_exit();  // the host's log copy hands over the last lines before the process ends
    start_again();
    std::fflush(nullptr);
    std::_Exit(result);
}
