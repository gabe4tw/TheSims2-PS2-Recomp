#include "settings_fill.hpp"

#include "remaster_ini.hpp"

#include <algorithm>
#include <cctype>

void fill_settings(quickmenu::Values &v, const SettingsState &st) {
    using quickmenu::Command;
    const auto set = [&](Command c) -> quickmenu::Setting & { return v.settings[static_cast<int>(c)]; };
    set(Command::look_filter) = {(st.look / 2 + 1) % 4, {"Off", "Crisp", "Soft", "Sharp"}};  // (look / 2: 0 crisp, 1 soft, 2 sharp, 3 off)
    set(Command::scale) = {st.scale_next - 1, {"1x", "2x", "3x", "4x"}, st.scale_next != st.scale_now, st.scale_locked, "set by PS2_SCALE"};
    set(Command::sharp_2d) = {st.sharp_next, {"Soft", "Sharp", "Sharpest"}, st.sharp_next != st.sharp_now, st.sharp_locked, "set by PS2_SHARP_2D"};
    set(Command::fullscreen) = {st.window_mode, {"Windowed", "Borderless", "Full screen"}};
    static constexpr Command kOptions[remaster::kOptionCount] = {Command::opt_frame_rate, Command::opt_ui_width, Command::opt_load_turbo, Command::opt_disc_speed,
                                                                 Command::opt_pad_merge,  Command::opt_deband,   Command::opt_textures,   Command::opt_vsync,
                                                                 Command::opt_skip_intro, Command::opt_aspect, Command::opt_aa, Command::opt_mouse_mode};
    for (int i = 0; i < remaster::kOptionCount; ++i) {
        const remaster::Option &o = remaster::g_options[i];
        quickmenu::Setting &s = set(kOptions[i]);
        for (std::size_t c = 0; c < o.values.size(); ++c) {
            std::string word = remaster::word(i, o.values[c]);
            if (i == 5 || i == 6) word[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(word[0])));
            s.choices.push_back(word);
            if (o.values[c] == o.next) s.index = static_cast<int>(c);
        }
        s.next_start = remaster::needs_restart(i) && o.next != o.now;
        s.restart = remaster::needs_restart(i);
        if (const std::string mod = remaster::mod_owner(i); !mod.empty()) {  // local change (launcher stage 3)
            s.locked = true;
            s.locked_note = "set by mod " + mod;
            s.next_start = false;
        }
    }
    // the two presets and the defaults (the choice of each setting; -1 leaves it alone). Defaults are the remaster's standard
    // settings, the same as Updated, plus automatic controllers.
    const auto preset = [&](Command c, int vanilla, int updated) {
        quickmenu::Setting &s = set(c);
        s.vanilla = vanilla;
        s.updated = s.fallback = updated;
    };
    const auto at_value = [](int option, int value) {
        const std::vector<int> &values = remaster::g_options[option].values;
        return static_cast<int>(std::find(values.begin(), values.end(), value) - values.begin());
    };
    preset(Command::look_filter, 2, 3);  // Soft / Sharp
    preset(Command::opt_aa, at_value(remaster::kAaOption, 0), at_value(remaster::kAaOption, 1));  // Off / Fast
    preset(Command::scale, 0, 2);  // 1x / 3x
    preset(Command::sharp_2d, 0, 1);  // Soft / Sharp
    // local change (user, 2026-11-21): the presets set the Graphics tab and the disc drive speed only; frame rate, fast-forward
    // loading and skip intro keep their own choice (Defaults still puts the standard value in)
    const auto defaults_only = [&](Command c, int fallback) {
        quickmenu::Setting &s = set(c);
        s.vanilla = s.updated = -1;
        s.fallback = fallback;
    };
    defaults_only(Command::opt_frame_rate, at_value(0, st.screen_hz && st.screen_hz < 60 ? 50 : 60));
    preset(Command::opt_ui_width, at_value(1, 100), at_value(1, 75));
    defaults_only(Command::opt_load_turbo, 1);
    preset(Command::opt_disc_speed, at_value(3, 1), at_value(3, 4));
    preset(Command::opt_deband, 0, 2);
    preset(Command::opt_textures, 0, st.texture_pack ? 1 : -1);
    defaults_only(Command::opt_skip_intro, 1);
    defaults_only(Command::opt_mouse_mode, at_value(remaster::kMouseOption, 1));  // (PC style)
    {  // the screen resolution (a slider over the screen's sizes; the first is the desktop's)
        quickmenu::Setting &s = set(Command::opt_screen_res);
        for (std::size_t i = 0; i < st.screen_sizes.size(); ++i)
            s.choices.push_back(std::to_string(st.screen_sizes[i].first) + " x " + std::to_string(st.screen_sizes[i].second) + (i == 0 ? " (desktop)" : ""));
        s.index = st.screen_choice >= 0 && st.screen_choice < static_cast<int>(st.screen_sizes.size()) ? st.screen_choice : 0;
        s.fallback = 0;
    }
    set(Command::scale).restart = set(Command::sharp_2d).restart = true;
    // the players: Automatic, All controllers (player 1) / Nobody (player 2), then each controller that is connected
    for (int player = 0; player < 2; ++player) {
        quickmenu::Setting &s = set(player ? Command::opt_player2 : Command::opt_player1);
        const std::vector<std::string> &ids = st.player_ids[player];
        for (std::size_t c = 0; c < ids.size(); ++c) {
            s.choices.push_back(c < st.player_words[player].size() ? st.player_words[player][c] : ids[c]);
            if (ids[c] == st.player_choice[player]) s.index = static_cast<int>(c);
        }
        s.fallback = 0;
    }
    {  // the EyeToy camera: None, then each webcam (used at the next start)
        quickmenu::Setting &s = set(Command::opt_webcam);
        for (std::size_t c = 0; c < st.webcam_ids.size(); ++c) {
            s.choices.push_back(c < st.webcam_words.size() ? st.webcam_words[c] : st.webcam_ids[c]);
            if (st.webcam_ids[c] == st.webcam_next) s.index = static_cast<int>(c);
        }
        s.next_start = st.webcam_next != st.webcam_now;
        s.restart = true;
    }
}
