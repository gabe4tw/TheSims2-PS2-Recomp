#pragma once

// Local change (the remaster): remaster.ini, the file of the quick menu's options ("PS2_NAME=value" lines; host/main.cpp turns them
// into environment variables at the start). Shared by the game window (host/window.cpp) and the launcher (host/launcher_main.cpp).

#include <map>
#include <string>
#include <vector>

namespace remaster {

struct Option {
    const char *variable;   // the environment variable (and the line in remaster.ini)
    std::vector<int> values;
    int fallback;           // what the variable means when it is not set
    int now = 0, next = 0;  // this run's value, and the file's for the next start
};
// the order of quickmenu's opt_ commands: PS2_REFRESH_HZ, PS2_UI_SQUEEZE, PS2_LOAD_TURBO, PS2_DISC_SPEED, PS2_PAD_MERGE, PS2_DEBAND,
// PS2_TEX_ENHANCED, PS2_VSYNC, PS2_SKIP_INTRO, PS2_ASPECT, PS2_AA, PS2_MOUSE_MODE
constexpr int kOptionCount = 12;
constexpr int kMouseOption = 11;  // PS2_MOUSE_MODE: 1 PC style (default), 0 as a controller
constexpr int kAaOption = 10;  // PS2_AA: 0 Off, 1 Fast, 3..9 supersampling 2x..8x (fixed numbers; 2 = Quality, experimental, not in the menus)
extern Option g_options[kOptionCount];
// the options used only from the next start (the others are used at once). Anti-aliasing: Off and Fast switch at once, a change into
// or out of Quality / Extra needs the next start (so the answer depends on its now / next).
bool needs_restart(int i);
// local change (anti-aliasing): whether the last read_env / read_file_into found a PS2_AA value. Without one the old "smooth edges"
// bit of keys.ini's look decides (aa_from_look), and the next write puts the line in.
extern bool g_aa_given;
void aa_from_look(int look);
std::string word(int index, int value);

// the lines the file holds besides g_options (an empty text = no line)
struct Extra {
    int window_mode = 0;                         // PS2_WINDOW_MODE: 0 windowed, 1 borderless, 2 full screen
    std::string player[2] = {"auto", "auto"};    // PS2_PLAYER1 / PS2_PLAYER2
    std::string webcam = "none";                 // PS2_WEBCAM
    std::string screen_res;                      // PS2_SCREEN_RES=WxH ("" = the desktop's)
    std::string padmap;                          // PS2_PADMAP ("" = the default buttons)
    std::string pokes_next;                      // PS2_GAME_POKES_NEXT (the Debug page's one-shot "keep for the next start")
    std::string card[2];                         // PS2_CARD1 / PS2_CARD2 (written only with card_library)
    bool card_library = false;
};

void read_env();  // g_options' now and next from the environment (the game window: main.cpp set them from the file)
// local change (launcher stage 3): settings an enabled mod holds (PS2_NAME -> the mod's name). Shown locked ("set by mod X"), and
// write() keeps the file's own line for them, so a mod's value never stays in remaster.ini after the mod is turned off.
extern std::map<std::string, std::string> g_mod_owned;
std::string mod_owner(int i);  // the mod holding g_options[i], "" = none
std::map<std::string, std::string> read_file(const std::string &path);  // PS2_NAME -> value
void read_file_into(const std::string &path);  // g_options' now and next from the file (the launcher)
Extra extra_from(const std::map<std::string, std::string> &lines);
// Writes the file (whole, or the old one stays): a header, g_options (next), PS2_SIMS2_TICKFIX=1, the Extra lines, then every PS2_ line
// of the old file this module does not own, unchanged and in their old order. An old PS2_GAME_POKES line (cheats) is dropped.
bool write(const std::string &path, const Extra &extra);
// The game's first start with a card library: the cards it chose go into the file (PS2_CARD1 / PS2_CARD2, "" = none) so the launcher
// and later starts show the same, but only when the file has no card lines yet; everything else in it stays byte for byte. true when written.
bool add_card_lines_if_missing(const std::string &path, const std::string &card1, const std::string &card2);

}  // namespace remaster
