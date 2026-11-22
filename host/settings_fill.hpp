#pragma once

// Local change (the remaster): the Settings page's values (choices, the chosen one, presets) from a plain state, so the game window
// (host/window.cpp, live values) and the launcher (values read from keys.ini and remaster.ini) fill the page the same way.

#include "keymap.hpp"
#include "quickmenu.hpp"

#include <string>
#include <utility>
#include <vector>

struct SettingsState {
    int look = keymap::kDefaultLook;                  // the picture look (keys.ini 'look'): filter * 2 + smooth edges
    int scale_now = 1, scale_next = 1;
    bool scale_locked = false;                        // set by PS2_SCALE
    int sharp_now = 1, sharp_next = 1;
    bool sharp_locked = false;                        // set by PS2_SHARP_2D
    int window_mode = 0;                              // 0 windowed, 1 borderless, 2 full screen
    std::vector<std::pair<int, int>> screen_sizes;    // [0] = the desktop's
    int screen_choice = 0;                            // index into screen_sizes (0 = the desktop's / none chosen)
    int screen_hz = 0;                                // the screen's refresh rate, 0 = unknown
    bool texture_pack = false;                        // PS2_TEX_PACK is set (HD textures exist)
    std::vector<std::string> player_ids[2], player_words[2];
    std::string player_choice[2] = {"auto", "auto"};
    std::vector<std::string> webcam_ids, webcam_words;
    std::string webcam_now = "none", webcam_next = "none";
};

// Every Settings value: the remaster options (remaster::g_options), look, scale, 2D sharpness, window, screen resolution, players,
// EyeToy camera, and the Vanilla / Updated / Defaults presets.
void fill_settings(quickmenu::Values &v, const SettingsState &s);
