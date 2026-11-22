// Local change: the Settings page's values from a plain state (host/settings_fill.cpp), no game or window needed.
// Build: ninja ps2settingsfilltest, run build/.../ps2settingsfilltest.
#include "../host/settings_fill.hpp"
#include "../host/remaster_ini.hpp"

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
using quickmenu::Command;
const quickmenu::Setting &at(const quickmenu::Values &v, Command c) { return v.settings[static_cast<int>(c)]; }
int index_of(const quickmenu::Setting &s, const std::string &word) {
    for (std::size_t i = 0; i < s.choices.size(); ++i)
        if (s.choices[i] == word) return static_cast<int>(i);
    return -1;
}
SettingsState base() {
    SettingsState s;
    s.screen_sizes = {{2560, 1440}, {1920, 1080}, {1280, 720}};
    for (int p = 0; p < 2; ++p) {
        s.player_ids[p] = {"auto", p ? "none" : "all", "GUID#0"};
        s.player_words[p] = {"Automatic", p ? "Nobody" : "All controllers", "Xbox 360 Controller"};
        s.player_choice[p] = "auto";
    }
    s.webcam_ids = {"none", "Cam"};
    s.webcam_words = {"None", "Cam"};
    s.webcam_now = s.webcam_next = "none";
    return s;
}
}  // namespace

int main() {
    for (int i = 0; i < remaster::kOptionCount; ++i) remaster::g_options[i].now = remaster::g_options[i].next = remaster::g_options[i].fallback;
    {
        SettingsState s = base();
        s.screen_hz = 60;
        s.texture_pack = true;
        quickmenu::Values v;
        fill_settings(v, s);
        check(at(v, Command::opt_frame_rate).fallback == index_of(at(v, Command::opt_frame_rate), "60 fps") && at(v, Command::opt_frame_rate).updated == -1 &&
                  at(v, Command::opt_frame_rate).vanilla == -1 && at(v, Command::opt_load_turbo).updated == -1 && at(v, Command::opt_skip_intro).vanilla == -1 &&
                  at(v, Command::opt_load_turbo).fallback == 1 && at(v, Command::opt_disc_speed).updated >= 0 && at(v, Command::opt_disc_speed).vanilla == 0,
              "presets: graphics and disc speed only; frame rate default 60");
        check(at(v, Command::opt_textures).updated == 1 && at(v, Command::scale).updated == 2 && at(v, Command::look_filter).vanilla == 2 && at(v, Command::look_filter).updated == 3,
              "presets_match_today: textures, scale, filter");
        check(at(v, Command::opt_frame_rate).choices.size() == 9 && at(v, Command::opt_frame_rate).index == 0, "presets_match_today: the rates, 50 chosen");
    }
    {
        SettingsState s = base();
        s.screen_hz = 50;
        quickmenu::Values v;
        fill_settings(v, s);
        check(at(v, Command::opt_frame_rate).fallback == index_of(at(v, Command::opt_frame_rate), "50 fps (original)"), "slow_screen_keeps_50");
        check(at(v, Command::opt_textures).updated == -1, "no_pack_leaves_textures");
    }
    {
        SettingsState s = base();
        s.screen_choice = 2;
        quickmenu::Values v;
        fill_settings(v, s);
        const quickmenu::Setting &r = at(v, Command::opt_screen_res);
        check(r.index == 2 && r.choices.size() == 3 && r.choices[0] == "2560 x 1440 (desktop)" && r.choices[2] == "1280 x 720", "screen_choice");
    }
    {
        SettingsState s = base();
        s.player_choice[0] = "GUID#0";
        s.webcam_next = "Cam";
        quickmenu::Values v;
        fill_settings(v, s);
        check(at(v, Command::opt_player1).index == 2 && at(v, Command::opt_player1).choices[2] == "Xbox 360 Controller" && at(v, Command::opt_player2).index == 0,
              "players_and_webcam: players");
        check(at(v, Command::opt_webcam).index == 1 && at(v, Command::opt_webcam).next_start, "players_and_webcam: webcam waits for the next start");
    }
    {
        SettingsState s = base();
        s.scale_now = 3;
        s.scale_next = 2;
        s.look = 3;  // soft + smooth edges
        quickmenu::Values v;
        fill_settings(v, s);
        check(at(v, Command::scale).index == 1 && at(v, Command::scale).next_start && at(v, Command::look_filter).index == 2, "look_and_scale");
        SettingsState plain = base();
        plain.look = 6;  // filter off (keys.ini "plain")
        quickmenu::Values w;
        fill_settings(w, plain);
        check(at(w, Command::look_filter).index == 0 && at(w, Command::look_filter).choices[0] == "Off" && at(w, Command::look_filter).choices.size() == 4,
              "filter_off_first");
    }
    {  // local change: anti-aliasing (PS2_AA). Off <-> Fast is used at once; into or out of Quality / Extra waits for the next start.
        remaster::Option &o = remaster::g_options[remaster::kAaOption];
        SettingsState s = base();
        quickmenu::Values v;
        o.now = 0, o.next = 1;
        fill_settings(v, s);
        const quickmenu::Setting &aa = at(v, Command::opt_aa);
        check(aa.choices.size() == 9 && aa.choices[0] == "Off" && aa.choices[1] == "Fast" && aa.choices[2] == "2x" && aa.choices[8] == "8x" && aa.index == 1 &&
                  !aa.next_start && !aa.restart && aa.vanilla == 0 && aa.updated == 1,
              "aa: words, Off -> Fast at once, presets");
        quickmenu::Values w;
        o.now = 1, o.next = 3;
        fill_settings(w, s);
        check(at(w, Command::opt_aa).index == 2 && at(w, Command::opt_aa).next_start && at(w, Command::opt_aa).restart, "aa: Fast -> 2x waits for the next start");
        quickmenu::Values x;
        o.now = 3, o.next = 1;
        fill_settings(x, s);
        check(at(x, Command::opt_aa).next_start, "aa: Extra -> Fast waits for the next start");
        // no PS2_AA line: the old smooth-edges bit of the look decides; with a line the look is ignored
        remaster::g_aa_given = false;
        remaster::aa_from_look(3);
        const bool from_odd = o.now == 1 && o.next == 1;
        remaster::aa_from_look(4);
        const bool from_even = o.now == 0 && o.next == 0;
        remaster::g_aa_given = true;
        o.now = o.next = 3;
        remaster::aa_from_look(5);
        check(from_odd && from_even && o.now == 3, "aa: old look bit used only without a PS2_AA line");
        o.now = o.next = o.fallback;
    }
    {  // a mod's setting (launcher stage 3) is locked with its name
        remaster::g_mod_owned = {{"PS2_DEBAND", "Sharp Mod"}};
        SettingsState s = base();
        quickmenu::Values v;
        fill_settings(v, s);
        check(at(v, Command::opt_deband).locked && at(v, Command::opt_deband).locked_note == "set by mod Sharp Mod" && !at(v, Command::opt_frame_rate).locked,
              "mod_setting_locked");
        remaster::g_mod_owned.clear();
    }
    std::printf("settings fill: %d of %d checks passed, %d failed\n", checks - failed, checks, failed);
    return failed ? 1 : 0;
}
