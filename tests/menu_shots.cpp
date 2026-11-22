// Pictures of every quick menu page, to look at without starting a game (host/quickmenu.hpp):
// ninja ps2menushots && ./ps2menushots <out folder> [font.ttf]
// Each page is written as a window-sized 32-bit TGA with alpha (see-through outside the panel), at 1600x900 and 2560x1440.
#include "../host/quickmenu.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

using quickmenu::Command;
using quickmenu::Input;

quickmenu::Setting &at(quickmenu::Values &v, Command c) { return v.settings[static_cast<int>(c)]; }

quickmenu::Values values() {
    quickmenu::Values v;
    at(v, Command::look_filter) = {3, {"Off", "Crisp", "Soft", "Sharp"}};
    at(v, Command::opt_aa) = {1, {"Off", "Fast", "2x", "3x", "4x", "5x", "6x", "7x", "8x"}};
    at(v, Command::scale) = {2, {"1x", "2x", "3x", "4x"}};
    at(v, Command::opt_deband) = {1, {"Off", "Light", "Strong"}};
    at(v, Command::opt_textures) = {1, {"Original", "Updated (HD)"}};
    at(v, Command::opt_ui_width) = {0, {"true shape (75 %)", "80 % wide", "85 % wide", "90 % wide", "stretched (100 %)"}};
    at(v, Command::sharp_2d) = {1, {"Soft", "Sharp", "Sharpest"}};
    at(v, Command::fullscreen) = {1, {"Windowed", "Borderless", "Full screen"}};
    at(v, Command::opt_screen_res) = {0, {"2560 x 1440 (desktop)", "1920 x 1080", "1600 x 900", "1280 x 720"}};
    at(v, Command::opt_aspect) = {1, {"4:3", "16:9", "21:9"}};
    at(v, Command::opt_frame_rate) = {1, {"50 fps (original)", "60 fps", "75 fps", "90 fps", "100 fps",
                                          "120 fps", "144 fps", "165 fps", "240 fps"}};
    at(v, Command::opt_load_turbo) = {1, {"off", "on"}};
    at(v, Command::opt_disc_speed) = {2, {"1x (original)", "2x", "4x", "8x", "16x"}, true};
    at(v, Command::opt_pad_merge) = {1, {"off", "on"}};
    at(v, Command::opt_player1) = {3, {"Automatic (first controller)", "All controllers", "1: Xbox 360 Controller (virtual)", "2: Xbox 360 Controller (virtual)",
                                       "3: Xbox Wireless Controller  - pressed just now", "4: Xbox 360 Controller (virtual)"}};
    at(v, Command::opt_player2) = {1, {"Automatic (second controller)", "Nobody", "1: Xbox 360 Controller (virtual)", "2: Xbox 360 Controller (virtual)",
                                       "3: Xbox Wireless Controller", "4: Xbox 360 Controller (virtual)"}};
    at(v, Command::opt_webcam) = {1, {"None (unplugged)", "Logitech C920 HD Pro Webcam", "Camo"}};
    at(v, Command::opt_webcam).restart = true;
    at(v, Command::opt_mouse_mode) = {0, {"PC style", "As a controller"}};  // local change
    for (Command c : {Command::look_filter, Command::scale, Command::opt_textures, Command::opt_frame_rate, Command::opt_ui_width}) at(v, c).updated = 0;
    const char *rows[][2] = {{"Cross", "Space, Enter, Keypad Enter"}, {"Circle", "E, Middle Mouse"}, {"Square", "Left Mouse"},
                             {"Triangle", "Right Mouse"}, {"L1", "Left Shift, Mouse X2"}, {"R1", "F"}, {"L2", "Q"}, {"R2", "R"},
                             {"Start", "Esc"}, {"Select", "Tab, Backspace"}, {"Left stick up", "W"}, {"Left stick down", "S"},
                             {"Left stick left", "A"}, {"Left stick right", "D"}};
    for (auto &r : rows) v.key_rows.push_back({r[0], r[1]});
    v.key_note = "Cross now: Space, Enter, Keypad Enter";
    v.slots[0] = {true, "16 Nov 2026, 21:05", "Goth family, before the party"};
    v.slots[3] = {true, "16 Nov 2026, 21:40"};
    v.card_library = true;
    v.slot_cards[0] = "Pleasantview";
    v.slot_cards[1] = "Strangetown";
    v.cards = {{"Belladonna Cove", "", 0}, {"Pleasantview", "17 Nov, 01:12", 1}, {"Riverview", "15 Nov, 20:44", 0}, {"Strangetown", "16 Nov, 22:03", 2},
               {"Twinbrook", "", 0}, {"Veronaville", "14 Nov, 18:30", 0}, {"Willow Creek", "", 0}, {"Windenburg", "", 0}, {"Zoe's story", "17 Nov, 00:10", 0}};
    return v;
}

void write_tga(const std::string &path, int w, int h, const quickmenu::Picture &p) {
    std::FILE *out = std::fopen(path.c_str(), "wb");
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return;
    }
    const unsigned char header[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                      static_cast<unsigned char>(w & 255), static_cast<unsigned char>(w >> 8),
                                      static_cast<unsigned char>(h & 255), static_cast<unsigned char>(h >> 8), 32, 0x28};  // top-left origin, 8 alpha bits
    std::fwrite(header, 1, sizeof header, out);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            std::uint32_t c = 0;
            const int px = x - p.x, py = y - p.y;
            if (px >= 0 && py >= 0 && px < p.width && py < p.height) c = p.rgba[static_cast<std::size_t>(py) * p.width + px];
            const unsigned char bgra[4] = {static_cast<unsigned char>(c >> 16), static_cast<unsigned char>(c >> 8), static_cast<unsigned char>(c),
                                           static_cast<unsigned char>(c >> 24)};
            std::fwrite(bgra, 1, 4, out);
        }
    std::fclose(out);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ps2menushots <out folder> [font.ttf]\n");
        return 2;
    }
    const std::string dir = argv[1];
    if (argc > 2) {
#ifdef _WIN32
        _putenv_s("PS2_MENU_FONT", argv[2]);
#else
        setenv("PS2_MENU_FONT", argv[2], 1);
#endif
    }
    const quickmenu::Values v = values();
    const int sizes[][2] = {{1600, 900}, {2560, 1440}};
    for (const auto &size : sizes) {
        const std::string tail = "_" + std::to_string(size[1]) + ".tga";
        const auto shot = [&](const char *name, auto &&steps) {
            quickmenu::Menu m;
            m.set_key_rows(static_cast<int>(v.key_rows.size()));
            m.open();
            steps(m);
            quickmenu::Picture p;
            m.draw(size[0], size[1], v, p);
            write_tga(dir + "/" + name + tail, size[0], size[1], p);
        };
        // local change: the launcher's menu (Mode::launcher)
        const auto launcher_shot = [&](const char *name, const std::string &status, auto &&steps) {
            quickmenu::Menu m(quickmenu::Mode::launcher);
            m.open();
            m.set_main_status(status);
            steps(m);
            quickmenu::Picture p;
            m.draw(size[0], size[1], v, p);
            write_tga(dir + "/" + name + tail, size[0], size[1], p);
        };
        launcher_shot("launcher_main", "", [](quickmenu::Menu &) {});
        {  // local change: the install pages (stage 2)
            quickmenu::Values w = v;
            w.install.iso = "C:\Games\Sims 2, The (Europe) (En,Fr,De,Es,It,Nl,Sv,No,Da,Fi).iso";
            w.install.can_install = true;
            w.install.downloads = {{"llvm-mingw 20261020 (C/C++ compiler)", "182 MB"}, {"CMake 4.5.0", "54 MB"}, {"Ninja 1.13.2", "1 MB"},
                                   {"PortableGit 2.56.1 (bash, curl, unzip)", "57 MB"}, {"Python 3.14.7 (embeddable)", "12 MB"},
                                   {"The remaster's source and the build's libraries (SDL2, wgpu, DXC)", "about 70 MB"}};
            w.install.total = "About 376 MB to download; 3.0 GB of free space needed.";
            w.install.free = "412.7 GB free here.";
            const auto install_shot = [&](const char *name, quickmenu::Page page, const quickmenu::Values &vals, auto &&steps) {
                quickmenu::Menu m(quickmenu::Mode::launcher);
                m.open();
                m.show(page);
                steps(m);
                quickmenu::Picture p;
                m.draw(size[0], size[1], vals, p);
                write_tga(dir + "/" + name + tail, size[0], size[1], p);
            };
            install_shot("install", quickmenu::Page::install, w, [&](quickmenu::Menu &m) { m.press(Input::down, w); });
            quickmenu::Values us = w;
            us.install.can_install = false;
            us.install.disc_line = "The US version (SLUS-21265) is not supported yet.";
            install_shot("install_slus", quickmenu::Page::install, us, [](quickmenu::Menu &) {});
            quickmenu::Values pk = w;
            pk.picker.folder = "D:\PCSX2\_games";
            pk.picker.entries = {{"..", 0}, {"BIOS", 1}, {"Saves", 1}, {"Bully (Europe).iso", 2}, {"Sims 2, The (Europe) (En,Fr,De,Es,It,Nl,Sv,No,Da,Fi).iso", 2}};
            install_shot("picker", quickmenu::Page::pick_iso, pk, [&](quickmenu::Menu &m) { for (int i = 0; i < 4; ++i) m.press(Input::down, pk); });
            quickmenu::Values pr = w;
            pr.progress.steps = {{"Free space", 2}, {"Build tools (download and unpack)", 2}, {"The remaster's source", 2}, {"Your data folder (cards, settings)", 2},
                                 {"Building the game from your disc", 1}};
            pr.progress.fraction = 0.61;
            pr.progress.line = "[install] step 4/5: building the game (this takes a few minutes)";
            install_shot("progress", quickmenu::Page::progress, pr, [](quickmenu::Menu &) {});
            quickmenu::Values pe = pr;
            pe.progress.steps[4].second = 3;
            pe.progress.error = "no connection to github.com (error 12007)";
            install_shot("progress_error", quickmenu::Page::progress, pe, [](quickmenu::Menu &) {});
            quickmenu::Menu up(quickmenu::Mode::launcher);
            up.set_update("v1.1");
            up.open();
            quickmenu::Picture p;
            up.draw(size[0], size[1], v, p);
            write_tga(dir + "/main_update" + tail, size[0], size[1], p);
        }
        launcher_shot("launcher_main_status", "Memory cards: slot 1 Bridgeport, slot 2 empty", [&](quickmenu::Menu &m) { m.press(Input::down, v); });
        launcher_shot("launcher_keys_greyed", "", [&](quickmenu::Menu &m) {
            m.press(Input::down, v);
            m.press(Input::down, v);
            m.press(Input::confirm, v);
            m.press(Input::tab_next, v);
            m.press(Input::tab_next, v);
            m.press(Input::right, v);
            m.press(Input::confirm, v);
            m.press(Input::confirm, v);
        });
        const auto settings = [&](quickmenu::Menu &m, int tab) {
            for (int i = 0; i < 5; ++i) m.press(Input::down, v);  // Resume -> Settings
            m.press(Input::confirm, v);
            for (int i = 0; i < tab; ++i) m.press(Input::tab_next, v);
        };
        shot("main", [&](quickmenu::Menu &m) { m.press(Input::up, v); m.press(Input::up, v); });
        shot("graphics", [&](quickmenu::Menu &m) { settings(m, 0); });
        shot("graphics_radio", [&](quickmenu::Menu &m) {
            settings(m, 0);
            m.press(Input::confirm, v);
            m.press(Input::down, v);
            m.press(Input::down, v);
            m.press(Input::down, v);
            m.press(Input::right, v);
            m.press(Input::confirm, v);
            m.press(Input::right, v);
        });
        shot("graphics_slider", [&](quickmenu::Menu &m) {
            settings(m, 0);
            m.press(Input::confirm, v);
            m.press(Input::down, v);
            m.press(Input::down, v);
            m.press(Input::right, v);
        });
        shot("graphics_buttons", [&](quickmenu::Menu &m) {
            settings(m, 0);
            m.press(Input::down, v);
            m.press(Input::right, v);
            m.press(Input::confirm, v);  // Updated: fills in, on to Apply
        });
        shot("game", [&](quickmenu::Menu &m) {
            settings(m, 1);
            m.press(Input::right, v);
            m.press(Input::confirm, v);
            m.press(Input::down, v);
        });
        shot("game_list_open", [&](quickmenu::Menu &m) {
            settings(m, 1);
            m.press(Input::confirm, v);
            m.press(Input::confirm, v);
            m.press(Input::down, v);
        });
        shot("controls", [&](quickmenu::Menu &m) { settings(m, 2); });
        shot("controls_player1", [&](quickmenu::Menu &m) {
            settings(m, 2);
            m.press(Input::confirm, v);
            m.press(Input::confirm, v);
        });
        shot("keys", [&](quickmenu::Menu &m) {
            settings(m, 2);
            m.press(Input::right, v);
            m.press(Input::confirm, v);
            m.press(Input::down, v);
            m.press(Input::confirm, v);
            m.press(Input::down, v);
        });
        shot("states", [&](quickmenu::Menu &m) {
            for (int i = 0; i < 3; ++i) m.press(Input::down, v);
            m.press(Input::confirm, v);
            m.press(Input::right, v);
        });
        shot("states_rename", [&](quickmenu::Menu &m) {
            for (int i = 0; i < 3; ++i) m.press(Input::down, v);
            m.press(Input::confirm, v);
            for (int i = 0; i < 4; ++i) m.press(Input::down, v);  // slot 4
            m.press(Input::right, v);                             // its Rename button
            m.press(Input::confirm, v);
            m.type_text("Before the w");
            m.press(Input::down, v);
        });
        shot("cards", [&](quickmenu::Menu &m) {
            for (int i = 0; i < 4; ++i) m.press(Input::down, v);
            m.press(Input::confirm, v);
            for (int i = 0; i < 5; ++i) m.press(Input::down, v);  // Riverview
            m.press(Input::right, v);                             // its Slot 2 button
        });
        shot("cards_rename", [&](quickmenu::Menu &m) {
            for (int i = 0; i < 4; ++i) m.press(Input::down, v);
            m.press(Input::confirm, v);
            for (int i = 0; i < 3; ++i) m.press(Input::down, v);
            m.press(Input::right, v);
            m.press(Input::right, v);
            m.press(Input::confirm, v);
        });
        shot("states_question", [&](quickmenu::Menu &m) {
            for (int i = 0; i < 3; ++i) m.press(Input::down, v);
            m.press(Input::confirm, v);
            m.press(Input::confirm, v);
        });
        quickmenu::Picture note;
        quickmenu::Menu::draw_note(size[0], size[1], "Quick save", note);
        write_tga(dir + "/note" + tail, size[0], size[1], note);
    }
    std::printf("written to %s\n", dir.c_str());
    return 0;
}
